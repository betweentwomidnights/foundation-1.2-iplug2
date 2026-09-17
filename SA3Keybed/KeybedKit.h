#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace keybed
{

// One playable key: an immutable stereo sample rendered at `sampleRate`, rooted at `midi` (sounding
// pitch, C4 = 60). Shared between the kit on disk, the UI, and the audio thread's bank snapshots.
struct NoteSample
{
  int midi = 60;
  int sampleRate = 44100;
  int frames = 0;
  std::vector<float> left;
  std::vector<float> right;
  float peak = 0.f;
};

using NoteSamplePtr = std::shared_ptr<const NoteSample>;

// What produced a kit, stored beside its WAVs as kit.json.
struct KitManifest
{
  std::string descriptor;
  bool wet = false;
  std::vector<std::string> fx;
  uint64_t seed = 0;
  int steps = 80;
  float cfgScale = 6.f;
  float sigmaMin = 0.03f;
  float sigmaMax = 500.f;
  std::string model = "foundation-1.2-keybeds";
  std::string encoding = "F16";
  std::string range;                // "C2-B5", "preview C4 x6", ...
  std::vector<int> labelMidis;      // prompt note labels, in render order
  std::vector<int> soundingMidis;   // keys that have a sample
  bool complete = false;            // false while rendering or after a cancel
};

// Documents/sa3-keybed (created on demand); empty on failure.
std::string AppDirectory();
std::string KitsDirectory();
std::string CreateKitDirectory(const std::string& descriptor, uint64_t seed);

// Small persisted preferences in AppDirectory()/settings.txt ("key=value" lines).
std::string LoadSetting(const std::string& key);
bool SaveSetting(const std::string& key, const std::string& value);

std::string NoteFileName(int midi);   // "Csharp3.wav"
bool WriteNoteWav(const std::string& path, const NoteSample& note, std::string& error);
bool WritePlanarWav(const std::string& path, const float* planar, int channels, int frames, int sampleRate,
                    std::string& error);
NoteSamplePtr ReadNoteWav(const std::string& path, int midi, std::string& error);

bool WriteKitManifest(const std::string& kitDir, const KitManifest& manifest, std::string& error);
bool ReadKitManifest(const std::string& kitDir, KitManifest& manifest, std::string& error);
bool WriteKitSfz(const std::string& kitDir, const std::vector<int>& soundingMidis, std::string& error);

// Loads every note WAV named by the manifest (or found by name when the manifest is missing).
std::vector<NoteSamplePtr> LoadKitSamples(const std::string& kitDir, KitManifest& manifest, std::string& error);

std::string FolderName(const std::string& path);

} // namespace keybed
