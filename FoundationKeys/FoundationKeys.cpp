#include "FoundationKeys.h"
#include "IPlug_include_in_plug_src.h"

#if IPLUG_EDITOR
#include "IControls.h"
#include "KeybedControl.h"
#include "SA3UITheme.h"
#endif

#include "sat/keybed.h"
#include "sat/model_paths.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <random>

namespace kb = sa3::sat::keybed;

namespace
{
constexpr uint32_t kStateMagic = 0x53334B42u;   // "S3KB"
constexpr uint32_t kStateVersion = 5u;   // 5: param count; 4: layers; 3: structured sound + locks; 2: FX tags; 1: FX index
// Params each state version saved, for versions that did not store the count.
constexpr int32_t kParamsInV3 = 9;    // gain .. fill gaps
constexpr int32_t kParamsInV4 = 12;   // + three layer volumes
constexpr const char* kVariant = "foundation-1.2-keybeds";

const char* RangeLabel(FoundationKeys::RangeChoice range)
{
  switch (range)
  {
    case FoundationKeys::RangeChoice::C2ToB5: return "C2-B5";
    case FoundationKeys::RangeChoice::C2ToF6: return "C2-F6";
    case FoundationKeys::RangeChoice::C2ToB6: return "C2-B6";
  }
  return "C2-B5";
}
} // namespace

FoundationKeys::FoundationKeys(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  GetParam(kParamGain)->InitDouble("Gain", 100., 0., 100., 0.1, "%");   // on top of the sampler's -12 dB headroom
  GetParam(kParamAttack)->InitDouble("Attack", 5., 0.5, 4000., 0.1, "ms", IParam::kFlagsNone, "Envelope", IParam::ShapePowCurve(3.));
  GetParam(kParamDecay)->InitDouble("Decay", 50., 1., 4000., 0.1, "ms", IParam::kFlagsNone, "Envelope", IParam::ShapePowCurve(3.));
  GetParam(kParamSustain)->InitDouble("Sustain", 100., 0., 100., 0.1, "%", IParam::kFlagsNone, "Envelope");
  GetParam(kParamRelease)->InitDouble("Release", 250., 5., 8000., 0.1, "ms", IParam::kFlagsNone, "Envelope", IParam::ShapePowCurve(3.));
  GetParam(kParamVelocity)->InitDouble("Velocity", 50., 0., 100., 1., "%");
  GetParam(kParamTune)->InitDouble("Tune", 0., -12., 12., 0.01, "st");
  GetParam(kParamOctave)->InitInt("Octave", 0, -2, 2);
  GetParam(kParamFillGaps)->InitBool("Fill Gaps", true);
  // RC's tri-layer mixer; only heard when the kit has support layers.
  static const char* const layerNames[kb::kLayerCount] = {"Main Layer", "Support 1", "Support 2"};
  for (int l = 0; l < kb::kLayerCount; ++l)
    GetParam(kParamLayerMain + l)->InitDouble(layerNames[l], kb::kLayerDefaultVolumes[l] * 100., 0., 100., 0.1, "%",
                                              IParam::kFlagsNone, "Layers");
  GetParam(kParamVoiceMode)->InitEnum("Voice Mode", 0, {"Poly", "Mono"});

  LoadGlobalSettings();

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    const int width = mWideMode ? PLUG_WIDE_WIDTH : PLUG_WIDTH;
    const int height = mWideMode ? PLUG_WIDE_HEIGHT : PLUG_HEIGHT;
    return MakeGraphics(*this, width, height, PLUG_FPS, GetScaleForScreen(width, height));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->SetLayoutOnResize(true);
    if (auto* existing = pGraphics->GetControlWithTag(kKeybedControlTag))
    {
      existing->SetTargetAndDrawRECTs(pGraphics->GetBounds());
      pGraphics->SetQwertyMidiKeyHandlerFunc([this](const IMidiMsg& msg) { SendMidiMsgFromUI(msg); });
      return;
    }
    pGraphics->AttachPanelBackground(COLOR_BLACK);
    pGraphics->EnableMouseOver(true);
    pGraphics->AttachTextEntryControl();
    // No AttachPopupMenuControl: the full-window control below is attached after it and would
    // paint over an in-graphics menu every frame. Without it, CreatePopupMenu uses the OS menu.
    if (!pGraphics->LoadFont(gary::ui::FontName, ROBOTO_FN))
      pGraphics->LoadFont(gary::ui::FontName, "Arial", ETextStyle::Normal);
    pGraphics->AttachControl(new KeybedControl(pGraphics->GetBounds(), *this), kKeybedControlTag);
    pGraphics->SetQwertyMidiKeyHandlerFunc([this](const IMidiMsg& msg) { SendMidiMsgFromUI(msg); });
  };
#endif
}

FoundationKeys::~FoundationKeys()
{
  mRender.Cancel();
  if (mKitLoader.joinable())
    mKitLoader.join();
  FinishKitStorageMove(true);
}

#if IPLUG_DSP
void FoundationKeys::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  mEngine.ProcessBlock(outputs, nFrames, mBank);
}

void FoundationKeys::ProcessMidiMsg(const IMidiMsg& msg)
{
  switch (msg.StatusMsg())
  {
    case IMidiMsg::kNoteOn:
    case IMidiMsg::kNoteOff:
    case IMidiMsg::kControlChange:
    case IMidiMsg::kPitchWheel:
    case IMidiMsg::kPolyAftertouch:
    case IMidiMsg::kChannelAftertouch:
      mEngine.ProcessMidiMsg(msg);
      break;
    default:
      break;
  }
}

void FoundationKeys::OnReset()
{
  mEngine.Reset(GetSampleRate(), GetBlockSize());
}

void FoundationKeys::OnParamChange(int paramIdx)
{
  auto& s = mEngine.settings;
  const double value = GetParam(paramIdx)->Value();
  switch (paramIdx)
  {
    case kParamGain: s.gain.store(value / 100.); break;
    case kParamAttack: s.attackMs.store(value); s.envelopeVersion.fetch_add(1); break;
    case kParamDecay: s.decayMs.store(value); s.envelopeVersion.fetch_add(1); break;
    case kParamRelease: s.releaseMs.store(value); s.envelopeVersion.fetch_add(1); break;
    case kParamSustain: s.sustain.store(value / 100.); break;
    case kParamVelocity: s.velocity.store(value / 100.); break;
    case kParamTune: s.tuneSemitones.store(value); break;
    case kParamOctave: s.octave.store(GetParam(paramIdx)->Int()); break;
    case kParamFillGaps: s.fillGaps.store(GetParam(paramIdx)->Bool()); break;
    case kParamLayerMain:
    case kParamLayerSupport1:
    case kParamLayerSupport2: s.layerVolume[(size_t)(paramIdx - kParamLayerMain)].store(value / 100.); break;
    case kParamVoiceMode: s.mono.store(GetParam(paramIdx)->Int() == 1); break;
    default: break;
  }
}
#endif

void FoundationKeys::OnIdle()
{
  std::vector<keybed::NoteSamplePtr> arrived;
  for (auto& event : mRender.DrainEvents())
  {
    switch (event.kind)
    {
      case keybed::KeybedEvent::Kind::Note:
        arrived.push_back(event.note);
        if (!event.kitDir.empty())
          mKitDir = event.kitDir;
        if (event.seed)
        {
          mLastSeed = event.seed;
          mHasLastSeed = true;
          mLastSeedDescriptor = mJobDescriptor;
        }
        break;
      case keybed::KeybedEvent::Kind::Finished:
      case keybed::KeybedEvent::Kind::Cancelled:
      case keybed::KeybedEvent::Kind::Failed:
      {
        const bool failed = event.kind == keybed::KeybedEvent::Kind::Failed;
        SetStatus(event.message, failed);
        if (!event.kitDir.empty())
        {
          mKitDir = event.kitDir;
          std::string ignored;
          keybed::ReadKitManifest(mKitDir, mManifest, ignored);
        }
        const double chunkSeconds = mRender.LastChunkSeconds();
        if (chunkSeconds > 0.5 && mSteps > 0)
        {
          // Normalize to 80 steps and smooth, so the build estimate tracks this machine.
          const double at80 = chunkSeconds * 80.0 / (double)mSteps;
          mChunkSecondsAt80 = 0.7 * mChunkSecondsAt80 + 0.3 * at80;
          keybed::SaveSetting("chunk_seconds", std::to_string(mChunkSecondsAt80));
        }
        mRender.Collect(mKeepResident);
        break;
      }
    }
  }
  if (!arrived.empty())
  {
    mBank.Publish(arrived, mReplaceBankOnNextNote);
    mReplaceBankOnNextNote = false;
  }
  InstallLoadedKit();
  mBank.CollectGarbage();
  FinishKitStorageMove();
  {
    bool ok = false;
    std::string dir, encoding, message;
    if (mDownloader.TakeResult(ok, dir, encoding, message))
    {
      if (ok)
      {
        SetModelsDir(dir);
        SetEncoding(encoding);
      }
      SetStatus(message, !ok);
    }
  }

#if IPLUG_EDITOR
  if (auto* ui = GetUI())
    if (auto* control = ui->GetControlWithTag(kKeybedControlTag))
      control->SetDirty(false);
#endif
}

#if IPLUG_EDITOR
bool FoundationKeys::OnHostRequestingSupportedViewConfiguration(int width, int height)
{
  return ConstrainEditorResize(width, height);
}

void FoundationKeys::OnHostSelectedViewConfiguration(int width, int height)
{
  mWideMode = width >= (PLUG_WIDTH + PLUG_WIDE_WIDTH) / 2;
  keybed::SaveSetting("wide_mode", mWideMode ? "1" : "0");
  if (GetUI())
    GetUI()->Resize(width, height, GetUI()->GetDrawScale(), true);
}
#endif

kb::SoundSpec FoundationKeys::Sound() const
{
  kb::SoundSpec sound = mLayers[(size_t)mEditLayer];
  sound.wet = mLayers[0].wet;   // render fx is shared: every layer shows main's
  sound.fx = mLayers[0].fx;
  return sound;
}

void FoundationKeys::SetSound(kb::SoundSpec sound)
{
  const int layer = std::clamp(mEditLayer, 0, mLayerCount - 1);
  mLayers[0].wet = sound.wet;
  mLayers[0].fx = sound.fx;
  mLayers[(size_t)layer] = std::move(sound);
  for (int l = 1; l < mLayerCount; ++l)
  {
    mLayers[(size_t)l].wet = mLayers[0].wet;
    mLayers[(size_t)l].fx = mLayers[0].fx;
  }
}

void FoundationKeys::AddLayer()
{
  if (mLayerCount >= kb::kLayerCount)
    return;
  // RC seeds each layer's dice from one base (tri_prompt_n); here the base is fresh each time.
  std::random_device device;
  const uint64_t base = ((uint64_t)device() << 32) ^ device();
  const int layer = mLayerCount++;
  kb::SoundSpec rolled = kb::random_sound(kb::layer_prompt_seed(base, layer), mLayers[0].wet);
  rolled.wet = mLayers[0].wet;
  rolled.fx = mLayers[0].fx;
  mLayers[(size_t)layer] = std::move(rolled);
  mEditLayer = layer;
}

void FoundationKeys::RemoveLayer(int layer)
{
  if (layer < 1 || layer >= mLayerCount)
    return;
  for (int l = layer; l + 1 < mLayerCount; ++l)
    mLayers[(size_t)l] = mLayers[(size_t)l + 1];
  --mLayerCount;
  mLayers[(size_t)mLayerCount] = kb::SoundSpec();
  mEditLayer = std::min(mEditLayer, mLayerCount - 1);
}

std::string FoundationKeys::LayerSummary() const
{
  // Short enough for one line with both supports: "support 1: Pad, Rich  ·  support 2: Cello, Warm".
  std::string summary;
  for (int l = 1; l < mLayerCount; ++l)
  {
    const kb::SoundSpec& sound = mLayers[(size_t)l];
    std::string name = !sound.subfamily.empty() ? sound.subfamily : sound.family;
    if (!sound.character.empty())
      name += (name.empty() ? "" : ", ") + sound.character.front();
    if (name.empty())
      name = kb::descriptor_of(sound);
    summary += (summary.empty() ? "" : "  ·  ") + std::string(kb::kLayerRoles[l]) + ": " + name;
  }
  return summary;
}

std::string FoundationKeys::Descriptor() const
{
  return kb::descriptor_of(mLayers[0]);
}

std::string FoundationKeys::DescriptorBundle() const
{
  std::string bundle;
  for (int l = 0; l < mLayerCount; ++l)
    bundle += (l ? " || " : "") + kb::descriptor_of(mLayers[(size_t)l]);
  return bundle;
}

void FoundationKeys::SetDescriptor(const std::string& text)
{
  // The main page's text field names the main layer.
  kb::SoundSpec sorted = kb::classify_descriptor(text);
  // Text that says nothing about the space keeps the current render fx.
  bool mentionsSpace = !kb::split_descriptor_tokens(text).fx.empty();
  for (const std::string& raw : kb::detail::split_commas(text))
  {
    const std::string low = kb::detail::lower(raw);
    mentionsSpace = mentionsSpace || low == "wet" || low == "dry";
  }
  if (!mentionsSpace)
  {
    sorted.wet = mLayers[0].wet;
    sorted.fx = mLayers[0].fx;
  }
  const int editing = mEditLayer;
  mEditLayer = 0;
  SetSound(std::move(sorted));
  mEditLayer = editing;
}

std::string FoundationKeys::FxLabel() const
{
  std::string label;
  for (const auto& tag : mLayers[0].fx)
    label += (label.empty() ? "" : " + ") + tag;
  return label;
}

void FoundationKeys::ApplyRoll(int layer, const kb::SoundSpec& rolled)
{
  kb::SoundSpec& sound = mLayers[(size_t)layer];
  if (!mLocks[kSectionInstrument])
  {
    sound.family = rolled.family;
    sound.subfamily = rolled.subfamily;
    sound.second_instrument.clear();
  }
  if (!mLocks[kSectionCharacter])
    sound.character = rolled.character;
  if (!mLocks[kSectionShape])
  {
    sound.articulation = rolled.articulation;
    sound.oscillator = rolled.oscillator;
  }
  if (layer == 0 && !mLocks[kSectionFx] && mLayers[0].wet)
    mLayers[0].fx = rolled.fx;   // RC's one-or-two-tag chains, shared by every layer
  sound.wet = mLayers[0].wet;
  sound.fx = mLayers[0].fx;
}

void FoundationKeys::RollSound()
{
  std::random_device device;
  const uint64_t seed = ((uint64_t)device() << 32) ^ device();
  ApplyRoll(mEditLayer, kb::random_sound(seed, mLayers[0].wet));
  if (mEditLayer == 0)
    for (int l = 1; l < mLayerCount; ++l)
      mLayers[(size_t)l].fx = mLayers[0].fx;
}

void FoundationKeys::RollAll()
{
  // RC's randomize_all_tri_layers: one base, and each layer rolls from its tri_prompt_n seed.
  std::random_device device;
  const uint64_t base = ((uint64_t)device() << 32) ^ device();
  for (int l = 0; l < mLayerCount; ++l)
    ApplyRoll(l, kb::random_sound(mLayerCount > 1 ? kb::layer_prompt_seed(base, l) : base, mLayers[0].wet));
  for (int l = 1; l < mLayerCount; ++l)
    mLayers[(size_t)l].fx = mLayers[0].fx;
}

void FoundationKeys::SetSteps(int steps)
{
  mSteps = std::clamp(steps, 2, 250);
}

void FoundationKeys::SetCfgScale(float cfg)
{
  mCfgScale = std::clamp(cfg, 1.f, 12.f);
}

void FoundationKeys::SetPreviewRootLabel(int midi)
{
  mPreviewRootLabel = kb::clamp_preview_root(midi, mPreviewCount);
}

void FoundationKeys::LoadGlobalSettings()
{
  const std::string encoding = keybed::LoadSetting("encoding");
  if (!encoding.empty())
    mEncoding = encoding;
  mModelsDir = keybed::MigratedPath(keybed::LoadSetting("models_dir"));
  mKitsDir = keybed::KitsDirectory();
  const std::string audioFormat = keybed::LoadSetting("kit_audio_format");
  mKitAudioFormat = audioFormat == "wav" ? keybed::AudioFormat::Wav : keybed::AudioFormat::Flac;
  mWideMode = keybed::LoadSetting("wide_mode") == "1";
  if (mModelsDir.empty())
  {
    // An explicit FOUNDATION_KEYS_MODELS_DIR, then (dev builds) the sa3.cpp checkout's staged models;
    // otherwise the per-user models folder the settings page downloads into.
    const char* env = std::getenv("FOUNDATION_KEYS_MODELS_DIR");
    if (env && *env)
      mModelsDir = env;
#ifdef FOUNDATION_KEYS_DEV_MODELS_DIR
    if (mModelsDir.empty() || !ModelsReady())
      mModelsDir = FOUNDATION_KEYS_DEV_MODELS_DIR;
#endif
    if (mModelsDir.empty() || !ModelsReady())
      mModelsDir = keybed::DefaultModelsDirectory();
  }
  const std::string resident = keybed::LoadSetting("keep_resident");
  if (!resident.empty())
    mKeepResident = resident != "0";
  const std::string chunkSeconds = keybed::LoadSetting("chunk_seconds");
  if (!chunkSeconds.empty())
    mChunkSecondsAt80 = std::clamp(std::strtod(chunkSeconds.c_str(), nullptr), 1.0, 600.0);
}

void FoundationKeys::SetModelsDir(const std::string& dir)
{
  if (dir == mModelsDir)
    return;
  mModelsDir = dir;
  keybed::SaveSetting("models_dir", dir);
  mRender.ReleaseModels();
}

void FoundationKeys::SetKitsDir(const std::string& dir)
{
  if (dir.empty() || KitStorageMoving())
    return;
  if (mKitStorageMover.joinable())
    FinishKitStorageMove();
  if (dir == mKitsDir)
    return;
  if (Busy())
  {
    SetStatus("finish the current build before changing kit storage", true);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mKitStorageMutex);
    mKitStorageMoveReady = false;
    mKitStorageMoveSucceeded = false;
    mKitStorageMoveDestination = dir;
    mKitStorageMoveError.clear();
  }
  const std::string source = mKitsDir;
  mKitStorageMoving.store(true, std::memory_order_release);
  mKitStorageMover = std::thread([this, source, dir]() {
    bool ok = false;
    std::string error;
    try
    {
      ok = keybed::CopyKitsDirectory(source, dir, error);
    }
    catch (const std::exception& e)
    {
      error = e.what();
    }
    catch (...)
    {
      error = "unexpected error while copying kits";
    }
    {
      std::lock_guard<std::mutex> lock(mKitStorageMutex);
      mKitStorageMoveSucceeded = ok;
      mKitStorageMoveError = std::move(error);
      mKitStorageMoveReady = true;
    }
    mKitStorageMoving.store(false, std::memory_order_release);
  });
  SetStatus("copying existing kits to the selected folder");
}

void FoundationKeys::FinishKitStorageMove(bool waitForCompletion)
{
  if (!mKitStorageMover.joinable())
    return;
  if (waitForCompletion)
    mKitStorageMover.join();
  else if (KitStorageMoving())
    return;
  else
    mKitStorageMover.join();
  bool ready = false;
  bool succeeded = false;
  std::string destination, error;
  {
    std::lock_guard<std::mutex> lock(mKitStorageMutex);
    ready = mKitStorageMoveReady;
    succeeded = mKitStorageMoveSucceeded;
    destination = mKitStorageMoveDestination;
    error = mKitStorageMoveError;
    mKitStorageMoveReady = false;
  }
  if (!ready)
    return;
  if (succeeded && keybed::SaveSetting("kits_dir", destination))
  {
    mKitsDir = destination;
    SetStatus("kit storage updated; the previous folder was kept as a backup");
  }
  else
  {
    if (succeeded)
      error = "kits were copied, but the new location could not be saved";
    SetStatus(error.empty() ? "could not change kit storage" : error, true);
  }
}

void FoundationKeys::SetKitAudioFormat(keybed::AudioFormat format)
{
  if (mKitAudioFormat == format)
    return;
  mKitAudioFormat = format;
  keybed::SaveSetting("kit_audio_format", format == keybed::AudioFormat::Flac ? "flac" : "wav");
}

void FoundationKeys::SetWideMode(bool wide)
{
  if (mWideMode == wide)
    return;
  mWideMode = wide;
  keybed::SaveSetting("wide_mode", wide ? "1" : "0");
#if IPLUG_EDITOR
  if (GetUI())
    GetUI()->Resize(wide ? PLUG_WIDE_WIDTH : PLUG_WIDTH,
                    wide ? PLUG_WIDE_HEIGHT : PLUG_HEIGHT,
                    GetUI()->GetDrawScale(), true);
#endif
}

void FoundationKeys::SetEncoding(const std::string& encoding)
{
  mEncoding = sa3::sat::normalize_encoding(encoding);
  keybed::SaveSetting("encoding", mEncoding);
}

void FoundationKeys::SetKeepResident(bool keep)
{
  mKeepResident = keep;
  keybed::SaveSetting("keep_resident", keep ? "1" : "0");
  if (!keep)
    mRender.ReleaseModels();
}

bool FoundationKeys::ModelsReady(std::string* missing) const
{
  sa3::sat::PipelinePaths paths;
  std::string error;
  const bool ok = sa3::sat::resolve_sat_large_model(mModelsDir, kVariant, mEncoding, mEncoding, mEncoding, &paths, &error);
  if (!ok && missing)
    *missing = error;
  return ok;
}

bool FoundationKeys::TierPresent(const std::string& encoding) const
{
  sa3::sat::PipelinePaths paths;
  return sa3::sat::resolve_sat_large_model(mModelsDir, kVariant, encoding, encoding, encoding, &paths, nullptr);
}

bool FoundationKeys::StartModelDownload()
{
  std::string dir = mModelsDir;
  if (dir.empty())
    dir = keybed::DefaultModelsDirectory();
  std::string error;
  if (!mDownloader.Start(dir, mEncoding, error))
  {
    SetStatus(error, true);
    return false;
  }
  return true;
}

bool FoundationKeys::StartPreview()
{
  const int root = kb::clamp_preview_root(mPreviewRootLabel, mPreviewCount);
  std::vector<kb::Chunk> chunks = kb::plan_preview(root, mPreviewCount);
  std::string label = "preview " + kb::midi_to_note_name(kb::label_to_sounding_midi(root)) + " x" +
                      std::to_string(mPreviewCount);
  return StartJob(std::move(chunks), std::move(label), true);
}

bool FoundationKeys::StartFullBuild()
{
  kb::FullRange range = kb::FullRange::C2ToB5;
  if (mRange == RangeChoice::C2ToF6) range = kb::FullRange::C2ToF6;
  if (mRange == RangeChoice::C2ToB6) range = kb::FullRange::C2ToB6;
  return StartJob(kb::plan_full_range(range), RangeLabel(mRange), false);
}

bool FoundationKeys::StartJob(std::vector<kb::Chunk> chunks, std::string rangeLabel, bool preview)
{
  if (KitStorageMoving())
  {
    SetStatus("wait for kit storage copy to finish", true);
    return false;
  }
  std::string missing;
  if (!ModelsReady(&missing))
  {
    SetStatus(missing, true);
    return false;
  }
  keybed::KeybedJob job;
  job.modelsDir = mModelsDir;
  job.variant = kVariant;
  job.encoding = mEncoding;
  for (int l = 0; l < mLayerCount; ++l)
    job.layers.push_back(kb::descriptor_of(mLayers[(size_t)l]));
  job.wet = mLayers[0].wet;
  job.fx = kb::fx_of(mLayers[0]);
  job.chunks = std::move(chunks);
  job.rangeLabel = std::move(rangeLabel);
  job.kitsDirectory = mKitsDir;
  job.audioFormat = mKitAudioFormat;
  job.steps = mSteps;
  job.cfgScale = mCfgScale;
  job.keepResident = mKeepResident;
  // RoyalCities' flow: a full build after a preview of the same descriptor keeps the preview's seed.
  if (mUseSeed)
    job.seed = mSeed;
  else if (!preview && mHasLastSeed && mLastSeedDescriptor == DescriptorBundle())
    job.seed = (int64_t)(mLastSeed & 0x7fffffffffffffffull);
  else
    job.seed = -1;

  std::string error;
  if (!mRender.Start(std::move(job), error))
  {
    SetStatus(error, true);
    return false;
  }
  mJobDescriptor = DescriptorBundle();
  mReplaceBankOnNextNote = true;
  {
    std::lock_guard<std::mutex> lock(mKitLoadMutex);   // a render supersedes a kit still loading
    mLoadedKit = LoadedKit();
  }
  SetStatus(preview ? "rendering preview" : "building keyboard");
  return true;
}

double FoundationKeys::EstimatedSeconds(int chunks) const
{
  return std::max(0, chunks) * mChunkSecondsAt80 * (double)mSteps / 80.0;
}

std::string FoundationKeys::StatusText() const
{
  if (mRender.Busy())
    return mRender.Status();
  std::lock_guard<std::mutex> lock(mStatusMutex);
  return mStatus;
}

void FoundationKeys::SetStatus(std::string text, bool error)
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  mStatus = std::move(text);
  mStatusIsError = error;
}

keybed::NoteSamplePtr FoundationKeys::SampleForKey(int key) const
{
  const keybed::BankSnapshot* bank = mBank.Latest();
  if (!bank || key < 0 || key > 127)
    return nullptr;
  return bank->exact[(size_t)key];
}

std::string FoundationKeys::KitLabel() const
{
  if (mKitDir.empty())
    return "no kit yet";
  return keybed::FolderName(mKitDir);
}

void FoundationKeys::ReleaseModels()
{
  if (mRender.Busy())
    return;
  mRender.ReleaseModels();
  SetStatus("models released");
}

void FoundationKeys::LoadKitFromFolder(const std::string& dir, bool adoptSettings)
{
  if (mKitLoader.joinable())
    mKitLoader.join();
  {
    std::lock_guard<std::mutex> lock(mKitLoadMutex);
    mLoadedKit = LoadedKit();
    mLoadedKit.dir = dir;
    mLoadedKit.adoptSettings = adoptSettings;
  }
  mKitLoading.store(true, std::memory_order_release);
  SetStatus("loading kit " + keybed::FolderName(dir));
  mKitLoader = std::thread([this, dir]() {
    keybed::KitManifest manifest;
    std::string error;
    std::vector<keybed::NoteSamplePtr> notes = keybed::LoadKitSamples(dir, manifest, error);
    {
      std::lock_guard<std::mutex> lock(mKitLoadMutex);
      if (mLoadedKit.dir == dir)   // not superseded by a render or another load
      {
        mLoadedKit.notes = std::move(notes);
        mLoadedKit.manifest = std::move(manifest);
        mLoadedKit.error = std::move(error);
        mLoadedKit.ready = true;
      }
    }
    mKitLoading.store(false, std::memory_order_release);
  });
}

void FoundationKeys::InstallLoadedKit()
{
  LoadedKit kit;
  {
    std::lock_guard<std::mutex> lock(mKitLoadMutex);
    if (!mLoadedKit.ready)
      return;
    kit = std::move(mLoadedKit);
    mLoadedKit = LoadedKit();
  }
  if (kit.notes.empty())
  {
    SetStatus(kit.error.empty() ? "no samples in that folder" : kit.error, true);
    return;
  }
  mBank.Publish(kit.notes, true);
  mKitDir = kit.dir;
  mManifest = kit.manifest;
  if (kit.adoptSettings && !kit.manifest.descriptor.empty())
  {
    std::vector<std::string> descriptors = kit.manifest.layerDescriptors;
    if (descriptors.size() < 2)
      descriptors = {kit.manifest.descriptor};
    mLayerCount = std::min((int)descriptors.size(), kb::kLayerCount);
    mEditLayer = 0;
    for (int l = 0; l < kb::kLayerCount; ++l)
    {
      mLayers[(size_t)l] = l < mLayerCount ? kb::classify_descriptor(descriptors[(size_t)l]) : kb::SoundSpec();
      mLayers[(size_t)l].wet = kit.manifest.wet;
      mLayers[(size_t)l].fx.clear();
      for (const auto& tag : kit.manifest.fx)
        kb::set_fx(mLayers[(size_t)l], tag);
    }
  }
  if (kit.adoptSettings && kit.manifest.seed)
  {
    mLastSeed = kit.manifest.seed;   // a layered kit's top-level seed is the base seed
    mHasLastSeed = true;
    mLastSeedDescriptor = DescriptorBundle();
  }
  SetStatus("loaded " + std::to_string(kit.notes.size()) + " notes" + (kit.error.empty() ? "" : " (" + kit.error + ")"),
            !kit.error.empty());
}

std::string FoundationKeys::NoteFilePath(int key) const
{
  const keybed::NoteSamplePtr note = key >= 0 && key <= 127 ? SampleForKey(key) : nullptr;
  if (mKitDir.empty() || !note)
    return {};
  std::filesystem::path path = std::filesystem::u8path(mKitDir);
  std::error_code ec;
  // A layered kit keeps each layer's WAVs in its own folder.
  const std::filesystem::path layerDir = path / kb::kLayerDirNames[std::clamp(note->layer, 0, kb::kLayerCount - 1)];
  if (std::filesystem::is_directory(layerDir, ec))
    path = layerDir;
  for (keybed::AudioFormat format : {keybed::AudioFormat::Flac, keybed::AudioFormat::Wav})
  {
    const std::filesystem::path candidate = path / keybed::NoteFileName(key, format);
    if (std::filesystem::is_regular_file(candidate, ec))
      return candidate.u8string();
  }
  return {};
}

void FoundationKeys::AuditionKey(int key, bool on)
{
  if (key < 0 || key > 127)
    return;
  IMidiMsg msg;
  if (on)
    msg.MakeNoteOnMsg(key, 100, 0);
  else
    msg.MakeNoteOffMsg(key, 0);
  SendMidiMsgFromUI(msg);
}

bool FoundationKeys::SerializeState(IByteChunk& chunk) const
{
  chunk.Put(&kStateMagic);
  chunk.Put(&kStateVersion);
  const auto putList = [&chunk](const std::vector<std::string>& items) {
    const int32_t count = (int32_t)items.size();
    chunk.Put(&count);
    for (const auto& item : items)
      chunk.PutStr(item.c_str());
  };
  const int32_t layerCount = mLayerCount;
  chunk.Put(&layerCount);
  for (int l = 0; l < mLayerCount; ++l)
  {
    const kb::SoundSpec& sound = mLayers[(size_t)l];
    chunk.PutStr(sound.family.c_str());
    chunk.PutStr(sound.subfamily.c_str());
    chunk.PutStr(sound.second_instrument.c_str());
    putList(sound.character);
    chunk.PutStr(sound.articulation.c_str());
    chunk.PutStr(sound.oscillator.c_str());
    putList(sound.extras);
  }
  const int32_t wet = mLayers[0].wet ? 1 : 0;
  chunk.Put(&wet);
  putList(mLayers[0].fx);
  int32_t locks = 0;
  for (int i = 0; i < kNumSections; ++i)
    locks |= mLocks[(size_t)i] ? (1 << i) : 0;
  chunk.Put(&locks);
  const int32_t useSeed = mUseSeed ? 1 : 0, hasLast = mHasLastSeed ? 1 : 0;
  const int32_t steps = mSteps, previewCount = mPreviewCount, root = mPreviewRootLabel,
                range = (int32_t)mRange;
  chunk.Put(&steps);
  chunk.Put(&mCfgScale);
  chunk.Put(&useSeed);
  chunk.Put(&mSeed);
  chunk.Put(&hasLast);
  chunk.Put(&mLastSeed);
  chunk.PutStr(mLastSeedDescriptor.c_str());
  chunk.Put(&previewCount);
  chunk.Put(&root);
  chunk.Put(&range);
  chunk.PutStr(mKitDir.c_str());
  const int32_t paramCount = NParams();
  chunk.Put(&paramCount);
  return SerializeParams(chunk);
}

int FoundationKeys::UnserializeState(const IByteChunk& chunk, int startPos)
{
  uint32_t magic = 0, version = 0;
  int pos = chunk.Get(&magic, startPos);
  if (pos < 0 || magic != kStateMagic)
    return UnserializeParams(chunk, startPos);   // params-only state from a host default
  pos = chunk.Get(&version, pos);
  WDL_String text;
  int32_t wet = 0, fx = -1, steps = 80, useSeed = 0, hasLast = 0, previewCount = 6, root = 60, range = 0;
  float cfg = 6.f;
  int64_t seed = 0;
  uint64_t lastSeed = 0;
  const auto getString = [&](std::string& out) {
    pos = chunk.GetStr(text, pos);
    out = text.Get();
  };
  const auto getList = [&](std::vector<std::string>& out) {
    int32_t count = 0;
    pos = chunk.Get(&count, pos);
    out.clear();
    for (int32_t i = 0; i < std::clamp<int32_t>(count, 0, 32) && pos >= 0; ++i)
    {
      pos = chunk.GetStr(text, pos);
      out.emplace_back(text.Get());
    }
  };
  std::array<kb::SoundSpec, kb::kLayerCount> layers;
  int32_t layerCount = 1;
  kb::SoundSpec& sound = layers[0];
  int32_t locks = 0;
  if (version >= 3u)
  {
    if (version >= 4u)
      pos = chunk.Get(&layerCount, pos);
    layerCount = std::clamp<int32_t>(layerCount, 1, kb::kLayerCount);
    for (int32_t l = 0; l < layerCount; ++l)
    {
      kb::SoundSpec& layer = layers[(size_t)l];
      getString(layer.family);
      getString(layer.subfamily);
      getString(layer.second_instrument);
      getList(layer.character);
      getString(layer.articulation);
      getString(layer.oscillator);
      getList(layer.extras);
    }
    pos = chunk.Get(&wet, pos);
    std::vector<std::string> fxTags;
    getList(fxTags);
    for (int32_t l = 0; l < layerCount; ++l)
    {
      layers[(size_t)l].wet = wet != 0;
      for (const auto& tag : fxTags)
        kb::set_fx(layers[(size_t)l], tag);
    }
    pos = chunk.Get(&locks, pos);
  }
  else
  {
    // Versions 1-2 stored the descriptor as text; sort it onto the controls.
    std::string descriptor;
    getString(descriptor);
    sound = kb::classify_descriptor(descriptor);
    pos = chunk.Get(&wet, pos);
    sound.wet = wet != 0;
    std::vector<std::string> fxTags;
    if (version >= 2u)
      getList(fxTags);
    else
    {
      pos = chunk.Get(&fx, pos);   // version 1 stored one index into the FX choices
      const auto& choices = kb::vocab::fx_choices();
      if (fx >= 0 && fx < (int32_t)choices.size())
        fxTags.push_back(choices[(size_t)fx]);
    }
    sound.fx.clear();
    for (const auto& tag : fxTags)
      kb::set_fx(sound, tag);
  }
  pos = chunk.Get(&steps, pos);
  pos = chunk.Get(&cfg, pos);
  pos = chunk.Get(&useSeed, pos);
  pos = chunk.Get(&seed, pos);
  pos = chunk.Get(&hasLast, pos);
  pos = chunk.Get(&lastSeed, pos);
  pos = chunk.GetStr(text, pos);
  mLastSeedDescriptor = text.Get();
  pos = chunk.Get(&previewCount, pos);
  pos = chunk.Get(&root, pos);
  pos = chunk.Get(&range, pos);
  pos = chunk.GetStr(text, pos);
  const std::string kitDir = keybed::MigratedPath(text.Get());
  int32_t paramCount = version >= 4u ? kParamsInV4 : kParamsInV3;
  if (version >= 5u)
    pos = chunk.Get(&paramCount, pos);
  if (pos < 0)
    return pos;

  mLayers = std::move(layers);
  mLayerCount = layerCount;
  mEditLayer = 0;
  for (int i = 0; i < kNumSections; ++i)
    mLocks[(size_t)i] = (locks >> i) & 1;
  SetSteps(steps);
  SetCfgScale(cfg);
  mUseSeed = useSeed != 0;
  mSeed = std::max<int64_t>(0, seed);
  mHasLastSeed = hasLast != 0;
  mLastSeed = lastSeed;
  mPreviewCount = (previewCount == 12 || previewCount == 24) ? previewCount : 6;
  mPreviewRootLabel = kb::clamp_preview_root(root, mPreviewCount);
  mRange = (RangeChoice)std::clamp<int32_t>(range, 0, 2);

  if (!kitDir.empty() && kitDir != mKitDir)
  {
    std::error_code ec;
    if (std::filesystem::is_directory(std::filesystem::u8path(kitDir), ec))
      LoadKitFromFolder(kitDir, false);
    else
      SetStatus("saved kit folder is missing: " + kitDir, true);
    mKitDir = kitDir;
  }
  // Read exactly the params that were saved: params added since keep their defaults, and an older
  // project does not read past its end (which iPlug's UnserializeParams reports as a failed load).
  ENTER_PARAMS_MUTEX
  for (int32_t i = 0; i < paramCount && pos >= 0; ++i)
  {
    double value = 0.;
    pos = chunk.Get(&value, pos);
    if (pos >= 0 && i < NParams())
      GetParam(i)->Set(value);
  }
  OnParamReset(kPresetRecall);
  LEAVE_PARAMS_MUTEX
  return pos;
}
