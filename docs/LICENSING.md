# Licensing review

Checked against the exact versions pinned in `cmake/Dependencies.cmake` (November 2026).
This is an engineering summary, not legal advice.

## Components used in the plugin today

| Component | Version | Licence | What it means for this product |
|---|---|---|---|
| NeuralAmpModelerCore (Steven Atkinson) | v0.6.0 | MIT | Free to embed and sell. Keep the copyright notice. |
| AudioDSPTools (Steven Atkinson) | `844680d` | MIT | Same as above. |
| Lanczos resampler inside AudioDSPTools (from iPlug2/WDL) | — | zlib-style | Commercial use allowed; keep the notice in the source files; altered versions must be marked. We use it unmodified. |
| Eigen | `bc3b398` | MPL-2.0 | Header-only use is fine in closed-source products. Changes to Eigen files themselves would have to be published. We do not modify Eigen. |
| nlohmann/json | bundled with NAM | MIT | Keep the notice. |
| JUCE | 9.0.5 | AGPLv3 **or** commercial JUCE licence | **Decision needed before release** (see below). |
| VST3 SDK (bundled in JUCE) | — | MIT (Steinberg, 2025) | Free to use. The VST logo/trademark has separate usage guidelines (`VST3_Usage_Guidelines.pdf` in the SDK). |

All of the above are reproduced in `THIRD_PARTY_NOTICES.md`, which must ship with the installer.

### JUCE — the one licensing decision to make

JUCE is dual-licensed. You can either:

1. **Release this plugin's source under the AGPLv3** (the whole plugin becomes open source), or
2. **Buy a JUCE commercial licence** to sell it closed-source. JUCE offers several tiers; check the current terms at juce.com before your first public release.

Nothing else in the dependency list forces either choice. This repository does not yet contain a licence for your own code — add one once you've decided.

## Models, captures and impulse responses

- `.nam` files are licensed by whoever captured them. The plugin ships with **no third-party models**.
- TONE3000's T3K licence lets users load and play downloaded models but forbids redistributing the files without the author's permission. The model library will therefore only manage files the user imports, and "export" will be limited to models the user captured or has rights to.
- NAM's example models used by the tests are MIT-licensed test fixtures from NeuralAmpModelerCore; they are downloaded at build time, not shipped.
- No cabinet IRs are bundled. Any IR pack added later needs written permission from its author.

## Planned components (not yet integrated)

| Component | Licence | Planned use |
|---|---|---|
| neural-amp-modeler trainer (Python) | MIT | Reference for the training port: architectures, loss, data split, `.nam` export format. Not shipped. |
| LibTorch (PyTorch C++ API) | BSD-3-Clause | In-plugin training on CPU / CUDA (Phase 4). Large binary; distribution size to be evaluated. |
| NAM standard capture signal (`v3_0_0.wav`) | to confirm with the NAM author | Played by the CAPTURE page. Redistribution terms must be confirmed before bundling it. |
| NVIDIA CUDA runtime | NVIDIA EULA (redistributable components) | Optional GPU training. Only the redistributable runtime libraries may be shipped. |

## Not used

- Nothing from Kemper or Neural DSP. "Inspired by" refers to workflow only; no code, assets or file formats are copied.
