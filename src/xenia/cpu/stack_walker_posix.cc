/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/stack_walker.h"

#include <dlfcn.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include "xenia/base/host_thread_context.h"
#include "xenia/base/logging.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/function.h"

namespace xe {
namespace cpu {

namespace {

// Xenia's POSIX threading claims SIGRTMIN + 0..2 (see threading_posix.cc).
// Take one from the top of the realtime range so the two never collide.
int GetCaptureSignal() { return SIGRTMAX - 2; }

// A capture in flight. Only one may be active at a time, serialized by
// PosixStackWalker::capture_mutex_. The target thread's signal handler fills
// this in and posts `done`.
struct CaptureRequest {
  uint64_t* frame_host_pcs;
  size_t frame_offset;
  size_t frame_count;
  HostThreadContext* out_host_context;
  size_t result_count;
  sem_t done;
};

std::atomic<CaptureRequest*> active_request{nullptr};

#if XE_ARCH_AMD64
void HostContextFromUContext(const ucontext_t* uc, HostThreadContext* out) {
  const auto& gr = uc->uc_mcontext.gregs;
  out->rip = uint64_t(gr[REG_RIP]);
  out->eflags = uint32_t(gr[REG_EFL]);
  out->rax = uint64_t(gr[REG_RAX]);
  out->rcx = uint64_t(gr[REG_RCX]);
  out->rdx = uint64_t(gr[REG_RDX]);
  out->rbx = uint64_t(gr[REG_RBX]);
  out->rsp = uint64_t(gr[REG_RSP]);
  out->rbp = uint64_t(gr[REG_RBP]);
  out->rsi = uint64_t(gr[REG_RSI]);
  out->rdi = uint64_t(gr[REG_RDI]);
  out->r8 = uint64_t(gr[REG_R8]);
  out->r9 = uint64_t(gr[REG_R9]);
  out->r10 = uint64_t(gr[REG_R10]);
  out->r11 = uint64_t(gr[REG_R11]);
  out->r12 = uint64_t(gr[REG_R12]);
  out->r13 = uint64_t(gr[REG_R13]);
  out->r14 = uint64_t(gr[REG_R14]);
  out->r15 = uint64_t(gr[REG_R15]);
}

void UContextFromHostContext(const HostThreadContext* in, ucontext_t* uc) {
  std::memset(uc, 0, sizeof(*uc));
  auto& gr = uc->uc_mcontext.gregs;
  gr[REG_RIP] = greg_t(in->rip);
  gr[REG_EFL] = greg_t(in->eflags);
  gr[REG_RAX] = greg_t(in->rax);
  gr[REG_RCX] = greg_t(in->rcx);
  gr[REG_RDX] = greg_t(in->rdx);
  gr[REG_RBX] = greg_t(in->rbx);
  gr[REG_RSP] = greg_t(in->rsp);
  gr[REG_RBP] = greg_t(in->rbp);
  gr[REG_RSI] = greg_t(in->rsi);
  gr[REG_RDI] = greg_t(in->rdi);
  gr[REG_R8] = greg_t(in->r8);
  gr[REG_R9] = greg_t(in->r9);
  gr[REG_R10] = greg_t(in->r10);
  gr[REG_R11] = greg_t(in->r11);
  gr[REG_R12] = greg_t(in->r12);
  gr[REG_R13] = greg_t(in->r13);
  gr[REG_R14] = greg_t(in->r14);
  gr[REG_R15] = greg_t(in->r15);
}
#endif  // XE_ARCH_AMD64

// Walks `cursor`, writing program counters into the caller's buffer.
// Returns the number of frames written.
size_t UnwindCursor(unw_cursor_t* cursor, uint64_t* frame_host_pcs,
                    size_t frame_offset, size_t frame_count) {
  size_t skipped = 0;
  size_t written = 0;
  while (written < frame_count) {
    unw_word_t pc = 0;
    if (unw_get_reg(cursor, UNW_REG_IP, &pc) != 0 || !pc) {
      break;
    }
    if (skipped < frame_offset) {
      ++skipped;
    } else {
      frame_host_pcs[written++] = uint64_t(pc);
    }
    if (unw_step(cursor) <= 0) {
      break;
    }
  }
  return written;
}

uint64_t HashStack(const uint64_t* frame_host_pcs, size_t count) {
  // FNV-1a. Only used for deduping identical stacks.
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < count; ++i) {
    hash ^= frame_host_pcs[i];
    hash *= 0x100000001B3ull;
  }
  return hash;
}

// Runs on the *target* thread. Unwinds from the interrupted context so the
// handler's own frames do not show up in the trace.
//
// NOTE: libunwind may allocate on first use per thread, which is not strictly
// async-signal-safe. This is the same tradeoff sampling profilers make, and the
// alternative (ptrace from a helper process) is far worse. This path is
// debug/profiling only and never runs during normal emulation.
void CaptureSignalHandler(int /*signal*/, siginfo_t* /*info*/, void* ucontext) {
  CaptureRequest* request = active_request.load(std::memory_order_acquire);
  if (!request) {
    return;
  }

  auto* uc = reinterpret_cast<ucontext_t*>(ucontext);
  unw_cursor_t cursor;
  size_t count = 0;
  if (unw_init_local2(&cursor, reinterpret_cast<unw_context_t*>(uc),
                      UNW_INIT_SIGNAL_FRAME) == 0) {
    count = UnwindCursor(&cursor, request->frame_host_pcs,
                         request->frame_offset, request->frame_count);
  }

#if XE_ARCH_AMD64
  if (request->out_host_context) {
    HostContextFromUContext(uc, request->out_host_context);
  }
#endif

  request->result_count = count;
  sem_post(&request->done);
}

}  // namespace

class PosixStackWalker : public StackWalker {
 public:
  explicit PosixStackWalker(backend::CodeCache* code_cache)
      : code_cache_(code_cache) {
    if (code_cache_) {
      code_cache_min_ = code_cache_->execute_base_address();
      code_cache_max_ = code_cache_min_ + code_cache_->total_size();
    }
  }

  bool Initialize() {
    struct sigaction action = {};
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    action.sa_sigaction = CaptureSignalHandler;
    sigfillset(&action.sa_mask);
    if (sigaction(GetCaptureSignal(), &action, nullptr) != 0) {
      XELOGE("Unable to install the stack capture signal handler");
      return false;
    }
    // Prime libunwind's caches so the first real capture does less work inside
    // a signal handler.
    unw_context_t context;
    unw_cursor_t cursor;
    if (unw_getcontext(&context) == 0 &&
        unw_init_local(&cursor, &context) == 0) {
      unw_step(&cursor);
    }
    return true;
  }

  size_t CaptureStackTrace(uint64_t* frame_host_pcs, size_t frame_offset,
                           size_t frame_count,
                           uint64_t* out_stack_hash = nullptr) override {
    unw_context_t context;
    unw_cursor_t cursor;
    if (unw_getcontext(&context) != 0) {
      return 0;
    }
    if (unw_init_local(&cursor, &context) != 0) {
      return 0;
    }
    // Skip our own frame on top of whatever the caller asked to skip.
    size_t count =
        UnwindCursor(&cursor, frame_host_pcs, frame_offset + 1, frame_count);
    if (out_stack_hash) {
      *out_stack_hash = HashStack(frame_host_pcs, count);
    }
    return count;
  }

  size_t CaptureStackTrace(void* thread_handle, uint64_t* frame_host_pcs,
                           size_t frame_offset, size_t frame_count,
                           const HostThreadContext* in_host_context,
                           HostThreadContext* out_host_context,
                           uint64_t* out_stack_hash = nullptr) override {
    size_t count = 0;

#if XE_ARCH_AMD64
    if (in_host_context) {
      // The caller already holds the register state (an exception handler, for
      // instance) - unwind straight from it, no signal round-trip needed.
      ucontext_t uc;
      UContextFromHostContext(in_host_context, &uc);
      unw_cursor_t cursor;
      if (unw_init_local2(&cursor, reinterpret_cast<unw_context_t*>(&uc),
                          UNW_INIT_SIGNAL_FRAME) == 0) {
        count =
            UnwindCursor(&cursor, frame_host_pcs, frame_offset, frame_count);
      }
      if (out_host_context) {
        *out_host_context = *in_host_context;
      }
      if (out_stack_hash) {
        *out_stack_hash = HashStack(frame_host_pcs, count);
      }
      return count;
    }
#endif  // XE_ARCH_AMD64

    auto thread = reinterpret_cast<pthread_t>(thread_handle);
    if (pthread_equal(thread, pthread_self())) {
      // Our own stack - the signal round-trip would deadlock.
      return CaptureStackTrace(frame_host_pcs, frame_offset, frame_count,
                               out_stack_hash);
    }

    // Bounded: a capture whose thread was stopped while it held this lock
    // (a pause suspends guest threads at any host instruction) would hold
    // it for good, and a save would freeze the game at its first step.
    std::unique_lock<std::timed_mutex> lock(capture_mutex_, std::defer_lock);
    if (!lock.try_lock_for(std::chrono::seconds(2))) {
      XELOGW(
          "Stack capture for thread {} skipped: another capture has held the "
          "lock for 2 s",
          thread_handle);
      return 0;
    }

    CaptureRequest request = {};
    request.frame_host_pcs = frame_host_pcs;
    request.frame_offset = frame_offset;
    request.frame_count = frame_count;
    request.out_host_context = out_host_context;
    request.result_count = 0;
    if (sem_init(&request.done, 0, 0) != 0) {
      return 0;
    }

    active_request.store(&request, std::memory_order_release);
    if (pthread_kill(thread, GetCaptureSignal()) != 0) {
      active_request.store(nullptr, std::memory_order_release);
      sem_destroy(&request.done);
      return 0;
    }

    // Bounded wait: a thread that cannot run (already stopped, or blocking the
    // signal) must not hang the debugger.
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    if (sem_timedwait(&request.done, &deadline) == 0) {
      count = request.result_count;
    } else {
      XELOGW("Timed out capturing a stack trace for thread {}", thread_handle);
    }

    active_request.store(nullptr, std::memory_order_release);
    sem_destroy(&request.done);

    if (out_stack_hash) {
      *out_stack_hash = HashStack(frame_host_pcs, count);
    }
    return count;
  }

  bool ResolveStack(uint64_t* frame_host_pcs, StackFrame* frames,
                    size_t frame_count) override {
    for (size_t i = 0; i < frame_count; ++i) {
      auto& frame = frames[i];
      std::memset(&frame, 0, sizeof(frame));
      frame.host_pc = frame_host_pcs[i];

      if (code_cache_ && frame.host_pc >= code_cache_min_ &&
          frame.host_pc < code_cache_max_) {
        // In the generated code range, so the code cache owns it.
        frame.type = StackFrame::Type::kGuest;
        auto function = code_cache_->LookupFunction(frame.host_pc);
        if (function) {
          frame.guest_symbol.function = function;
          if (function->is_guest()) {
            auto guest_function = static_cast<GuestFunction*>(function);
            frame.guest_pc =
                guest_function->MapMachineCodeToGuestAddress(frame.host_pc);
          }
        } else {
          frame.guest_symbol.function = nullptr;
        }
      } else {
        // Emulator or system code.
        frame.type = StackFrame::Type::kHost;
        Dl_info info = {};
        if (dladdr(reinterpret_cast<void*>(frame.host_pc), &info) &&
            info.dli_sname) {
          frame.host_symbol.address =
              reinterpret_cast<uint64_t>(info.dli_saddr);
          std::strncpy(frame.host_symbol.name, info.dli_sname,
                       sizeof(frame.host_symbol.name) - 1);
          frame.host_symbol.name[sizeof(frame.host_symbol.name) - 1] = '\0';
        }
      }
    }
    return true;
  }

 private:
  backend::CodeCache* code_cache_ = nullptr;
  uintptr_t code_cache_min_ = 0;
  uintptr_t code_cache_max_ = 0;
  std::timed_mutex capture_mutex_;
};

std::unique_ptr<StackWalker> StackWalker::Create(
    backend::CodeCache* code_cache) {
#if !XE_ARCH_AMD64
  XELOGD("Stack walker unimplemented on this posix architecture");
  return nullptr;
#else
  auto walker = std::make_unique<PosixStackWalker>(code_cache);
  if (!walker->Initialize()) {
    XELOGE("Unable to initialize the posix stack walker");
    return nullptr;
  }
  return walker;
#endif
}

}  // namespace cpu
}  // namespace xe
