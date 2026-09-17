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
shifts it. Keys without a sample borrow the nearest one and repitch it ("fill gaps").

Every render is saved under `Documents/sa3-keybed/kits/<time>-<name>-<seed>/`: one 32-bit float WAV
per key, the raw chunks, `kit.json`, and a `kit.sfz` for other samplers. The plugin state stores the
kit folder and reloads it in the background with the project. Drag the note waveform out to export
that key's WAV, or drag the kit name to export the whole kit folder.

Formats: VST3, CLAP, and a standalone app.

## Build (Windows)

1. Build sa3.cpp with SAT in libsa3 (the `feature/foundation-1.2-keybeds-abi` branch; its
   `build.cmd` passes `-DSA3_BUILD_SAT=ON`): `cd ..\sa3.cpp && build.cmd cuda` (or `vulkan`).
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

Point settings at a folder containing, for one tier (F16, Q8_0, or Q5_K_M):

- `foundation-1.2-keybeds-dit-1.1B-v1.0-<tier>.gguf`
- `t5-base-encoder-128tok-0.1B-v1.0-<tier>.gguf`
- `stable-audio-open-oobleck-v1.0-<tier>.gguf`

Download them from [thepatch/foundation-1.2-keybeds-GGUF](https://huggingface.co/thepatch/foundation-1.2-keybeds-GGUF),
or convert the DiT yourself with `sa3.cpp/tools/convert_sat_dit.py` (see
`sa3.cpp/docs/STABLE_AUDIO_OPEN_1.md`); the T5 and Oobleck files are Foundation-1's. The build defaults to
`../sa3.cpp/models/publication-stage-sat/foundation-1.2-keybeds-GGUF`.

## Test

- `build/out-test/Release/SA3KeybedEngineTest.exe <models dir> [steps]` renders a sine preview
  through libsa3, plays every key through the sampler at 48 kHz and checks its pitch, then checks gap
  filling, octave shift, kit reload, and cancel.
- VST3 validator: 47 passed, 0 failed.

The Foundation-1.2 weights are under the Stability AI Community License.
