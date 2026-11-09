# Architecture

One VST3 plugin. Everything — playback, capture, training, library — lives inside it.
The rule that shapes the design: **the audio thread only ever runs DSP**. Every slow
or allocating operation (file I/O, model loading, recording to disk, training) runs on
background workers and hands finished results to the audio thread through lock-free
handoffs.

```
                    ┌────────────────────────── VST3 plugin ───────────────────────────┐
 host audio ──►     │  AUDIO THREAD (real-time, no alloc / locks / I/O)                │
                    │   input select ─► [gate ─► comp ─► stomps] ─► AMP ─► [cab ─► EQ   │
                    │                                     ▲          ─► mod ─► delay   │
                    │                                     │             ─► reverb] ─►  │ ─► host
                    │                      atomic handoff │                            │
                    │  WORKERS ───────────────────────────┴────────────────────────    │
                    │   model loader · library indexer · capture recorder · trainer   │
                    │                                                                  │
                    │  UI (message thread): AMP · CAPTURE · CAB · STOMP · EFFECTS ·    │
                    │                       LIBRARY · SETTINGS                         │
                    └──────────────────────────────────────────────────────────────────┘
   [ ] = later phases
```

## Source layout

| Path | What | Depends on |
|---|---|---|
| `src/engine/` | Real-time DSP engine. Plain C++20, **no JUCE**, so it is unit-tested headlessly. | NeuralAmpModelerCore, AudioDSPTools |
| `src/plugin/` | JUCE `AudioProcessor`, parameters, state, editor. | engine, JUCE |
| `src/plugin/ui/` | Look-and-feel, meters, pages. | JUCE |
| `tests/` | `engine_tests` (headless) and `plugin_tests` (processor + editor). | — |
| `cmake/` | Pinned dependency fetching; NAM core build. | — |

## The amp stage (Phase 1 — implemented)

### `NamModelInstance`
Wraps one genuine `nam::DSP` from NeuralAmpModelerCore v0.6.0.

- Parses the `.nam` JSON once on the loader thread and builds the network with
  `nam::get_dsp`. Every architecture NAM core supports loads (WaveNet A1/A2, LSTM,
  ConvNet, Slimmable containers, …); only mono-in/mono-out models are accepted in the amp slot.
- **Sample rate**: if the host runs at the model's training rate the network runs
  directly. Otherwise it runs at its training rate inside AudioDSPTools'
  `ResamplingContainer` (Lanczos, A = 12) — the same resampler the official NAM
  plugin uses. Files without a stored rate use NAM's 48 kHz convention, and the UI says so.
- Metadata shown in the UI comes only from the file. Missing fields stay empty.

### `AmpEngine`
- **Loader thread**: loads, validates and warms up models, then publishes a
  ready-to-run slot through one `std::atomic<Slot*>`. A newer request supersedes an
  older one still loading. A failed load leaves the current model playing and reports why.
- **Audio thread**: at the top of a block it takes the pending slot (if any), and
  crossfades old → new over 30 ms with a sin²/cos² curve. The old slot goes back to
  the worker through a fixed-size SPSC queue for deletion. No allocation, no locks.
- **Gain staging** (same semantics as the official NAM plugin):
  - *Input calibration*: adds `interfaceInputDbu − model.input_level_dbu`, so the model
    sees the level the real amp saw during capture.
  - *Output mode*: **Raw**; **Normalized** (`−18 dB − model.loudness`); **Calibrated**
    (`model.output_level_dbu − interfaceOutputDbu`, falling back to Normalized when the
    model lacks an output level).
  - All gains are ramped per block (click-free).
- `prepare()` (from `prepareToPlay`) re-targets the playing and pending models to a new
  rate/block size under a mutex shared only with the loader — never with the audio thread.

### Verified by `engine_tests`
| Test | Result on the Linux dev machine |
|---|---|
| Engine vs. NeuralAmpModelerCore directly (A1 WaveNet, A2, LSTM) | bit-identical (max error 0) |
| 48 kHz model on 44.1 / 96 kHz hosts | level within 0.001 dB of native; latency reported (27 / 42 samples) |
| Heap allocations on the audio thread (malloc-level hook), steady state and during model switches | 0 |
| Model switch under a continuous signal | largest sample step during the crossfade ≤ the models' own |
| Invalid / missing files | previous model keeps playing, clear error text |

## Capture & training (Phase 4 — implemented)

| Path | What |
|---|---|
| `src/capture/` | NAM v3 signal verification (MD5), `CaptureTransport` (sample-synchronous play + record, pre-allocated, RT-safe), NAM's latency calibration, level and replicate checks, v3 train/validation cut |
| `src/train/` | `WaveNet` with hand-written backprop, parameters in NAM's `.nam` weight order; `trainWaveNet` (Adam, exponential decay, threaded batches, best-ESR checkpoint); NAM loudness |
| `src/plugin/CaptureController` | Workflow state, background training thread, validation render, A/B `AuditionPlayer`, saving |
| `src/plugin/ui/CapturePage` | The five steps |

**Why a C++ trainer instead of LibTorch.** LibTorch is ~1–2 GB with CUDA and could not be
fetched or tested on the build machine. NAM's classic WaveNet is small enough that a direct
implementation is practical: it uses the same architecture definitions, loss, optimiser and
schedule as NAM's v0.10 trainer, and every exported model is checked against
NeuralAmpModelerCore. The trade-off is CPU-only training. The newer "packed"/A2 architectures
of NAM's latest trainer are not trained in-plugin (they are still *played*).

### Original design notes

**Routing inside a host.** A VST3 cannot open the audio interface itself; the host owns
it. Capture therefore runs through the plugin's own buses:
- The capture signal plays from the plugin's **main output** (routed by the user to the
  interface output that feeds the reamp box).
- The amp's return is read from the plugin's **main input** (stereo layouts allow the
  dry reference on one channel and the amp return on the other) or a **sidechain bus**.
- The CAPTURE page shows the host's actual sample rate, buffer size and channel layout,
  explains what must be routed in the DAW, and verifies it with a calibration loop
  before recording. Device selection itself stays a host setting; the page says so.

**Signal.** The plugin plays NAM's standard capture file (`v3_0_0.wav` — the same file
TONE3000 distributes) so captures are compatible with NAM's trainer and alignment.
Calibration uses its built-in alignment blips to measure round-trip latency.

**Recording.** The audio thread writes into a pre-allocated lock-free ring buffer; a
worker drains it to disk. Playback and capture never block each other.

**Training without Python.** NAM's official trainer is Python + PyTorch. (Originally planned
on LibTorch; implemented as a dependency-free C++ port instead — see above.)
Correctness gate: a model trained in-plugin must (1) export a `.nam` file that
NeuralAmpModelerCore loads, (2) produce output matching the C++ training graph, and
(3) reach an ESR comparable to the Python trainer on the same capture. Only then does
the UI report success.

**Validation.** A/B against the recorded amp reference using the same input, level-matched
and latency-aligned, with null/difference listening and spectra.
