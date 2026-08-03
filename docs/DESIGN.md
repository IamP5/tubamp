# tubamp — Design

A Logic Pro (Audio Unit) amp signal-chain plugin centered on NAM A2 captures, with a
TONE3000 browser for installing models, a cab IR loader, and a fixed effects chain —
in the spirit of ToneX / HX Stomp / Kemper.

Research backing every decision here lives in `docs/research/`.

## Targets

- macOS AU (`aufx Tamp Tuba`) + Standalone (for dev), JUCE 8.0.15, CMake, C++20.
- Local dev: ad-hoc codesign, `COPY_PLUGIN_AFTER_BUILD` to `~/Library/Audio/Plug-Ins/Components`,
  validate with `auval -v aufx Tamp Tuba`.
- NAM engine: NeuralAmpModelerCore v0.5.4 (`NAM_SAMPLE_FLOAT=1`, `NAM_ENABLE_A2_FAST`),
  sources globbed into a `nam_core` static lib, force-loaded on macOS
  (architectures self-register via static initializers).
- Resampling: `ResamplingNAM` pattern from the official plugin using
  `dsp::ResamplingContainer` from AudioDSPTools (submodule of the core repo).
  Latency reported via `setLatencySamples`.

## Signal chain (user-buildable order — see docs/UI-REDESIGN.md)

```
Input Trim → [ user-arranged blocks: Gate | Comp | Drive | NAM | Cab IR | EQ | Mod
              | Delay | Reverb — drag to reorder, add/remove, per-block bypass ]
           → DC Blocker → Output Level
```

The chain order is plugin *state* (string of block tokens), not a parameter: all APVTS
ids stay frozen for AU/automation compatibility. It reaches the audio thread as one
packed `std::atomic<uint64_t>` (`src/dsp/ChainOrder.h`: 4-bit count + 4 bits/entry,
lock-free, allocation-free decode). A removed block is absent from the order; a
bypassed block stays in it with its automatable `*_on` param off — both keep their
gain smoothers in step so re-enabling never jumps. A deliberately-empty chain
serializes as `"-"`; a missing property (legacy state/presets) restores the default
order shown above. Latency is only reported while the NAM block is present *and*
enabled. The gate keeps the reference plugin's split behavior generalized: it measures
at its own position and defers its reduction to the model's mono output whenever the
amp runs later in the chain.

Default-order rationale (competitor research): gate first (largest S/N headroom at raw
input), comp/drive pre-amp, ambience post. NAM captures are typically amp/preamp-only,
so a separate IR cab block is the NAM-ecosystem convention (unlike ToneX's inseparable
captures). Mono through NAM (models are mono), stereo afterward.

UI (Cortex Control-inspired, spec in docs/UI-REDESIGN.md): header (presets, A/B,
settings) / signal-chain lane of accent-colored icon tiles (drag-reorder, [+] add,
right-click remove, power LEDs) / parameter panel for the selected block / footer with
trim+level knobs and segmented meters. Headless UI screenshots: `tubamp_uishot`
target (tools/UiSnapshot.cpp) renders the real editor to a PNG without a window.

## NAM engine rules (real-time safety)

- `nam::get_dsp()` / `Reset()` / `prewarm()` only ever on a background thread.
- Staged-swap: loader thread fills `stagedModel`, audio thread adopts it at the top of
  `processBlock`; the *old* model is pushed to a garbage slot released on the message
  thread (fixes the reference implementation's audio-thread destructor gotcha).
- `juce::ScopedNoDenormals` in `processBlock`.
- Model native SR (assume 48 kHz when unreported) vs host SR handled by `ResamplingNAM`.
- Loudness normalization: when the model carries loudness metadata and "normalize" is on,
  apply `(-18 dB − modelLoudness)` output makeup.

## TONE3000 integration

Per API research: OAuth 2.0 + PKCE, no anonymous access, ToS prohibits scraping/caching.
Design:
- **Select flow** (`prompt=select_tone`, `format=nam`, `architecture=2`, `preview=true`)
  opened in the system browser; loopback HTTP listener on `127.0.0.1` catches the
  redirect with `code` + `tone_id`; token exchange via `POST /api/v1/oauth/token`.
- Then `GET /api/v1/models?tone_id=…&architecture=2` and download each chosen
  `model_url` **with Bearer header** into the local library. Also `architecture=1`
  fetch on demand (omitting the param excludes A2 — legacy default).
- Publishable key (`t3k_pub_…`) + redirect URI are user-supplied in Settings
  (stored in app properties); the plugin ships with none.
- Downloads only on explicit user action; per-user; "Powered by TONE3000" attribution
  in the browser UI. No bundling of catalog content.
- **Manual import always works with zero setup**: drag-drop / file-chooser `.nam` and
  `.wav` IR import into the library.

## Library & presets

- `~/Library/Application Support/tubamp/models/` (`.nam`), `.../irs/` (`.wav`),
  `.../presets/` (`.json`).
- Preset = full APVTS state + model file name + IR file name + metadata (name, tags).
  Factory defaults created on first run. A/B compare slots (differentiator — no major
  competitor ships true A/B), favorites.
- Kemper-style gain staging: input trim (drive-matching) and per-preset output level
  are separate parameters.

## Parameters (APVTS, ids stable)

See `src/Parameters.h` — the single source of truth. Blocks: input, gate, comp, drive,
amp (NAM in/out gain + normalize), cab (+ low/high cut), eq (bass/mid/treble/presence),
mod (type/rate/depth/mix), delay (time/feedback/mix), reverb (size/damping/mix), output.

## Module map

| Path | Role | Impl notes |
|---|---|---|
| `src/PluginProcessor.*` | chain wiring, APVTS, state (model/IR paths) | `juce::dsp` blocks |
| `src/Parameters.h` | param ids + layout | header-only |
| `src/dsp/NamEngine.*` | load/stage/swap + ResamplingNAM + normalize | NAM core |
| `src/dsp/FxBlocks.{h,cpp}` | Drive, ToneStackEQ, Modulation, DelayFx, ReverbFx, CabSim | thin `juce::dsp` wrappers |
| `src/library/ModelLibrary.*` | scan/import models+IRs | juce::File |
| `src/library/PresetManager.*` | presets, A/B, favorites | JSON via juce::var |
| `src/library/Tone3000Client.*` | PKCE OAuth, search/models/download | juce::URL, StreamingSocket |
| `src/ui/*`, `src/PluginEditor.*` | amp-style UI | custom LookAndFeel |
