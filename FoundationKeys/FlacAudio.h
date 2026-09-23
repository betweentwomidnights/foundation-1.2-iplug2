#pragma once

#include "KeybedKit.h"

namespace keybed
{

bool WritePlanarFlac(const std::string& path, const float* planar, int channels, int frames, int sampleRate,
                     std::string& error);
NoteSamplePtr ReadNoteFlac(const std::string& path, int midi, std::string& error, int layer, bool layered);

} // namespace keybed
