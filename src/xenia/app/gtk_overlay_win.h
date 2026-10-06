/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_GTK_OVERLAY_WIN_H_
#define XENIA_APP_GTK_OVERLAY_WIN_H_

#include "xenia/base/platform_win.h"

typedef struct _GtkWidget GtkWidget;

namespace xe {
namespace app {

// A GTK widget shown over the client area of a Win32 window, as the GTK main
// window's overlay does on Linux: a borderless GTK window owned by `owner`
// (so it stays above it and is minimised with it) that follows the owner's
// client area as it moves, resizes and enters or leaves fullscreen.
class GtkOverlayWin {
 public:
  GtkOverlayWin(HWND owner, GtkWidget* child);
  ~GtkOverlayWin();
  GtkOverlayWin(const GtkOverlayWin&) = delete;
  GtkOverlayWin& operator=(const GtkOverlayWin&) = delete;

  void Show(bool show);
  bool shown() const { return shown_; }
  GtkWidget* window() const { return window_; }

 private:
  static LRESULT CALLBACK OwnerSubclassProc(HWND hwnd, UINT message,
                                            WPARAM wparam, LPARAM lparam,
                                            UINT_PTR id, DWORD_PTR data);
  void FollowOwner();

  HWND owner_;
  GtkWidget* window_;
  HWND hwnd_ = nullptr;
  bool shown_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_GTK_OVERLAY_WIN_H_
