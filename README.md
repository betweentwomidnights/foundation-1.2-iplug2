# SA3Keybed

An iPlug2 instrument that prompts a playable keyboard out of RoyalCities'
[Foundation-1.2 Keybeds](https://huggingface.co/RoyalCities/Foundation-1) model, running locally
through [sa3.cpp](../sa3.cpp)'s libsa3 C ABI. Describe a sound, preview six notes, build a full
48-60 note keyboard, and play it from your DAW. The visual language matches `sa3.cpp-iplug2-demo`.

## How it works

Foundation-1.2 Keybeds renders chromatic runs: six notes, 3.0 s each with 0.25 s gaps, in one
20 s pass. SA3Keybed follows RoyalCities' keybed recipe (`sa3.cpp/src/sat/keybed.h`):

1. plan chunks (preview: 6/12/24 notes; keyboard: RC's C2-B5, C2-F6, or C2-B6 prompt labels);
2. render every chunk with one shared seed and resident models through `sa3_api_v1::generate`
   (variant `foundation-1.2-keybeds`, DPM++ 3M SDE, 80 steps, CFG 6, sigma 0.03-500, raw loudness);
3. slice each chunk on the 3.25 s grid, trim silent tails, fade, and map one sample per key;
4. publish the samples to a lock-free bank as each chunk lands, so keys become playable mid-build.

The model renders one octave below its prompt labels, so keys are mapped by sounding pitch:
MIDI 60 plays middle C. The default C2-B5 keyboard therefore covers C1-B4, and the octave control
shifts it. Keys without a sample borrow the nearest one and repitch it ("fill gaps"). Playback is
poly (32 voices) or mono: one key at a time, last-note priority, returning to a key still held, with a
20 ms crossfade between notes.

Every render is saved under `Documents/sa3-keybed/kits/<time>-<name>-<seed>/`: one 32-bit float WAV
per key, the raw chunks, `kit.json`, and a `kit.sfz` for other samplers. The plugin state stores the
kit folder and reloads it in the background with the project. Drag the note waveform out to export
that key's WAV, or drag the kit name to export the whole kit folder.

### Layered keybeds

RoyalCities' multi-layered keybeds stack up to three independently rendered keyboards: a Main layer
and two Supports. In the sound sheet (`edit`), `+ layer` adds a Support tab with its own sound;
range, steps, CFG, and render fx are shared. A build renders layer by layer, Main first, so the main
keyboard is playable after a third of the time. Each layer's seed comes from the base seed exactly as
RC derives it (`sa3::sat::keybed::layer_seed`), so a layered kit is reproducible from one number.
The layers mix only at playback, with a volume per layer (host parameters, defaulting to RC's
90 / 60 / 35 %, under his 0.55 master so full-scale layers don't clip) that appears under the
playback sliders once a kit has layers. A layer that hasn't rendered yet reads "rendering...", and
main plays at its mixed level from the first note, so nothing jumps when the supports arrive. A layered kit keeps
each layer in `layer_1_main`, `layer_2_support`, and `layer_3_support`, and its top-level `kit.sfz`
plays them together at the same mix.

Formats: VST3, CLAP, and a standalone app.

## Build (Windows)

1. Build sa3.cpp with SAT in libsa3 (`build.cmd` passes `-DSA3_BUILD_SAT=ON`):
   `cd ..\sa3.cpp && build.cmd cuda` (or `vulkan`).
2. Fetch the iPlug2 fork and its SDKs:
   `git submodule update --init` then, in git-bash,
   `cd vendor/iPlug2/Dependencies/IPlug && ./download-iplug-sdks.sh && ./download-clap-sdks.sh`.
3. Configure and build:
   ```
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release --target SA3Keybed-vst3 SA3Keybed-clap SA3Keybed-app
   ```
   `SA3_BUILD_DIR` defaults to `../sa3.cpp/build-cuda` when present. `sa3.dll`, `ggml*.dll`, and (for
   CUDA builds) the CUDA runtime are copied next to the plugin binary. libsa3 is loaded on the first
   render, never at plugin scan.

## Models

Open settings, pick a tier, and press **download**:

| tier | size | |
|---|---|---|
| F16 | 2.5 GB | reference quality |
| Q8_0 | 1.4 GB | |
| Q5_K_M | 1.0 GB | |
| Q4_K_M | 0.9 GB | smallest; fastest to load |

The three files of a tier (`foundation-1.2-keybeds-dit-1.1B-v1.0-<tier>.gguf`,
`t5-base-encoder-128tok-0.1B-v1.0-<tier>.gguf`, `stable-audio-open-oobleck-v1.0-<tier>.gguf`) come from
[thepatch/foundation-1.2-keybeds-GGUF](https://huggingface.co/thepatch/foundation-1.2-keybeds-GGUF) via
the system curl (Windows 10+, macOS). Each downloads to a `.part` file that resumes after a cancel or
a crash and is renamed only once its size matches Hugging Face's, so a partial file is never loaded;
files already present are skipped. Downloads land in the models folder shown in settings, which
defaults to per-user app data rather than Documents (often cloud-synced):
`%LOCALAPPDATA%\sa3-keybed\models` on Windows, `~/Library/Application Support/sa3-keybed/models` on macOS.
`SA3_KEYBED_MODELS_DIR` overrides it.

You can also point settings at any folder that already holds a tier, or convert the DiT yourself with
`sa3.cpp/tools/convert_sat_dit.py` (see `sa3.cpp/docs/STABLE_AUDIO_OPEN_1.md`); the T5 and Oobleck files
are Foundation-1's. Dev builds also look in
`../sa3.cpp/models/publication-stage-sat/foundation-1.2-keybeds-GGUF`; configure release builds with
`-DSA3_KEYBED_DEV_MODELS=OFF` so no build-machine path ships in the binary.

## Test

- `build/out-test/Release/SA3KeybedEngineTest.exe <models dir> [steps]` renders a sine preview
  through libsa3, plays every key through the sampler at 48 kHz and checks its pitch, then checks gap
  filling, octave shift, mono/poly, kit reload, a three-layer keybed (seeds, folders, mix), and cancel.
- VST3 validator: 47 passed, 0 failed.

The Foundation-1.2 weights are under the Stability AI Community License.
