# GTK3 on Windows, for the Settings window and game library the Linux build
# draws with GTK. Optional: XE_GTK3_ROOT (or the environment variable of the
# same name) is an unpacked GTK3 build from gvsbuild
# (github.com/wingtk/gvsbuild, GTK3_Gvsbuild_*_x64.zip) with bin, include,
# lib and share. Left empty, the build has no GTK, as upstream.

set(XE_GTK3_ROOT "$ENV{XE_GTK3_ROOT}" CACHE PATH
    "Unpacked gvsbuild GTK3 (bin, include, lib, share); empty: no GTK")

if(NOT XE_GTK3_ROOT)
  return()
endif()
if(NOT EXISTS "${XE_GTK3_ROOT}/include/gtk-3.0/gtk/gtk.h")
  message(FATAL_ERROR "XE_GTK3_ROOT=${XE_GTK3_ROOT} has no include/gtk-3.0")
endif()
message(STATUS "GTK3 for Windows: ${XE_GTK3_ROOT}")

set(XE_UI_GTK ON)
add_compile_definitions(XE_UI_GTK=1)

set(_gtk "${XE_GTK3_ROOT}")
set(GTK3_INCLUDE_DIRS
  "${_gtk}/include/gtk-3.0"
  "${_gtk}/include/glib-2.0"
  "${_gtk}/lib/glib-2.0/include"
  "${_gtk}/include/pango-1.0"
  "${_gtk}/include/harfbuzz"
  "${_gtk}/include/cairo"
  "${_gtk}/include/gdk-pixbuf-2.0"
  "${_gtk}/include/atk-1.0"
  "${_gtk}/include/fribidi"
  "${_gtk}/include/freetype2"
  "${_gtk}/include/pixman-1"
  "${_gtk}/include"
)
set(GTK3_LIBRARIES)
foreach(_lib gtk-3 gdk-3 pangowin32-1.0 pangocairo-1.0 pango-1.0 harfbuzz
             atk-1.0 cairo-gobject cairo gdk_pixbuf-2.0 gio-2.0 gobject-2.0
             glib-2.0 intl)
  list(APPEND GTK3_LIBRARIES "${_gtk}/lib/${_lib}.lib")
endforeach()
# GTK's headers as system headers: their warnings are not ours.
include_directories(SYSTEM ${GTK3_INCLUDE_DIRS})

# The DLLs gtk-3-vs17.dll needs, directly or not (llvm-objdump -p, followed
# down), and the data GTK reads relative to them: GLib takes the folder of
# a DLL that is not called bin as the installation prefix, so share/ goes
# next to the executable.
set(XE_GTK3_RUNTIME_DLLS
  atk-1.0-0.dll cairo-2.dll cairo-gobject-2.dll epoxy-0.dll ffi-8.dll
  fontconfig-1.dll freetype-6.dll fribidi-0.dll gdk-3-vs17.dll
  gdk_pixbuf-2.0-0.dll gio-2.0-0.dll glib-2.0-0.dll gmodule-2.0-0.dll
  gobject-2.0-0.dll gtk-3-vs17.dll harfbuzz.dll iconv.dll intl.dll
  jpeg62.dll libexpat.dll libpng16.dll pango-1.0-0.dll pangocairo-1.0-0.dll
  pangoft2-1.0-0.dll pangowin32-1.0-0.dll pcre2-8-0.dll pixman-1-0.dll
  tiff.dll zlib1.dll
)

# Copies the runtime next to `target`'s executable after each build.
function(xe_copy_gtk3_runtime target)
  set(_dlls)
  foreach(_dll ${XE_GTK3_RUNTIME_DLLS})
    list(APPEND _dlls "${XE_GTK3_ROOT}/bin/${_dll}")
  endforeach()
  set(_out "$<TARGET_FILE_DIR:${target}>")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_dlls} "${_out}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${XE_GTK3_ROOT}/share/icons/Adwaita" "${_out}/share/icons/Adwaita"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${XE_GTK3_ROOT}/share/icons/hicolor" "${_out}/share/icons/hicolor"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${XE_GTK3_ROOT}/share/glib-2.0/schemas"
            "${_out}/share/glib-2.0/schemas"
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${XE_GTK3_ROOT}/share/doc -DDST=${_out}/share/doc
            -P "${PROJECT_SOURCE_DIR}/cmake/XeniaGtkLicenses.cmake"
    VERBATIM)
endfunction()
