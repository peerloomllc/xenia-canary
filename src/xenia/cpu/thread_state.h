/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_THREAD_STATE_H_
#define XENIA_CPU_THREAD_STATE_H_

#include <string>

#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

class Export;

class Processor;

class ThreadState {
 public:
  ThreadState(Processor* processor, uint32_t thread_id, uint32_t stack_base = 0,
              uint32_t pcr_address = 0);
  ~ThreadState();

  Processor* processor() const { return processor_; }
  Memory* memory() const { return memory_; }
  void* backend_data() const { return backend_data_; }
  ppc::PPCContext* context() const { return context_; }
  uint32_t thread_id() const { return thread_id_; }

  static void Bind(ThreadState* thread_state);
  static ThreadState* Get();
  static uint32_t GetThreadID();

  // The kernel export this thread is currently executing (set by the shim
  // trampoline for the duration of the call), and the host return address
  // into the JIT import stub that called it. Save states use this to find
  // the guest thunk of a blocking wait without unwinding JIT frames, which
  // has no unwind info on POSIX.
  const Export* current_export() const { return current_export_; }
  void* current_export_return_address() const {
    return current_export_return_address_;
  }
  void set_current_export(const Export* export_entry, void* return_address) {
    current_export_ = export_entry;
    current_export_return_address_ = return_address;
  }
  // Host PC of the guest instruction whose access is being emulated by the
  // MMIO handler, while that handler runs on this thread.
  uint64_t current_exception_pc() const { return current_exception_pc_; }
  // True while the thread is blocked inside a threading wait or sleep (see
  // threading::SetThreadBlockingWaitSlot).
  bool in_blocking_wait() const { return in_blocking_wait_; }
  struct ExceptionScope {
    ExceptionScope(ThreadState* ts, uint64_t pc)
        : ts_(ts), previous_(ts ? ts->current_exception_pc_ : 0) {
      if (ts_) {
        ts_->current_exception_pc_ = pc;
      }
    }
    ~ExceptionScope() {
      if (ts_) {
        ts_->current_exception_pc_ = previous_;
      }
    }
    ThreadState* ts_;
    uint64_t previous_;
  };
  struct ExportScope {
    ExportScope(ThreadState* ts, const Export* export_entry,
                void* return_address)
        : ts_(ts),
          previous_(ts ? ts->current_export_ : nullptr),
          previous_return_(ts ? ts->current_export_return_address_ : nullptr) {
      if (ts_) {
        ts_->set_current_export(export_entry, return_address);
      }
    }
    ~ExportScope() {
      if (ts_) {
        ts_->set_current_export(previous_, previous_return_);
      }
    }
    ThreadState* ts_;
    const Export* previous_;
    void* previous_return_;
  };

 private:
  Processor* processor_;
  Memory* memory_;
  void* backend_data_;

  uint32_t pcr_address_ = 0;
  uint32_t thread_id_ = 0;

  const Export* current_export_ = nullptr;
  void* current_export_return_address_ = nullptr;
  uint64_t current_exception_pc_ = 0;
  bool in_blocking_wait_ = false;

  // NOTE: must be 64b aligned for SSE ops.
  ppc::PPCContext* context_;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_THREAD_STATE_H_
