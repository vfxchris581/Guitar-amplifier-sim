# Guitar Amplifier Sim

A guitar amp modeler, profiler, cab loader and effects suite in **one VST3 plugin** —
built on genuine [Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
technology. Windows, macOS and Linux.

> **Status: Phase 1 (core NAM playback) complete.** The plugin loads and plays real
> `.nam` captures. Capture, training, cabinets and effects are the next phases — see
> [docs/ROADMAP.md](docs/ROADMAP.md). Nothing in the UI is a placeholder: sections appear
> as they are built.

## What works now

- Loads any `.nam` model NeuralAmpModelerCore v0.6.0 supports (WaveNet A1/A2, LSTM, ConvNet, …) — via button or drag & drop.
- Output identical to NAM's reference engine (tested bit-for-bit).
- Runs at any host sample rate: models play at their training rate through the same Lanczos resampler the official NAM plugin uses; latency is reported to the host.
- Model switching on a background thread with a 30 ms crossfade — no dropouts, no clicks, no allocation on the audio thread (tested).
- Input calibration and Raw / Normalized / Calibrated output modes, using the levels stored in the model file.
- Shows the model's real metadata (author, gear, tone, levels, ESR…) and nothing it doesn't contain.
- Sessions remember the model; a missing file is reported without losing the reference.
- Peak meters with clip latch, CPU readout.

## Building

Requirements: CMake ≥ 3.24, a C++20 compiler (MSVC 2022, Xcode 15+, GCC 11+/Clang 14+).
Dependencies (JUCE, NAM core, Eigen, AudioDSPTools) are downloaded automatically at pinned versions.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

The plugin is written to `build/src/plugin/GuitarAmpSim_artefacts/Release/VST3/`.
Copy the `.vst3` bundle to your VST3 folder:

| OS | Folder |
|---|---|
| Windows | `C:\Program Files\Common Files\VST3` |
| macOS | `~/Library/Audio/Plug-Ins/VST3` |
| Linux | `~/.vst3` |

Linux build packages (Ubuntu/Debian): `libasound2-dev libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxext-dev libxi-dev libxcomposite-dev libxrender-dev libfreetype-dev libfontconfig1-dev`.

### Pre-built plugins
Every push builds all three platforms in GitHub Actions (tests + pluginval at
strictness 10). Download the VST3 from the run's **Artifacts**. On macOS, unsigned
downloads are quarantined; clear it with
`xattr -dr com.apple.quarantine "Guitar Amplifier Sim.vst3"`.

## Testing

```bash
./build/tests/engine_tests      # NAM parity, resampling, RT-safety, switching, errors, CPU benchmark
./build/tests/plugin_tests      # processor, state save/restore, missing-model handling
```

## Documentation

- [Architecture](docs/ARCHITECTURE.md) — threading model, amp engine, capture/training design
- [Roadmap](docs/ROADMAP.md) — phases and status
- [Licensing review](docs/LICENSING.md) — every dependency, plus the JUCE licence decision
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## Product name

"Guitar Amplifier Sim" is a working name. Change `PRODUCT_NAME` and the IDs at the top
of `CMakeLists.txt` before the first public release (plugin/manufacturer codes must not
change after release, or hosts will treat it as a different plugin).
