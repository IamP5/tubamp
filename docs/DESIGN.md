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

## Signal chain (user-buildable order — see docs/REACT-UI.md)

```
Input Trim → [ user-arranged blocks: Gate | Comp | Drive | NAM | Cab IR | EQ | Mod
              | Delay | Reverb | FX 1 | FX 2 | FX 3 — drag to reorder, add/remove,
              per-block bypass; Comp, Drive, EQ, Mod, Delay and Reverb may each sit
              in the chain up to three times over ]
           → DC Blocker → Output Level
```

The chain order is plugin *state* (comma-separated block tokens), not a parameter: all
APVTS ids stay frozen for AU/automation compatibility. `chain::BlockId` names a block
**instance**, not a type: six of the nine built-in kinds (comp, drive, eq, mod, delay,
reverb) have a pool of 3 instances each (`chain::maxInstancesPerKind`), every instance
its own id, token, tile and real APVTS parameters — a chain block a host cannot
automate is not a block. Instance 1 keeps the kind's original id/token/param ids
(`comp` ≡ `comp_threshold`, …), so state written before the pool existed loads
unchanged; gate, amp, cab and the three FX slots stay singletons. It reaches the audio
thread as one packed `std::atomic<unsigned __int128>` (`src/dsp/ChainOrder.h`: 5-bit
count + 5 bits/entry, lock-free, allocation-free decode — widened from the original
`uint64_t`/4-bit encoding once the instance pool pushed the id space past 15). A
removed block is absent from the order; a bypassed block stays in it with its
automatable `*_on` param off — both keep their gain smoothers in step so re-enabling
never jumps — and a block that re-enters the chain (removed then re-added, or a
state/preset/A-B switch) gets its own `reset()` before it next processes, so it never
replays what it was holding when it left (a removed delay must not resume minutes-old
feedback). A deliberately-empty chain serializes as `"-"`.

A fresh instance starts with **only the amp** (`chain::defaultOrder()`, R1) — everything
else is added by the user. `chain::classicOrder()`, the nine-block arrangement above
(gate through reverb), is not the default any more; it survives as the fallback for
state that predates the user-arrangeable chain (a missing or unparseable `chainOrder`
property) and as the shape every factory preset restores (`PresetManager.cpp` stages it
explicitly around each preset's capture — captured live it would otherwise be {amp},
same as any other fresh instance). `chain::parseOrder` is the tolerant, no-fallback
parse live UI input goes through (unknown tokens dropped, duplicates keep the first
occurrence, `"-"`/`""`/garbage all mean deliberately empty); `chain::parseOrderOrLegacy`
is `parseOrder` plus the classic-order fallback, used only for persisted bytes (state
restore, preset load, A/B recall) — never for a live `setChainOrder` call, where "the
user emptied the chain" must never resurrect a chain nobody asked for.

State written with the instance pool stays readable by a build that predates it, and
vice versa: an order containing at least one of the twelve original ids (or none at
all) round-trips through the `chainOrder` property exactly as before. An order made
entirely of instances 2/3 has no original id in it, so an old build would read it back
as the classic nine — a loud, wrong chain — instead of failing to read it at all; that
case writes `"-"` to `chainOrder` (which the old build reads as "the user emptied the
chain": quiet and wrong in the harmless direction) and the real token list to a new
`chainOrderV2` property, which readers on the current build prefer whenever it is
present.

Two mirrors of the same pair: `uiChainOrder` and `uiChainRows` (row lengths
partitioning the order across the board's rows; `{}` = auto-wrap, the fallback
whenever the published rows don't validate — every length ≥ 1, summing to the order's
length) are guarded together by one `juce::CriticalSection chainLock` (a leaf lock,
the same precedent as `pathLock`/`editorSizeLock`) and published as one consistent
pair by `publishChainOrder()`, so a host autosave racing a restore can never observe a
mismatched split. `packedChain` — the audio thread's copy — is still a single
whole-value atomic store outside the lock, same model as before: concurrent writers
stay benign and the audio thread never sees a torn order. Rule: never hold
`chainLock` across `onChainChanged` — update the mirror, release the lock, then fire.

Latency is only reported while the NAM block is present *and* enabled. The gate keeps
the reference plugin's split behavior generalized: it measures at its own position and
defers its reduction to the model's mono output whenever the amp runs later in the
chain. The amp's own tone stack (`amp_eq_*`, R2) runs inside the amp block itself, on
the mono path, between that deferred gate reduction and the amp-out gain — model →
gate gain → tone stack → amp-out, mirroring the reference plugin's order — so a chain
with no standalone `eq` block still has amp tone controls; whole-amp bypass (`amp_on`
off) skips the EQ too.

Default-order rationale (competitor research — still the shape `classicOrder()`
preserves): gate first (largest S/N headroom at raw input), comp/drive pre-amp,
ambience post. NAM captures are typically amp/preamp-only, so a separate IR cab block
is the NAM-ecosystem convention (unlike ToneX's inseparable captures). Mono through
NAM (models are mono), stereo afterward.

**FX 1–3 (external AudioUnit slots)** are the three blocks that carry no DSP of their
own: each hosts one third-party AU effect the user picks, anywhere in the order. Like
every other block a slot only enters the chain when the user adds it from the picker,
but unlike a built-in block its own DSP instance does not exist the moment it enters
the order — an empty or not-yet-instantiated slot is a bit-exact pass-through, which is
what lets the order reach the audio thread long before the instance behind it exists
(state restore relies on exactly that). The plugin assignment and its state blob
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
amp (NAM in/out gain + normalize), cab (+ low/high cut), eq (bass/mid/treble), mod
(type/rate/depth/mix), delay (time/feedback/mix), reverb (size/damping/mix), output,
plus `fx1_on`/`fx2_on`/`fx3_on` — the FX slots' bypasses. Every id above, plus
instances 2 and 3 of comp/drive/eq/mod/delay/reverb (`comp2_*`/`comp3_*`, …, same
ranges/defaults/steps as instance 1, one array per key indexed by `chain::instanceOf`)
for the chain-instance pool, plus the amp's own tone stack (`amp_eq_on/_bass/_mid/
_treble`, see §Signal chain), is appended at the end of the layout in that order, so
every pre-existing id keeps its index. A hosted FX-slot plugin's own parameters are not
among them and are not exposed to the host: they have no id until the plugin loads —
docs/AU-SLOTS.md §Known limitations.

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
