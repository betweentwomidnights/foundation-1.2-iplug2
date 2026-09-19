// Headless end-to-end check of the instrument path the plugin uses:
//   libsa3 (runtime-loaded) -> KeybedRenderService -> kit on disk -> KeybedBank -> MIDI -> voices.
//
// Usage: FoundationKeysEngineTest MODELS_DIR [STEPS] [ENCODING]
// Renders a six-note sine preview, plays every key through the sampler at a 48 kHz host rate, and
// checks each output's pitch against its MIDI key (Goertzel energy at f vs f/2, 2f, and +-1 semitone).
// Then checks gap filling, the octave parameter, mono/poly, kit reload from disk, a three-layer keybed,
// and cancel.
#include "KeybedKit.h"
#include "KeybedRenderService.h"
#include "KeybedSampler.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace keybed;
namespace kb = sa3::sat::keybed;

namespace
{
int gFailures = 0;

void Check(bool ok, const std::string& what)
{
  std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what.c_str());
  if (!ok)
    ++gFailures;
}

double Goertzel(const std::vector<double>& x, double sampleRate, double freq)
{
  const double w = 2.0 * 3.14159265358979323846 * freq / sampleRate;
  const double c = 2.0 * std::cos(w);
  double s1 = 0.0, s2 = 0.0;
  for (double v : x)
  {
    const double s0 = v + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return s1 * s1 + s2 * s2 - c * s1 * s2;
}

double KeyHz(int midi) { return 440.0 * std::pow(2.0, (midi - 69) / 12.0); }

// Returns the best-matching MIDI key among [expected-13, expected+13] by spectral energy.
int DominantKey(const std::vector<double>& x, double sampleRate, int expected)
{
  int best = expected;
  double bestEnergy = -1.0;
  for (int k = expected - 13; k <= expected + 13; ++k)
  {
    const double e = Goertzel(x, sampleRate, KeyHz(k));
    if (e > bestEnergy)
    {
      bestEnergy = e;
      best = k;
    }
  }
  return best;
}

std::vector<KeybedEvent> WaitForEnd(KeybedRenderService& service, double timeoutSeconds, std::vector<NoteSamplePtr>* notes,
                                    int cancelAfterNotes = -1)
{
  std::vector<KeybedEvent> ends;
  const auto start = std::chrono::steady_clock::now();
  std::string lastStatus;
  for (;;)
  {
    for (auto& e : service.DrainEvents())
    {
      if (e.kind == KeybedEvent::Kind::Note)
      {
        if (notes)
          notes->push_back(e.note);
        if (cancelAfterNotes >= 0 && notes && (int)notes->size() >= cancelAfterNotes)
          service.Cancel();
      }
      else
        ends.push_back(e);
    }
    if (!ends.empty())
      return ends;
    const std::string status = service.Status();
    if (status != lastStatus && status.find("sampling") == std::string::npos)
      std::printf("  status: %s\n", (lastStatus = status).c_str());
    if (std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() > timeoutSeconds)
      return ends;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

// Plays `key` for `seconds` and returns the mono mix after the attack.
std::vector<double> Play(KeybedEngine& engine, KeybedBank& bank, int key, double seconds, double hostRate)
{
  const int block = 256;
  std::vector<sample> left(block), right(block);
  sample* outputs[2] = {left.data(), right.data()};
  IMidiMsg on;
  on.MakeNoteOnMsg(key, 127, 0);
  engine.ProcessMidiMsg(on);
  std::vector<double> mono;
  const int total = (int)(seconds * hostRate);
  for (int done = 0; done < total; done += block)
  {
    engine.ProcessBlock(outputs, block, bank);
    for (int i = 0; i < block; ++i)
      mono.push_back(0.5 * (left[(size_t)i] + right[(size_t)i]));
  }
  IMidiMsg off;
  off.MakeNoteOffMsg(key, 0);
  engine.ProcessMidiMsg(off);
  for (int i = 0; i < (int)(0.6 * hostRate); i += block)   // let the release finish
    engine.ProcessBlock(outputs, block, bank);
  const size_t skip = (size_t)(0.15 * hostRate);
  return std::vector<double>(mono.begin() + (long)std::min(skip, mono.size()), mono.end());
}

// Runs the engine for `seconds` with whatever is held and returns the mono mix.
std::vector<double> Run(KeybedEngine& engine, KeybedBank& bank, double seconds, double hostRate)
{
  const int block = 256;
  std::vector<sample> left(block), right(block);
  sample* outputs[2] = {left.data(), right.data()};
  std::vector<double> mono;
  for (int done = 0; done < (int)(seconds * hostRate); done += block)
  {
    engine.ProcessBlock(outputs, block, bank);
    for (int i = 0; i < block; ++i)
      mono.push_back(0.5 * (left[(size_t)i] + right[(size_t)i]));
  }
  return mono;
}

void Key(KeybedEngine& engine, int key, bool on)
{
  IMidiMsg msg;
  if (on)
    msg.MakeNoteOnMsg(key, 127, 0);
  else
    msg.MakeNoteOffMsg(key, 0);
  engine.ProcessMidiMsg(msg);
}

double Rms(const std::vector<double>& x)
{
  double e = 0.0;
  for (double v : x) e += v * v;
  return x.empty() ? 0.0 : std::sqrt(e / (double)x.size());
}
} // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    std::fprintf(stderr, "usage: %s MODELS_DIR [STEPS] [ENCODING]\n", argv[0]);
    return 2;
  }
  const std::string modelsDir = argv[1];
  const int steps = argc > 2 ? std::atoi(argv[2]) : 40;
  const std::string encoding = argc > 3 ? argv[3] : "F16";
  const double hostRate = 48000.0;

  std::printf("1. render a six-note sine preview (labels C5..F5, sounding C4..F4), %d steps, %s\n", steps, encoding.c_str());
  KeybedRenderService service;
  KeybedJob job;
  job.modelsDir = modelsDir;
  job.encoding = encoding;
  job.layers = {"Pure Tone, Sine, Clean"};
  job.chunks = kb::plan_preview(72, 6);
  job.rangeLabel = "engine test";
  job.seed = 7;
  job.steps = steps;
  std::string error;
  Check(service.Start(job, error), "service starts" + (error.empty() ? std::string() : ": " + error));
  std::vector<NoteSamplePtr> notes;
  const auto started = std::chrono::steady_clock::now();
  auto ends = WaitForEnd(service, 180.0, &notes);
  const double renderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  Check(!ends.empty() && ends.back().kind == KeybedEvent::Kind::Finished,
        "preview finished in " + std::to_string(renderSeconds) + " s: " + (ends.empty() ? "timeout" : ends.back().message));
  if (ends.empty() || ends.back().kind != KeybedEvent::Kind::Finished)
    return 1;
  const std::string kitDir = ends.back().kitDir;
  service.Collect(true);
  Check(notes.size() == 6, "six notes arrived incrementally (" + std::to_string(notes.size()) + ")");
  Check(ends.back().seed == 7, "seed 7 reported");
  Check(!kitDir.empty(), "kit saved to " + kitDir);

  std::printf("2. play every key through the sampler at %.0f Hz\n", hostRate);
  KeybedBank bank;
  bank.Publish(notes, true);
  KeybedEngine engine;
  engine.Reset(hostRate, 256);
  engine.settings.velocity.store(0.0);
  for (const auto& note : notes)
  {
    const std::vector<double> out = Play(engine, bank, note->midi, 1.0, hostRate);
    const int heard = DominantKey(out, hostRate, note->midi);
    Check(heard == note->midi && Rms(out) > 1e-3,
          "key " + kb::midi_to_note_name(note->midi) + " sounds " + kb::midi_to_note_name(heard) +
          " (rms " + std::to_string(Rms(out)) + ")");
  }

  std::printf("3. gap filling, octave shift, and fill-gaps off\n");
  {
    const int gapKey = 57;   // A3: below the kit, repitched down from C4
    auto out = Play(engine, bank, gapKey, 1.0, hostRate);
    Check(DominantKey(out, hostRate, gapKey) == gapKey, "gap key A3 repitches to A3");
    engine.settings.octave.store(-1);
    out = Play(engine, bank, 72, 1.0, hostRate);   // C5 pressed, octave -1 -> plays the C4 sample
    Check(DominantKey(out, hostRate, 60) == 60, "octave -1: pressing C5 plays C4");
    engine.settings.octave.store(0);
    engine.settings.fillGaps.store(false);
    out = Play(engine, bank, gapKey, 0.5, hostRate);
    Check(Rms(out) < 1e-6, "fill gaps off leaves A3 silent");
    engine.settings.fillGaps.store(true);
  }

  std::printf("3b. mono and poly\n");
  {
    // Energy at a key's fundamental over the last two thirds of a capture (after any crossfade).
    const auto energy = [&](const std::vector<double>& x, int key) {
      const std::vector<double> tail(x.begin() + (long)(x.size() / 3), x.end());
      return Goertzel(tail, hostRate, KeyHz(key));
    };
    const double single = Rms(Play(engine, bank, 64, 0.6, hostRate));
    engine.settings.mono.store(true);
    Key(engine, 60, true);
    Run(engine, bank, 0.3, hostRate);
    Key(engine, 64, true);   // C4 still held
    auto out = Run(engine, bank, 0.6, hostRate);
    Check(energy(out, 64) > 100.0 * energy(out, 60), "mono: holding C4 and pressing E4 plays only E4");
    const double monoLevel = Rms(std::vector<double>(out.begin() + (long)(out.size() / 3), out.end()));
    Check(monoLevel < 1.5 * single, "mono: one voice, not a stack (" + std::to_string(monoLevel / single) + "x a single note)");
    Key(engine, 64, false);  // back to the key still held
    out = Run(engine, bank, 0.6, hostRate);
    Check(energy(out, 60) > 100.0 * energy(out, 64), "mono: releasing E4 returns to the held C4");
    Key(engine, 60, false);
    Run(engine, bank, 0.6, hostRate);
    out = Run(engine, bank, 0.2, hostRate);
    Check(Rms(out) < 1e-6, "mono: silent once every key is up");

    engine.settings.mono.store(false);
    Key(engine, 60, true);
    Key(engine, 64, true);
    out = Run(engine, bank, 0.6, hostRate);
    const double c = energy(out, 60), e = energy(out, 64);
    Check(c > 0.05 * e && e > 0.05 * c, "poly: C4 and E4 sound together");
    Key(engine, 60, false);
    Key(engine, 64, false);
    Run(engine, bank, 0.6, hostRate);
  }

  std::printf("3c. headroom\n");
  {
    const auto peakOf = [](const std::vector<double>& x) {
      double p = 0.;
      for (double v : x)
        p = std::max(p, std::fabs(v));
      return p;
    };
    double loudest = 0.;
    for (const auto& note : notes)
      loudest = std::max(loudest, (double)note->peak);
    Key(engine, 60, true);
    const double one = peakOf(Run(engine, bank, 0.5, hostRate));
    Key(engine, 60, false);
    Run(engine, bank, 0.6, hostRate);
    const double oneDb = 20. * std::log10(one / std::max(1e-9, loudest));
    Check(oneDb < -11. && oneDb > -13., "one key plays 12 dB below its sample (" + std::to_string(oneDb) + " dB)");
    for (int key : {60, 62, 64, 65})
      Key(engine, key, true);
    const double chord = peakOf(Run(engine, bank, 0.5, hostRate));
    for (int key : {60, 62, 64, 65})
      Key(engine, key, false);
    Run(engine, bank, 0.6, hostRate);
    Check(chord < 1.0, "a four-note chord stays below full scale (peak " + std::to_string(chord) + ")");
  }

  std::printf("4. reload the kit from disk\n");
  {
    KitManifest manifest;
    std::string loadError;
    const auto loaded = LoadKitSamples(kitDir, manifest, loadError);
    Check(loaded.size() == 6 && manifest.complete && manifest.seed == 7 && manifest.descriptor == job.layers[0],
          "kit.json + WAVs reload (" + std::to_string(loaded.size()) + " notes)" + (loadError.empty() ? "" : ": " + loadError));
    bool identical = loaded.size() == notes.size();
    for (size_t i = 0; identical && i < loaded.size(); ++i)
      identical = loaded[i]->frames == notes[i]->frames && loaded[i]->left == notes[i]->left;
    Check(identical, "reloaded float WAVs are bit-identical");
  }

  std::printf("5. a three-layer keybed (RC's Main + Supports): per-layer seeds, folders, and mix\n");
  {
    KeybedJob layered = job;
    layered.layers = {"Pure Tone, Sine, Clean", "Pure Tone, Sine, Warm", "Pure Tone, Sine, Bright"};
    layered.seed = 7;
    Check(service.Start(layered, error), "layered render starts" + (error.empty() ? std::string() : ": " + error));
    std::vector<NoteSamplePtr> layerNotes;
    ends = WaitForEnd(service, 300.0, &layerNotes);
    Check(!ends.empty() && ends.back().kind == KeybedEvent::Kind::Finished,
          "layered render finished: " + (ends.empty() ? std::string("timeout") : ends.back().message));
    service.Collect(true);
    bool mainFirst = layerNotes.size() == 18;
    for (size_t i = 0; mainFirst && i < layerNotes.size(); ++i)
      mainFirst = layerNotes[i]->layer == (int)(i / 6);
    Check(mainFirst, "18 notes arrived layer by layer, main first");
    bool tagged = !layerNotes.empty();
    for (const auto& note : layerNotes)
      tagged = tagged && note->layered;
    Check(tagged, "every note is tagged as part of a layered kit");

    const std::string layeredDir = ends.empty() ? std::string() : ends.back().kitDir;
    KitManifest manifest;
    std::string loadError;
    const auto loaded = LoadKitSamples(layeredDir, manifest, loadError);
    bool seeds = manifest.seed == 7 && manifest.layerSeeds.size() == 3 && manifest.layerDescriptors.size() == 3;
    for (int l = 0; seeds && l < 3; ++l)
      seeds = manifest.layerSeeds[(size_t)l] == kb::layer_seed(7, l);
    Check(seeds, "top-level kit.json: base seed 7 and RC's SHA-1 layer seeds");
    int perLayer[3] = {0, 0, 0};
    for (const auto& note : loaded)
      if (note->layer >= 0 && note->layer < 3)
        ++perLayer[note->layer];
    Check(loaded.size() == 18 && perLayer[0] == 6 && perLayer[1] == 6 && perLayer[2] == 6,
          "layer folders reload with their layer tags (" + std::to_string(loaded.size()) + " notes)" +
            (loadError.empty() ? "" : ": " + loadError));

    KeybedBank layeredBank;
    layeredBank.Publish(loaded, true);
    Check(layeredBank.Latest() && layeredBank.Latest()->layerCount == 3, "bank sees three layers");
    auto out = Play(engine, layeredBank, 60, 1.0, hostRate);
    Check(DominantKey(out, hostRate, 60) == 60 && Rms(out) > 1e-3, "layered C4 sounds C4");
    const double full = Rms(out);
    engine.settings.layerVolume[0].store(0.0);
    out = Play(engine, layeredBank, 60, 1.0, hostRate);
    Check(Rms(out) > 1e-4 && Rms(out) < full, "main muted: the supports still sound, quieter");
    engine.settings.layerVolume[0].store(0.90);

    // Mid-build: only main has landed, but the kit is layered, so main already plays at RC's mix
    // (master 0.55 x 0.90) instead of jumping down when the supports arrive.
    std::vector<NoteSamplePtr> mainOnly, mainPlain;
    for (const auto& note : loaded)
      if (note->layer == 0)
      {
        mainOnly.push_back(note);
        auto plain = std::make_shared<NoteSample>(*note);
        plain->layered = false;
        mainPlain.push_back(std::move(plain));
      }
    KeybedBank partialBank, plainBank;
    partialBank.Publish(mainOnly, true);
    plainBank.Publish(mainPlain, true);
    const BankSnapshot* partial = partialBank.Latest();
    Check(partial && partial->layered && partial->hasLayer[0] && !partial->hasLayer[1] && !partial->hasLayer[2],
          "main-only bank of a layered kit: layered, supports not present yet");
    const double mixed = Rms(Play(engine, partialBank, 60, 1.0, hostRate));
    const double plain = Rms(Play(engine, plainBank, 60, 1.0, hostRate));
    const double ratio = plain > 0 ? mixed / plain : 0;
    Check(std::fabs(ratio - 0.55 * 0.90) < 0.01, "main alone plays at RC's master x main level (" + std::to_string(ratio) + ")");

    for (auto& v : engine.settings.layerVolume)
      v.store(0.0);
    out = Play(engine, layeredBank, 60, 0.5, hostRate);
    Check(Rms(out) < 1e-6, "every layer muted: silence");
    engine.settings.layerVolume[0].store(0.90);
    engine.settings.layerVolume[1].store(0.60);
    engine.settings.layerVolume[2].store(0.35);
  }

  std::printf("6. cancel a full build after its first chunk\n");
  {
    job.chunks = kb::plan_full_range(kb::FullRange::C2ToB5);
    job.seed = -1;
    Check(service.Start(job, error), "full build starts");
    std::vector<NoteSamplePtr> partial;
    ends = WaitForEnd(service, 180.0, &partial, 6);
    Check(!ends.empty() && ends.back().kind == KeybedEvent::Kind::Cancelled,
          "cancelled: " + (ends.empty() ? std::string("timeout") : ends.back().message));
    Check(partial.size() == 6, "first chunk's six notes remain playable");
    service.Collect(false);
  }

  std::printf(gFailures ? "FoundationKeysEngineTest: %d failure(s)\n" : "FoundationKeysEngineTest: ok\n", gFailures);
  return gFailures ? 1 : 0;
}
