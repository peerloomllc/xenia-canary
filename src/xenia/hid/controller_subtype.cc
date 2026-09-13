/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/controller_subtype.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "xenia/base/logging.h"
#include "xenia/base/utf8.h"
#include "xenia/hid/input.h"

namespace xe {
namespace hid {

namespace {

std::string_view Trim(std::string_view value) {
  const size_t first = value.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return std::string_view();
  }
  return value.substr(first, value.find_last_not_of(" \t") - first + 1);
}

}  // namespace

std::optional<uint8_t> ForcedControllerSubtype(std::string_view setting,
                                               size_t user_index,
                                               std::string_view device_name) {
  if (setting.empty()) {
    return std::nullopt;
  }
  static const std::unordered_map<std::string, uint8_t> kinds = {
      {"gamepad", XINPUT_DEVSUBTYPE_GAMEPAD},
      {"guitar", XINPUT_DEVSUBTYPE_GUITAR},
      {"guitar_alternate", XINPUT_DEVSUBTYPE_GUITAR_ALTERNATE},
      {"guitar_bass", XINPUT_DEVSUBTYPE_GUITAR_BASS},
      {"drums", XINPUT_DEVSUBTYPE_DRUM_KIT},
      {"wheel", XINPUT_DEVSUBTYPE_WHEEL},
      {"arcade_stick", XINPUT_DEVSUBTYPE_ARCADE_STICK},
      {"arcade_pad", XINPUT_DEVSUBTYPE_ARCADE_PAD},
      {"flight_stick", XINPUT_DEVSUBTYPE_FLIGHT_STICK},
      {"dance_pad", XINPUT_DEVSUBTYPE_DANCE_PAD},
  };
  const std::string name = xe::utf8::lower_ascii(device_name);
  for (const auto& raw : xe::utf8::split(setting, ",")) {
    // The kind is what follows the last colon, so a device whose name
    // carries one still matches.
    const size_t colon = raw.rfind(':');
    if (colon == std::string_view::npos) {
      continue;
    }
    const std::string_view which = Trim(raw.substr(0, colon));
    const std::string_view kind = Trim(raw.substr(colon + 1));
    if (which.empty() || kind.empty()) {
      continue;
    }
    // A plain number is a slot, anything else is part of a device's name.
    // Numbers are the trap: slots are handed out in the order devices are
    // plugged in, so "0:guitar" left in the config from a session with a
    // guitar plugged in applies to whatever is in slot 0 now. A pad told it
    // is a guitar loses its right stick to the whammy handling, and the
    // camera in whatever is being played then rotates on its own with
    // nothing touching the sticks. Matching the name cannot do that. It has
    // to be the name rather than asking SDL what the device is, because a
    // guitar republished as a virtual pad is indistinguishable from a real
    // one.
    const bool is_slot = std::all_of(which.begin(), which.end(), [](char c) {
      return std::isdigit(static_cast<unsigned char>(c)) != 0;
    });
    if (is_slot) {
      if (static_cast<size_t>(std::strtoul(std::string(which).c_str(), nullptr,
                                           10)) != user_index) {
        continue;
      }
    } else if (name.empty() ||
               name.find(xe::utf8::lower_ascii(which)) == std::string::npos) {
      continue;
    }
    const auto it = kinds.find(xe::utf8::lower_ascii(kind));
    if (it == kinds.end()) {
      XELOGW("HID: controller_subtypes: '{}' is not a kind I know", kind);
      return std::nullopt;
    }
    return it->second;
  }
  return std::nullopt;
}

}  // namespace hid
}  // namespace xe
