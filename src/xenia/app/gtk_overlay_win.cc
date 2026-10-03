/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#if XE_UI_GTK

#include "xenia/app/gtk_overlay_win.h"

#include <gdk/gdkwin32.h>
#include <gtk/gtk.h>

#include <CommCtrl.h>

#pragma comment(lib, "comctl32.lib")

namespace xe {
namespace app {

GtkOverlayWin::GtkOverlayWin(HWND owner, GtkWidget* child) : owner_(owner) {
  window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window_), "Xenia game library");
  gtk_window_set_decorated(GTK_WINDOW(window_), FALSE);
  gtk_window_set_skip_taskbar_hint(GTK_WINDOW(window_), TRUE);
  gtk_container_add(GTK_CONTAINER(window_), child);
  // Alt+F4 on it hides it, as File > Game Library does, rather than
  // destroying the library.
  g_signal_connect(window_, "delete-event",
                   G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) {
                     static_cast<GtkOverlayWin*>(data)->Show(false);
                     return gboolean(TRUE);
                   }),
                   this);
  gtk_widget_realize(window_);
  hwnd_ = static_cast<HWND>(
      gdk_win32_window_get_handle(gtk_widget_get_window(window_)));
  SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner_));
  SetWindowSubclass(owner_, OwnerSubclassProc, reinterpret_cast<UINT_PTR>(this),
                    reinterpret_cast<DWORD_PTR>(this));
}

GtkOverlayWin::~GtkOverlayWin() {
  if (owner_) {
    RemoveWindowSubclass(owner_, OwnerSubclassProc,
                         reinterpret_cast<UINT_PTR>(this));
  }
  gtk_widget_destroy(window_);
}

void GtkOverlayWin::Show(bool show) {
  if (show == shown_) {
    return;
  }
  shown_ = show;
  if (show) {
    gtk_widget_show_all(window_);
    gtk_window_present(GTK_WINDOW(window_));
    // After showing: GDK puts a window it maps back where it last placed
    // it itself, over a position set while the window was hidden.
    FollowOwner();
  } else {
    bool had_focus = GetForegroundWindow() == hwnd_;
    gtk_widget_hide(window_);
    // Back to the game, which the keyboard belongs to now.
    if (had_focus && owner_) {
      SetForegroundWindow(owner_);
    }
  }
}

void GtkOverlayWin::FollowOwner() {
  if (!owner_ || !hwnd_ || IsIconic(owner_)) {
    return;
  }
  RECT client;
  GetClientRect(owner_, &client);
  POINT origin = {client.left, client.top};
  ClientToScreen(owner_, &origin);
  SetWindowPos(hwnd_, nullptr, origin.x, origin.y, client.right - client.left,
               client.bottom - client.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK GtkOverlayWin::OwnerSubclassProc(HWND hwnd, UINT message,
                                                  WPARAM wparam, LPARAM lparam,
                                                  UINT_PTR id, DWORD_PTR data) {
  auto* overlay = reinterpret_cast<GtkOverlayWin*>(data);
  switch (message) {
    case WM_MOVE:
    case WM_SIZE:
    case WM_WINDOWPOSCHANGED:
      if (overlay->shown_) {
        overlay->FollowOwner();
      }
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(hwnd, OwnerSubclassProc, id);
      overlay->owner_ = nullptr;
      break;
    default:
      break;
  }
  return DefSubclassProc(hwnd, message, wparam, lparam);
}

}  // namespace app
}  // namespace xe

#endif  // XE_UI_GTK
