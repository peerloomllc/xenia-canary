/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_APU_FLAGS_H_
#define XENIA_APU_APU_FLAGS_H_

#include "xenia/base/cvar.h"
DECLARE_bool(mute)

#endif  // XENIA_APU_APU_FLAGS_H_
DECLARE_string(fast_forward_audio);
DECLARE_string(slow_motion_audio);

namespace xe {
namespace apu {
// Sets --mute at runtime as a config override (saved with the config).
void SetMuteOverride(bool mute);
}  // namespace apu
}  // namespace xe
