add_library(xenia-cpu STATIC)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR})
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/backend)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/compiler/passes)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/hir)
xe_platform_sources(xenia-cpu ${CMAKE_CURRENT_SOURCE_DIR}/ppc)
target_include_directories(xenia-cpu PRIVATE
  ${PROJECT_SOURCE_DIR}/third_party/llvm/include
)
target_link_libraries(xenia-cpu PUBLIC xenia-base mspack)

# stack_walker_posix.cc unwinds host and JIT frames with libunwind, and resolves
# host symbols with dladdr.
if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBUNWIND IMPORTED_TARGET libunwind libunwind-generic)
  if(LIBUNWIND_FOUND)
    target_link_libraries(xenia-cpu PUBLIC PkgConfig::LIBUNWIND ${CMAKE_DL_LIBS})
  else()
    message(FATAL_ERROR
      "libunwind not found. Install libunwind development headers "
      "(libunwind-devel on Fedora, libunwind-dev on Debian/Ubuntu).")
  endif()
endif()

xe_target_defaults(xenia-cpu)

if(XENIA_BUILD_TESTS)
  set(CMAKE_FOLDER "tests")
  add_subdirectory(testing)
  add_subdirectory(ppc/testing)
endif()
