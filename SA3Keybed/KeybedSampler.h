#pragma once

// Sample-playback engine for a generated keybed. The UI thread publishes immutable bank snapshots;
// the audio thread reads the newest one lock-free at the start of each block. Voices keep a
// shared_ptr to the samples they play, and retired snapshots are freed on the UI thread only after
// the audio thread has moved past them and a grace period has elapsed, so sample memory is not
// normally released on the audio thread.
//
// A layered keybed (RC's Main + two Supports) holds one sample per layer per key. A voice plays
// every layer that has a sample for its key, each at its layer volume, like DecentSampler groups.

#include "IPlugConstants.h"
#include "IPlugMidi.h"
#include "ADSREnvelope.h"
#include "MidiSynth.h"

#include "KeybedKit.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

namespace keybed
{

using namespace iplug;

constexpr int kMaxLayers = 3;

struct BankSnapshot
{
  std::array<std::array<NoteSamplePtr, 128>, kMaxLayers> layers{};
  std::array<std::array<int, 128>, kMaxLayers> nearestKey{};   // -1 when that layer is empty
  std::array<NoteSamplePtr, 128> exact{};   // UI view: the main layer's sample, else any layer's
  uint64_t epoch = 0;
  int count = 0;        // keys with at least one sample
  int layerCount = 0;   // layers with at least one sample

  void BuildIndex()
  {
    count = 0;
    layerCount = 0;
    for (int k = 0; k < 128; ++k)
    {
      exact[(size_t)k] = nullptr;
      for (int l = 0; l < kMaxLayers && !exact[(size_t)k]; ++l)
        exact[(size_t)k] = layers[(size_t)l][(size_t)k];
      count += exact[(size_t)k] ? 1 : 0;
    }
    for (int l = 0; l < kMaxLayers; ++l)
    {
      const auto& samples = layers[(size_t)l];
      bool any = false;
      for (int k = 0; k < 128; ++k)
      {
        auto& nearest = nearestKey[(size_t)l][(size_t)k];
        nearest = -1;
        for (int d = 0; d < 128 && nearest < 0; ++d)
        {
          // prefer the sample below: repitching up from a lower root keeps attacks tighter
          if (k - d >= 0 && samples[(size_t)(k - d)]) nearest = k - d;
          else if (k + d < 128 && samples[(size_t)(k + d)]) nearest = k + d;
        }
        any = any || samples[(size_t)k];
      }
      layerCount += any ? 1 : 0;
    }
  }
};

class KeybedBank
{
public:
  ~KeybedBank()
  {
    delete mCurrent.load(std::memory_order_acquire);
  }

  // UI thread: install `notes` (each at its own layer), replacing the whole bank when `replace`,
  // otherwise merging over it.
  void Publish(const std::vector<NoteSamplePtr>& notes, bool replace)
  {
    auto next = std::make_unique<BankSnapshot>();
    const BankSnapshot* current = mCurrent.load(std::memory_order_acquire);
    if (current && !replace)
      next->layers = current->layers;
    for (const auto& note : notes)
      if (note && note->midi >= 0 && note->midi < 128 && note->layer >= 0 && note->layer < kMaxLayers)
        next->layers[(size_t)note->layer][(size_t)note->midi] = note;
    next->BuildIndex();
    next->epoch = ++mNextEpoch;
    BankSnapshot* old = mCurrent.exchange(next.release(), std::memory_order_acq_rel);
    if (old)
      mRetired.push_back({std::unique_ptr<BankSnapshot>(old), mNextEpoch, Clock::now()});
  }

  void Clear() { Publish({}, true); }

  // UI thread (OnIdle): free snapshots the audio thread no longer reads.
  void CollectGarbage()
  {
    const uint64_t acked = mAckedEpoch.load(std::memory_order_acquire);
    const auto now = Clock::now();
    for (size_t i = 0; i < mRetired.size();)
    {
      const auto& r = mRetired[i];
      if (acked >= r.replacedBy && now - r.retiredAt > std::chrono::seconds(20))
        mRetired.erase(mRetired.begin() + (long)i);
      else
        ++i;
    }
  }

  // UI thread: the snapshot as last published (for drawing).
  const BankSnapshot* Latest() const { return mCurrent.load(std::memory_order_acquire); }

  // Audio thread.
  const BankSnapshot* Acquire()
  {
    const BankSnapshot* snap = mCurrent.load(std::memory_order_acquire);
    mAckedEpoch.store(snap ? snap->epoch : 0, std::memory_order_release);
    return snap;
  }

private:
  using Clock = std::chrono::steady_clock;
  struct Retired
  {
    std::unique_ptr<BankSnapshot> snapshot;
    uint64_t replacedBy = 0;
    Clock::time_point retiredAt;
  };
  std::atomic<BankSnapshot*> mCurrent{nullptr};
  std::atomic<uint64_t> mAckedEpoch{0};
  uint64_t mNextEpoch = 0;
  std::vector<Retired> mRetired;
};

struct SamplerSettings
{
  std::atomic<double> gain{0.8};
  std::atomic<double> attackMs{5.};
  std::atomic<double> decayMs{50.};
  std::atomic<double> sustain{1.};
  std::atomic<double> releaseMs{250.};
  std::atomic<double> velocity{0.5};   // 0: velocity ignored (RC's DecentSampler default), 1: full
  std::atomic<double> tuneSemitones{0.};
  std::atomic<int> octave{0};
  std::atomic<bool> fillGaps{true};
  std::atomic<uint32_t> envelopeVersion{1};
  // RC's tri-layer volumes; used only once a kit has more than one layer.
  std::array<std::atomic<double>, kMaxLayers> layerVolume{{{0.90}, {0.60}, {0.35}}};
};

class KeybedEngine;

class KeybedVoice final : public SynthVoice
{
public:
  explicit KeybedVoice(KeybedEngine& engine)
  : mEngine(engine)
  , mEnv("amp", [this]() { StartPending(); })
  {
  }

  bool GetBusy() const override { return mEnv.GetBusy(); }

  inline void Trigger(double level, bool isRetrigger) override;

  void Release() override { mEnv.Release(); }

  inline void ProcessSamplesAccumulating(sample** inputs, sample** outputs, int nInputs, int nOutputs,
                                         int startIdx, int nFrames) override;

  void SetSampleRateAndBlockSize(double sampleRate, int blockSize) override
  {
    mHostRate = sampleRate;
    mEnv.SetSampleRate(sampleRate);
    mAppliedEnvelopeVersion = 0;   // stage increments depend on the rate
  }

private:
  struct Layer
  {
    NoteSamplePtr sample;
    double semitones = 0.;   // key distance from the sample root, before tune/bend
    double pos = 0.;
  };

  inline void ApplyEnvelopeTimes();
  inline void StartPending();

  static inline float Hermite(const std::vector<float>& d, int frames, double pos)
  {
    const int i = (int)pos;
    const float t = (float)(pos - i);
    auto at = [&](int k) { return d[(size_t)std::clamp(k, 0, frames - 1)]; };
    const float xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
  }

  KeybedEngine& mEngine;
  ADSREnvelope<sample> mEnv;
  std::array<Layer, kMaxLayers> mLayers{};
  std::array<Layer, kMaxLayers> mPending{};
  bool mLayered = false;          // the bank this note came from had more than one layer
  bool mPendingLayered = false;
  double mHostRate = 44100.;
  uint32_t mAppliedEnvelopeVersion = 0;
};

class KeybedEngine
{
public:
  static constexpr int kVoices = 32;

  KeybedEngine()
  {
    for (int i = 0; i < kVoices; ++i)
      mSynth.AddVoice(new KeybedVoice(*this), 0);
    for (auto& held : mHeld)
      held.store(false, std::memory_order_relaxed);
  }

  void Reset(double sampleRate, int blockSize)
  {
    mSynth.SetSampleRateAndBlockSize(sampleRate, blockSize);
    mSynth.Reset();
  }

  void ProcessMidiMsg(const IMidiMsg& msg)
  {
    const int status = msg.StatusMsg();
    if (status == IMidiMsg::kNoteOn && msg.Velocity() > 0)
    {
      mHeld[(size_t)msg.NoteNumber()].store(true, std::memory_order_relaxed);
      mLastKey.store(msg.NoteNumber(), std::memory_order_relaxed);
    }
    else if (status == IMidiMsg::kNoteOff || status == IMidiMsg::kNoteOn)
      mHeld[(size_t)msg.NoteNumber()].store(false, std::memory_order_relaxed);
    mSynth.AddMidiMsgToQueue(msg);
  }

  void ProcessBlock(sample** outputs, int nFrames, KeybedBank& bank)
  {
    for (int c = 0; c < 2; ++c)
      std::memset(outputs[c], 0, (size_t)nFrames * sizeof(sample));
    mBlockBank = bank.Acquire();
    mSynth.ProcessBlock(nullptr, outputs, 0, 2, nFrames);
    const sample target = (sample)settings.gain.load(std::memory_order_relaxed);
    for (int s = 0; s < nFrames; ++s)
    {
      mSmoothedGain += (target - mSmoothedGain) * (sample)0.002;
      outputs[0][s] *= mSmoothedGain;
      outputs[1][s] *= mSmoothedGain;
    }
  }

  bool Held(int key) const { return key >= 0 && key < 128 && mHeld[(size_t)key].load(std::memory_order_relaxed); }
  int LastKey() const { return mLastKey.load(std::memory_order_relaxed); }

  SamplerSettings settings;

private:
  friend class KeybedVoice;
  MidiSynth mSynth{VoiceAllocator::kPolyModePoly, MidiSynth::kDefaultBlockSize};
  const BankSnapshot* mBlockBank = nullptr;   // audio thread only
  sample mSmoothedGain = 0.8;
  std::array<std::atomic<bool>, 128> mHeld;
  std::atomic<int> mLastKey{-1};
};

inline void KeybedVoice::ApplyEnvelopeTimes()
{
  const uint32_t version = mEngine.settings.envelopeVersion.load(std::memory_order_relaxed);
  if (version == mAppliedEnvelopeVersion)
    return;
  mEnv.SetStageTime(ADSREnvelope<sample>::kAttack, (sample)mEngine.settings.attackMs.load(std::memory_order_relaxed));
  mEnv.SetStageTime(ADSREnvelope<sample>::kDecay, (sample)mEngine.settings.decayMs.load(std::memory_order_relaxed));
  mEnv.SetStageTime(ADSREnvelope<sample>::kRelease, (sample)mEngine.settings.releaseMs.load(std::memory_order_relaxed));
  mAppliedEnvelopeVersion = version;
}

inline void KeybedVoice::Trigger(double level, bool isRetrigger)
{
  ApplyEnvelopeTimes();
  const SamplerSettings& s = mEngine.settings;
  const BankSnapshot* bank = mEngine.mBlockBank;
  const int key = std::clamp((int)mKey + 12 * s.octave.load(std::memory_order_relaxed), 0, 127);
  const bool fill = s.fillGaps.load(std::memory_order_relaxed);
  bool any = false;
  for (int l = 0; l < kMaxLayers; ++l)
  {
    Layer& pending = mPending[(size_t)l];
    pending = Layer{};
    if (!bank)
      continue;
    const auto& samples = bank->layers[(size_t)l];
    int root = samples[(size_t)key] ? key : -1;
    if (root < 0 && fill)
      root = bank->nearestKey[(size_t)l][(size_t)key];
    if (root >= 0)
    {
      pending.sample = samples[(size_t)root];
      pending.semitones = (double)(key - root);
      any = true;
    }
  }
  mPendingLayered = bank && bank->layerCount > 1;
  const double sens = std::clamp(s.velocity.load(std::memory_order_relaxed), 0., 1.);
  const double gain = (1. - sens) + sens * level * level;
  bool playing = false;
  for (const auto& layer : mLayers)
    playing = playing || layer.sample;
  if (isRetrigger && playing)
    mEnv.Retrigger(gain);   // StartPending runs when the steal fade reaches zero
  else
  {
    StartPending();
    mEnv.Start(gain);
  }
  (void)any;
}

inline void KeybedVoice::StartPending()
{
  for (int l = 0; l < kMaxLayers; ++l)
  {
    mLayers[(size_t)l] = std::move(mPending[(size_t)l]);
    mPending[(size_t)l] = Layer{};
  }
  mLayered = mPendingLayered;
}

inline void KeybedVoice::ProcessSamplesAccumulating(sample** inputs, sample** outputs, int nInputs, int nOutputs,
                                                    int startIdx, int nFrames)
{
  const SamplerSettings& s = mEngine.settings;
  const sample sustain = (sample)std::clamp(s.sustain.load(std::memory_order_relaxed), 0., 1.);
  const double bendOctaves = mInputs[kVoiceControlPitchBend].endValue;
  const double tune = s.tuneSemitones.load(std::memory_order_relaxed);
  std::array<double, kMaxLayers> volume{};
  for (int l = 0; l < kMaxLayers; ++l)
    volume[(size_t)l] = mLayered ? s.layerVolume[(size_t)l].load(std::memory_order_relaxed) : 1.;
  // The increment is recomputed only when a layer's sample changes (e.g. a retrigger swap).
  std::array<const NoteSample*, kMaxLayers> rateFor{};
  std::array<double, kMaxLayers> increment{};
  for (int i = startIdx; i < startIdx + nFrames; ++i)
  {
    const sample env = mEnv.Process(sustain);   // may swap in pending samples on a retrigger
    bool sounding = false;
    for (int l = 0; l < kMaxLayers; ++l)
    {
      Layer& layer = mLayers[(size_t)l];
      const NoteSample* note = layer.sample.get();
      if (!note || note->frames < 2)
        continue;
      if (layer.pos >= (double)(note->frames - 1))
      {
        layer.sample.reset();
        continue;
      }
      if (note != rateFor[(size_t)l])
      {
        increment[(size_t)l] = std::pow(2., (layer.semitones + tune) / 12. + bendOctaves) *
                               (double)note->sampleRate / mHostRate;
        rateFor[(size_t)l] = note;
      }
      const double g = env * volume[(size_t)l];
      outputs[0][i] += (sample)(Hermite(note->left, note->frames, layer.pos) * g);
      outputs[1][i] += (sample)(Hermite(note->right, note->frames, layer.pos) * g);
      layer.pos += increment[(size_t)l];
      sounding = true;
    }
    if (!sounding && mEnv.GetBusy())
    {
      bool pending = false;
      for (const auto& p : mPending)
        pending = pending || p.sample;
      if (!pending)
        mEnv.Kill(true);   // every layer's sample has ended
    }
  }
}

} // namespace keybed
