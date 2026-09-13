/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_CONTROLLER_SUBTYPE_H_
#define XENIA_HID_CONTROLLER_SUBTYPE_H_

#include <cstdint>
#include <optional>
#include <string_view>

namespace xe {
namespace hid {

// Resolves the --controller_subtypes setting for one device.
//
// An entry is which:kind. "which" is part of the device's name, matched
// case-insensitively, or a plain number meaning that slot. A name is the
// better half of the two: slots are handed out in the order devices are
// plugged in, so a slot entry left in the config from an earlier session
// applies to whatever is in that slot now.
//
// Returns nothing when no entry applies, which leaves the kind SDL reports.
std::optional<uint8_t> ForcedControllerSubtype(std::string_view setting,
                                               size_t user_index,
                                               std::string_view device_name);

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_CONTROLLER_SUBTYPE_H_
