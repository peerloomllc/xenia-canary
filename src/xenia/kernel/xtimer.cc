/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xtimer.h"

#include "xenia/base/clock.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xthread.h"

namespace xe {
namespace kernel {

XTimer::XTimer(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {}

XTimer::~XTimer() = default;

XTimer::XTimer() : XObject(kObjectType) {}

void XTimer::Initialize(uint32_t timer_type) {
  assert_false(timer_);
  timer_type_ = timer_type;
  switch (timer_type) {
    case 0:  // NotificationTimer
      timer_ = xe::threading::Timer::CreateManualResetTimer();
      break;
    case 1:  // SynchronizationTimer
      timer_ = xe::threading::Timer::CreateSynchronizationTimer();
      break;
    default:
      assert_always();
      break;
  }
  assert_not_null(timer_);
}

X_STATUS XTimer::SetTimer(int64_t due_time, uint32_t period_ms,
                          uint32_t routine, uint32_t routine_arg, bool resume) {
  X_STATUS result = Arm(due_time, period_ms, routine, routine_arg,
                        XThread::GetCurrentThread());
  if (resume) {
    XThread::SetLastError(X_ERROR_NOT_SUPPORTED);
    return X_STATUS_TIMER_RESUME_IGNORED;
  }
  return result;
}

X_STATUS XTimer::Arm(int64_t due_time, uint32_t period_ms, uint32_t routine,
                     uint32_t routine_arg, XThread* thread) {
  using xe::chrono::WinSystemClock;
  using xe::chrono::XSystemClock;

  std::lock_guard<std::mutex> lock(timer_lock_);

  period_ms_ = period_ms;
  period_ms = Clock::ScaleGuestDurationMillis(period_ms);
  XSystemClock::time_point due_guest;
  if (due_time < 0) {
    // Any timer implementation uses absolute times eventually, convert as early
    // as possible for increased accuracy
    auto after = xe::chrono::hundrednanoseconds(-due_time);
    due_guest = XSystemClock::now() + after;
  } else {
    due_guest = XSystemClock::from_file_time(due_time);
    // NT fires a timer whose absolute due time is 0 or already past at
    // once. Clamp before any host clock conversion: a FILETIME of 0 is the
    // year 1601, which overflows a nanosecond steady clock and armed the
    // timer about 200 years out (Eternal Sonata's 5 ms audio tick, set
    // with due time 0, never fired on Linux: no sound).
    auto now_guest = XSystemClock::now();
    if (due_guest < now_guest) {
      due_guest = now_guest;
    }
  }
  WinSystemClock::time_point due_tp =
      date::clock_cast<WinSystemClock>(due_guest);
  due_guest_filetime_ = XSystemClock::to_file_time(due_guest);
  armed_ = true;

  // Stash routine for callback.
  callback_thread_ = thread;
  callback_routine_ = routine;
  callback_routine_arg_ = routine_arg;

  // This callback will only be issued when the timer is fired.
  // Capture values by value to avoid racing with a future SetTimer() call.
  std::function<void()> callback = nullptr;
  if (callback_routine_ && callback_thread_) {
    auto cb_thread = callback_thread_;
    auto cb_routine = callback_routine_;
    auto cb_routine_arg = callback_routine_arg_;
    callback = [cb_thread, cb_routine, cb_routine_arg]() {
      // Queue APC to call back routine with (arg, low, high).
      // It'll be executed on the thread that requested the timer.
      uint64_t time = xe::Clock::QueryGuestSystemTime();
      uint32_t time_low = static_cast<uint32_t>(time);
      uint32_t time_high = static_cast<uint32_t>(time >> 32);
      XELOGI(
          "XTimer enqueuing timer callback to {:08X}({:08X}, {:08X}, {:08X})",
          cb_routine, cb_routine_arg, time_low, time_high);
      cb_thread->EnqueueApc(cb_routine, cb_routine_arg, time_low, time_high);
    };
  }

  bool result;
  if (!period_ms) {
    result = timer_->SetOnceAt(due_tp, std::move(callback));
  } else {
    result = timer_->SetRepeatingAt(
        due_tp, std::chrono::milliseconds(period_ms), std::move(callback));
  }

  return result ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

// Layout: type, armed, due (guest file time), period, routine, arg, callback
// thread handle, signaled. A one-shot timer that has already fired is saved
// as signaled and re-signaled on restore; an armed one is re-armed at the
// same guest time, so a due time that passed while the state sat on disk
// fires at once.
bool XTimer::Save(ByteStream* stream) {
  if (!SaveObject(stream)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(timer_lock_);
  // A zero-length wait tells whether it is signaled. That consumes the
  // signal of a synchronization timer, so put it back.
  // A periodic timer is left alone: the zero-length wait would consume its
  // signal and SetOnceAfter would replace the repeating schedule with a
  // single shot, so it fired once more and never again (Eternal Sonata's
  // audio mixer runs on one: every save silenced the game within ten
  // seconds). The restore re-arms periodic timers whatever the flag says.
  bool signaled = false;
  if (timer_ && !period_ms_) {
    signaled = xe::threading::Wait(timer_.get(), false,
                                   std::chrono::milliseconds(0)) ==
               xe::threading::WaitResult::kSuccess;
    if (signaled && timer_type_ == 1) {
      timer_->SetOnceAfter(xe::chrono::hundrednanoseconds(0));
    }
  }
  stream->Write<uint32_t>(timer_type_);
  stream->Write<uint8_t>(armed_ ? 1 : 0);
  stream->Write<uint64_t>(due_guest_filetime_);
  stream->Write<uint32_t>(period_ms_);
  stream->Write<uint32_t>(callback_routine_);
  stream->Write<uint32_t>(callback_routine_arg_);
  stream->Write<uint32_t>(callback_thread_ ? callback_thread_->handle() : 0);
  stream->Write<uint8_t>(signaled ? 1 : 0);
  XELOGD("XTimer {:08X}: type {} armed {} period {} signaled {}", handle(),
         timer_type_, armed_, period_ms_, signaled);
  return true;
}

object_ref<XTimer> XTimer::Restore(KernelState* kernel_state,
                                   ByteStream* stream) {
  auto timer = new XTimer();
  timer->kernel_state_ = kernel_state;
  if (!timer->RestoreObject(stream)) {
    delete timer;
    return nullptr;
  }
  uint32_t type = stream->Read<uint32_t>();
  bool armed = stream->Read<uint8_t>() != 0;
  uint64_t due = stream->Read<uint64_t>();
  uint32_t period_ms = stream->Read<uint32_t>();
  uint32_t routine = stream->Read<uint32_t>();
  uint32_t routine_arg = stream->Read<uint32_t>();
  uint32_t thread_handle = stream->Read<uint32_t>();
  bool signaled = stream->Read<uint8_t>() != 0;

  timer->Initialize(type);
  XThread* thread = nullptr;
  if (thread_handle) {
    thread = kernel_state->object_table()
                 ->LookupObject<XThread>(thread_handle)
                 .get();
  }
  uint64_t now = xe::chrono::XSystemClock::to_file_time(
      xe::chrono::XSystemClock::now());
  if (armed && (period_ms || due > now)) {
    // Still pending (or periodic): arm it again at the saved guest time.
    timer->Arm(int64_t(due), period_ms, routine, routine_arg, thread);
  } else if (signaled) {
    // Fired before the save; the callback APC (if any) is already in guest
    // memory. Just signal.
    timer->timer_->SetOnceAfter(xe::chrono::hundrednanoseconds(0));
  }
  XELOGI("XTimer {:08X} restored: type {} armed {} period {} due {} signaled {}",
         timer->handle(), type, armed, period_ms,
         armed ? (due > now ? "future" : "past") : "-", signaled);
  return object_ref<XTimer>(timer);
}

X_STATUS XTimer::Cancel() {
  std::lock_guard<std::mutex> lock(timer_lock_);
  armed_ = false;
  return timer_->Cancel() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

}  // namespace kernel
}  // namespace xe
