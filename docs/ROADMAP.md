# Roadmap

A phase is done only when its milestone is demonstrated, not when its UI exists.

| Phase | Milestone | Status |
|---|---|---|
| 1. Core NAM playback | A working VST3 playing genuine NAM profiles | **Done in code and automated tests. Needs a real-DAW check with a guitar (see below).** |
| 4. Integrated Capture Studio | Capture and train a genuine NAM model without leaving the plugin | **Next** (spec: CAPTURE is the milestone after playback) |
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

## Phase 4 — Capture Studio plan

1. Routing check & calibration loop (plugin output → reamp → amp/load box → plugin input), level meters, clipping/noise detection, latency measurement with NAM's alignment blips.
2. Capture recorder: plays NAM's standard capture file, records the return through a lock-free ring buffer to disk, cancel/recover.
3. Training: C++ port of NAM's trainer on LibTorch (CPU + CUDA), background worker, live loss/ESR, cancel; exported `.nam` must load in NeuralAmpModelerCore before success is reported.
4. Validation: level-matched A/B of real amp vs. model on the same input, difference monitoring, waveform and spectrum views; save or discard.
5. Capture profiles: several captures per amp (Clean / Crunch / Rhythm / Lead) with their real settings recorded.
