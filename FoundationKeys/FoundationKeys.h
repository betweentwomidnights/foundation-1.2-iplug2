#pragma once

#include "IPlug_include_in_plug_hdr.h"

#include "KeybedKit.h"
#include "KeybedRenderService.h"
#include "KeybedSampler.h"
#include "ModelDownload.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

const int kNumPresets = 1;

enum EParams
{
  kParamGain = 0,
  kParamAttack,
  kParamDecay,
  kParamSustain,
  kParamRelease,
  kParamVelocity,
  kParamTune,
  kParamOctave,
  kParamFillGaps,
  kParamLayerMain,       // layered keybeds: per-layer volume (RC's tri-layer mixer)
  kParamLayerSupport1,
  kParamLayerSupport2,
  kParamVoiceMode,       // poly / mono (appended: host automation indices stay put)
  kNumParams
};

using namespace iplug;

class FoundationKeys final : public iplug::Plugin
{
public:
  enum class RangeChoice { C2ToB5 = 0, C2ToF6, C2ToB6 };

  FoundationKeys(const InstanceInfo& info);
  ~FoundationKeys() override;

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;
  void ProcessMidiMsg(const IMidiMsg& msg) override;
  void OnReset() override;
  void OnParamChange(int paramIdx) override;
#endif
  void OnIdle() override;
  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;

  // --- UI-thread API used by KeybedControl ---------------------------------------------------------
  // The sound is structured (sa3::sat::keybed::SoundSpec): the sound sheet edits its parts, and
  // typed text is sorted onto them. The descriptor string is always derived from it.
  enum Section { kSectionInstrument = 0, kSectionCharacter, kSectionShape, kSectionFx, kNumSections };
  // Layered keybeds (RC's Main + up to two Supports): Sound()/SetSound() act on the layer open in
  // the sound sheet. Render fx is shared by every layer, so it always reads from and writes to main.
  sa3::sat::keybed::SoundSpec Sound() const;
  void SetSound(sa3::sat::keybed::SoundSpec sound);
  const sa3::sat::keybed::SoundSpec& Layer(int layer) const { return mLayers[(size_t)std::clamp(layer, 0, mLayerCount - 1)]; }
  int LayerCount() const { return mLayerCount; }
  int EditLayer() const { return mEditLayer; }
  void SetEditLayer(int layer) { mEditLayer = std::clamp(layer, 0, mLayerCount - 1); }
  void AddLayer();                 // a new support layer with a rolled sound
  void RemoveLayer(int layer);     // supports only
  std::string LayerSummary() const;   // the supports' descriptors, for the main page
  std::string Descriptor() const;     // main layer
  // Typed or pasted text: sorted onto the controls. Text with no FX or wet/dry words keeps the
  // current render fx.
  void SetDescriptor(const std::string& text);
  // Dice: a new RC-weighted sound for the layer in the sheet; locked sections are kept.
  void RollSound();
  // Main-page dice: every layer, from one base seed (RC's randomize_all_tri_layers).
  void RollAll();
  bool Locked(int section) const { return section >= 0 && section < kNumSections && mLocks[(size_t)section]; }
  void SetLocked(int section, bool locked) { if (section >= 0 && section < kNumSections) mLocks[(size_t)section] = locked; }
  bool Wet() const { return mLayers[0].wet; }
  // Turning wet on with no tag chosen picks the model's most common space, so "wet" always
  // names a sound; the tag can still be cleared to let the model decide.
  void SetWet(bool wet);
  // FX tags a wet prompt carries, one per category; empty lets the model choose the space.
  const std::vector<std::string>& FxTags() const { return mLayers[0].fx; }
  std::string FxLabel() const;
  int Steps() const { return mSteps; }
  void SetSteps(int steps);
  float CfgScale() const { return mCfgScale; }
  void SetCfgScale(float cfg);
  bool UseSeed() const { return mUseSeed; }
  void SetUseSeed(bool use) { mUseSeed = use; }
  int64_t SeedValue() const { return mSeed; }
  void SetSeedValue(int64_t seed) { mSeed = seed < 0 ? 0 : seed; }
  bool HasLastSeed() const { return mHasLastSeed; }
  uint64_t LastSeed() const { return mLastSeed; }
  int PreviewCount() const { return mPreviewCount; }
  void SetPreviewCount(int count) { mPreviewCount = count; }
  int PreviewRootLabel() const { return mPreviewRootLabel; }
  void SetPreviewRootLabel(int midi);
  RangeChoice Range() const { return mRange; }
  void SetRange(RangeChoice range) { mRange = range; }

  std::string ModelsDir() const { return mModelsDir; }
  void SetModelsDir(const std::string& dir);
  std::string Encoding() const { return mEncoding; }
  void SetEncoding(const std::string& encoding);
  bool KeepResident() const { return mKeepResident; }
  void SetKeepResident(bool keep);
  bool ModelsReady(std::string* missing = nullptr) const;
  bool TierPresent(const std::string& encoding) const;   // all three files of a tier in ModelsDir()
  void ReleaseModels();

  // Downloads the selected tier from Hugging Face into ModelsDir(), then selects it.
  bool StartModelDownload();
  void CancelModelDownload() { mDownloader.Cancel(); }
  bool Downloading() const { return mDownloader.Busy(); }
  float DownloadProgress() const { return mDownloader.Progress(); }
  std::string DownloadStatus() const { return mDownloader.Status(); }
  std::string DownloadingTier() const { return mDownloader.Encoding(); }

  bool StartPreview();
  bool StartFullBuild();
  void CancelRender() { mRender.Cancel(); }
  bool Busy() const { return mRender.Busy(); }
  float Progress() const { return mRender.Progress(); }
  std::string StatusText() const;
  bool StatusIsError() const { return mStatusIsError; }
  std::vector<int> ActiveLabels() const { return mRender.ActiveLabels(); }
  // Rough wall-clock for a build of `chunks`, learned from this machine's last renders.
  double EstimatedSeconds(int chunks) const;

  const keybed::BankSnapshot* Bank() const { return mBank.Latest(); }
  bool KeyHeld(int key) const { return mEngine.Held(key); }
  int LastPlayedKey() const { return mEngine.LastKey(); }
  keybed::NoteSamplePtr SampleForKey(int key) const;
  std::string KitDir() const { return mKitDir; }
  std::string KitLabel() const;
  // Reads the kit's WAVs on a background thread; OnIdle installs them. adoptSettings takes the kit's
  // descriptor/seed (a user load); false when restoring saved state.
  void LoadKitFromFolder(const std::string& dir, bool adoptSettings = true);
  bool LoadingKit() const { return mKitLoading.load(std::memory_order_acquire); }
  // Path of the WAV for `key` in the current kit, or empty when it has not been saved.
  std::string NoteFilePath(int key) const;
  void AuditionKey(int key, bool on);

private:
  bool StartJob(std::vector<sa3::sat::keybed::Chunk> chunks, std::string rangeLabel, bool preview);
  void InstallLoadedKit();
  void SetStatus(std::string text, bool error = false);
  void LoadGlobalSettings();

  keybed::KeybedEngine mEngine;
  keybed::KeybedBank mBank;
  keybed::KeybedRenderService mRender;
  keybed::ModelDownloader mDownloader;

  // generation settings (UI thread; persisted in the state chunk)
  std::array<sa3::sat::keybed::SoundSpec, 3> mLayers{
    sa3::sat::keybed::classify_descriptor("Keys, Rhodes Piano, Warm, Soft")};
  int mLayerCount = 1;
  int mEditLayer = 0;
  void ApplyRoll(int layer, const sa3::sat::keybed::SoundSpec& rolled);
  std::string DescriptorBundle() const;   // RC's _descriptor_bundle: every layer's descriptor
  std::array<bool, kNumSections> mLocks{};
  int mSteps = 80;
  float mCfgScale = 6.f;
  bool mUseSeed = false;
  int64_t mSeed = 0;
  bool mHasLastSeed = false;
  uint64_t mLastSeed = 0;
  std::string mLastSeedDescriptor;
  int mPreviewCount = 6;
  int mPreviewRootLabel = 60;   // prompt label C4 (sounds C3)
  RangeChoice mRange = RangeChoice::C2ToB5;

  // global settings (settings.txt)
  std::string mModelsDir;
  std::string mEncoding = "F16";
  bool mKeepResident = true;
  double mChunkSecondsAt80 = 10.0;   // measured seconds per chunk at 80 steps

  // kit
  std::string mKitDir;
  keybed::KitManifest mManifest;
  bool mReplaceBankOnNextNote = false;
  std::string mJobDescriptor;   // descriptor of the running job, remembered with its seed

  struct LoadedKit
  {
    std::string dir;
    bool adoptSettings = true;
    bool ready = false;
    std::vector<keybed::NoteSamplePtr> notes;
    keybed::KitManifest manifest;
    std::string error;
  };
  std::thread mKitLoader;
  std::atomic<bool> mKitLoading{false};
  std::mutex mKitLoadMutex;
  LoadedKit mLoadedKit;   // guarded by mKitLoadMutex

  mutable std::mutex mStatusMutex;
  std::string mStatus = "ready";
  bool mStatusIsError = false;
};
