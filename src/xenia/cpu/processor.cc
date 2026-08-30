/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <fstream>
#include <set>
#include <cstdlib>
#include <cstring>

#include "xenia/cpu/processor.h"

#include "xenia/cpu/backend/code_cache.h"

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/base/string_buffer.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

// TODO(benvanik): based on compiler support
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif

#if 0 && DEBUG
#define DEFAULT_DEBUG_FLAG true
#else
#define DEFAULT_DEBUG_FLAG false
#endif

DEFINE_bool(debug, DEFAULT_DEBUG_FLAG,
            "Allow debugging and retain debug information.", "General");
DEFINE_path(trace_function_data_path, "", "File to write trace data to.",
            "CPU");
DEFINE_bool(break_on_start, false, "Break into the debugger on startup.",
            "CPU");
DEFINE_string(
    find_guest_pattern, "",
    "Diagnostic: hex value[:hex mask] list; every aligned word in the XEX "
    "range matching is listed on the first stack dump.",
    "CPU");
DEFINE_string(
    find_guest_refs, "",
    "Diagnostic: hex guest addresses (comma separated); on the first stack dump, scan the "
    "executable (82000000-83400000) for lis/addis + d-form references to it "
    "and log each site.",
    "CPU");
DEFINE_string(
    find_guest_calls, "",
    "Diagnostic: hex guest addresses (comma separated); on the first stack "
    "dump, scan the executable for b/bl instructions targeting each and log "
    "the sites.",
    "CPU");
DEFINE_string(
    poke_guest_memory, "",
    "Diagnostic: addr:hexvalue[@seconds] list; each 32-bit word is written "
    "(big-endian) at the first stack dump at or after that many seconds.",
    "CPU");
DEFINE_string(
    watch_guest_pointer, "",
    "Hex guest address to sample on every stack dump. Dumps the word there and "
    "the memory it points at, for watching a value a spin loop polls.",
    "CPU");
DEFINE_string(
    disasm_guest_windows, "",
    "Comma-separated hex guest address ranges (start-end) to disassemble on "
    "the first periodic stack dump. For reading code the dump only names.",
    "CPU");
DEFINE_int32(
    stack_dump_interval_seconds, 0,
    "If non-zero, periodically log every guest thread's call stack with guest "
    "PCs resolved. For diagnosing hangs. Requires a working stack walker.",
    "CPU");

namespace xe {
namespace kernel {
class XThread;
}  // namespace kernel

namespace cpu {

using xe::cpu::ppc::PPCOpcode;
using xe::kernel::XThread;

using namespace xe::literals;

class BuiltinModule : public Module {
 public:
  explicit BuiltinModule(Processor* processor)
      : Module(processor), name_("builtin") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  bool ContainsAddress(uint32_t address) override {
    return (address & 0xFFFFFFF0) == 0xFFFFFFF0;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(new BuiltinFunction(this, address));
  }

 private:
  std::string name_;
};

Processor::Processor(xe::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {}

Processor::~Processor() {
  stack_dump_running_ = false;
  if (stack_dump_thread_.joinable()) {
    stack_dump_thread_.join();
  }

  {
    auto global_lock = global_critical_region_.Acquire();
    modules_.clear();
  }

  frontend_.reset();
  backend_.reset();

  if (functions_trace_file_) {
    functions_trace_file_->Flush();
    functions_trace_file_.reset();
  }
}

bool Processor::Setup(std::unique_ptr<backend::Backend> backend) {
  // TODO(benvanik): query mode from debugger?
  debug_info_flags_ = 0;

  auto frontend = std::make_unique<ppc::PPCFrontend>(this);
  // TODO(benvanik): set options/etc.

  // Must be initialized by subclass before calling into this.
  assert_not_null(memory_);

  std::unique_ptr<Module> builtin_module(new BuiltinModule(this));
  builtin_module_ = builtin_module.get();
  modules_.push_back(std::move(builtin_module));

  if (frontend_ || backend_) {
    return false;
  }

  if (!backend) {
    return false;
  }
  if (!backend->Initialize(this)) {
    return false;
  }
  if (!frontend->Initialize()) {
    return false;
  }

  backend_ = std::move(backend);
  frontend_ = std::move(frontend);

  // Stack walker is used when profiling, debugging, and dumping.
  // Note that creation may fail, in which case we'll have to disable those
  // features.
  // The code cache may be unavailable in case of a "null" backend.
  cpu::backend::CodeCache* code_cache = backend_->code_cache();
  if (code_cache) {
    stack_walker_ = StackWalker::Create(code_cache);
  }
  if (!stack_walker_) {
    // TODO(benvanik): disable features.
    if (cvars::debug) {
      XELOGW("Disabling --debug due to lack of stack walker");
      cvars::debug = false;
    }
  }

  // Optional hang-diagnosis watchdog: periodically log every guest thread's
  // call stack with guest PCs resolved.
  if (cvars::stack_dump_interval_seconds > 0) {
    if (!stack_walker_) {
      XELOGW("--stack_dump_interval_seconds needs a stack walker; ignoring");
    } else {
      stack_dump_running_ = true;
      stack_dump_thread_ = std::thread([this]() {
        xe::threading::set_name("Stack Dump Watchdog");
        const auto interval =
            std::chrono::seconds(cvars::stack_dump_interval_seconds);
        while (stack_dump_running_) {
          auto deadline = std::chrono::steady_clock::now() + interval;
          while (stack_dump_running_ &&
                 std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
          }
          if (!stack_dump_running_) {
            break;
          }
          DumpThreadStacks();
        }
      });
    }
  }

  // Open the trace data path, if requested.
  functions_trace_path_ = cvars::trace_function_data_path;
  if (!functions_trace_path_.empty()) {
    functions_trace_file_ =
        ChunkedMappedMemoryWriter::Open(functions_trace_path_, 32_MiB, true);
  }

  return true;
}

void Processor::PreLaunch() {
  if (cvars::break_on_start) {
    // Start paused.
    XELOGI("Breaking into debugger because of --break_on_start...");
    execution_state_ = ExecutionState::kRunning;
    Pause();
  } else {
    // Start running.
    execution_state_ = ExecutionState::kRunning;
  }
}

bool Processor::AddModule(std::unique_ptr<Module> module) {
  auto global_lock = global_critical_region_.Acquire();
  modules_.push_back(std::move(module));
  return true;
}

void Processor::RemoveModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();

  auto itr = std::ranges::find_if(
      std::as_const(modules_),
      [name](std::unique_ptr<xe::cpu::Module> const& module) {
        return module->name() == name;
      });

  if (itr != modules_.cend()) {
    const std::vector<uint32_t> addressed_functions =
        (*itr)->GetAddressedFunctions();

    modules_.erase(itr);

    for (const uint32_t entry : addressed_functions) {
      RemoveFunctionByAddress(entry);
    }
  }
}

Module* Processor::GetModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& module : modules_) {
    if (module->name() == name) {
      return module.get();
    }
  }
  return nullptr;
}

std::vector<Module*> Processor::GetModules() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Module*> clone(modules_.size());
  for (const auto& module : modules_) {
    clone.push_back(module.get());
  }
  return clone;
}

Function* Processor::DefineBuiltin(const std::string_view name,
                                   BuiltinFunction::Handler handler, void* arg0,
                                   void* arg1) {
  uint32_t address = next_builtin_address_;
  next_builtin_address_ += 4;

  Function* function;
  builtin_module_->DeclareFunction(address, &function);
  function->set_end_address(address + 4);
  function->set_name(name);

  auto builtin_function = static_cast<BuiltinFunction*>(function);
  builtin_function->SetupBuiltin(handler, arg0, arg1);

  function->set_status(Symbol::Status::kDeclared);
  return function;
}

Function* Processor::QueryFunction(uint32_t address) {
  auto entry = entry_table_.Get(address);
  if (!entry) {
    return nullptr;
  }
  return entry->function;
}

std::vector<Function*> Processor::FindFunctionsWithAddress(uint32_t address) {
  return entry_table_.FindWithAddress(address);
}

void Processor::RemoveFunctionByAddress(uint32_t address) {
  entry_table_.Delete(address);
}

Function* Processor::ResolveFunction(uint32_t address) {
  Entry* entry;
  Entry::Status status = entry_table_.GetOrCreate(address, &entry);
  if (status == Entry::STATUS_NEW) {
    // Needs to be generated. We have the 'lock' on it and must do so now.

    // Grab symbol declaration.
    auto function = LookupFunction(address);

    if (!function) {
      entry_table_.MarkFailed(entry);
      return nullptr;
    }

    if (!DemandFunction(function)) {
      entry_table_.MarkFailed(entry);
      return nullptr;
    }
    // only add it to the list of resolved functions if resolving succeeded
    auto module_for = function->module();

    auto xexmod = dynamic_cast<XexModule*>(module_for);
    if (xexmod) {
      auto addr_flags = xexmod->GetInstructionAddressFlags(address);
      if (addr_flags) {
        addr_flags->was_resolved = 1;
      }
    }

    entry_table_.MarkReady(entry, function, function->end_address());
    status = Entry::STATUS_READY;
  }
  if (status == Entry::STATUS_READY) {
    // Ready to use.
    return entry->function;
  } else {
    // Failed or bad state.
    return nullptr;
  }
}
Module* Processor::LookupModule(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  // TODO(benvanik): sort by code address (if contiguous) so can bsearch.
  // TODO(benvanik): cache last module low/high, as likely to be in there.
  for (const auto& module : modules_) {
    if (module->ContainsAddress(address)) {
      return module.get();
    }
  }
  return nullptr;
}
Function* Processor::LookupFunction(uint32_t address) {
  // TODO(benvanik): fast reject invalid addresses/log errors.

  // Find the module that contains the address.
  Module* code_module = LookupModule(address);

  if (!code_module) {
    // No module found that could contain the address.
    return nullptr;
  }

  return LookupFunction(code_module, address);
}

Function* Processor::LookupFunction(Module* module, uint32_t address) {
  // Atomic create/lookup symbol in module.
  // If we get back the NEW flag we must declare it now.
  Function* function = nullptr;
  auto symbol_status = module->DeclareFunction(address, &function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undeclared, so declare now.
    assert_true(function->is_guest());
    if (!frontend_->DeclareFunction(static_cast<GuestFunction*>(function))) {
      function->set_status(Symbol::Status::kFailed);
      return nullptr;
    }
    function->set_status(Symbol::Status::kDeclared);
  }
  return function;
}

bool Processor::DemandFunction(Function* function) {
  // Lock function for generation. If it's already being generated
  // by another thread this will block and return DECLARED.
  auto module = function->module();
  auto symbol_status = module->DefineFunction(function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undefined, so define now.
    assert_true(function->is_guest());
    if (!frontend_->DefineFunction(static_cast<GuestFunction*>(function),
                                   debug_info_flags_)) {
      function->set_status(Symbol::Status::kFailed);
      return false;
    }

    // Before we give the symbol back to the rest, let the debugger know.
    OnFunctionDefined(function);

    function->set_status(Symbol::Status::kDefined);
    symbol_status = function->status();
  }

  if (symbol_status == Symbol::Status::kFailed) {
    // Symbol likely failed.
    return false;
  }

  return true;
}

bool Processor::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  auto context = thread_state->context();

  // Pad out stack a bit, as some games seem to overwrite the caller by about
  // 16 to 32b.
  context->r[1] -= 64 + 112;

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint64_t previous_lr = context->lr;
  context->lr = 0xBCBCBCBC;

  // Execute the function.
  auto result = function->Call(thread_state, uint32_t(context->lr));

  context->lr = previous_lr;
  context->r[1] += 64 + 112;

  return result;
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  return function->Call(thread_state, 0xBCBCBCBC);
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address,
                            uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  auto context = thread_state->context();
  for (size_t i = 0; i < std::min(arg_count, static_cast<size_t>(8)); ++i) {
    context->r[3 + i] = args[i];
  }

  if (arg_count > 7) {
    // Rest of the arguments go on the stack.
    // FIXME: This assumes arguments are 32 bits!
    auto stack_arg_base =
        memory()->TranslateVirtual((uint32_t)context->r[1] + 0x54 - (64 + 112));
    for (size_t i = 0; i < arg_count - 8; i++) {
      xe::store_and_swap<uint32_t>(stack_arg_base + (i * 8),
                                   (uint32_t)args[i + 8]);
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return context->r[3];
}

bool Processor::Save(ByteStream* stream) {
  stream->Write(kProcessorSaveSignature);
  return true;
}

bool Processor::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kProcessorSaveSignature) {
    XELOGE("Processor::Restore - Invalid magic value!");
    return false;
  }

  // Clear cached thread data for zombie threads.
  std::vector<uint32_t> to_delete;
  for (auto& it : thread_debug_infos_) {
    if (it.second->state == ThreadDebugInfo::State::kZombie) {
      it.second->thread_handle = 0;
      to_delete.push_back(it.first);
    }
  }
  for (uint32_t thread_id : to_delete) {
    thread_debug_infos_.erase(thread_id);
  }

  return true;
}

uint8_t* Processor::AllocateFunctionTraceData(size_t size) {
  if (!functions_trace_file_) {
    return nullptr;
  }
  return functions_trace_file_->Allocate(size);
}

void Processor::OnFunctionDefined(Function* function) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address_type() == Breakpoint::AddressType::kGuest) {
      if (function->ContainsAddress(breakpoint->guest_address())) {
        if (breakpoint->is_installed()) {
          backend_->InstallBreakpoint(breakpoint, function);
        }
      }
    }
  }
}

void Processor::OnThreadCreated(uint32_t thread_handle,
                                ThreadState* thread_state, Thread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto thread_info = std::make_unique<ThreadDebugInfo>();
  thread_info->thread_id = thread_state->thread_id();
  thread_info->thread = thread;
  thread_info->state = ThreadDebugInfo::State::kAlive;
  thread_info->suspended = false;
  thread_info->thread_handle = thread_handle;
  // A restored thread reuses its saved id; replace the exited entry.
  thread_debug_infos_.erase(thread_info->thread_id);
  thread_debug_infos_.emplace(thread_info->thread_id, std::move(thread_info));
}

void Processor::OnThreadExit(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kExited;
}

void Processor::OnThreadDestroyed(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  it->second->thread_handle = 0;
  thread_debug_infos_.erase(it);
}

void Processor::OnThreadEnteringWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kWaiting;
}

void Processor::OnThreadLeavingWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  if (thread_info->state == ThreadDebugInfo::State::kWaiting) {
    thread_info->state = ThreadDebugInfo::State::kAlive;
  }
}

std::vector<ThreadDebugInfo*> Processor::QueryThreadDebugInfos() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<ThreadDebugInfo*> result;
  for (auto& it : thread_debug_infos_) {
    result.push_back(it.second.get());
  }
  return result;
}

ThreadDebugInfo* Processor::QueryThreadDebugInfo(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  const auto& it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void Processor::AddBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Add to breakpoints map.
  breakpoints_.push_back(breakpoint);

  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Resume();
  }
}

void Processor::RemoveBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Uninstall (if needed).
  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Suspend();
  }

  // Remove from breakpoint map.
  auto it = std::ranges::find(breakpoints_, breakpoint);
  breakpoints_.erase(it);
}

Breakpoint* Processor::FindBreakpoint(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address() == address) {
      return breakpoint;
    }
  }
  return nullptr;
}

void Processor::set_debug_listener(DebugListener* debug_listener) {
  if (debug_listener == debug_listener_) {
    return;
  }
  if (debug_listener_) {
    // Detach old debug listener.
    debug_listener_->OnDetached();
    debug_listener_ = nullptr;
  }
  if (debug_listener) {
    debug_listener_ = debug_listener;
  } else {
    if (execution_state_ == ExecutionState::kPaused) {
      XELOGI("Debugger detaching while execution is paused; continuing...");
      Continue();
    }
  }
}

void Processor::DemandDebugListener() {
  if (debug_listener_) {
    // Already present.
    debug_listener_->OnFocus();
    return;
  }
  if (!debug_listener_handler_) {
    XELOGE("Debugger demanded a listener but no handler was registered.");
    xe::debugging::Break();
    return;
  }
  set_debug_listener(debug_listener_handler_(this));
}

bool Processor::OnThreadBreakpointHit(Exception* ex) {
  // Where to resume, taken from the exception itself. thread_info's
  // host_context is not safe for this: while this thread is parked below,
  // any other thread's breakpoint hit (or a stack dump) calls
  // UpdateThreadExecutionStates, which re-captures every thread's host
  // context - for a thread sitting in sem_wait inside this handler that is
  // a libc address, and resuming the JIT frame there ran off into garbage.
  const uint64_t trap_pc = ex->pc();
  auto global_lock = global_critical_region_.Acquire();

  // Suspend all threads (but ourselves).
  SuspendAllThreads();

  // Lookup thread info block.
  auto it = thread_debug_infos_.find(ThreadState::GetThreadID());
  if (it == thread_debug_infos_.end()) {
    // Not found - exception on a thread we don't know about?
    assert_always("UD2 on a thread we don't track");
    return false;
  }
  auto thread_info = it->second.get();

  // Run through and uninstall all breakpoint UD2s to get us back to a clean
  // state.
  if (execution_state_ != ExecutionState::kStepping) {
    SuspendAllBreakpoints();
  }

  // Update all thread states with their latest values, using the context we
  // got from the exception instead of a sampled value (as it would just show
  // the exception handler).
  UpdateThreadExecutionStates(thread_info->thread_id, ex->thread_context());

  // Walk the captured thread stack and look for breakpoints at any address in
  // the stack. We just look for the first one.
  {
    std::string desc;
    for (size_t i = 0; i < std::min<size_t>(thread_info->frames.size(), 6);
         ++i) {
      desc += fmt::format(" {:08X}/{:X}", thread_info->frames[i].guest_pc,
                          thread_info->frames[i].host_pc);
    }
    std::string bps;
    for (auto b : breakpoints_) {
      bps += fmt::format(" {:08X}", b->guest_address());
    }
    XELOGI("OnThreadBreakpointHit: thread {} frames:{} breakpoints:{} "
           "exception_pc={:X} ts_match={}",
           thread_info->thread_id, desc, bps,
           ThreadState::Get() ? ThreadState::Get()->current_exception_pc() : 0,
           ThreadState::Get() == thread_info->thread->thread_state());
  }
  Breakpoint* breakpoint = nullptr;
  for (size_t i = 0; i < thread_info->frames.size(); ++i) {
    auto& frame = thread_info->frames[i];
    for (auto scan_breakpoint : breakpoints_) {
      bool matched = false;
      if (scan_breakpoint->address_type() == Breakpoint::AddressType::kGuest) {
        matched = scan_breakpoint->guest_address() == frame.guest_pc;
        if (!matched) {
          // A guest instruction that emitted no code shares its host offset
          // with the next one, so the trap maps back to a different guest
          // PC than the breakpoint was set on. Compare host addresses too.
          scan_breakpoint->ForEachHostAddress([&](uint64_t host_address) {
            if (host_address == frame.host_pc) {
              matched = true;
            }
          });
        }
      } else if (scan_breakpoint->address_type() ==
                 Breakpoint::AddressType::kHost) {
        matched = scan_breakpoint->host_address() == frame.host_pc;
      }
      if (matched) {
        breakpoint = scan_breakpoint;
        break;
      }
    }
    if (breakpoint) {
      breakpoint->OnHit(thread_info, frame.host_pc);
      break;
    }
  }

  // We are waiting on the debugger now. Either wait for it to continue, add a
  // new step, or direct us somewhere else.
  // The debugger will ResumeAllThreads or just resume us (depending on what
  // it wants to do).
  execution_state_ = ExecutionState::kPaused;
  thread_info->suspended = true;

  // Must unlock, or we will deadlock.
  global_lock.unlock();

  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  ResumeAllThreads();
  thread_info->thread->thread()->Suspend();

  // Apply thread context changes.
  // TODO(benvanik): apply to all threads?
  // The ud2 overwrote the first two bytes of a real instruction unless
  // --emit_source_annotations padded each source offset with a nop. When the
  // breakpoints were uninstalled above (everything but the debugger's
  // stepping mode) the original bytes are back, so resume at the same PC
  // and re-execute the instruction; skipping two bytes would resume in the
  // middle of it.
#if XE_ARCH_AMD64
  ex->set_resume_pc(trap_pc +
                    (execution_state_ == ExecutionState::kStepping ? 2 : 0));
#elif XE_ARCH_ARM64
  ex->set_resume_pc(trap_pc + 2);
#else
#error Instruction pointer not specified for the target CPU architecture.
#endif  // XE_ARCH

  // Resume execution.
  return true;
}

void Processor::OnStepCompleted(ThreadDebugInfo* thread_info) {
  auto global_lock = global_critical_region_.Acquire();
  execution_state_ = ExecutionState::kPaused;
  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  // Note that we stay suspended.
}

bool Processor::OnUnhandledException(Exception* ex) {
  // If we have no listener return right away.
  // TODO(benvanik): DemandDebugListener()?
  if (!debug_listener_) {
    return false;
  }

  // If this isn't a managed thread, fail - let VS handle it for now.
  if (!Thread::IsInThread()) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Suspend all guest threads (but this one).
  SuspendAllThreads();

  UpdateThreadExecutionStates(Thread::GetCurrentThreadId(),
                              ex->thread_context());

  // Stop and notify the listener.
  // This will take control.
  assert_true(execution_state_ == ExecutionState::kRunning);
  execution_state_ = ExecutionState::kPaused;

  // Notify debugger that exceution stopped.
  // debug_listener_->OnException(info);
  debug_listener_->OnExecutionPaused();

  // Suspend self.
  Thread::GetCurrentThread()->thread()->Suspend();

  return true;
}

void Processor::ShowDebugger() {
  if (debug_listener_) {
    debug_listener_->OnFocus();
  } else {
    DemandDebugListener();
  }
}

bool Processor::SuspendAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (thread_info->suspended) {
      // Already suspended - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be suspended - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't suspend ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    bool did_suspend = thread->thread()->Suspend(nullptr);
    assert_true(did_suspend);
    thread_info->suspended = true;
  }
  return true;
}

bool Processor::ResumeThread(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return false;
  }
  auto thread_info = it->second.get();
  assert_true(thread_info->suspended);
  assert_false(thread_info->state == ThreadDebugInfo::State::kExited ||
               thread_info->state == ThreadDebugInfo::State::kZombie);
  thread_info->suspended = false;
  auto thread = thread_info->thread;
  return thread->thread()->Resume();
}

bool Processor::ResumeAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (!thread_info->suspended) {
      // Not suspended by us - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be resumed - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't resume ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    thread_info->suspended = false;
    bool did_resume = thread->thread()->Resume();
    assert_true(did_resume);
  }
  return true;
}

void Processor::UpdateThreadExecutionStates(
    uint32_t override_thread_id, HostThreadContext* override_context) {
  auto global_lock = global_critical_region_.Acquire();
  uint64_t frame_host_pcs[64];
  xe::cpu::StackFrame cpu_frames[64];
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    auto thread = thread_info->thread;
    if (!thread) {
      continue;
    }
    if (thread_info->state == ThreadDebugInfo::State::kZombie ||
        thread_info->state == ThreadDebugInfo::State::kExited) {
      // The host thread is gone; signalling its stale pthread_t for a stack
      // capture never completes and costs the capture timeout every time.
      thread_info->frames.clear();
      continue;
    }

    // Grab PPC context.
    // Note that this is only up to date if --store_all_context_values is
    // enabled (or --debug).
    if (thread->can_debugger_suspend()) {
      std::memcpy(&thread_info->guest_context,
                  thread->thread_state()->context(),
                  sizeof(thread_info->guest_context));
    }

    // Grab stack trace and X64 context then resolve all symbols.
    uint64_t hash;
    HostThreadContext* in_host_context = nullptr;
    if (override_thread_id == thread_info->thread_id) {
      // If we were passed an override context we use that. Otherwise, ask the
      // stack walker for a new context.
      in_host_context = override_context;
    }
    size_t count = stack_walker_->CaptureStackTrace(
        thread->thread()->native_handle(), frame_host_pcs, 0,
        xe::countof(frame_host_pcs), in_host_context,
        &thread_info->host_context, &hash);
    stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
    thread_info->frames.resize(count);
    for (size_t i = 0; i < count; ++i) {
      auto& cpu_frame = cpu_frames[i];
      auto& frame = thread_info->frames[i];
      frame.host_pc = cpu_frame.host_pc;
      frame.host_function_address = cpu_frame.host_symbol.address;
      frame.guest_pc = cpu_frame.guest_pc;
      frame.guest_function_address = 0;
      frame.guest_function = nullptr;
      auto function = cpu_frame.guest_symbol.function;
      if (cpu_frame.type == cpu::StackFrame::Type::kGuest && function) {
        frame.guest_function_address = function->address();
        frame.guest_function = function;
      } else {
        std::strncpy(frame.name, cpu_frame.host_symbol.name,
                     xe::countof(frame.name));
        frame.name[xe::countof(frame.name) - 1] = 0;
      }
    }
  }
}

namespace {
// Disassembles a window of PPC instructions around a guest PC straight out of
// guest memory. Cheaper than --disassemble_functions, which builds and keeps
// the disassembly for every translated function.
// A guest address is only safe to dereference if a heap owns it and the page
// is committed. TranslateVirtual() happily hands back a pointer into unmapped
// space, and reading that trips the access-violation handler.
std::vector<std::string> SplitList(const std::string& list) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    if (end > start) out.push_back(list.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

// "832471A0,83313668" -> values; empty/invalid entries skipped.
std::vector<uint32_t> ParseHexList(const std::string& list) {
  std::vector<uint32_t> out;
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    if (end > start) {
      out.push_back(uint32_t(
          std::strtoul(list.substr(start, end - start).c_str(), nullptr, 16)));
    }
    start = end + 1;
  }
  return out;
}

bool GuestAddressReadable(Memory* memory, uint32_t address) {
  if (!address) {
    return false;
  }
  const auto heap = memory->LookupHeap(address);
  if (!heap) {
    return false;
  }
  uint32_t protect = 0;
  if (!const_cast<BaseHeap*>(heap)->QueryProtect(address, &protect)) {
    return false;
  }
  return protect != 0;
}

void DumpGuestBytes(Memory* memory, uint32_t address, uint32_t count) {
  if (!GuestAddressReadable(memory, address) ||
      !GuestAddressReadable(memory, address + count - 1)) {
    XELOGI("      [{:08X}] unmapped", address);
    return;
  }
  auto host_ptr = memory->TranslateVirtual(address);
  StringBuffer buffer;
  for (uint32_t i = 0; i < count; ++i) {
    buffer.AppendFormat("{:02X} ", host_ptr[i]);
  }
  XELOGI("      [{:08X}] {}", address, buffer.to_string());
}

void DumpGuestDisasmWindow(Memory* memory, uint32_t guest_pc, int before,
                           int after);

void DumpGuestStackWords(Memory* memory, uint32_t sp, uint32_t words) {
  for (uint32_t i = 0; i < words; i += 4) {
    StringBuffer line;
    for (uint32_t j = 0; j < 4 && i + j < words; ++j) {
      uint32_t address = sp + (i + j) * 4;
      if (!GuestAddressReadable(memory, address)) {
        line.AppendFormat("........  ");
        continue;
      }
      uint32_t value =
          xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
      bool code = value >= 0x82000000 && value < 0x90000000;
      line.AppendFormat("{:08X}{} ", value, code ? "*" : " ");
    }
    XELOGI("      sp+{:03X}: {}", i * 4, line.to_string());
  }
}

void DumpGuestBackChain(Memory* memory, uint32_t sp, int max_frames) {
  uint32_t frame = sp;
  int disasm_budget = 2;
  for (int i = 0; i < max_frames; ++i) {
    if (!frame || (frame & 3) || !GuestAddressReadable(memory, frame)) {
      break;
    }
    uint32_t next = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(frame));
    if (next <= frame) {
      break;  // back chain must grow upward
    }
    if (!GuestAddressReadable(memory, next) ||
        !GuestAddressReadable(memory, frame + 8)) {
      break;
    }
    // The saved LR lives at a small fixed offset in the caller's frame. Print
    // both candidates and let the plausible one (a code address) speak.
    // This game's compiler parks the caller's return address at +8 of the
    // frame that saved it, not of the caller's frame, so read it from `frame`.
    auto frame_ptr = memory->TranslateVirtual(frame);
    uint32_t lr4 = xe::load_and_swap<uint32_t>(frame_ptr + 4);
    uint32_t lr8 = xe::load_and_swap<uint32_t>(frame_ptr + 8);
    const char* mark4 = (lr4 >= 0x82000000 && lr4 < 0x90000000) ? "*" : " ";
    const char* mark8 = (lr8 >= 0x82000000 && lr8 < 0x90000000) ? "*" : " ";
    XELOGI("      frame {:08X} -> {:08X}  lr+4={:08X}{} lr+8={:08X}{}", frame,
           next, lr4, mark4, lr8, mark8);
    // Disassemble around the first couple of plausible return addresses - the
    // immediate callers are what matter when chasing a spin.
    // The saved LR sits at +4 in some frames and +8 in others, so take
    // whichever looks like code.
    if (disasm_budget > 0 && *mark8 == '*') {
      --disasm_budget;
      DumpGuestDisasmWindow(memory, lr8, 8, 6);
    }
    frame = next;
  }
}

void DumpGuestDisasmWindow(Memory* memory, uint32_t guest_pc, int before,
                           int after) {
  uint32_t start = guest_pc - uint32_t(before) * 4;
  for (int i = 0; i < before + after + 1; ++i) {
    uint32_t address = start + uint32_t(i) * 4;
    if (!GuestAddressReadable(memory, address)) {
      continue;
    }
    uint32_t code = xe::load_and_swap<uint32_t>(memory->TranslateVirtual(address));
    StringBuffer buffer;
    if (!xe::cpu::ppc::DisasmPPC(address, code, &buffer)) {
      XELOGI("      {} {:08X}  {:08X}  <undecodable>",
             address == guest_pc ? "->" : "  ", address, code);
      continue;
    }
    XELOGI("      {} {:08X}  {:08X}  {}", address == guest_pc ? "->" : "  ",
           address, code, buffer.to_string());
  }
}
}  // namespace

void Processor::DumpThreadStacks() {
  if (!stack_walker_) {
    XELOGW("DumpThreadStacks: no stack walker on this platform");
    return;
  }
  UpdateThreadExecutionStates();
  auto infos = QueryThreadDebugInfos();
  XELOGI("===== guest thread stacks ({} threads) =====", infos.size());
  static bool disasm_windows_done = false;
  if (!disasm_windows_done && !cvars::disasm_guest_windows.empty()) {
    disasm_windows_done = true;
    const std::string& spec = cvars::disasm_guest_windows;
    size_t pos = 0;
    while (pos < spec.size()) {
      size_t end = spec.find(',', pos);
      if (end == std::string::npos) {
        end = spec.size();
      }
      std::string token = spec.substr(pos, end - pos);
      pos = end + 1;
      size_t dash = token.find('-');
      if (dash == std::string::npos) {
        continue;
      }
      uint32_t lo = uint32_t(std::strtoul(token.substr(0, dash).c_str(),
                                          nullptr, 16)) & ~3u;
      uint32_t hi = uint32_t(std::strtoul(token.substr(dash + 1).c_str(),
                                          nullptr, 16)) & ~3u;
      if (hi < lo) {
        continue;
      }
      XELOGI("===== disasm window {:08X}-{:08X} =====", lo, hi);
      DumpGuestDisasmWindow(memory_, lo, 0, int((hi - lo) / 4));
    }
  }
  static bool refs_scanned = false;
  if (!cvars::find_guest_pattern.empty() && !refs_scanned) {
    // value:mask pairs; every 4-byte aligned word in the XEX range whose
    // (word & mask) == value is listed (first 60 per pattern).
    for (auto item_view : xe::utf8::split(cvars::find_guest_pattern, ",")) {
      std::string item(item_view);
      size_t colon = item.find(':');
      uint32_t value = uint32_t(strtoul(std::string(item.substr(0, colon)).c_str(), nullptr, 16));
      uint32_t mask = colon == std::string::npos
                          ? 0xFFFFFFFFu
                          : uint32_t(strtoul(std::string(item.substr(colon + 1)).c_str(), nullptr, 16));
      uint32_t found = 0;
      for (uint32_t addr = 0x82000000; addr < 0x83400000; addr += 4) {
        if ((addr & 0xFFF) == 0 && !GuestAddressReadable(memory_, addr)) {
          addr += 0x1000 - 4;
          continue;
        }
        uint32_t word = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(addr));
        if ((word & mask) != value) {
          continue;
        }
        if (++found <= 60) {
          XELOGI("find_guest_pattern {:08X}:{:08X}: {:08X} = {:08X}", value, mask, addr, word);
        }
      }
      XELOGI("find_guest_pattern {:08X}:{:08X}: {} hit(s)", value, mask, found);
    }
  }
  if (!cvars::find_guest_refs.empty() && !refs_scanned) {
    refs_scanned = true;
    for (uint32_t target : ParseHexList(cvars::find_guest_refs)) {
    // PPC materialises a 32-bit address as lis rA, hi; op rX, lo(rA), where
    // hi = (target + 0x8000) >> 16 and lo = target - (hi << 16) (signed).
    uint16_t hi = uint16_t((target + 0x8000) >> 16);
    int16_t lo = int16_t(target - (uint32_t(hi) << 16));
    uint32_t found = 0;
    for (uint32_t addr = 0x82000000; addr < 0x83400000; addr += 4) {
      if ((addr & 0xFFF) == 0 && !GuestAddressReadable(memory_, addr)) {
        addr += 0x1000 - 4;
        continue;
      }
      uint32_t code = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(addr));
      // addis rD, 0, hi (lis).
      if ((code >> 26) != 15 || ((code >> 16) & 31) != 0 ||
          uint16_t(code & 0xFFFF) != hi) {
        continue;
      }
      uint32_t rd = (code >> 21) & 31;
      // Look ahead a few instructions for a d-form using rd with offset lo.
      for (uint32_t k = 4; k <= 12 * 4; k += 4) {
        if (!GuestAddressReadable(memory_, addr + k)) break;
        uint32_t c2 =
            xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(addr + k));
        uint32_t op = c2 >> 26;
        uint32_t ra = (c2 >> 16) & 31;
        uint32_t rt = (c2 >> 21) & 31;
        int16_t d = int16_t(c2 & 0xFFFF);
        // addi/lwz/stw/lhz/sth/lbz/stb/lfs/stfs... all use the d-form
        // opcode range 14, 32-55; the site is one that has ra == rd, d == lo.
        bool dform = op == 14 || (op >= 32 && op <= 55);
        if (dform && ra == rd && d == lo) {
          XELOGI("find_guest_refs {:08X}: {:08X} lis r{},{:04X}; {:08X} op{} "
                 "r{},{}(r{})",
                 target, addr, rd, hi, addr + k, op, rt, d, ra);
          ++found;
          break;
        }
        if (rt == rd && (op == 14 || op == 15 || (op >= 32 && op <= 47))) {
          break;  // rd overwritten first
        }
      }
    }
    XELOGI("find_guest_refs {:08X}: {} site(s)", target, found);
    }
  }
  static bool calls_scanned = false;
  if (!cvars::find_guest_calls.empty() && !calls_scanned) {
    calls_scanned = true;
    for (uint32_t target : ParseHexList(cvars::find_guest_calls)) {
      uint32_t found = 0;
      for (uint32_t addr = 0x82000000; addr < 0x83400000; addr += 4) {
        if ((addr & 0xFFF) == 0 && !GuestAddressReadable(memory_, addr)) {
          addr += 0x1000 - 4;
          continue;
        }
        uint32_t code =
            xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(addr));
        if ((code >> 26) != 18 || (code & 2)) {
          continue;  // not a relative b/bl
        }
        int32_t li = int32_t(code << 6) >> 6;  // sign-extend 26 bits
        uint32_t dest = uint32_t(int32_t(addr) + (li & ~3));
        if (dest == target) {
          XELOGI("find_guest_calls {:08X}: {:08X} {}", target, addr,
                 (code & 1) ? "bl" : "b");
          ++found;
        }
      }
      XELOGI("find_guest_calls {:08X}: {} site(s)", target, found);
    }
  }
  {
    static int dump_count = 0;
    static std::set<std::string> poked;
    ++dump_count;
    int elapsed = dump_count * std::max(1, int(cvars::stack_dump_interval_seconds));
    for (const std::string& spec : SplitList(cvars::poke_guest_memory)) {
      if (poked.count(spec)) continue;
      size_t colon = spec.find(':'), at = spec.find('@');
      if (colon == std::string::npos) continue;
      int when = at == std::string::npos ? 0 : std::atoi(spec.c_str() + at + 1);
      if (elapsed < when) continue;
      uint32_t address = uint32_t(std::strtoul(spec.substr(0, colon).c_str(), nullptr, 16));
      uint32_t value = uint32_t(std::strtoul(spec.substr(colon + 1, at == std::string::npos ? std::string::npos : at - colon - 1).c_str(), nullptr, 16));
      poked.insert(spec);
      if (!GuestAddressReadable(memory_, address)) {
        XELOGI("poke {}: [{:08X}] unmapped", spec, address);
        continue;
      }
      uint32_t before = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(address));
      xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(address), value);
      XELOGI("poke {}: [{:08X}] {:08X} -> {:08X} at ~{} s", spec, address, before, value, elapsed);
    }
  }
  for (const std::string& spec : SplitList(cvars::watch_guest_pointer)) {
    // Chain syntax: 6E1AE650>3C>0+2C0 = read [6E1AE650], add 3C, read,
    // add 0, read, add 2C0 -> the address watched. '+' adds, '>' reads.
    uint32_t watch = 0;
    bool ok = true;
    {
      size_t i = 0;
      while (i < spec.size() && spec[i] != '>' && spec[i] != '+') ++i;
      watch = uint32_t(std::strtoul(spec.substr(0, i).c_str(), nullptr, 16));
      while (ok && i < spec.size()) {
        char op = spec[i++];
        size_t j = i;
        while (j < spec.size() && spec[j] != '>' && spec[j] != '+') ++j;
        uint32_t v = uint32_t(std::strtoul(spec.substr(i, j - i).c_str(),
                                           nullptr, 16));
        i = j;
        if (op == '>') {
          if (!GuestAddressReadable(memory_, watch)) {
            XELOGI("watch {}: [{:08X}] unmapped in chain", spec, watch);
            ok = false;
            break;
          }
          watch = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(watch));
        }
        watch += v;
      }
    }
    if (!ok) {
      continue;
    }
    if (spec.find_first_of(">+") != std::string::npos) {
      XELOGI("watch {} -> {:08X}", spec, watch);
      if (GuestAddressReadable(memory_, watch)) {
        DumpGuestBytes(memory_, watch, 112);
      }
      continue;
    }
    if (GuestAddressReadable(memory_, watch)) {
      uint32_t target =
          xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(watch));
      XELOGI("watch [{:08X}] = {:08X}", watch, target);
      if (GuestAddressReadable(memory_, target)) {
        DumpGuestBytes(memory_, target, 112);
      } else {
        XELOGI("      -> {:08X} unmapped", target);
      }
    } else {
      XELOGI("watch [{:08X}] unmapped", watch);
    }
  }
  for (auto* info : infos) {
    if (!info->thread) {
      continue;
    }
    XELOGI("thread {:08X} '{}' state={} frames={}", info->thread_id,
           info->thread->thread_name(), static_cast<int>(info->state),
           info->frames.size());
    // Threads parked inside a kernel call have no guest frame, but their
    // context still says where in guest code they called from.
    {
      const auto& ctx = info->guest_context;
      uint32_t lr = uint32_t(ctx.lr);
      XELOGI("    ctx lr={:08X} r1={:08X} r3={:08X} r4={:08X} r31={:08X}", lr,
             uint32_t(ctx.r[1]), uint32_t(ctx.r[3]), uint32_t(ctx.r[4]),
             uint32_t(ctx.r[31]));
      // The backend records the guest LR at every function entry, which is an
      // exact guest call chain - no ABI guessing, and no mistaking string data
      // for a return address the way a stack scan does.
      auto* thread_state = info->thread->thread_state();
      if (thread_state && backend_) {
        uint32_t chain[24];
        uint32_t chain_sp[24];
        size_t chain_count = backend_->GetGuestCallChain(
            thread_state->context(), chain, chain_sp, xe::countof(chain));
        if (chain_count) {
          XELOGI("    guest call chain ({} frames, innermost first):",
                 chain_count);
          for (size_t c = 0; c < chain_count; ++c) {
            XELOGI("      [{:2}] {:08X}  entry sp={:08X}", c, chain[c],
                   chain_sp[c]);
          }
          // Callee-saved registers live just below each frame's entry stack
          // pointer, so this is where a frame's arguments can be recovered
          // after deeper calls have clobbered the registers.
          for (size_t c = 0; c < chain_count && c <= 2; ++c) {
            if (!chain_sp[c]) {
              continue;
            }
            XELOGI("      saved regs below frame [{:2}] sp {:08X}:", c,
                   chain_sp[c]);
            DumpGuestBytes(memory_, chain_sp[c] - 0x20, 32);
          }
          // Disassemble around the innermost few callers - that is where a
          // spin loop lives.
          for (size_t c = 1; c < chain_count && c <= 3; ++c) {
            if (chain[c] < 0x82000000 || chain[c] >= 0x90000000) {
              continue;
            }
            XELOGI("      --- caller [{:2}] {:08X} ---", c, chain[c]);
            DumpGuestDisasmWindow(memory_, chain[c], 10, 8);
          }
        } else {
          // No backend support - fall back to the PowerPC stack back chain.
          DumpGuestBackChain(memory_, uint32_t(ctx.r[1]), 12);
        }
      }
      if (lr >= 0x82000000 && lr < 0x90000000) {
        DumpGuestDisasmWindow(memory_, lr, 8, 6);
        // Guest calls are not host calls, so the guest chain only exists in
        // guest memory. Where a function stashes its caller's LR is decided by
        // the game's compiler, so read the prologue rather than guessing an
        // ABI offset.
        // Dump the raw guest stack words and flag the ones that look like
        // code, which shows where this game's compiler parks the saved LR
        // instead of assuming an ABI offset.
        DumpGuestStackWords(memory_, uint32_t(ctx.r[1]), 40);
      }
    }
    size_t shown = 0;
    for (auto& frame : info->frames) {
      if (shown++ >= 12) {
        break;
      }
      if (frame.guest_pc) {
        XELOGI("    guest {:08X} (fn {:08X}) {}", frame.guest_pc,
               frame.guest_function_address, frame.name);
        DumpGuestDisasmWindow(memory_, frame.guest_pc, 10, 26);
        // Guest registers for the frame, so addresses the code is polling can
        // be turned into concrete guest pointers.
        const auto& ctx = info->guest_context;
        XELOGI("      r3={:08X} r11={:08X} r29={:08X} r31={:08X} lr={:08X}",
               uint32_t(ctx.r[3]), uint32_t(ctx.r[11]), uint32_t(ctx.r[29]),
               uint32_t(ctx.r[31]), uint32_t(ctx.lr));
        // Spin loops usually poll memory hanging off a register. Dump a window
        // around r29 so the polled value is visible across successive samples.
        DumpGuestBytes(memory_, uint32_t(ctx.r[29]) + 0x2AB0, 32);
      } else {
        XELOGI("    host  {:016X} {}", frame.host_pc, frame.name);
      }
    }
  }
  XELOGI("===== end guest thread stacks =====");
}

void Processor::SuspendAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Suspend();
  }
}

void Processor::ResumeAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Resume();
  }
}

void Processor::Pause() {
  {
    auto global_lock = global_critical_region_.Acquire();
    assert_true(execution_state_ == ExecutionState::kRunning);
    SuspendAllThreads();
    SuspendAllBreakpoints();
    UpdateThreadExecutionStates();
    execution_state_ = ExecutionState::kPaused;
    if (debug_listener_) {
      debug_listener_->OnExecutionPaused();
    }
  }
  DemandDebugListener();
}

void Processor::Continue() {
  auto global_lock = global_critical_region_.Acquire();
  if (execution_state_ == ExecutionState::kRunning) {
    return;
  } else if (execution_state_ == ExecutionState::kStepping) {
    assert_always("cancel stepping not done yet");
  }
  execution_state_ = ExecutionState::kRunning;
  ResumeAllBreakpoints();
  ResumeAllThreads();

  if (debug_listener_) {
    debug_listener_->OnExecutionContinued();
  }
}

void Processor::StepHostInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint64_t new_host_pc = backend_->CalculateNextHostInstruction(
      thread_info, thread_info->frames[0].host_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kHost, new_host_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

void Processor::StepGuestInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);

  uint32_t next_pc = CalculateNextGuestInstruction(
      thread_info, thread_info->frames[0].guest_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kGuest, next_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));
  AddBreakpoint(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

GuestFunction* Processor::FindImportStub(const Export* export_entry) {
  // Not cached: a save-state restore reloads the executable module, and a
  // cached GuestFunction* from before the reload pointed at freed memory
  // (its address read back as 0, and every thread in a blocking export was
  // then dropped from the next save).
  auto global_lock = global_critical_region_.Acquire();
  GuestFunction* found = nullptr;
  for (auto& module : modules_) {
    module->ForEachFunction([&](Function* function) {
      if (!found && function->is_guest()) {
        auto guest_function = reinterpret_cast<GuestFunction*>(function);
        if (guest_function->export_data() == export_entry) {
          found = guest_function;
        }
      }
    });
    if (found) {
      break;
    }
  }
  return found;
}

// How long a single step may take before the thread is declared stuck.
static constexpr int kStepTimeoutMs = 5000;

bool Processor::StepToGuestAddress(uint32_t thread_id, uint32_t pc,
                                   const std::function<bool()>& give_up) {
  XELOGI("StepToGuestAddress: thread {} -> {:08X}", thread_id, pc);
  auto functions = FindFunctionsWithAddress(pc);
  if (functions.empty()) {
    // Function hasn't been generated yet. Generate it.
    if (!ResolveFunction(pc)) {
      XELOGE(
          "Processor::StepToAddress({:08X}) - Function could not be resolved",
          pc);
      return false;
    }
  }

  // Instruct the thread to step forwards. The breakpoint has to be
  // registered with the processor (not just patched in by the backend), or
  // OnThreadBreakpointHit finds nothing to match the hit against and the
  // fence never fires.
  threading::Fence fence;
  cpu::Breakpoint bp(
      this, Breakpoint::AddressType::kGuest, pc,
      [&fence](Breakpoint* breakpoint, ThreadDebugInfo* thread_info,
               uint64_t host_address) { fence.Signal(); });
  {
    auto global_lock = global_critical_region_.Acquire();
    execution_state_ = ExecutionState::kRunning;
  }
  AddBreakpoint(&bp);

  // HACK
  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint32_t suspend_count = 1;
  while (suspend_count) {
    thread_info->thread->thread()->Resume(&suspend_count);
  }

  bool hit = false;
  bool gave_up = false;
  for (int waited_ms = 0; waited_ms < kStepTimeoutMs && !hit;
       waited_ms += 50) {
    hit = fence.WaitFor(std::chrono::milliseconds(50));
    if (!hit && give_up && give_up()) {
      gave_up = true;
      break;
    }
  }
  {
    // The hit handler uninstalled every breakpoint and left the processor
    // "paused" for a debugger that is not there; put both back.
    auto global_lock = global_critical_region_.Acquire();
    auto it = std::ranges::find(breakpoints_, &bp);
    if (it != breakpoints_.end()) {
      breakpoints_.erase(it);
    }
    if (bp.is_installed()) {
      bp.Uninstall();
    }
    execution_state_ = ExecutionState::kRunning;
    // The hit handler marked this thread as suspended by the processor. It
    // is parked by suspend count only; leaving the flag set would make the
    // next breakpoint's ResumeAllThreads wake it in the middle of a save.
    thread_info->suspended = false;
  }
  if (!hit) {
    // The thread never reached the breakpoint (blocked, or on a different
    // path). Park it again so the caller can give up on it cleanly.
    if (gave_up) {
      XELOGI("StepToGuestAddress: thread {} will not reach {:08X} (caller's "
             "condition); giving up the step",
             thread_id, pc);
    } else {
      XELOGE("StepToGuestAddress: thread {} did not reach {:08X} in {} ms",
             thread_id, pc, kStepTimeoutMs);
    }
    thread_info->thread->thread()->Suspend(nullptr);
    return false;
  }

  return true;
}

uint32_t Processor::StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = pc;
  d.code = xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);

  // Must be on a branch.
  assert_true(xe::cpu::ppc::GetOpcodeInfo(opcode).group ==
              xe::cpu::ppc::PPCOpcodeGroup::kB);

  auto thread_info = QueryThreadDebugInfo(thread_id);
  auto thread = thread_info->thread;
  auto context = thread->thread_state()->context();

  if (d.code == 0x4E800020) {
    // blr
    uint32_t nia = uint32_t(context->lr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (d.code == 0x4E800420) {
    // bctr
    uint32_t nia = uint32_t(context->ctr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bx) {
    // bx
    uint32_t nia = d.I.ADDR();
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bcx || opcode == PPCOpcode::bcctrx ||
             opcode == PPCOpcode::bclrx) {
    threading::Fence fence;
    auto callback = [this, &fence, &pc](Breakpoint* breakpoint,
                                        ThreadDebugInfo* thread_info,
                                        uint64_t host_address) {
      // Report where the thread actually stopped: the trap's host address
      // mapped back to guest, which can differ from the breakpoint's guest
      // address when the instruction before it emitted no code.
      pc = breakpoint->guest_address();
      auto function = backend_->code_cache()->LookupFunction(host_address);
      if (function) {
        uint32_t mapped = function->MapMachineCodeToGuestAddress(host_address);
        if (mapped) {
          pc = mapped;
        }
      }
      fence.Signal();
    };

    {
      auto global_lock = global_critical_region_.Acquire();
      execution_state_ = ExecutionState::kRunning;
    }
    cpu::Breakpoint bpf(this, Breakpoint::AddressType::kGuest, pc + 4,
                        callback);
    AddBreakpoint(&bpf);

    uint32_t nia = 0;
    if (opcode == PPCOpcode::bcx) {
      // bcx
      nia = d.B.ADDR();
    } else if (opcode == PPCOpcode::bcctrx) {
      // bcctrx
      nia = uint32_t(context->ctr);
    } else if (opcode == PPCOpcode::bclrx) {
      // bclrx
      nia = uint32_t(context->lr);
    }

    cpu::Breakpoint bpt(this, Breakpoint::AddressType::kGuest, nia, callback);
    AddBreakpoint(&bpt);

    // HACK
    uint32_t suspend_count = 1;
    while (suspend_count) {
      thread->thread()->Resume(&suspend_count);
    }

    bool hit = fence.WaitFor(std::chrono::milliseconds(kStepTimeoutMs));
    {
      // As in StepToGuestAddress: the hit handler uninstalled everything and
      // left the processor "paused"; unregister both and put it back.
      auto global_lock = global_critical_region_.Acquire();
      thread_info->suspended = false;
      for (cpu::Breakpoint* b : {&bpt, &bpf}) {
        auto it = std::ranges::find(breakpoints_, b);
        if (it != breakpoints_.end()) {
          breakpoints_.erase(it);
        }
        if (b->is_installed()) {
          b->Uninstall();
        }
      }
      execution_state_ = ExecutionState::kRunning;
    }
    if (!hit) {
      XELOGE("StepIntoGuestBranchTarget: thread {} did not reach either "
             "target of {:08X} in {} ms",
             thread_id, pc, kStepTimeoutMs);
      thread->thread()->Suspend(nullptr);
      return 0;
    }
  }

  return pc;
}

std::string Processor::DescribeGlobalLockOwner() {
#if XE_PLATFORM_LINUX == 1 && XE_ENABLE_FAST_LINUX_MUTEX == 1
  const pid_t tid = global_critical_region::mutex().owner();
  if (!tid) {
    return " (free by the time it was checked)";
  }
  auto read = [tid](const char* what) {
    std::ifstream f(fmt::format("/proc/self/task/{}/{}", tid, what));
    std::string line;
    std::getline(f, line);
    return line;
  };
  std::string stat = read("stat");
  size_t close = stat.rfind(')');
  std::string state = close != std::string::npos && close + 2 < stat.size()
                          ? stat.substr(close + 2, 1)
                          : "?";
  return fmt::format(" (owner tid {} '{}', state {}, wchan {})", tid,
                     read("comm"), state, read("wchan"));
#else
  return std::string();
#endif
}

uint32_t Processor::StepToGuestSafePoint(uint32_t thread_id, bool ignore_host,
                                         uint32_t guest_suspend_count,
                                         bool* out_parked_in_self_suspend,
    const std::function<bool()>& give_up,
    std::pair<uint32_t, int32_t>* out_memory_fixup) {
  // This cannot be done if we're the calling thread!
  if (thread_id == ThreadState::GetThreadID()) {
    assert_always(
        "Processor::StepToSafePoint(): target thread is the calling thread!");
    return 0;
  }
  if (!ignore_host && !in_step_to_safe_point_) {
    nudge_attempts_remaining_ = 10;
  }
  struct DepthGuard {
    bool& flag;
    bool outer;
    DepthGuard(bool& f) : flag(f), outer(!f) { flag = true; }
    ~DepthGuard() {
      if (outer) {
        flag = false;
      }
    }
  } depth_guard(in_step_to_safe_point_);
  // The global critical region is recursive and defers Thread::Suspend while
  // it is held, so a paused guest thread never owns it; a thread whose
  // suspension was deferred and that then blocked inside the region does.
  // Bounded, and the owner named, so a save fails instead of freezing the
  // game with the saver stuck here before its first log line.
  ThreadDebugInfo* thread_info = nullptr;
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto global_lock = global_critical_region::TryAcquire();
    while (!global_lock.owns_lock()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        XELOGE(
            "StepToGuestSafePoint: thread {} - the global critical region "
            "was not free after 5 s{}",
            thread_id, DescribeGlobalLockOwner());
        return 0;
      }
      xe::threading::Sleep(std::chrono::milliseconds(1));
      global_lock = global_critical_region::TryAcquire();
    }
    auto it = thread_debug_infos_.find(thread_id);
    if (it == thread_debug_infos_.end()) {
      XELOGE("StepToGuestSafePoint: thread {} - no debug info", thread_id);
      return 0;
    }
    thread_info = it->second.get();
  }
  auto thread = thread_info->thread;

  // Now the fun part begins: Registers are only guaranteed to be synchronized
  // with the PPC context at a basic block boundary. Unfortunately, we most
  // likely stopped the thread at some point other than a boundary. We need to
  // step forward until we reach a boundary, and then perform the save.
  uint64_t frame_host_pcs[64];
  cpu::StackFrame cpu_frames[64];
  size_t count = stack_walker_->CaptureStackTrace(
      thread->thread()->native_handle(), frame_host_pcs, 0,
      xe::countof(frame_host_pcs), nullptr, nullptr);
  stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
  if (count == 0) {
    XELOGE("StepToGuestSafePoint: thread {} - stack capture returned 0 frames",
           thread_id);
    return 0;
  }
  {
    std::string desc;
    for (size_t i = 0; i < std::min<size_t>(count, 12); ++i) {
      bool guest = cpu_frames[i].type == cpu::StackFrame::Type::kGuest;
      desc += fmt::format(" [{}:{}{:08X}{}{}]", i, guest ? "g" : "h",
                          guest ? uint64_t(cpu_frames[i].guest_pc)
                                : uint64_t(cpu_frames[i].host_pc),
                          cpu_frames[i].host_symbol.name[0] ? " " : "",
                          cpu_frames[i].host_symbol.name);
    }
    XELOGI("StepToGuestSafePoint: thread {} {} frames:{}", thread_id, count,
           desc);
  }

  auto& first_frame = cpu_frames[0];
  if (ignore_host) {
    for (size_t i = 0; i < count; i++) {
      if (cpu_frames[i].type == cpu::StackFrame::Type::kGuest &&
          cpu_frames[i].guest_pc) {
        first_frame = cpu_frames[i];
        break;
      }
    }
  }

  // Check if we're in guest code or host code.
  uint32_t pc = 0;
  if (first_frame.type == cpu::StackFrame::Type::kGuest) {
    auto& frame = first_frame;
    if (!frame.guest_pc) {
      // Lame. The guest->host thunk is a "guest" function.
      frame = cpu_frames[1];
    }

    pc = frame.guest_pc;

    // We're in guest code.
    // First: Find a synchronizing instruction and go to it.
    xe::cpu::ppc::PPCDecodeData d;
    const xe::cpu::ppc::PPCOpcodeInfo* sync_info = nullptr;
    d.address = cpu_frames[0].guest_pc - 4;
    do {
      d.address += 4;
      d.code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(d.code);
      if (opcode_info.type == cpu::ppc::PPCOpcodeType::kSync) {
        sync_info = &opcode_info;
        break;
      }
    } while (true);

    if (d.address != pc) {
      if (!StepToGuestAddress(thread_id, d.address)) {
        return 0;
      }
      pc = d.address;
    }

    // Okay. Now we're on a synchronizing instruction but we need to step
    // past it in order to get a synchronized context.
    // If we're on a branching instruction, it's guaranteed only going to have
    // two possible targets. For non-branching instructions, we can just step
    // over them.
    if (sync_info->group == xe::cpu::ppc::PPCOpcodeGroup::kB) {
      pc = StepIntoGuestBranchTarget(thread_id, d.address);
    }
  } else {
    // We're in host code. Search backwards til we can get an idea of where
    // we are.
    cpu::GuestFunction* thunk_func = nullptr;
    cpu::Export* export_data = nullptr;
    uint32_t first_pc = 0;
    for (int i = 0; i < count; i++) {
      auto& frame = cpu_frames[i];
      if (frame.type == cpu::StackFrame::Type::kGuest && frame.guest_pc) {
        auto func = frame.guest_symbol.function;
        assert_true(func->is_guest());

        if (!first_pc) {
          first_pc = frame.guest_pc;
        }

        thunk_func = reinterpret_cast<cpu::GuestFunction*>(func);
        export_data = thunk_func->export_data();
        if (export_data) {
          break;
        }
      }
    }

    // Unwinding stops at the guest->host thunk where JIT code has no unwind
    // info (POSIX), so the import stub is never seen. The shim trampoline
    // records the export being executed and the host return address into
    // the stub; use that instead.
    if (!export_data) {
      auto ts = thread->thread_state();
      if (ts && ts->current_export()) {
        auto stub = FindImportStub(ts->current_export());
        if (stub) {
          thunk_func = stub;
          export_data = const_cast<cpu::Export*>(ts->current_export());
          XELOGI("StepToGuestSafePoint: thread {} inside export {} via stub "
                 "{:08X} ({})",
                 thread_id, export_data->name, stub->address(),
                 (export_data->tags & cpu::ExportTag::kBlocking) ? "blocking"
                                                                 : "non-blocking");
        }
      }
    }

    // A thread suspended inside the MMIO handler shows only host and signal
    // frames; the handler recorded the JIT pc of the instruction it is
    // emulating. Map that back to guest and step past it.
    if (!export_data && !first_pc) {
      auto ts = thread->thread_state();
      uint64_t rip = ts ? ts->current_exception_pc() : 0;
      if (rip) {
        auto fn = backend_->code_cache()->LookupFunction(rip);
        if (fn) {
          first_pc = fn->MapMachineCodeToGuestAddress(rip);
          XELOGI("StepToGuestSafePoint: thread {} in MMIO handler at host "
                 "{:X} -> guest {:08X}",
                 thread_id, rip, first_pc);
        }
      }
    }

    // A blocking export is re-executed on restore, which is only right if
    // the thread is still inside the wait. If the wait already completed
    // and the thread was stopped on its way back to guest code, the wakeup
    // has been consumed and re-waiting would hang, so treat it as
    // non-blocking and run it back to guest code instead. Frames above the
    // suspend signal frame belong to the suspension itself and are skipped.
    // If the export is blocking, we wrap up and save inside the export thunk.
    // When we're restored, we'll call the blocking export again. Running the
    // thread back to guest code instead is not possible here: its host call
    // stack is still deep inside the wait's C++ frames, which cannot be
    // unwound, so the guest would eventually return through a corrupt frame.
    // Re-issuing the wait on restore is safe because the event/semaphore
    // states are part of the saved memory image.
    if (export_data && !(export_data->tags & cpu::ExportTag::kBlocking) &&
        export_data->name && !std::strcmp(export_data->name, "NtSuspendThread") &&
        guest_suspend_count > 0) {
      // Parked in its own NtSuspendThread until another thread resumes it:
      // it never returns on its own, so save it like a blocking wait (the
      // call is re-issued on restore; XThread knows not to count it twice).
      XELOGI(
          "StepToGuestSafePoint: thread {} parked in NtSuspendThread (guest "
          "suspend count {}); saved at the call, re-issued on restore",
          thread_id, guest_suspend_count);
      if (out_parked_in_self_suspend) {
        *out_parked_in_self_suspend = true;
      }
      pc = thunk_func->address();
    } else if (export_data &&
               (export_data->tags & cpu::ExportTag::kBlocking)) {
      pc = thunk_func->address();
    } else if (export_data && export_data->name &&
               !std::strcmp(export_data->name, "RtlEnterCriticalSection")) {
      // Not tagged blocking, but it waits on the section's event in host
      // code once the spin fails, and the owner is another guest thread,
      // suspended like the rest: a waiter never reaches its return address.
      // Give it a moment (an owner or a re-entering thread returns at once),
      // then save it at the call like a blocking wait. The call is re-issued
      // on restore and increments lock_count again, so the saved image gets
      // this thread's increment taken out (r3 is still the section).
      pc = static_cast<uint32_t>(thread->thread_state()->context()->lr);
      auto started = std::chrono::steady_clock::now();
      if (!StepToGuestAddress(thread_id, pc, [&give_up, started]() {
            return (give_up && give_up()) ||
                   std::chrono::steady_clock::now() - started >
                       std::chrono::milliseconds(300);
          })) {
        uint32_t cs = static_cast<uint32_t>(
            thread->thread_state()->context()->r[3]);
        XELOGI(
            "StepToGuestSafePoint: thread {} waiting in RtlEnterCriticalSection "
            "on {:08X}; saved at the call, re-issued on restore, lock count "
            "adjusted in the image",
            thread_id, cs);
        if (out_memory_fixup) {
          *out_memory_fixup = {cs + 0x10, -1};  // lock_count, host order
        }
        pc = thunk_func->address();
      }
    } else if (export_data) {
      // Non-blocking. Run until we return from the thunk.
      pc = static_cast<uint32_t>(thread->thread_state()->context()->lr);
      if (!StepToGuestAddress(thread_id, pc, give_up)) {
        return 0;
      }
    } else if (first_pc) {
      // We're in the MMIO handler/mfmsr/something calling out of the guest
      // that doesn't use an export. If the current instruction is
      // synchronizing, we can just save here. Otherwise, step forward
      // (and call ourselves again so we run the correct logic).
      uint32_t code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(first_pc));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(code);
      XELOGI("StepToGuestSafePoint: thread {} host frame over guest {:08X} "
             "({}), {}",
             thread_id, first_pc, static_cast<int>(opcode_info.type),
             opcode_info.type == xe::cpu::ppc::PPCOpcodeType::kSync
                 ? "sync - saving here"
                 : "stepping to next instruction");
      if (opcode_info.type == xe::cpu::ppc::PPCOpcodeType::kSync) {
        // Good to go.
        pc = first_pc;
      } else {
        // Step forward and run this logic again.
        if (!StepToGuestAddress(thread_id, first_pc + 4)) {
          return 0;
        }
        return StepToGuestSafePoint(thread_id, true, guest_suspend_count,
                                    out_parked_in_self_suspend, give_up,
                                    out_memory_fixup);
      }
    } else if (!ignore_host && nudge_attempts_remaining_ > 0) {
      // No guest frame, no export, no fault in progress: the thread is
      // sitting in a host<->guest thunk or another helper the unwinder
      // cannot see through. Let it run for a moment and look again; the
      // window is a handful of instructions wide.
      --nudge_attempts_remaining_;
      // Resume until it actually runs (the count can be above one), then
      // put exactly one suspension back; the caller's Resume() removes it.
      uint32_t before_resume = 0, before_suspend = 0, count = 1;
      int resumes = 0;
      while (count > 0 && resumes < 8) {
        count = 0;
        thread->thread()->Resume(&count);
        if (resumes == 0) {
          before_resume = count;
        }
        ++resumes;
        if (count <= 1) {
          break;
        }
      }
      // Back off: JIT compilation or a long host helper can take a few ms.
      int sleep_us = 200 << (9 - nudge_attempts_remaining_);
      xe::threading::Sleep(std::chrono::microseconds(sleep_us));
      thread->thread()->Suspend(&before_suspend);
      XELOGI("StepToGuestSafePoint: thread {} - no guest frame (exception pc "
             "{:X}), nudged {} us ({} left; counts {}->{})",
             thread_id,
             thread->thread_state() ? thread->thread_state()->current_exception_pc()
                                    : 0,
             sleep_us, nudge_attempts_remaining_, before_resume, before_suspend);
      return StepToGuestSafePoint(thread_id, false, guest_suspend_count,
                                  out_parked_in_self_suspend);
    } else {
      // We've managed to catch a thread before it called into the guest.
      // Set a breakpoint on its startup procedure and capture it there.
      // TODO(DrChat): Reimplement
      XELOGE("StepToGuestSafePoint: thread {} - no guest frame found",
             thread_id);
      assert_always("Unimplemented");
      /*
      auto creation_params = thread->creation_params();
      pc = creation_params->xapi_thread_startup
               ? creation_params->xapi_thread_startup
               : creation_params->start_address;
      StepToGuestAddress(thread_id, pc);
      */
    }
  }

  return pc;
}

bool TestPpcCondition(const xe::cpu::ppc::PPCContext* context, uint32_t bo,
                      uint32_t bi, bool check_ctr, bool check_cond) {
  bool ctr_ok = true;
  if (check_ctr) {
    if (select_bits(bo, 2, 2)) {
      ctr_ok = true;
    } else {
      uint32_t new_ctr_value = static_cast<uint32_t>(context->ctr - 1);
      if (select_bits(bo, 1, 1)) {
        ctr_ok = new_ctr_value == 0;
      } else {
        ctr_ok = new_ctr_value != 0;
      }
    }
  }
  bool cond_ok = true;
  if (check_cond) {
    if (select_bits(bo, 4, 4)) {
      cond_ok = true;
    } else {
      uint8_t cr = *(reinterpret_cast<const uint8_t*>(&context->cr0) +
                     (4 * (bi >> 2)) + (bi & 3));
      if (select_bits(bo, 3, 3)) {
        cond_ok = cr != 0;
      } else {
        cond_ok = cr == 0;
      }
    }
  }
  return ctr_ok && cond_ok;
}

uint32_t Processor::CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                                  uint32_t current_pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = current_pc;
  d.code = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);
  if (d.code == 0x4E800020) {
    // blr -- unconditional branch to LR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    return target_pc;
  } else if (d.code == 0x4E800420) {
    // bctr -- unconditional branch to CTR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    return target_pc;
  } else if (opcode == PPCOpcode::bx) {
    // b/ba/bl/bla
    uint32_t target_pc = d.I.ADDR();
    return target_pc;
  } else if (opcode == PPCOpcode::bcx) {
    // bc/bca/bcl/bcla
    uint32_t target_pc = d.B.ADDR();
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.B.BO(),
                                        d.B.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bclrx) {
    // bclr/bclrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bcctrx) {
    // bcctr/bcctrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), false, true);
    return test_passed ? target_pc : current_pc + 4;
  } else {
    return current_pc + 4;
  }
}
uint32_t Processor::GuestAtomicIncrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t* host_address = context->TranslateVirtual<uint32_t*>(guest_address);

  uint32_t result;
  while (true) {
    result = *host_address;
    // todo: should call a processor->backend function that acquires a
    // reservation instead of using host atomics
    if (xe::atomic_cas(result, xe::byte_swap(xe::byte_swap(result) + 1),
                       host_address)) {
      break;
    }
  }
  return xe::byte_swap(result);
}
uint32_t Processor::GuestAtomicDecrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t* host_address = context->TranslateVirtual<uint32_t*>(guest_address);

  uint32_t result;
  while (true) {
    result = *host_address;
    // todo: should call a processor->backend function that acquires a
    // reservation instead of using host atomics
    if (xe::atomic_cas(result, xe::byte_swap(xe::byte_swap(result) - 1),
                       host_address)) {
      break;
    }
  }
  return xe::byte_swap(result);
}

uint32_t Processor::GuestAtomicOr32(ppc::PPCContext* context,
                                    uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(
      xe::atomic_or(context->TranslateVirtual<volatile int32_t*>(guest_address),
                    xe::byte_swap(mask)));
}
uint32_t Processor::GuestAtomicXor32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(xe::atomic_xor(
      context->TranslateVirtual<volatile int32_t*>(guest_address),
      xe::byte_swap(mask)));
}
uint32_t Processor::GuestAtomicAnd32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  return xe::byte_swap(xe::atomic_and(
      context->TranslateVirtual<volatile int32_t*>(guest_address),
      xe::byte_swap(mask)));
}

bool Processor::GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                                 uint32_t new_value, uint32_t guest_address) {
  return xe::atomic_cas(xe::byte_swap(old_value), xe::byte_swap(new_value),
                        context->TranslateVirtual<uint32_t*>(guest_address));
}
}  // namespace cpu
}  // namespace xe
