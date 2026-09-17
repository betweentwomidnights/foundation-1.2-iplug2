#include "SA3Keybed.h"
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
#include <filesystem>
#include <random>

#ifndef SA3_KEYBED_DEFAULT_MODELS_DIR
#define SA3_KEYBED_DEFAULT_MODELS_DIR "models"
#endif

namespace kb = sa3::sat::keybed;

namespace
{
constexpr uint32_t kStateMagic = 0x53334B42u;   // "S3KB"
constexpr uint32_t kStateVersion = 1u;
constexpr const char* kVariant = "foundation-1.2-keybeds";

const char* RangeLabel(SA3Keybed::RangeChoice range)
{
  switch (range)
  {
    case SA3Keybed::RangeChoice::C2ToB5: return "C2-B5";
    case SA3Keybed::RangeChoice::C2ToF6: return "C2-F6";
    case SA3Keybed::RangeChoice::C2ToB6: return "C2-B6";
  }
  return "C2-B5";
}
} // namespace

SA3Keybed::SA3Keybed(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  GetParam(kParamGain)->InitDouble("Gain", 80., 0., 100., 0.1, "%");
  GetParam(kParamAttack)->InitDouble("Attack", 5., 0.5, 4000., 0.1, "ms", IParam::kFlagsNone, "Envelope", IParam::ShapePowCurve(3.));
  GetParam(kParamDecay)->InitDouble("Decay", 50., 1., 4000., 0.1, "ms", IParam::kFlagsNone, "Envelope", IParam::ShapePowCurve(3.));
  GetParam(kParamSustain)->InitDouble("Sustain", 100., 0., 100., 0.1, "%", IParam::kFlagsNone, "Envelope");
  GetParam(kParamRelease)->InitDouble("Release", 250., 5., 8000., 0.1, "ms", IParam::kFlagsNone, "Envelope", IParam::ShapePowCurve(3.));
  GetParam(kParamVelocity)->InitDouble("Velocity", 50., 0., 100., 1., "%");
  GetParam(kParamTune)->InitDouble("Tune", 0., -12., 12., 0.01, "st");
  GetParam(kParamOctave)->InitInt("Octave", 0, -2, 2);
  GetParam(kParamFillGaps)->InitBool("Fill Gaps", true);

  LoadGlobalSettings();

#if IPLUG_EDITOR
  mMakeGraphicsFunc = [&]() {
    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT));
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachPanelBackground(COLOR_BLACK);
    pGraphics->EnableMouseOver(true);
    pGraphics->AttachTextEntryControl();
    pGraphics->AttachPopupMenuControl();
    if (!pGraphics->LoadFont(gary::ui::FontName, ROBOTO_FN))
      pGraphics->LoadFont(gary::ui::FontName, "Arial", ETextStyle::Normal);
    pGraphics->AttachControl(new KeybedControl(pGraphics->GetBounds(), *this), kKeybedControlTag);
    pGraphics->SetQwertyMidiKeyHandlerFunc([this](const IMidiMsg& msg) { SendMidiMsgFromUI(msg); });
  };
#endif
}

SA3Keybed::~SA3Keybed()
{
  mRender.Cancel();
  if (mKitLoader.joinable())
    mKitLoader.join();
}

#if IPLUG_DSP
void SA3Keybed::ProcessBlock(sample** inputs, sample** outputs, int nFrames)
{
  mEngine.ProcessBlock(outputs, nFrames, mBank);
}

void SA3Keybed::ProcessMidiMsg(const IMidiMsg& msg)
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

void SA3Keybed::OnReset()
{
  mEngine.Reset(GetSampleRate(), GetBlockSize());
}

void SA3Keybed::OnParamChange(int paramIdx)
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
    default: break;
  }
}
#endif

void SA3Keybed::OnIdle()
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

#if IPLUG_EDITOR
  if (auto* ui = GetUI())
    if (auto* control = ui->GetControlWithTag(kKeybedControlTag))
      control->SetDirty(false);
#endif
}

std::string SA3Keybed::Descriptor() const
{
  return mDescriptor;
}

void SA3Keybed::SetDescriptor(const std::string& text)
{
  const kb::DescriptorTokens tokens = kb::split_descriptor_tokens(text);
  std::vector<std::string> parts = tokens.body;
  // FX words typed into the descriptor imply Wet; keep them so they reach the prompt.
  if (!tokens.fx.empty())
  {
    parts.insert(parts.end(), tokens.fx.begin(), tokens.fx.end());
    mWet = true;
  }
  std::string joined;
  for (const auto& part : parts)
    joined += (joined.empty() ? "" : ", ") + part;
  mDescriptor = joined;
}

void SA3Keybed::RollDescriptor()
{
  std::random_device device;
  const uint64_t seed = ((uint64_t)device() << 32) ^ device();
  mDescriptor = kb::random_descriptor(seed, mWet).descriptor;
}

void SA3Keybed::SetSteps(int steps)
{
  mSteps = std::clamp(steps, 2, 250);
}

void SA3Keybed::SetCfgScale(float cfg)
{
  mCfgScale = std::clamp(cfg, 1.f, 12.f);
}

void SA3Keybed::SetPreviewRootLabel(int midi)
{
  mPreviewRootLabel = kb::clamp_preview_root(midi, mPreviewCount);
}

void SA3Keybed::LoadGlobalSettings()
{
  mModelsDir = keybed::LoadSetting("models_dir");
  if (mModelsDir.empty())
  {
    const char* env = std::getenv("SA3_KEYBED_MODELS_DIR");
    mModelsDir = env && *env ? env : SA3_KEYBED_DEFAULT_MODELS_DIR;
  }
  const std::string encoding = keybed::LoadSetting("encoding");
  if (!encoding.empty())
    mEncoding = encoding;
  const std::string resident = keybed::LoadSetting("keep_resident");
  if (!resident.empty())
    mKeepResident = resident != "0";
  const std::string chunkSeconds = keybed::LoadSetting("chunk_seconds");
  if (!chunkSeconds.empty())
    mChunkSecondsAt80 = std::clamp(std::strtod(chunkSeconds.c_str(), nullptr), 1.0, 600.0);
}

void SA3Keybed::SetModelsDir(const std::string& dir)
{
  if (dir == mModelsDir)
    return;
  mModelsDir = dir;
  keybed::SaveSetting("models_dir", dir);
  mRender.ReleaseModels();
}

void SA3Keybed::SetEncoding(const std::string& encoding)
{
  mEncoding = sa3::sat::normalize_encoding(encoding);
  keybed::SaveSetting("encoding", mEncoding);
}

void SA3Keybed::SetKeepResident(bool keep)
{
  mKeepResident = keep;
  keybed::SaveSetting("keep_resident", keep ? "1" : "0");
  if (!keep)
    mRender.ReleaseModels();
}

bool SA3Keybed::ModelsReady(std::string* missing) const
{
  sa3::sat::PipelinePaths paths;
  std::string error;
  const bool ok = sa3::sat::resolve_sat_large_model(mModelsDir, kVariant, mEncoding, mEncoding, mEncoding, &paths, &error);
  if (!ok && missing)
    *missing = error;
  return ok;
}

bool SA3Keybed::StartPreview()
{
  const int root = kb::clamp_preview_root(mPreviewRootLabel, mPreviewCount);
  std::vector<kb::Chunk> chunks = kb::plan_preview(root, mPreviewCount);
  std::string label = "preview " + kb::midi_to_note_name(kb::label_to_sounding_midi(root)) + " x" +
                      std::to_string(mPreviewCount);
  return StartJob(std::move(chunks), std::move(label), true);
}

bool SA3Keybed::StartFullBuild()
{
  kb::FullRange range = kb::FullRange::C2ToB5;
  if (mRange == RangeChoice::C2ToF6) range = kb::FullRange::C2ToF6;
  if (mRange == RangeChoice::C2ToB6) range = kb::FullRange::C2ToB6;
  return StartJob(kb::plan_full_range(range), RangeLabel(mRange), false);
}

bool SA3Keybed::StartJob(std::vector<kb::Chunk> chunks, std::string rangeLabel, bool preview)
{
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
  job.descriptor = mDescriptor;
  job.wet = mWet;
  if (mWet && mFxIndex >= 0 && mFxIndex < (int)kb::vocab::fx_choices().size())
    job.fx.push_back(kb::vocab::fx_choices()[(size_t)mFxIndex]);
  job.chunks = std::move(chunks);
  job.rangeLabel = std::move(rangeLabel);
  job.steps = mSteps;
  job.cfgScale = mCfgScale;
  job.keepResident = mKeepResident;
  // RoyalCities' flow: a full build after a preview of the same descriptor keeps the preview's seed.
  if (mUseSeed)
    job.seed = mSeed;
  else if (!preview && mHasLastSeed && mLastSeedDescriptor == mDescriptor)
    job.seed = (int64_t)(mLastSeed & 0x7fffffffffffffffull);
  else
    job.seed = -1;

  std::string error;
  if (!mRender.Start(std::move(job), error))
  {
    SetStatus(error, true);
    return false;
  }
  mJobDescriptor = mDescriptor;
  mReplaceBankOnNextNote = true;
  {
    std::lock_guard<std::mutex> lock(mKitLoadMutex);   // a render supersedes a kit still loading
    mLoadedKit = LoadedKit();
  }
  SetStatus(preview ? "rendering preview" : "building keyboard");
  return true;
}

double SA3Keybed::EstimatedSeconds(int chunks) const
{
  return std::max(0, chunks) * mChunkSecondsAt80 * (double)mSteps / 80.0;
}

std::string SA3Keybed::StatusText() const
{
  if (mRender.Busy())
    return mRender.Status();
  std::lock_guard<std::mutex> lock(mStatusMutex);
  return mStatus;
}

void SA3Keybed::SetStatus(std::string text, bool error)
{
  std::lock_guard<std::mutex> lock(mStatusMutex);
  mStatus = std::move(text);
  mStatusIsError = error;
}

keybed::NoteSamplePtr SA3Keybed::SampleForKey(int key) const
{
  const keybed::BankSnapshot* bank = mBank.Latest();
  if (!bank || key < 0 || key > 127)
    return nullptr;
  return bank->exact[(size_t)key];
}

std::string SA3Keybed::KitLabel() const
{
  if (mKitDir.empty())
    return "no kit yet";
  return keybed::FolderName(mKitDir);
}

void SA3Keybed::ReleaseModels()
{
  if (mRender.Busy())
    return;
  mRender.ReleaseModels();
  SetStatus("models released");
}

void SA3Keybed::LoadKitFromFolder(const std::string& dir, bool adoptSettings)
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

void SA3Keybed::InstallLoadedKit()
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
    mDescriptor = kit.manifest.descriptor;
    mWet = kit.manifest.wet;
  }
  if (kit.adoptSettings && kit.manifest.seed)
  {
    mLastSeed = kit.manifest.seed;
    mHasLastSeed = true;
    mLastSeedDescriptor = mDescriptor;
  }
  SetStatus("loaded " + std::to_string(kit.notes.size()) + " notes" + (kit.error.empty() ? "" : " (" + kit.error + ")"),
            !kit.error.empty());
}

std::string SA3Keybed::NoteFilePath(int key) const
{
  if (mKitDir.empty() || key < 0 || key > 127 || !SampleForKey(key))
    return {};
  const std::filesystem::path path = std::filesystem::u8path(mKitDir) / keybed::NoteFileName(key);
  std::error_code ec;
  return std::filesystem::is_regular_file(path, ec) ? path.u8string() : std::string();
}

void SA3Keybed::AuditionKey(int key, bool on)
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

bool SA3Keybed::SerializeState(IByteChunk& chunk) const
{
  chunk.Put(&kStateMagic);
  chunk.Put(&kStateVersion);
  chunk.PutStr(mDescriptor.c_str());
  const int32_t wet = mWet ? 1 : 0, useSeed = mUseSeed ? 1 : 0, hasLast = mHasLastSeed ? 1 : 0;
  const int32_t fx = mFxIndex, steps = mSteps, previewCount = mPreviewCount, root = mPreviewRootLabel,
                range = (int32_t)mRange;
  chunk.Put(&wet);
  chunk.Put(&fx);
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
  return SerializeParams(chunk);
}

int SA3Keybed::UnserializeState(const IByteChunk& chunk, int startPos)
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
  pos = chunk.GetStr(text, pos);
  mDescriptor = text.Get();
  pos = chunk.Get(&wet, pos);
  pos = chunk.Get(&fx, pos);
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
  const std::string kitDir = text.Get();
  if (pos < 0)
    return pos;

  mWet = wet != 0;
  mFxIndex = std::clamp<int>(fx, -1, (int)kb::vocab::fx_choices().size() - 1);
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
  return UnserializeParams(chunk, pos);
}
