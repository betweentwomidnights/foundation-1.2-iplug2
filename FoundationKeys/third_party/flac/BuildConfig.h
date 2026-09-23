#pragma once

#define FLAC__NO_DLL 1
#define FLAC__HAS_OGG 0
#define PACKAGE_VERSION "1.4.3"

#define flac_min(a, b) ((a) < (b) ? (a) : (b))
#define flac_max(a, b) ((a) > (b) ? (a) : (b))

#include "all.h"
