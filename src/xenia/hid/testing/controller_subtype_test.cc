/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/controller_subtype.h"

#include "third_party/catch/include/catch.hpp"
#include "xenia/hid/input.h"

namespace xe::hid::test {

TEST_CASE("controller_subtypes empty", "[controller_subtype]") {
  REQUIRE_FALSE(ForcedControllerSubtype("", 0, "DualSense Wireless Controller")
                    .has_value());
}

TEST_CASE("controller_subtypes by slot", "[controller_subtype]") {
  REQUIRE(ForcedControllerSubtype("0:guitar", 0, "CRKD Guitar") ==
          XINPUT_DEVSUBTYPE_GUITAR);
  REQUIRE_FALSE(
      ForcedControllerSubtype("0:guitar", 1, "CRKD Guitar").has_value());
  REQUIRE(ForcedControllerSubtype("1:drums", 1, "whatever") ==
          XINPUT_DEVSUBTYPE_DRUM_KIT);
}

TEST_CASE("controller_subtypes by device name", "[controller_subtype]") {
  REQUIRE(ForcedControllerSubtype("crkd guitar:guitar", 0, "CRKD Guitar") ==
          XINPUT_DEVSUBTYPE_GUITAR);
  SECTION("matched case-insensitively, anywhere in the name") {
    REQUIRE(ForcedControllerSubtype("GUITAR:guitar", 3, "CRKD Guitar") ==
            XINPUT_DEVSUBTYPE_GUITAR);
  }
  SECTION("a name entry leaves other devices alone, whatever slot") {
    for (size_t slot = 0; slot < 4; ++slot) {
      REQUIRE_FALSE(ForcedControllerSubtype("crkd guitar:guitar", slot,
                                            "DualSense Wireless Controller")
                        .has_value());
    }
  }
}

TEST_CASE("controller_subtypes name is never read as slot 0",
          "[controller_subtype]") {
  // A pad told it is a guitar loses its right stick to the whammy handling,
  // and the camera rotates on its own with nothing touching the sticks. An
  // entry naming a device must never reach a device that does not match it,
  // however a non-numeric slot is parsed.
  REQUIRE_FALSE(
      ForcedControllerSubtype("crkd:guitar", 0, "DualSense").has_value());
  REQUIRE_FALSE(
      ForcedControllerSubtype("guitar:guitar", 0, "DualSense").has_value());
  REQUIRE_FALSE(
      ForcedControllerSubtype("0a:guitar", 0, "DualSense").has_value());
}

TEST_CASE("controller_subtypes lists and spacing", "[controller_subtype]") {
  const char* setting = " 0:gamepad , crkd guitar:guitar ";
  REQUIRE(ForcedControllerSubtype(setting, 0, "DualSense") ==
          XINPUT_DEVSUBTYPE_GAMEPAD);
  REQUIRE(ForcedControllerSubtype(setting, 1, "CRKD Guitar") ==
          XINPUT_DEVSUBTYPE_GUITAR);
}

TEST_CASE("controller_subtypes malformed entries", "[controller_subtype]") {
  REQUIRE_FALSE(
      ForcedControllerSubtype("guitar", 0, "CRKD Guitar").has_value());
  REQUIRE_FALSE(ForcedControllerSubtype("0:", 0, "CRKD Guitar").has_value());
  REQUIRE_FALSE(
      ForcedControllerSubtype(":guitar", 0, "CRKD Guitar").has_value());
  REQUIRE_FALSE(
      ForcedControllerSubtype("0:tambourine", 0, "CRKD Guitar").has_value());
}

TEST_CASE("controller_subtypes name containing a colon",
          "[controller_subtype]") {
  REQUIRE(
      ForcedControllerSubtype("acme: guitar co:guitar", 0, "ACME: Guitar Co") ==
      XINPUT_DEVSUBTYPE_GUITAR);
}

}  // namespace xe::hid::test
