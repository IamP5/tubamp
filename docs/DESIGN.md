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
- Plugin hosting for the FX slots: `PLUGINHOST_AU TRUE` on `juce_add_plugin` — the
  keyword form, not a bare `JUCE_PLUGINHOST_AU` define, because it also links
  CoreAudioKit/AudioUnit for the AU and Standalone link steps.
- Resampling: `ResamplingNAM` pattern from the official plugin using
  `dsp::ResamplingContainer` from AudioDSPTools (submodule of the core repo).
  Latency reported via `setLatencySamples`.

## Signal chain (user-buildable order — see docs/UI-REDESIGN.md)

```
Input Trim → [ user-arranged blocks: Gate | Comp | Drive | NAM | Cab IR | EQ | Mod
              | Delay | Reverb | FX 1 | FX 2 | FX 3 — drag to reorder, add/remove,
              per-block bypass ]
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

**FX 1–3 (external AudioUnit slots)** are the three blocks that carry no DSP of their
own: each hosts one third-party AU effect the user picks, anywhere in the order. They
are the only blocks absent from `defaultOrder()` — a slot enters the chain when the
user adds it — and an empty or not-yet-instantiated slot is a bit-exact pass-through,
which is what lets the order reach the audio thread long before the instances behind it
exist (state restore relies on exactly that). The plugin assignment and its state blob
live in plugin state under `FXSLOTS`, like the model/IR paths; only `fxN_on` is a
parameter. Hosting a real AU inside an AU forced four decisions against the obvious
implementation — metadata-only discovery, never preparing a hosted instance from
`prepareToPlay`, latency that ignores bypass, and never destroying an instance on the
audio thread. Each one has a specific failure mode behind it: **docs/AU-SLOTS.md**.

UI (Cortex Control-inspired): header (presets, A/B, settings) / signal-chain lane of
accent-colored icon tiles (drag-reorder, [+] add, right-click remove, power LEDs) /
parameter panel for the selected block / footer with trim+level knobs and segmented
meters. It is a React SPA in `ui/`, hosted in a WebBrowserComponent by
`src/WebEditor.*` — spec and bridge contract in docs/REACT-UI.md. There is no headless
screenshot target any more (a WKWebView does not render into a JUCE Graphics context);
UI iteration goes through `npm run dev` and the mock bridge in a browser.

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
mod (type/rate/depth/mix), delay (time/feedback/mix), reverb (size/damping/mix), output,
plus `fx1_on`/`fx2_on`/`fx3_on` — the FX slots' bypasses, appended at the end so every
pre-existing id keeps its index. Those three are the *only* parameters the FX slots
contribute: a hosted plugin's own parameters are not exposed to the host (they have no
id until the plugin loads — docs/AU-SLOTS.md §Known limitations).

## Module map

| Path | Role | Impl notes |
|---|---|---|
| `src/PluginProcessor.*` | chain wiring, APVTS, state (model/IR paths) | `juce::dsp` blocks |
| `src/Parameters.h` | param ids + layout | header-only |
| `src/dsp/NamEngine.*` | load/stage/swap + ResamplingNAM + normalize | NAM core |
| `src/dsp/FxBlocks.{h,cpp}` | Drive, ToneStackEQ, Modulation, DelayFx, ReverbFx, CabSim | thin `juce::dsp` wrappers |
| `src/dsp/FxHost.{h,cpp}` | the three external-AU slots: staged swap, bypass latency, deferred re-prepare | threading contract in the class comment |
| `src/dsp/FxCatalog.{h,cpp}` | AU discovery (metadata-only) + async instantiation | AudioComponent registry walk |
| `src/library/ModelLibrary.*` | scan/import models+IRs | juce::File |
| `src/library/PresetManager.*` | presets, A/B, favorites | JSON via juce::var |
| `src/library/Tone3000Client.*` | PKCE OAuth, search/models/download | juce::URL, StreamingSocket |
| `src/WebEditor.*` | editor shell: relays, native functions, resource provider | `juce::WebBrowserComponent` |
| `ui/` | the UI itself (React + TS + Vite SPA) | bundled into the binary as a zip |
