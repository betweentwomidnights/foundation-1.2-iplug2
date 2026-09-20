# The sa3/ggml dylibs are built in sa3.cpp's build tree, so they carry that tree's absolute rpaths
# and reference each other as @rpath/libggml*.dylib. Copying them beside the binary is not enough:
# without @loader_path the loader keeps resolving them from the build machine, and the bundle fails
# on any other machine. Add @loader_path to each copied dylib, then re-sign (install_name_tool
# invalidates the signature, and arm64 refuses to load an unsigned dylib).
file(GLOB _sa3_dylibs "${BUNDLE_DIR}/*.dylib")
foreach(_lib ${_sa3_dylibs})
  execute_process(COMMAND otool -l "${_lib}" OUTPUT_VARIABLE _load_cmds ERROR_QUIET)
  if(NOT _load_cmds MATCHES "path @loader_path ")
    execute_process(COMMAND install_name_tool -add_rpath "@loader_path" "${_lib}"
                    RESULT_VARIABLE _rc ERROR_QUIET OUTPUT_QUIET)
    if(NOT _rc EQUAL 0)
      message(WARNING "install_name_tool -add_rpath failed for ${_lib}")
    endif()
  endif()
  execute_process(COMMAND codesign --force --sign - "${_lib}" ERROR_QUIET OUTPUT_QUIET)
endforeach()
