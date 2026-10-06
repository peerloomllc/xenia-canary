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

// True for a guitar that reports itself as an ordinary pad, so nothing but
// its USB ids says what it is: the CRKD Les Paul (0351:1300 in its Xbox mode,
// 0351:4161 in its other one).
bool IsKnownGuitar(uint16_t vendor_id, uint16_t product_id);

// The subtype a title needs before it plays a guitar as an instrument, or
// nothing for a title not listed. Guitar Hero III and Aerosmith sort the
// plain guitar subtype in with the pads and want the alternate one; the later
// Guitar Hero titles want the plain one.
std::optional<uint8_t> TitleGuitarSubtype(uint32_t title_id);

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_CONTROLLER_SUBTYPE_H_
