#pragma once

#define FLAC__NO_DLL 1
#define FLAC__HAS_OGG 0
#define PACKAGE_VERSION "1.4.3"

// lpc_flac.c defines a static lround when it sees __GNUC__ without HAVE_LROUND. clang defines
// __GNUC__ and macOS already declares lround in <math.h>, so that shim collides with the real
// declaration. Every toolchain we build with has C99 lround; MSVC below 1800 takes its own
// branch before this is consulted.
#define HAVE_LROUND 1

#define flac_min(a, b) ((a) < (b) ? (a) : (b))
#define flac_max(a, b) ((a) > (b) ? (a) : (b))

#include "all.h"
