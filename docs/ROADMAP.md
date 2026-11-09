# Roadmap

A phase is done only when its milestone is demonstrated, not when its UI exists.

| Phase | Milestone | Status |
|---|---|---|
| 1. Core NAM playback | A working VST3 playing genuine NAM profiles | **Done in code and automated tests. Needs a real-DAW check with a guitar (see below).** |
| 4. Integrated Capture Studio | Capture and train a genuine NAM model without leaving the plugin | **Done in code and automated tests (simulated amp). Needs a real amp test.** |
| 2. Cabinet & IR engine | Professional amp + cab processing | Planned |
| 3. Effects & routing | Complete guitar rig | Planned |
| 5. Model & preset management | Library, search, presets, import/export | Planned |
| 6. Professional interface | Full GUI polish | Planned |
| 7. Testing & release | Installers, host matrix, docs, licence decisions | Planned |

## Phase 1 — sign-off checklist

Automated (CI on Windows, macOS, Linux):
- [x] NAM inference bit-identical to NeuralAmpModelerCore
- [x] Correct playback at 44.1 / 48 / 96 kHz with reported latency
- [x] Zero audio-thread allocations, including during model switches
- [x] Click-free model switching
- [x] Session save/restore, missing-file handling
- [x] pluginval strictness 10

Manual — in your DAW, with a guitar:
- [ ] Load a high-gain and a clean `.nam` capture; compare against the official NAM plugin with the same model and settings (they should sound identical)
- [ ] Switch models while playing — no dropouts or clicks
- [ ] Change the project sample rate (44.1 ↔ 48 ↔ 96 kHz) with a model loaded
- [ ] Save, close and reopen the project — model and settings return
- [ ] Note CPU % at your usual buffer size

## Phase 4 — sign-off checklist

Automated:
- [x] Capture signal recognised by NAM's own MD5
- [x] Sample-exact playback/recording through `processBlock`; latency detected to the sample
- [x] NAM's replicate-ESR check catches an amp that changed mid-capture; clipping/silence reported
- [x] Trainer: finite-difference gradient check; exported `.nam` matches NeuralAmpModelerCore to ~1e-8 for all four presets
- [x] Training converges on a real NAM model's behaviour; NAM core reproduces the trainer's validation ESR
- [x] Loudness metadata identical to NAM's Python implementation
- [x] Full workflow in process: level check -> capture -> train -> save -> loads and plays in AMP with its metadata

Manual — with a real amp:
- [ ] Download NAM's `v3_0_0.wav`, load it on step 1, route send/return in the DAW at 48 kHz
- [ ] Level check: latency found, return peak around -12 to -6 dBFS, no clipping
- [ ] Capture and train Standard for 100 epochs; note the time and ESR
- [ ] A/B the result; optionally train the saved `output.wav` with NAM's official trainer and compare ESRs

Known limits: training runs on the CPU (no GPU inside the plugin), captures need the project at 48 kHz, and only NAM's v3 signal is accepted.

## Phase 4 — original plan

1. Routing check & calibration loop (plugin output → reamp → amp/load box → plugin input), level meters, clipping/noise detection, latency measurement with NAM's alignment blips.
2. Capture recorder: plays NAM's standard capture file, records the return through a lock-free ring buffer to disk, cancel/recover.
3. Training: C++ port of NAM's trainer on LibTorch (CPU + CUDA), background worker, live loss/ESR, cancel; exported `.nam` must load in NeuralAmpModelerCore before success is reported.
4. Validation: level-matched A/B of real amp vs. model on the same input, difference monitoring, waveform and spectrum views; save or discard.
5. Capture profiles: several captures per amp (Clean / Crunch / Rhythm / Lead) with their real settings recorded.
