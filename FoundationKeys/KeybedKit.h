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
  int layer = 0;          // 0 = main; 1-2 = supports of a layered keybed
  bool layered = false;   // part of a multi-layer kit, even before its supports have rendered
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
  // Layered kits only (the top-level kit.json): each layer lives in its own folder
  // (sa3::sat::keybed::kLayerDirNames) with its own kit.json and WAVs.
  std::vector<std::string> layerDescriptors;
  std::vector<uint64_t> layerSeeds;
};

// Documents/Foundation Keys (created on demand); empty on failure. Data from before the rename
// (a "sa3-keybed" folder) is moved across the first time.
constexpr const char* kAppFolderName = "Foundation Keys";
std::string AppDirectory();
std::string KitsDirectory();
// Where downloads land by default: per-user app data, not Documents (often cloud-synced), since a tier
// is 0.9-2.5 GB. %LOCALAPPDATA%/Foundation Keys/models; ~/Library/Application Support/Foundation Keys/models.
std::string DefaultModelsDirectory();
// A saved path into a pre-rename "sa3-keybed" folder, pointed at its new home once that exists.
std::string MigratedPath(const std::string& path);
std::string CreateKitDirectory(const std::string& descriptor, uint64_t seed);

// Small persisted preferences in AppDirectory()/settings.txt ("key=value" lines).
std::string LoadSetting(const std::string& key);
bool SaveSetting(const std::string& key, const std::string& value);

std::string NoteFileName(int midi);   // "Csharp3.wav"
bool WriteNoteWav(const std::string& path, const NoteSample& note, std::string& error);
bool WritePlanarWav(const std::string& path, const float* planar, int channels, int frames, int sampleRate,
                    std::string& error);
NoteSamplePtr ReadNoteWav(const std::string& path, int midi, std::string& error, int layer = 0,
                          bool layered = false);

bool WriteKitManifest(const std::string& kitDir, const KitManifest& manifest, std::string& error);
bool ReadKitManifest(const std::string& kitDir, KitManifest& manifest, std::string& error);
bool WriteKitSfz(const std::string& kitDir, const std::vector<int>& soundingMidis, std::string& error);
// One SFZ group per layer, at RC's tri-layer volumes, so other samplers play the layers together.
bool WriteLayeredKitSfz(const std::string& kitDir, const std::vector<std::vector<int>>& layerMidis,
                        std::string& error);

// Loads every note WAV named by the manifest (or found by name when the manifest is missing). A
// layered kit loads each layer's folder, tagging samples with their layer.
std::vector<NoteSamplePtr> LoadKitSamples(const std::string& kitDir, KitManifest& manifest, std::string& error);

std::string FolderName(const std::string& path);

} // namespace keybed
