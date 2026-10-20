/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <gtk/gtk.h>
#include <cstdio>
#include <cstdlib>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/ui/window_icon_gtk.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_gtk.h"

namespace {

void SetDefaultWindowIcon() {
  GList* icons = nullptr;
  for (const xe::ui::WindowIconPng& png : xe::ui::kWindowIcons) {
    GdkPixbufLoader* loader = gdk_pixbuf_loader_new_with_type("png", nullptr);
    if (!loader) {
      continue;
    }
    if (gdk_pixbuf_loader_write(loader, png.data, png.size, nullptr) &&
        gdk_pixbuf_loader_close(loader, nullptr)) {
      GdkPixbuf* pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
      if (pixbuf) {
        icons = g_list_append(icons, g_object_ref(pixbuf));
      }
    }
    g_object_unref(loader);
  }
  if (icons) {
    gtk_window_set_default_icon_list(icons);
    g_list_free_full(icons, g_object_unref);
  }
}

}  // namespace

int main(int argc_pre_gtk, char** argv_pre_gtk) {
  // Before touching anything GTK+, make sure that when running on Wayland,
  // we'll still get an X11 (Xwayland) window
  // also allow users to override this
  if (!secure_getenv("GDK_BACKEND")) {
    setenv("GDK_BACKEND", "x11", 1);
  }

  // The window class (WM_CLASS) comes from the program name, which is the
  // binary's file name by default. Fix it so desktop files can match the
  // window (StartupWMClass=xenia_canary) whatever the binary is called.
  g_set_prgname("xenia_canary");

  // Initialize GTK+, which will handle and remove its own arguments from argv.
  // Both GTK+ and Xenia use --option=value argument format (see man
  // gtk-options), however, it's meaningless to try to parse the same argument
  // both as a GTK+ one and as a cvar. Make GTK+ options take precedence in case
  // of a name collision, as there's an alternative way of setting Xenia options
  // (the config).
  int argc_post_gtk = argc_pre_gtk;
  char** argv_post_gtk = argv_pre_gtk;
  if (!gtk_init_check(&argc_post_gtk, &argv_post_gtk)) {
    // Logging has not been initialized yet.
    std::fputs("Failed to initialize GTK+\n", stderr);
    return EXIT_FAILURE;
  }
  // F10 is GTK's "activate the menu bar" key; Xenia uses it as a hotkey
  // (load state), and both firing at once opened the File menu on every
  // load. Alt+<mnemonic> still opens the menus.
  g_object_set(gtk_settings_get_default(), "gtk-menu-bar-accel", "", nullptr);
  // Without a window icon the taskbar shows the generic X11 one.
  SetDefaultWindowIcon();

  int result;

  {
    xe::ui::GTKWindowedAppContext app_context;

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

    cvar::ParseLaunchArguments(argc_post_gtk, argv_post_gtk,
                               app->GetPositionalOptionsUsage(),
                               app->GetPositionalOptions());

    // Initialize logging. Needs parsed cvars.
    xe::InitializeLogging(app->GetName());

    if (app->OnInitialize()) {
      app_context.RunMainGTKLoop();
      result = EXIT_SUCCESS;
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  // Logging may still be needed in the destructors.
  xe::ShutdownLogging();

  return result;
}
