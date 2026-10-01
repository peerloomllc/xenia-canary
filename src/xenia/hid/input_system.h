/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_INPUT_SYSTEM_H_
#define XENIA_HID_INPUT_SYSTEM_H_

#include <atomic>
#include <bitset>
#include <memory>
#include <vector>
#include "xenia/base/mutex.h"
#include "xenia/hid/input.h"
#include "xenia/hid/input_driver.h"
#include "xenia/hid/portal/portal.h"
#include "xenia/xbox.h"

namespace xe {
namespace ui {
class Window;
}  // namespace ui
}  // namespace xe

namespace xe {
namespace hid {

class InputSystem {
 public:
  explicit InputSystem(xe::ui::Window* window);
  ~InputSystem();

  xe::ui::Window* window() const { return window_; }

  X_STATUS Setup();

  void AddDriver(std::unique_ptr<InputDriver> driver);

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps);
  X_RESULT GetState(uint32_t user_index, uint32_t flags,
                    X_INPUT_STATE* out_state);
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration);
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke);

  bool GetVibrationCvar();
  void ToggleVibration();

  const std::bitset<XUserMaxUserCount> GetConnectedSlots() const {
    return connected_slots;
  }

  uint32_t GetLastUsedSlot() const { return last_used_slot; }

  // The host UI (the menus and the game library, opened from a controller)
  // takes the pad while it is up, so a d-pad press that moves the menu
  // selection does not also reach the running title. Separate from XAM's own
  // dialog flag, which belongs to the guest's dialogs and must not be
  // cleared by ours.
  // True when that slot's controller is kept for the emulator's own UI
  // (--ui_only_controllers) and must not be shown to a title.
  bool IsUiOnlySlot(uint32_t user_index) const;

  void set_ui_holds_pad(bool value) { ui_holds_pad_.store(value); }
  bool ui_holds_pad() const { return ui_holds_pad_.load(); }

  Portal* GetPortal() { return portal_.get(); }

  std::unique_lock<xe_unlikely_mutex> lock();

 private:
  typedef std::pair<uint16_t, uint16_t> joystick_value;

  const std::string controller_slot_state_change_message[2] = {
      "Controller disconnected from slot {}.",
      "New controller connected to slot {}."};

  void UpdateUsedSlot(InputDriver* driver, uint8_t slot, bool connected);
  void AdjustDeadzoneLevels(const uint8_t slot, X_INPUT_GAMEPAD* gamepad);
  X_INPUT_VIBRATION ModifyVibrationLevel(X_INPUT_VIBRATION* vibration);

  std::atomic<bool> ui_holds_pad_{false};

  std::vector<InputDriver*> FilterDrivers(uint32_t flags);

  xe::ui::Window* window_ = nullptr;

  std::vector<std::unique_ptr<InputDriver>> drivers_;

  std::unique_ptr<Portal> portal_;

  std::bitset<XUserMaxUserCount> connected_slots = {};
  std::array<std::pair<joystick_value, joystick_value>, XUserMaxUserCount>
      controllers_max_joystick_value = {};
  uint32_t last_used_slot = 0;

  xe_unlikely_mutex lock_;
};

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_INPUT_SYSTEM_H_
