/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/kernel_flags.h"

DEFINE_bool(headless, false,
            "Don't display any UI, using defaults for prompts as needed.",
            "UI");
DEFINE_bool(log_high_frequency_kernel_calls, false,
            "Log kernel calls with the kHighFrequency tag.", "Logging");
DEFINE_string(
    trace_event_handles, "",
    "Trace event object operations (create/set/clear/pulse/wait) at info "
    "level, outside the high-frequency gate. Comma-separated hex handles, or "
    "'all' for every event object. Each line carries a global sequence number "
    "and a microsecond timestamp so interleavings between threads can be "
    "reconstructed.",
    "Logging");
