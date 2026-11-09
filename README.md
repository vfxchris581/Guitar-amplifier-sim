# Guitar Amplifier Sim

A guitar amp modeler, profiler, cab loader and effects suite in **one VST3 plugin** —
built on genuine [Neural Amp Modeler](https://github.com/sdatkinson/NeuralAmpModelerCore)
technology. Windows, macOS and Linux.

> **Status:** Phase 1 (NAM playback), Phase 4 (integrated Capture Studio: capture,
> in-plugin training, A/B validation), Phase 2 (dual-mic cabinet / IR engine) and
> Phase 3 (stompboxes, effects, reorderable chain) are implemented and pass automated
> tests. The model/preset library is next — see [docs/ROADMAP.md](docs/ROADMAP.md).
> Nothing in the UI is a placeholder: sections appear as they are built.

## What works now

- Loads any `.nam` model NeuralAmpModelerCore v0.6.0 supports (WaveNet A1/A2, LSTM, ConvNet, …) — via button or drag & drop.
- Output identical to NAM's reference engine (tested bit-for-bit).
- Runs at any host sample rate: models play at their training rate through the same Lanczos resampler the official NAM plugin uses; latency is reported to the host.
- Model switching on a background thread with a 30 ms crossfade — no dropouts, no clicks, no allocation on the audio thread (tested).
- Input calibration and Raw / Normalized / Calibrated output modes, using the levels stored in the model file.
- Shows the model's real metadata (author, gear, tone, levels, ESR…) and nothing it doesn't contain.
- Sessions remember the model; a missing file is reported without losing the reference.
- Peak meters with clip latch, CPU readout.

**CAPTURE** — profile a real amp without leaving the plugin:
1. *Setup & routing*: drawn connection guide (load box or mic'd cab), send/return channel selection, and NAM's standard capture signal (verified by the same MD5 NAM's trainer uses).
2. *Level check*: 7 s of NAM's signal; measures round-trip latency (NAM's blip algorithm), return level, clipping and background noise.
3. *Capture*: plays the full 3:10 signal sample-synchronously and records the return; runs NAM's validation-replicate consistency check.
4. *Train*: a C++ port of NAM's WaveNet trainer runs in the background on all CPU cores — Standard / Lite / Feather / Nano, epochs, learning rate, decay and batch size configurable, live loss/ESR chart and time remaining, cancel any time.
5. *Compare & save*: level-matched A/B of the real amp vs. the model on held-out audio, difference monitoring, waveform and spectrum views, then save (a standard `.nam` with full metadata, plus a NAM-compatible `output.wav`) or discard.

**CAB** — impulse-response cabinet with two microphones:
- Mono and stereo WAV/AIFF IRs at any sample rate (converted to the host rate), zero-latency convolution.
- Per mic: level, pan, polarity, 0–10 ms fractional delay, high-pass, low-pass, IR length (with fade), normalisation.
- Auto mode bypasses the cab when the loaded amp capture already contains one (from the model's `gear_type`), so you never stack two cabinets by accident.
- IR library with the categories 1x12 / 2x12 / 4x12 / Open-back / Closed-back / Vintage / Modern / Metal / Rock / Clean, search, import and drag & drop. No third-party IRs are bundled.

**STOMP** — noise gate, compressor (threshold, ratio, attack, release, makeup, mix), overdrive, clean boost, distortion and fuzz on a pedalboard you reorder by dragging. The drives are original circuit-inspired designs (mid-hump soft clipping, op-amp hard clipping with gain-bandwidth roll-off, biased transistor fuzz) running 4x oversampled — measured aliasing below -88 dB.

**EFFECTS** — parametric EQ (HPF, LPF, low/high shelves, four bands) with live response curve and spectrum analyser; modulation (chorus, phaser, flanger); delay (digital, analog, ping-pong, tempo sync, feedback filtering, stereo width); reverb (room, hall and plate FDNs, dispersive spring). Units reorder by drag and drop; whole sections can be bypassed; the chain order is saved with the session.

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
./build/tests/trainer_tests     # gradient check, export parity with NAM core, training convergence, loudness
./build/tests/capture_tests     # play/record sync, latency, data checks, capture-to-model
./build/tests/plugin_tests      # processor, state, missing models, full CAPTURE workflow through processBlock
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
