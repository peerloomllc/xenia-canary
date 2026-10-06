add_library(xenia-hid-sdl STATIC)
xe_platform_sources(xenia-hid-sdl ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(xenia-hid-sdl PUBLIC xenia-base xenia-hid xenia-ui SDL2)
xe_target_defaults(xenia-hid-sdl)
if(WIN32)
  # The device tree (CM_*), for guitars Windows republishes under generic ids.
  target_link_libraries(xenia-hid-sdl PRIVATE cfgmgr32)
endif()
