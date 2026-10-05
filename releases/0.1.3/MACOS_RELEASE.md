# FoundationKeys v0.1.3: M4 release check

Use the `feature/v0.1.3-release` branch until the v0.1.3 tag is created.
Use sa3.cpp's `feature/v0.1.1-release` branch until v0.1.1 is tagged. Update
submodules recursively in both repositories. The expected ggml SHA starts
with `60f49e09`. Keep these sibling checkouts together.

```sh
cd ../sa3.cpp
cmake -S . -B build-plugin-metal-011 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSA3_BUILD_SAT=ON -DSA3_METAL=ON \
  -DGGML_NATIVE=OFF -DSA3_PRIVATE_GGML=ON
cmake --build build-plugin-metal-011 --parallel 4
ctest --test-dir build-plugin-metal-011 --output-on-failure
cd ../foundation-1.2-iplug2 # use your local checkout name
cmake -S . -B build-release-metal-013 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES=arm64 -DSA3_BUILD_DIR=../sa3.cpp/build-plugin-metal-011 \
  -DFOUNDATION_KEYS_DEV_MODELS=OFF
cmake --build build-release-metal-013 --parallel 4 --target \
  FoundationKeys-vst3 FoundationKeys-clap FoundationKeys-app FoundationKeysEngineTest
find build-release-metal-013 -name FoundationKeysEngineTest -type f
# Run that executable with your Foundation-1.2 Keybeds model folder:
# /path/to/FoundationKeysEngineTest /path/to/models 40 F16
```

Check every VST3/CLAP/app bundle contains `libsa3.dylib` and private-named
`libsa3-metal-60f49e09-ggml*.dylib`. The CMake post-build path repair adds
`@loader_path` to each library. Inspect dependencies with `otool -L` and load
from a copied bundle outside the source/build folders. Render, save/reload a
FLAC kit, layer sounds, and cancel; also load the older FoundationKeys first
in a DAW and render the new one.

After checks pass, stage the VST3, CLAP and app from `build-release-metal-013/out`
in one clean folder. Sign nested dylibs first, then each bundle with your
existing Developer ID Application identity, using `codesign --force --options
runtime --timestamp --sign "$IDENTITY"`. Verify with `codesign --verify --deep
--strict`. Create `FoundationKeys-0.1.3-macos-metal-arm64.zip` using `ditto -c -k
--norsrc`, submit with your existing `xcrun notarytool` credentials/profile and
`--wait`, and require status `Accepted`. Generate SHA-256 after packaging.
Keep Windows assets on the same release when uploading the Mac archive.
Do not publish an unnotarized archive. Report the engine/host result and zip
hash before publication so the coordinated release notes can be finalized.
