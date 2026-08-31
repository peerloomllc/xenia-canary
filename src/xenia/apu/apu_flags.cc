/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/apu_flags.h"

DEFINE_bool(mute, false, "Mutes all audio output.", "APU")
DEFINE_string(fast_forward_audio, "stretch",
              "Audio while the time scalar is above 1: 'stretch' keeps pitch "
              "(SoundTouch), 'resample' shifts pitch with speed, 'mute'.",
              "APU");
DEFINE_string(slow_motion_audio, "resample",
              "Audio while the time scalar is below 1: 'stretch', 'resample' "
              "or 'mute'.",
              "APU");

namespace xe {
namespace apu {
// cv_mute is file-static, so a runtime override has to be issued from here.
void SetMuteOverride(bool mute) { OVERRIDE_bool(mute, mute); }
}  // namespace apu
}  // namespace xe
