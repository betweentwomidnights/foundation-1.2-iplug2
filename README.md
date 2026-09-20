# Foundation Keys

A free, open-source instrument (VST3, CLAP, standalone) that turns a text prompt into a playable keyboard
with RoyalCities' **[Foundation-1.2](https://huggingface.co/RoyalCities/Foundation-1)** Keybeds model,
running locally through **[sa3.cpp](https://github.com/betweentwomidnights/sa3.cpp)**.

![Foundation Keys](docs/screenshot.png)

**Lineage**
- model: [RoyalCities/Foundation-1](https://huggingface.co/RoyalCities/Foundation-1) (Foundation-1.2 Keybeds)
- upstream: [RoyalCities/RC-stable-audio-tools](https://github.com/RoyalCities/RC-stable-audio-tools), the keybed recipe this follows
- inference: [sa3.cpp](https://github.com/betweentwomidnights/sa3.cpp) (ggml; CUDA, Vulkan, Metal)
- GGUFs: [thepatch/foundation-1.2-keybeds-GGUF](https://huggingface.co/thepatch/foundation-1.2-keybeds-GGUF)
- framework: [iPlug2 fork](https://github.com/betweentwomidnights/iPlug2) (`sa3-demo-tempo` branch)

## How it works

Describe a sound (or roll the dice), preview six notes, then build a 48-60 key keyboard. Following
RoyalCities' recipe, each 20 s pass renders six chromatic notes with one shared seed; the notes are sliced
into 3 s samples, one per key, and become playable as each pass lands. Up to two **support layers**
(`edit` > `+ layer`) render after the main one and mix at playback, at RC's volumes and seeds.

The model renders an octave below its prompt labels, so keys are mapped by sounding pitch (MIDI 60 plays
middle C). Kits save to `Documents/Foundation Keys/kits` as WAVs plus a `kit.sfz`; drag a note or a kit
out of the plugin to use it elsewhere.

## Models

Settings downloads a tier from Hugging Face (resumable) into `%LOCALAPPDATA%\Foundation Keys\models`
(`~/Library/Application Support/Foundation Keys/models` on macOS), or points at a folder that has one:
F16 2.5 GB, Q8_0 1.4 GB, Q5_K_M 1.0 GB, Q4_K_M 0.9 GB.

## Build

You need CMake 3.20+, git, and a C++17 toolchain: Visual Studio 2022 on Windows, Xcode with its command
line tools on macOS (`brew install cmake` if `cmake` isn't on your PATH).

Clone this repo and sa3.cpp side by side, then build libsa3 with SAT (Foundation) support:

```
git clone --recurse-submodules https://github.com/betweentwomidnights/foundation-1.2-iplug2.git
git clone https://github.com/betweentwomidnights/sa3.cpp.git
cd sa3.cpp && build.cmd cuda          # or: build.cmd vulkan / ./build.sh metal
```

If you already cloned without `--recurse-submodules`, run `git submodule update --init --recursive` in
this repo before configuring — `vendor/iPlug2` is empty otherwise and `find_package(iPlug2)` fails.

Fetch the iPlug2 SDKs (git-bash on Windows), then build the plugin:

```
cd foundation-1.2-iplug2/vendor/iPlug2/Dependencies/IPlug && ./download-iplug-sdks.sh
cd ../../../..
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSA3_BUILD_DIR=../sa3.cpp/build-cuda   # or build-vulkan / build-metal
cmake --build build --config Release --target FoundationKeys-vst3 FoundationKeys-clap FoundationKeys-app
```

`download-iplug-sdks.sh` already calls the CLAP and WAM scripts; the WAM one runs `sudo rm` and will ask
for a password, which you can skip — only VST3 and CLAP matter here. Pass `-DCMAKE_BUILD_TYPE=Release`:
the macOS and Linux generators are single-config, so `--config Release` alone leaves you with an
unoptimized build.

The backend's runtime libraries are copied beside the plugin and re-pointed at `@loader_path` on macOS, so
the bundles run off the build machine; libsa3 loads on first render, never at plugin scan. For release
builds, configure this repo with `-DFOUNDATION_KEYS_DEV_MODELS=OFF` and build sa3.cpp with
`-DGGML_NATIVE=OFF` (portable CPU code and CUDA architectures). `build.sh`/`build.cmd` take only a backend
name, so configure that one by hand:

```
cd sa3.cpp && cmake -S . -B build-metal -DCMAKE_BUILD_TYPE=Release -DSA3_BUILD_SAT=ON -DSA3_METAL=ON -DGGML_NATIVE=OFF
cmake --build build-metal --config Release -j
```

The headless engine test renders through libsa3 and checks pitch, layers, mono/poly, headroom, kit reload,
and cancel. It takes a models directory:

```
build/out-test/Release/FoundationKeysEngineTest.exe <models dir>   # Windows
build/out-test/FoundationKeysEngineTest <models dir>               # macOS / Linux
```

## Releases

macOS ships as a signed, notarized zip rather than an installer, and it's Apple Silicon only. Drag
`FoundationKeys.vst3` into `/Library/Audio/Plug-Ins/VST3` and `FoundationKeys.clap` into
`/Library/Audio/Plug-Ins/CLAP` — make that second folder if it isn't there, macOS doesn't create it
until something installs a CLAP. The app goes wherever you like.

That's a gap, not a preference: if you'd like a proper DMG like
[gary4juce](https://github.com/betweentwomidnights/gary4juce) gets, ask nicely in an issue and it'll
happen in the next tagged release. The zip is codesigned and notarized at least, so Gatekeeper won't
fight you about it.

## License

Foundation-1.2 weights: Stability AI Community License (see the model card). The T5 encoder is Apache-2.0.
