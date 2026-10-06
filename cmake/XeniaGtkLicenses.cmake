# cmake -DSRC=<gvsbuild share/doc> -DDST=<out share/doc> -P this file:
# copies the licence texts of the GTK runtime shipped with the Windows build.
file(GLOB_RECURSE _files RELATIVE "${SRC}"
  "${SRC}/*/COPYING*" "${SRC}/*/LICENSE*" "${SRC}/*/LicenseRef*")
foreach(_file ${_files})
  get_filename_component(_dir "${DST}/${_file}" DIRECTORY)
  file(MAKE_DIRECTORY "${_dir}")
  file(COPY_FILE "${SRC}/${_file}" "${DST}/${_file}" ONLY_IF_DIFFERENT)
endforeach()
