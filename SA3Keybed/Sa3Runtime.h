#pragma once

#include "libsa3_v1.h"

#include <string>

namespace keybed
{

// libsa3 is loaded from the plugin's own folder on first render, never at module load, so strict host
// scanners can instantiate the plugin without resolving ggml/CUDA. Returns nullptr and fills `error`
// when the library or its V1 table is unavailable. Thread-safe; the table lives for the process.
const sa3_api_v1* LoadSa3Api(std::string& error);

} // namespace keybed
