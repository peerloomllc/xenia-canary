/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/windowed_app_context_win.h"

#include <cstdlib>

#include "xenia/base/platform_win.h"

#if XE_UI_GTK
#include <gtk/gtk.h>

#include <vector>

#include "xenia/base/logging.h"
#endif

namespace xe {
namespace ui {

#if XE_UI_GTK
namespace {
// GDK's Windows keymap is read from the keyboard layout DLL (kbdus.dll and
// the like), which Wine does not have, so under Wine the keymap is empty.
// GTK finds a key binding through the keymap, so every binding was dead
// there: no arrow keys in a list, no Tab between controls, no Return on a
// row, from the keyboard or the pad. Under Wine, run the binding of a
// navigation key by its keyval instead, the way GTK's own activation does.

// Emits the signals of one binding entry on `object`.
bool EmitBindingSignals(GObject* object, GtkBindingEntry* entry) {
  bool handled = false;
  for (GtkBindingSignal* binding = entry->signals; binding;
       binding = binding->next) {
    guint id = g_signal_lookup(binding->signal_name, G_OBJECT_TYPE(object));
    if (!id) {
      continue;
    }
    GSignalQuery query;
    g_signal_query(id, &query);
    if (query.n_params != guint(binding->n_args) ||
        !(query.signal_flags & G_SIGNAL_ACTION)) {
      continue;
    }
    const GValue unset = G_VALUE_INIT;
    std::vector<GValue> params(query.n_params + 1, unset);
    g_value_init(&params[0], G_OBJECT_TYPE(object));
    g_value_set_object(&params[0], object);
    bool valid = true;
    for (guint i = 0; i < query.n_params && valid; ++i) {
      GType type = query.param_types[i] & ~G_SIGNAL_TYPE_STATIC_SCOPE;
      GtkBindingArg* arg = &binding->args[i];
      GValue* param = &params[i + 1];
      g_value_init(param, type);
      GValue source = G_VALUE_INIT;
      switch (G_TYPE_FUNDAMENTAL(arg->arg_type)) {
        case G_TYPE_LONG:
          g_value_init(&source, G_TYPE_LONG);
          g_value_set_long(&source, arg->d.long_data);
          valid = g_value_transform(&source, param);
          g_value_unset(&source);
          break;
        case G_TYPE_DOUBLE:
          g_value_init(&source, G_TYPE_DOUBLE);
          g_value_set_double(&source, arg->d.double_data);
          valid = g_value_transform(&source, param);
          g_value_unset(&source);
          break;
        case G_TYPE_STRING:
          if (G_TYPE_FUNDAMENTAL(type) == G_TYPE_ENUM) {
            GEnumClass* enum_class =
                static_cast<GEnumClass*>(g_type_class_ref(type));
            GEnumValue* value =
                g_enum_get_value_by_name(enum_class, arg->d.string_data);
            if (!value) {
              value = g_enum_get_value_by_nick(enum_class, arg->d.string_data);
            }
            valid = value != nullptr;
            if (value) {
              g_value_set_enum(param, value->value);
            }
            g_type_class_unref(enum_class);
          } else if (G_TYPE_FUNDAMENTAL(type) == G_TYPE_STRING) {
            g_value_set_static_string(param, arg->d.string_data);
          } else {
            valid = false;
          }
          break;
        default:
          valid = false;
          break;
      }
    }
    if (valid) {
      GValue result = G_VALUE_INIT;
      if (query.return_type != G_TYPE_NONE) {
        g_value_init(&result, query.return_type);
      }
      g_signal_emitv(params.data(), id, 0, &result);
      if (G_IS_VALUE(&result)) {
        g_value_unset(&result);
      }
      handled = true;
    }
    for (GValue& value : params) {
      if (G_IS_VALUE(&value)) {
        g_value_unset(&value);
      }
    }
  }
  return handled;
}

bool ActivateBindingByKeyval(GtkWidget* widget, guint keyval,
                             GdkModifierType modifiers) {
  for (GType type = G_OBJECT_TYPE(widget); type && type != G_TYPE_OBJECT;
       type = g_type_parent(type)) {
    GtkBindingSet* set = gtk_binding_set_by_class(g_type_class_peek(type));
    for (GtkBindingEntry* entry = set ? set->entries : nullptr; entry;
         entry = entry->set_next) {
      if (!entry->destroyed && entry->keyval == keyval &&
          entry->modifiers == modifiers) {
        return EmitBindingSignals(G_OBJECT(widget), entry);
      }
    }
  }
  return false;
}

gint WineKeySnooper(GtkWidget* grab_widget, GdkEventKey* event, gpointer) {
  if (event->type != GDK_KEY_PRESS) {
    return FALSE;
  }
  guint keyval = event->keyval;
  auto modifiers =
      GdkModifierType(event->state & gtk_accelerator_get_default_mod_mask());
  switch (keyval) {
    case GDK_KEY_ISO_Left_Tab:
      // What a keymap makes of Shift+Tab; GTK binds Tab + Shift.
      keyval = GDK_KEY_Tab;
      modifiers = GdkModifierType(modifiers | GDK_SHIFT_MASK);
      break;
    case GDK_KEY_Up:
    case GDK_KEY_Down:
    case GDK_KEY_Left:
    case GDK_KEY_Right:
    case GDK_KEY_Tab:
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_Escape:
    case GDK_KEY_Page_Up:
    case GDK_KEY_Page_Down:
    case GDK_KEY_Home:
    case GDK_KEY_End:
      break;
    default:
      // Not space or a letter: those are typed, and the window's bindings
      // (space activates the focused widget) would swallow them.
      return FALSE;
  }
  GtkWidget* toplevel = gtk_widget_get_toplevel(grab_widget);
  GtkWidget* widget = GTK_IS_WINDOW(toplevel)
                          ? gtk_window_get_focus(GTK_WINDOW(toplevel))
                          : nullptr;
  if (!widget) {
    widget = toplevel;
  }
  for (; widget; widget = gtk_widget_get_parent(widget)) {
    if (ActivateBindingByKeyval(widget, keyval, modifiers)) {
      return TRUE;
    }
  }
  return FALSE;
}
}  // namespace
#endif  // XE_UI_GTK

bool Win32WindowedAppContext::pending_functions_window_class_registered_;

Win32WindowedAppContext::~Win32WindowedAppContext() {
  if (pending_functions_hwnd_) {
    DestroyWindow(pending_functions_hwnd_);
  }
  if (user32_module_) {
    FreeLibrary(user32_module_);
  }
  if (shcore_module_) {
    FreeLibrary(shcore_module_);
  }
}

bool Win32WindowedAppContext::Initialize() {
  // Logging possibly not initialized in this function yet.

  // Obtain function pointers that may be used for windows if available.
  shcore_module_ = LoadLibraryW(L"SHCore.dll");
  if (shcore_module_) {
    per_monitor_dpi_v1_api_available_ = true;
    per_monitor_dpi_v1_api_available_ &=
        (*reinterpret_cast<void**>(
             &per_monitor_dpi_v1_api_.get_dpi_for_monitor) =
             GetProcAddress(shcore_module_, "GetDpiForMonitor")) != nullptr;
  }
  user32_module_ = LoadLibraryW(L"user32.dll");
  if (user32_module_) {
    per_monitor_dpi_v2_api_available_ = true;
    per_monitor_dpi_v2_api_available_ &=
        (*reinterpret_cast<void**>(
             &per_monitor_dpi_v2_api_.adjust_window_rect_ex_for_dpi) =
             GetProcAddress(user32_module_, "AdjustWindowRectExForDpi")) !=
        nullptr;
    per_monitor_dpi_v2_api_available_ &=
        (*reinterpret_cast<void**>(
             &per_monitor_dpi_v2_api_.enable_non_client_dpi_scaling) =
             GetProcAddress(user32_module_, "EnableNonClientDpiScaling")) !=
        nullptr;
    per_monitor_dpi_v2_api_available_ &=
        (*reinterpret_cast<void**>(
             &per_monitor_dpi_v2_api_.get_dpi_for_system) =
             GetProcAddress(user32_module_, "GetDpiForSystem")) != nullptr;
    per_monitor_dpi_v2_api_available_ &=
        (*reinterpret_cast<void**>(
             &per_monitor_dpi_v2_api_.get_dpi_for_window) =
             GetProcAddress(user32_module_, "GetDpiForWindow")) != nullptr;
  }

  // Create the message-only window for executing pending functions - using a
  // window instead of executing them between iterations so non-main message
  // loops, such as Windows modals, can execute pending functions too.
  static constexpr WCHAR kPendingFunctionsWindowClassName[] =
      L"XeniaPendingFunctionsWindowClass";
  if (!pending_functions_window_class_registered_) {
    WNDCLASSEXW pending_functions_window_class = {};
    pending_functions_window_class.cbSize =
        sizeof(pending_functions_window_class);
    pending_functions_window_class.lpfnWndProc = PendingFunctionsWndProc;
    pending_functions_window_class.hInstance = hinstance_;
    pending_functions_window_class.lpszClassName =
        kPendingFunctionsWindowClassName;
    if (!RegisterClassExW(&pending_functions_window_class)) {
      return false;
    }
    pending_functions_window_class_registered_ = true;
  }
  pending_functions_hwnd_ = CreateWindowExW(
      0, kPendingFunctionsWindowClassName, L"Xenia Pending Functions",
      WS_OVERLAPPED, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
      HWND_MESSAGE, nullptr, hinstance_, this);
  if (!pending_functions_hwnd_) {
    return false;
  }

#if XE_UI_GTK
  // Without GTK's DLLs the executable would not have started; this fails
  // only if GDK cannot open its display. The host UI then has no GTK windows.
  // Under Wine (Proton) Pango's Windows font path draws text with letters
  // missing; its fontconfig path reads the same fonts correctly.
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll && GetProcAddress(ntdll, "wine_get_version") &&
      !getenv("PANGOCAIRO_BACKEND")) {
    _putenv_s("PANGOCAIRO_BACKEND", "fc");
  }
  gtk_available_ = gtk_init_check(nullptr, nullptr);
  if (gtk_available_) {
    // Dark or light as Windows' own app mode setting says, as a native
    // window would be.
    DWORD light = 1;
    DWORD size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER,
                 L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                 L"Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light,
                 &size);
    g_object_set(gtk_settings_get_default(),
                 "gtk-application-prefer-dark-theme", light ? FALSE : TRUE,
                 nullptr);
    GdkKeymapKey* keys = nullptr;
    gint key_count = 0;
    if (!gdk_keymap_get_entries_for_keyval(
            gdk_keymap_get_for_display(gdk_display_get_default()), GDK_KEY_Down,
            &keys, &key_count) ||
        !key_count) {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      gtk_key_snooper_install(WineKeySnooper, nullptr);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
    g_free(keys);
  }
#endif

  return true;
}

void Win32WindowedAppContext::NotifyUILoopOfPendingFunctions() {
  while (!PostMessageW(pending_functions_hwnd_,
                       kPendingFunctionsWindowClassMessageExecute, 0, 0)) {
    Sleep(1);
  }
}

void Win32WindowedAppContext::PlatformQuitFromUIThread() {
  // Send WM_QUIT to whichever loop happens to process it - may be the loop of a
  // built-in modal window, which is unaware of HasQuitFromUIThread, don't let
  // it delay quitting indefinitely.
  PostQuitMessage(EXIT_SUCCESS);
#if XE_UI_GTK
  // GDK's event source takes WM_QUIT off the queue with every other message
  // and drops it, so the GLib loop has to be told directly.
  if (gtk_main_loop_) {
    g_main_loop_quit(static_cast<GMainLoop*>(gtk_main_loop_));
  }
#endif
}

int Win32WindowedAppContext::RunMainMessageLoop() {
  int result = EXIT_SUCCESS;
#if XE_UI_GTK
  if (gtk_available_) {
    XELOGI("GTK {}.{}.{}: running the GLib main loop", gtk_get_major_version(),
           gtk_get_minor_version(), gtk_get_micro_version());
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    gtk_main_loop_ = loop;
    if (!HasQuitFromUIThread()) {
      g_main_loop_run(loop);
    }
    gtk_main_loop_ = nullptr;
    g_main_loop_unref(loop);
    // Quitting may also have started outside PlatformQuitFromUIThread.
    QuitFromUIThread();
    return result;
  }
#endif
  MSG message;
  // The HasQuitFromUIThread check is not absolutely required, but for
  // additional safety in case WM_QUIT is not received for any reason.
  while (!HasQuitFromUIThread()) {
    BOOL message_result = GetMessageW(&message, nullptr, 0, 0);
    if (message_result == 0 || message_result == -1) {
      // WM_QUIT (0 - this is the primary message loop, no need to resend, also
      // contains the result from PostQuitMessage in wParam) or an error
      // (-1). Quitting the context will run the pending functions. Getting
      // WM_QUIT doesn't imply that QuitFromUIThread has been called already, it
      // may originate in some place other than PlatformQuitFromUIThread as
      // well - call it to finish everything including the pending functions.
      QuitFromUIThread();
      result = message_result ? EXIT_FAILURE : int(message.wParam);
      break;
    }
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  return result;
}

LRESULT CALLBACK Win32WindowedAppContext::PendingFunctionsWndProc(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_CLOSE) {
    // Need the window for the entire context's lifetime, don't allow anything
    // to close it.
    return 0;
  }
  if (message == WM_NCCREATE) {
    SetWindowLongPtrW(
        hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(
            reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams));
  } else {
    auto app_context = reinterpret_cast<Win32WindowedAppContext*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (app_context) {
      switch (message) {
        case WM_DESTROY:
          // The message-only window owned by the context is being destroyed,
          // thus the context won't be able to execute pending functions
          // anymore - can't continue functioning normally.
          app_context->QuitFromUIThread();
          break;
        case kPendingFunctionsWindowClassMessageExecute:
          app_context->ExecutePendingFunctionsFromUIThread();
          return 0;
        default:
          break;
      }
    }
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace ui
}  // namespace xe
