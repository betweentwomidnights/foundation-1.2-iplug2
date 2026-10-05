cmake_minimum_required(VERSION 3.20)
project(FoundationKeysRepo VERSION 0.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IPLUG2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/iPlug2" CACHE PATH "iPlug2 root directory")
# defaults to a sibling checkout (clone sa3.cpp next to this repo); override with -DSA3_CPP_DIR=...
get_filename_component(_SA3_SIBLING "${CMAKE_CURRENT_SOURCE_DIR}/../sa3.cpp" ABSOLUTE)
set(SA3_CPP_DIR "${_SA3_SIBLING}" CACHE PATH "sa3.cpp repository root")
set(_SA3_DEFAULT_BUILD_DIR "${SA3_CPP_DIR}/build")
# ggml's libraries may carry a prefix (sa3.cpp's SA3_PRIVATE_GGML: sa3-cuda-<commit>-ggml-cuda.dll).
file(GLOB _SA3_DEFAULT_CUDA_DLL "${SA3_CPP_DIR}/build-cuda/bin/Release/*ggml-cuda.dll")
if(EXISTS "${SA3_CPP_DIR}/build-cuda/Release/sa3.lib" AND _SA3_DEFAULT_CUDA_DLL)
  set(_SA3_DEFAULT_BUILD_DIR "${SA3_CPP_DIR}/build-cuda")
endif()
set(SA3_BUILD_DIR "${_SA3_DEFAULT_BUILD_DIR}" CACHE PATH "sa3.cpp build directory containing sa3.dll (built with SA3_BUILD_SAT=ON)")
if(DEFINED ENV{CUDA_PATH})
  file(TO_CMAKE_PATH "$ENV{CUDA_PATH}" _SA3_CUDA_PATH)
else()
  set(_SA3_CUDA_PATH "C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.8")
endif()
set(SA3_CUDA_RUNTIME_DIR "${_SA3_CUDA_PATH}/bin" CACHE PATH "Optional CUDA runtime DLL directory")
set(IPLUG_DEPLOY_PLUGINS OFF CACHE BOOL "Deploy built plugins to system directories" FORCE)

include("${IPLUG2_DIR}/iPlug2.cmake")
find_package(iPlug2 REQUIRED)

# libsa3_v1.h plus the header-only Foundation keybed policy (sat/keybed.h). The plugin never links
# sa3: it loads sa3.dll at render time so host scanners never touch ggml.
add_library(sa3_headers INTERFACE)
target_include_directories(sa3_headers INTERFACE "${SA3_CPP_DIR}/src")

add_library(sa3_ui_theme INTERFACE)
target_include_directories(sa3_ui_theme INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/Shared")

# The plugin runs inside a DAW, where Windows binds an import of ggml.dll to any ggml.dll already
# loaded: an older FoundationKeys, or another vendor's ggml plugin, would hand sa3.dll its ggml.
# Release builds therefore use a sa3.cpp build configured with -DSA3_PRIVATE_GGML=ON, whose ggml
# libraries are named after the backend and ggml commit (sa3-vulkan-ca7bcb6f-ggml*.dll).
file(GLOB SA3_RUNTIME_DLLS
  "${SA3_BUILD_DIR}/bin/Release/sa3.dll"
  "${SA3_BUILD_DIR}/bin/Release/*ggml*.dll"
)
if(EXISTS "${SA3_BUILD_DIR}/bin/Release/ggml.dll" OR EXISTS "${SA3_BUILD_DIR}/ggml/src/libggml.dylib")
  set(_SA3_PLAIN_GGML_MESSAGE
    "${SA3_BUILD_DIR} names its ggml libraries plain ggml*, which can bind to another plugin's ggml in "
    "the same DAW. Reconfigure that sa3.cpp build with -DSA3_PRIVATE_GGML=ON for anything you ship.")
  if(DEFINED FOUNDATION_KEYS_DEV_MODELS AND NOT FOUNDATION_KEYS_DEV_MODELS)
    message(FATAL_ERROR ${_SA3_PLAIN_GGML_MESSAGE})
  else()
    message(WARNING ${_SA3_PLAIN_GGML_MESSAGE})
  endif()
endif()
if(APPLE)
  file(GLOB_RECURSE SA3_RUNTIME_DYLIBS
    "${SA3_BUILD_DIR}/libsa3*.dylib"
    "${SA3_BUILD_DIR}/ggml/src/lib*ggml*.dylib"
    "${SA3_BUILD_DIR}/ggml/src/ggml-*/*.dylib"
  )
  # ggml lays each library down under three names (libggml.dylib, libggml.0.dylib,
  # libggml.0.17.0.dylib) that all carry the same @rpath install name, so copying every one triples
  # the payload and the notarization signing work. Keep only the soname the binaries actually load.
  # A SA3_PRIVATE_GGML build prefixes the name: libsa3-metal-<commit>-ggml.0.dylib.
  list(FILTER SA3_RUNTIME_DYLIBS INCLUDE REGEX "/(libsa3\\.dylib|lib[A-Za-z0-9_-]*ggml[a-z-]*\\.[0-9]+\\.dylib)$")
endif()
# Only bundle the CUDA runtime when the selected sa3 build actually uses the CUDA backend.
file(GLOB _SA3_CUDA_DLL "${SA3_BUILD_DIR}/bin/Release/*ggml-cuda.dll")
if(_SA3_CUDA_DLL)
  file(GLOB SA3_CUDA_RUNTIME_DLLS
    "${SA3_CUDA_RUNTIME_DIR}/cudart64_*.dll"
    "${SA3_CUDA_RUNTIME_DIR}/cublas64_*.dll"
    "${SA3_CUDA_RUNTIME_DIR}/cublasLt64_*.dll"
  )
  list(APPEND SA3_RUNTIME_DLLS ${SA3_CUDA_RUNTIME_DLLS})
endif()
set(SA3_RUNTIME_LIBS ${SA3_RUNTIME_DLLS} ${SA3_RUNTIME_DYLIBS})
list(REMOVE_DUPLICATES SA3_RUNTIME_LIBS)

add_subdirectory(FoundationKeys)
