# tubamp React UI — implementation spec

The native JUCE editor is replaced by a JUCE 8 `WebBrowserComponent` (WKWebView)
hosting a Vite + React + TypeScript SPA. The main surface is a Figma/Miro-style
pan/zoom board where the user builds the signal chain. Aesthetic: Vercel/Geist
dark; motion via the `motion` library (motion.dev).

Research backing (all code-cited, read before implementing your slice):
- `docs/research/juce-webview.md` — WebView integration recipe (JUCE APIs verified
  against `build/_deps/juce-src`, CMake bundling, relays, dev-server switch, risks)
- `docs/research/vercel-design.md` — design tokens, typography, component patterns,
  Geist font vendoring
- `docs/research/motion-design.md` — motion tokens, per-interaction choreography,
  WKWebView performance guardrails
- `docs/research/current-ui-inventory.md` — every behavior the new UI must keep
  (and the JUCE workarounds it must drop)

## Invariants (unchanged from docs/DESIGN.md / docs/UI-REDESIGN.md)

- All APVTS param ids frozen; `auval -v aufx Tamp Tuba` must pass.
- Chain order is state, not a parameter; UI publishes it via
  `processor.setChainOrder` only. Order crosses the bridge as token arrays
  (`["gate","comp",...]`, `chain::BlockInfo::token`); empty array = deliberately
  empty chain.
- No UI state persisted in web storage (origin is `juce://juce.backend`) — all
  persistence stays in C++ (APVTS / properties / library files).
- Real-time safety rules untouched; the UI only ever talks to message-thread APIs.

## Repository layout

```
ui/                         # the SPA (npm workspace root for the frontend)
  package.json  vite.config.ts  tsconfig.json  index.html
  src/
    juce/                   # vendored juce-framework-frontend (JUCE 8.0.15) + index.d.ts
    bridge/                 # typed bridge (real + mock) — THE contract, see below
    store/                  # zustand store fed by bridge events
    theme/                  # tokens.css, fonts.css, fonts/*.woff2, motion.ts
    components/             # primitives: Knob, Toggle, Segmented, Menu, Tooltip, Modal, Toast, Meter…
    features/
      board/                # the canvas: stage, grid, nodes, connectors, drag, picker
      panel/                # param dock: generic bodies + amp + cab + mod
      header/               # brand, preset pill, A/B, t3k status, settings
      footer/               # trim/level knobs, meters, info line
      settings/             # settings sheet (t3k key via native prompt, sign out)
    App.tsx  main.tsx
src/WebEditor.h/.cpp        # new editor: WebBrowserComponent + relays + native fns + events
src/PluginEditor.h/.cpp     # DELETED (with all of src/ui/*)
CMakeLists.txt              # webui bundling, target split (see juce-webview.md §CMake)
```

`tubamp_uishot` target and `tools/UiSnapshot.cpp` are removed (WKWebView cannot be
snapshotted headlessly). UI iteration happens against `npm run dev` + mock bridge
in a normal browser. `tubamp_smoke` keeps building exactly as today
(`JUCE_WEB_BROWSER=0`, no editor sources, `TUBAMP_HEADLESS` guard on `createEditor`).

## Bridge contract (single source of truth: `ui/src/bridge/types.ts`)

### Parameters — JUCE relays, id == APVTS id verbatim

Six of the nine built-in kinds (comp, drive, eq, mod, delay, reverb) can sit in the
chain up to three times (`chain::maxInstancesPerKind`); instance 1 keeps the
original id (`comp_threshold`), instances 2 and 3 get `<kind>2_<key>` /
`<kind>3_<key>`, appended at the end of `Parameters.cpp`'s layout so every
pre-existing id keeps its index. The amp's own tone stack (`amp_eq_*`) lives at
the same tail — eq-shaped, but owned by the amp block, not an instance of `eq`.

- Sliders (89): the 29 v1 ids — input_trim, output_level, gate_threshold,
  comp_threshold, comp_ratio, comp_attack, comp_release, comp_makeup, drive_gain,
  drive_tone, drive_level, amp_input, amp_output, amp_cal_level, amp_slim,
  cab_lowcut, cab_highcut, eq_bass, eq_mid, eq_treble, mod_rate, mod_depth,
  mod_mix, delay_time, delay_feedback, delay_mix, reverb_size, reverb_damping,
  reverb_mix — plus, appended: `comp2_*`/`comp3_*` (threshold, ratio, attack,
  release, makeup), `drive2_*`/`drive3_*` (gain, tone, level), `eq2_*`/`eq3_*`
  (bass, mid, treble), `mod2_*`/`mod3_*` (rate, depth, mix), `delay2_*`/`delay3_*`
  (time, feedback, mix), `reverb2_*`/`reverb3_*` (size, damping, mix),
  `amp_eq_bass`/`amp_eq_mid`/`amp_eq_treble`, appended for the stereo chain
  (docs/STEREO.md §4): `delay_ratio`/`delay2_ratio`/`delay3_ratio`,
  `delay_width`/`delay2_width`/`delay3_width`,
  `reverb_width`/`reverb2_width`/`reverb3_width`, and, appended for split/mix
  (docs/SPLIT.md §2): `amp2_input`, `amp2_output`, `split_xover`, `mix_alevel`,
  `mix_blevel`, `mix_apan`, `mix_bpan`, `mix_level`.
- Toggles (30): the 13 v1 ids — gate_on, comp_on, drive_on, amp_on, cab_on, eq_on,
  mod_on, delay_on, reverb_on, amp_cal_input, fx1_on, fx2_on, fx3_on — plus every
  instance's bypass (`comp2_on`, `comp3_on`, `drive2_on`, `drive3_on`, `eq2_on`,
  `eq3_on`, `mod2_on`, `mod3_on`, `delay2_on`, `delay3_on`, `reverb2_on`,
  `reverb3_on`), `amp_eq_on`, and, appended for split/mix (docs/SPLIT.md §2):
  `amp2_on`, `split_on`, `mix_on`, `mix_bphase`. `amp_stereo` (docs/STEREO.md §1)
  is REMOVED — never shipped, superseded by `amp2` as an ordinary chain block.
- Combos (8): amp_out_mode, mod_type, mod2_type, mod3_type — mod is the only v1
  duplicable kind with a choice parameter — plus `delay_mode`/`delay2_mode`/
  `delay3_mode` (stereo chain, docs/STEREO.md §4) and `split_mode`
  (docs/SPLIT.md §2, choices FROZEN: Copy | L/R | X-Over).

JS uses `getSliderState(id)` etc. from the vendored frontend lib; wrap in hooks
(`useSliderParam`, `useToggleParam`, `useComboParam`) that subscribe to BOTH
`valueChangedEvent` and `propertiesChangedEvent` (properties arrive async) and
call `sliderDragStarted/Ended` around gestures (host automation touch).

**These lists are the whole relay surface.** A hosted plugin's parameters are NOT
relays and never enter `SLIDER_PARAM_IDS` / `paramMeta` / `useSliderParam` — see
§External AudioUnit slots below. 89/30/8 is the whole surface; a hosted plugin never
grows it.

### Chain blocks

28 block tokens (`chain::BlockInfo`, `BLOCK_IDS` in `bridge/types.ts`) name block
INSTANCES, not block types. The nine built-in kinds are `gate, comp, drive, amp, cab,
eq, mod, delay, reverb`; six of them (comp, drive, eq, mod, delay, reverb) can sit in
the chain up to three times, each instance its own token, tile and APVTS params —
instance 1 keeps the kind's own token (`comp`), instances 2 and 3 are `comp2`/`comp3`.
gate, amp, cab and the three external-AudioUnit slots `fx1, fx2, fx3` ("FX Slot 1..3",
short name "FX 1".."FX 3") stay singletons. The fx blocks are absent from the default
order — they only appear in the chain once the user adds them from the [+] picker —
and an fx block with no plugin assigned is a pass-through, so it is legal to have one
in the chain forever without loading anything. Everything else about them is ordinary
block behaviour: drag to reorder, remove, and a `fxN_on` power LED wired to a normal
automatable APVTS toggle.

**Split/mix (docs/SPLIT.md)** adds four more singleton tokens: `split` and `mix` are
ordinary tiles with panels (SplitBody/MixBody, below); `amp2` is a second,
freely-placeable NAM engine block — inside a lane or anywhere serially, like any
other block; `lane2` is a hidden STRUCTURAL token marking the lane-A/lane-B boundary
— it is never rendered as a card, never selectable, never has a panel, and its
`BlockInfo` exists purely so `getUiState`/`setChainOrder` round-trip it losslessly.
A flat order fully encodes the fork: `..., split, <lane A ids...>, lane2, <lane B
ids...>, mix, ...` (empty lanes legal). When `chainOrder` contains the full triple in
that relative order, the board renders a lane container in place of the run between
`split` and `mix` — SPLIT tile, an upper half-track for lane A's cards, a lower
half-track for lane B's, MIX tile, connectors fanning out from split and converging
into mix. Any other arrangement (a lone `split`, `mix` before `split`, a missing
`lane2`) is NOT a valid structure and must never be committed as one — the store
carries TS mirrors of the C++ `findStructure`/`sanitizeStructure` helpers beside
`normaliseChainRows` and flattens to serial before any `setChainOrder` call; the C++
`sanitizeStructure` is the backstop, not the primary guard. `amp2` is not structural
and survives flattening like any ordinary block.

**A fresh instance's chain is `["amp"]`** (`chain::defaultOrder()`) — everything else
is added by the user. The nine-block arrangement plugin instances used to start with
(`chain::classicOrder()`) still exists, as the fallback for state saved before the
chain was user-arrangeable and as the shape factory presets restore.

`bridge/types.ts` derives kind/instance identity from `KIND_INSTANCES` rather than
hand-listing 24 entries twice:

- `BLOCK_KINDS` — the nine built-in kinds, in enum order (also the picker's row
  order); `DUPLICABLE_KINDS` — the six that have more than one instance.
- `kindOf(id)` — the kind a token is an instance of (`comp3` → `comp`); an fx slot is
  its own kind.
- `instanceOf(id)` — 0-based instance number, mirroring `chain::instanceOf` (`comp` is
  0, `comp3` is 2); user-facing text shows `instanceOf(id) + 1`.
- `instanceTokensOfKind(kind)` — a kind's instance tokens, instance 1 first.
- `blockRecord(make)` — builds a `Record<BlockId, T>` from one factory over all 24
  tokens; every identity table (`BLOCK_INFO`, `BLOCK_ACCENT`, `KNOB_SPECS`,
  `BLOCK_GLYPH`) is written this way so an instance can never drift from its kind's
  colour, glyph or knob row.

### Native functions (all return JSON; names verbatim)

```ts
getUiState(): UiState                       // full hydration snapshot
setChainOrder(tokens: string[], rows: number[]): void  // order + row lengths travel
                                            // together, one write per gesture;
                                            // rows that do not partition tokens
                                            // are stored as [] (auto-wrap)
loadModel(path: string): { error?: string }
clearModel(): void
loadModelB(path: string): { error?: string }        // engine B (docs/STEREO.md §1);
                                                     // owned by Amp2Body now, not an
                                                     // AmpBody STEREO toggle (removed,
                                                     // docs/SPLIT.md) — same
                                                     // validation/error surface as
                                                     // loadModel, targets engine B only
clearModelB(): void
loadIr(path: string): { error?: string }
clearIr(): void
importModel(): { path?: string; name?: string; error?: string }  // FileChooser (.nam); installs AND loads
importIr(): { path?: string; name?: string; error?: string }     // FileChooser (.wav); installs AND loads
loadPreset(path: string): { error?: string }
savePresetAs(): { error?: string }          // native AlertWindow text prompt (Logic kbd limitation), saves
deletePreset(path: string): { error?: string }
setPresetFavorite(path: string, favorite: boolean): void
abCapture(slot: 0 | 1): void
abRecall(slot: 0 | 1): void
t3kConfigure(): { error?: string }          // native AlertWindow prompt for publishable key
                                            // (optional override — a default client_id is baked
                                            // in via CMake TUBAMP_T3K_CLIENT_ID, so t3k is
                                            // configured out of the box; empty input restores it)
t3kSignOut(): void
t3kStartSelectFlow(): void                  // async; results arrive as events
t3kDownloadModel(model: T3kModel): void     // async; progress/completion as events
t3kSignIn(): void                           // standard OAuth flow; t3kStatus/t3kError follow
t3kBrowse(req: T3kBrowseRequest): T3kBrowseResult      // one catalog page (or {error})
t3kListModels(toneId: number): { models?: T3kModel[]; error?: string }
t3kSetFavorite(toneId: number, favorite: boolean): { error?: string }

// --- external AudioUnit slots (fx1..fx3), see §External AudioUnit slots
fxListPlugins(): { plugins: FxPluginEntry[]; supported: boolean }
fxLoad(slot: FxSlotIndex, identifier: string): { error?: string }   // async; fxSlotChanged reports the outcome
fxClear(slot: FxSlotIndex): void
fxOpenEditor(slot: FxSlotIndex): { error?: string }   // the plugin's own native window
fxSetParam(slot: FxSlotIndex, index: number, value01: number): void
fxBeginGesture(slot: FxSlotIndex, index: number): void
fxEndGesture(slot: FxSlotIndex, index: number): void
fxWatchSlot(slot: FxSlotIndex | -1): void             // which slot gets fxParamValues; -1 = none

// --- window size + embedded hosted editor, see §Window size and embedding
getEditorSize(): EditorSizeLimits
setEditorSize(w: number, h: number): EditorSize            // clamped; returns what was applied
fxSetEmbedSlot(slot: FxSlotIndex | -1): { error?: string } // -1 unmounts; error => pop out instead
fxSetEmbedRect(x, y, w, h): void                           // the hole, in CSS px from the page origin
fxSetEmbedVisible(visible: boolean): void                  // hide without unmounting
fxSetEmbedMinWindow(w: number, h: number): void            // resize floor while embedded; grows
                                                           // the window once if smaller; 0,0 releases
```

```ts
interface UiState {
  chainOrder: string[]           // [] = deliberately empty chain (the "-" sentinel)
  chainRows: number[]            // row lengths partitioning chainOrder; [] = auto —
                                 // C++ keeps no layout it could not validate, and
                                 // the UI wraps for itself
  model: ModelInfo | null       // { path, name, sampleRateHz, loudnessDb?, inputLevelDbu?,
                                //   outputLevelDbu?, gearType?, includesCab,
                                //   isSlimmable, latencySamples }
  modelB: ModelInfo | null      // engine B, managed from Amp2Body (docs/SPLIT.md §5),
                                //   not an AmpBody STEREO section (removed). Same shape
                                //   as `model`, null when no B model is loaded.
                                //   `latencySamples` is the same combined scalar as
                                //   `model`'s — reported host latency is serial-section
                                //   latency + max(laneA, laneB) (docs/SPLIT.md §3)
  ir: FileEntry | null          // { path, name }
  models: FileEntry[]
  irs: FileEntry[]
  presets: PresetInfo[]         // { name, path, favorite, tags }
  currentPresetName: string
  ab: { activeSlot: 0 | 1; aHasState: boolean; bHasState: boolean }
  t3k: { configured: boolean; authenticated: boolean; username: string | null }
  fxSlots: FxSlotState[]        // always 3 entries, indexed by slot
  editorSize: EditorSizeLimits  // { width, height, minWidth, minHeight,
                                //   maxWidth, maxHeight } — max = what fits the display
  fxEmbed: FxEmbedState         // { slot: FxSlotIndex | -1, width, height, error }
  fxSupported: boolean          // false in builds without JUCE_PLUGINHOST_AU;
                                // the FX panels then explain themselves
}
type FxSlotIndex = 0 | 1 | 2
interface FxPluginEntry { identifier; name; manufacturer; version }  // identifier ==
                                // PluginDescription::fileOrIdentifier, the only field
                                // needed to load it
interface FxParamInfo { index: number; name: string; label: string;
                        value: number /* 0..1 */; text: string /* plugin's own */ }
interface FxSlotState {
  slot: FxSlotIndex
  identifier: string; name: string; manufacturer: string
  occupied: boolean   // a plugin is assigned
  missing: boolean    // assigned but not runnable here (almost always: not installed
                      // on this machine). Its saved settings are preserved and
                      // re-saved untouched — never present this as "empty"
  live: boolean       // an instance is actually processing
  loading: boolean    // instantiation in flight
  latencySamples: number
  error: string       // why it is not live, when it isn't
  hasEditor: boolean  // ships its own editor window
  params: FxParamInfo[]   // empty until live
}
interface T3kModel { id: number; name: string; modelUrl: string; size: string;
                     architecture: string; kind: "nam" | "wav" }
// browser types (full definitions in ui/src/bridge/types.ts):
// T3kBrowseRequest { kind: "models"|"irs"; shelf: "all"|"favorites";
//                    sort: "trending"|"newest"|"downloads"; query; gear; page }
// T3kTone { id, title, description, gear, format, imageUrl, creator{username,
//           avatarUrl}, downloadsCount, favoritesCount, favorited, makes, tags,
//           sizes, modelsCount, createdAt }
```

### Events (C++ → JS, `emitEventIfBrowserIsVisible`)

```
"meters"         { in: number, out: number }        // 30 Hz editor timer, linear peaks
"chainChanged"   { chainOrder: string[], chainRows: number[] }  // onChainChanged incl.
                                                    // state restore
"libraryChanged" { models, irs }
"presetChanged"  { presets, currentPresetName, ab }
"modelChanged"   { model: ModelInfo | null }
"modelBChanged"  { modelB: ModelInfo | null }        // engine B, Amp2Body's model
                                                     // (docs/SPLIT.md §5); same "poll
                                                     // and emit on change" reasoning as
                                                     // modelChanged
"irChanged"      { ir: FileEntry | null }
"t3kStatus"      { configured, authenticated }
"t3kToneSelected"{ toneId: number, models: T3kModel[] }
"t3kProgress"    { modelId: number, progress: number }   // [0,1]
"t3kComplete"    { modelId: number, path: string }       // model already imported+loaded C++-side? NO:
                                                          // C++ downloads into models dir, fires
                                                          // libraryChanged; UI decides loadModel(path)
"t3kError"       { message: string; modelId?: number }  // modelId => clear that download row
"fxSlotChanged"  { slot: FxSlotIndex, state: FxSlotState }   // anything structural:
                                                    // plugin, status, param list
"fxParamValues"  { slot: FxSlotIndex, values: number[], texts: string[] }
                                                    // meter-rate; index-aligned with
                                                    // that slot's params; only for the
                                                    // slot set by fxWatchSlot()
"editorSizeChanged" EditorSizeLimits                // the window changed size, INCLUDING when
                                                    // we changed it ourselves — one source of truth
"fxEmbedChanged" FxEmbedState                       // an embed mounted, unmounted, was refused,
                                                    // or reported a new size (can arrive twice)
```

C++ side notes (see juce-webview.md for exact API):
- Member order: relays → WebBrowserComponent → attachments. One WebBrowserComponent
  per editor; `withKeepPageLoadedWhenBrowserIsHidden()`; dtor stops timers and
  clears `onChainChanged` / `library.onChanged` / `presets.onPresetChanged`.
- Resource provider serves the zipped Vite `dist/` from BinaryData with correct
  MIME (`text/javascript` for .js mandatory); root `juce://juce.backend/`.
- Dev switch `TUBAMP_WEBUI_DEV`: goToURL http://localhost:5173 + CORS allowed-origin
  + SinglePageBrowser whitelist. Ship builds compile it out.
- Text entry (preset name, T3K key) happens in native AlertWindow prompts —
  Logic/AS WebViews get no keyboard. The web UI still renders read-only fields +
  buttons that call the native fns.
- All async completion callbacks guard with the aliveFlag/SafePointer patterns
  already used in the codebase.

### External AudioUnit slots (fx1..fx3)

Design record with the reasoning behind each rule: **docs/AU-SLOTS.md**. What the UI
has to know:

**Hosted plugin parameters are not host-automatable in this version.** JUCE's
`WebSliderRelay` (and every other relay) is constructed with a static parameter id at
editor-construction time and attached to one APVTS parameter. A hosted plugin's
parameter list does not exist until the user picks a plugin, and it changes wholesale
when the slot is reloaded — there is no id to freeze, so there is no relay, so there is
no APVTS parameter, so Logic never sees these parameters and cannot write an automation
lane for them. The consequence is deliberate and must be visible in the UI (the FX panel
says so), not discovered by a user whose automation lane silently does nothing.

The slot's own **`fxN_on` bypass is a normal APVTS bool** and automates exactly like
every other block's power switch — that is the automatable handle a slot offers. It is
also why a bypassed-but-loaded slot keeps reporting its plugin's latency (see
AU-SLOTS.md §(c)); the UI must not present bypass as "the plugin costs nothing now".

So hosted params ride plain native calls and one metered event, never relays:

- The list arrives in `FxSlotState.params` (`fxSlotChanged` / `getUiState`), each entry
  carrying its own `index`, `name`, `label`, `value`, `text` — hosted params are
  discovered, not looked up in a frozen table.
- Values are **always normalised 0..1**. That is the only representation a hosted
  parameter guarantees; the number to *show* is the plugin's own `text`, never a
  reconstructed unit string.
- Dragging calls `fxSetParam(slot, index, value01)` wrapped in
  `fxBeginGesture` / `fxEndGesture`, so the plugin sees a proper gesture.
- Live values arrive as `fxParamValues` — values and texts index-aligned with `params`,
  pushed only for the slot named by `fxWatchSlot()`. Call `fxWatchSlot` as the selected
  block changes (and `fxWatchSlot(-1)` when leaving an fx block) so we only meter what
  is on screen. It is separate from `fxSlotChanged` precisely because it fires at meter
  rate and must not churn the parameter list or the React tree.
- `fxOpenEditor(slot)` opens the plugin's *own native* window. On Logic that is also the
  only place its text fields get keyboard focus — same limitation as our own preset-name
  prompt.

Slot rendering follows `FxSlotState`, in this order: `!fxSupported` → explain, no
picker; `!occupied` → picker (`fxListPlugins()`, cheap, call it on demand); `loading` →
spinner; `missing` → name + `error`, and say the settings are kept; `live` → params +
"Open editor" when `hasEditor`. `latencySamples` belongs on screen whenever non-zero.

### Mock bridge (`ui/src/bridge/mock.ts`)

Auto-selected when `window.__JUCE__` is absent. Full fake `UiState` — `chainOrder:
["amp"]`, `chainRows: []` (fresh-instance default, board wraps for itself), three fx
slots, `fxSupported: true` with a fake plugin list and one occupied slot with
parameters, a loaded model with metadata, 6 presets, library entries, t3k
configured+authed, an emulated window size the grip really resizes, one plugin whose
embed is refused and one that reports its editor size twice — param states with real
ranges/skew from `docs/research/current-ui-inventory.md` generated from each kind's
base spec for every instance, fake 30 Hz meters (musical envelope), simulated t3k
select/download flows with progress, latency ~90 samples. Three of the mock presets
carry their own `{chainOrder, chainRows}` and `loadPreset` adopts both; one pair is
deliberately mismatched (rows that do not partition the order) to exercise the
auto-wrap fallback outside C++. Dev-only code path; tree-shaken out is NOT required
(guarded at runtime), but keep it in a separate chunk if trivial.

### Window size and embedding

Two facts drive this whole area, and neither is negotiable:

1. **The window is resized only by us.** `ResizableCornerComponent` is a JUCE-painted
   child and the WebView is a native view covering every one of them, so a real corner
   grip would be invisible. An AU host cannot resize us either — JUCE's wrapper reverts a
   host-driven resize on the next `parentSizeChanged` but propagates a plugin-driven one
   through `childBoundsChanged` → `resizeHostWindow`. So the SPA **draws its own grip**
   (`features/resize`, bottom-right, over the footer's padding gutter) and drives
   `setEditorSize`. `editorSizeChanged` is the single source of truth for the current
   size — it fires for our own writes too, so the page never assumes a request landed.
2. **An embedded hosted editor is a native view composited ABOVE the WebView.** The page
   cannot draw over it. It is a *hole*: `features/embed` reserves the rectangle, reports
   it with `fxSetEmbedRect` (re-measured on any layout change, rate-limited to animation
   frames), and calls `fxSetEmbedVisible(false)` whenever anything must appear on top.
   Overlays never call that themselves — they declare themselves once
   (`components/overlay.tsx`: `<Scrim>` or `useBlockingOverlay`) and the embed is the
   only subscriber, so a future overlay cannot forget. `Tooltip` consults the same module
   for the hole and flips placement rather than landing inside it.

`fxEmbedChanged` can arrive **more than once** for one mount (many AUs only report a real
size after their view attaches). The reserved box is sized from it each time and capped at
the board band, which turns "does not fit" into a measurable shortfall — the embed then
asks for a window that much bigger. It only ever grows: shrinking back would fight a user
who sized the window deliberately.

Minimum size **1000×600** (default 1280×800), derived in `theme/tokens.css` §App metrics from the widest dock
body and the shortest usable board band; same numbers as `kMinEditorWidth/Height` in
`src/WebEditor.cpp`. The C++ clamp is the one that matters (it owns the window).

## UX structure (the board is the app)

Fluid viewport (min 1000×600, see §Window size and embedding), `--bg-app` with subtle
radial wash, dot-grid stage. Header (48) and footer (56) are the only fixed bands; the
board is full-screen underneath them. The panel is not a reserved band — it is an
overlay that opens over the board's bottom edge on selection and gives the space back
when nothing is selected (see the Panel bullet below); block cards keep their fixed
face, because the board's own zoom is the density control.

- **Board (full-screen)**: pan/zoom stage (wheel = zoom to cursor, ctrl/cmd+wheel fine
  zoom, space-drag / middle-drag / two-finger = pan, double-click empty = zoom-to-fit;
  all springs per motion.ts). IN and OUT terminal nodes; block cards in chain order,
  wrapped into rows (`chainRows` — a hand-arranged partition, or `[]` = auto-wrap at
  `ROW_WRAP` = 6), joined by connectors (SVG, animated flow chevrons; segment entering
  the selected card tinted with its accent; a row wrap gets its own path shape — out
  right of the row end, down, back left into the next row's start — rather than a
  straight line across the gap). Block card: ~112×80, icon + short name + power LED
  (click LED = bypass toggle without select), accent treatments per vercel-design.md;
  instance cards caption "COMP 2" etc. from `BLOCK_INFO`.
  - **Drag (`useChainDrag`)** is 2-D, with row geometry computed ONCE at gesture start
    (from the order minus the dragged card) and held frozen for the gesture: the target
    row comes from the pointer's y-band against that frozen layout, the insertion index
    from x, and the preview reflow follows the target rather than chasing the pointer
    directly — this is what keeps the preview from oscillating. Exactly one row-height
    band below the last row is the new-row strip; below that, or outside the stage
    bounds, cancels the drag (mouse only — a Logic WebView has no reliable keyboard, so
    there is no ESC path). Commit happens once on pointerup: one `setChainOrder(tokens,
    rows)` publish per gesture; a row left empty by the drop collapses out of `rows`.
  - **Selection**: a click — pointerup without drag or pan travel — selects; pointerdown
    alone never does (otherwise the panel would open over the card being picked up). A
    selection that would sit under the panel band pans the stage minimally into view
    (ensure-visible). Tapping the empty stage background (`onBackgroundTap`, owned by
    `useStageTransform`, fired on pointerup below the pan-travel threshold with no
    double-click pending) dismisses the selection — never on pointerdown, so panning
    can't close the panel, and double-click-to-fit doesn't dismiss either.
  - Kebab menu on card (Bypass/Enable, Remove) — NO contextmenu (Logic crash risk;
    suppress globally). [+] node at the end of the last row when a kind still has a
    free instance → picker menu of the nine kinds plus the fx slots, a "2/3" count
    badge next to a duplicable kind that has instances left, disabled once a kind is
    exhausted, amp/cab hidden once they're placed. Every connector gap (after IN,
    between cards, in the wrap gutter) additionally carries a hover-revealed insert
    [+] with the same picker that adds the block at that position (`addBlock`'s
    optional `at`), hidden while a drag is in flight. Zoom-to-fit + zoom % control,
    bottom-right (MIN_ZOOM derived from the wrapped rows' bounding box, not a single
    24-wide lane). Empty chain → empty-state with "Add a block".
- **Panel**: a selection-gated overlay card, summoned by clicking a block and closed
  by its own close (✕) button, tapping the block again, `onBackgroundTap`, or the
  block leaving the chain — never opened on hydrate or on an incoming `chainChanged`,
  since nothing auto-selects (R4: the board is the surface, the panel only appears on
  request). Content morph via AnimatePresence popLayout; accent header (icon,
  instance-aware name — a loaded fx slot shows the plugin's name with the slot as a
  tag — power pill bound to the block's `*_on` param, remove-from-chain, close). Body
  dispatch is keyed by `kindOf(selected)`, so every instance of a kind shares its body:
  generic knob rows (gate/comp/drive/eq/delay/reverb, any instance), mod (type combo +
  knobs, any instance), amp (model mgmt + status + T3K + knobs + out-mode + slim when
  isSlimmable + a **Tone** section — Bass/Mid/Treble knobs on `amp_eq_bass/mid/treble`
  plus a power toggle on `amp_eq_on`, KnobRow-style; the amp has its own tone stack
  whether or not an `eq` block is in the chain — AmpBody owns ENGINE A ONLY: model B
  management lives in Amp2Body, not here, see below), amp2 (`Amp2Body`: model B
  management moved wholesale from AmpBody's former STEREO section, docs/STEREO.md §1,
  SUPERSEDED — model picker over the library list, "Use model A" shortcut that calls
  `loadModelB` with A's loaded path, clear, status — plus `amp2_input`/`amp2_output`
  knobs; NO tone stack of its own, the panel notes that users add an `eq` instance if
  they want one, docs/SPLIT.md §3), split (`SplitBody`: `split_mode` ParamMenu
  {Copy, L/R, X-Over} + `split_xover` knob, shown only in X-Over mode), mix
  (`MixBody`: A/B level + pan knobs, phase-B toggle, master `mix_level`), cab (IR mgmt
  + cut knobs), fx1/fx2/fx3 (`fxSlotIndexOf`; plugin picker or loaded-plugin header +
  "Show editor here" (embed, see §Window size and embedding) + "Open plugin window" +
  a generic knob grid over `FxSlotState.params` — see §External AudioUnit slots; never
  `useSliderParam`). `lane2` never opens a panel — it is filtered out of selection
  entirely, same as it is filtered out of every card-rendering path (§Chain blocks).
  CSS system (`theme/tokens.css`): `--panel-band` is 0px at rest and becomes
  `--dock-h + --dock-gap` under `[data-panel-open]`, which `App.tsx` stamps on the app
  root exactly while a block is selected. Everything that must not underlap the open
  panel reads `--panel-band` directly (the board's zoom bar, the embed hole, the
  board's own ensure-visible pan via `useStageTransform`'s inset) or through the
  derived `--overlay-bottom` (the toaster) — never a hardcoded dock height, and
  `--overlay-bottom` is never removed outright even if nothing currently reads it: a
  dangling `var()` on the toaster kills its position silently. Selection is UI-local
  state only, never sent to C++ and never auto-restored.
- **Full-rig captures**: when the loaded model declares a gear type taken through
  a cabinet (`includesCab`, from the .nam's `metadata.gear_type`), the amp states
  it and the cab warns about it — an "AMP + CAB" chip in the amp panel, a "+ CAB"
  pill on the amp card, and, only while both blocks are actually in the path
  (`useRigCab().doubleCab`), a "2× CAB" pill plus amber ring on the cab card and a
  notice with a "Bypass cab" action in the cab panel's action row. Informational
  only: nothing is bypassed automatically, and a capture with no gear metadata
  (most hand-trained files) shows nothing at all.
- **Header (48px)**: brand dot + wordmark; preset pill (prev | name menu | next |
  save); A/B segmented control (click recall, shift-click capture, tooltips,
  slot has-state dots); T3K status glyph; settings gear.
- **Footer (56px)**: IN TRIM knob + meter, info line (model SR · latency),
  meter + OUT LEVEL knob. Meters: motion-value driven, single shared rAF,
  asymmetric attack/release smoothing, segment thresholds per inventory doc
  (time-based decay, not tick-based).
- **Overlays**: settings sheet, T3K tone-selected model list w/ download
  progress, toasts for errors (replaces silent failures where inventory doc
  flags them — e.g. zero-models tone now gets a toast).
- **TONE3000 browser (`features/t3k-browser/`)**: a fixed 380px inspector drawer
  (fixed while the rest is fluid: the row layout is designed at that width, and a
  wider window should give the board the space, not the catalog)
  docked to the right edge, y=48 to the app bottom, spring slide-in, NO
  backdrop — the board stays visible and interactive (validated by prototype
  variant C, branch `prototype/t3k-browser`). Two stacked levels with push
  navigation: level 1 browse (T3K mark + close; Models/IRs segmented; sort
  menu; filter chips — favorites, gear; optional debounced search field;
  compact 64px rows: 44px art, title, creator+gear+A2/IR badge, counts,
  heart), level 2 tone detail (back chevron, art banner with "IN CHAIN" pill
  when the loaded model belongs to the tone, creator/stats/heart, description,
  make/tag chips, file rows with Get → progress → Loaded). Persistent footer:
  "Powered by TONE3000" + loaded capture name. Opened from AmpBody (models
  tab) and CabBody (IRs tab); unauthenticated state shows a TONE3000
  partnership splash with Sign in (t3kSignIn) / key setup (t3kConfigure).
  Tone art: `imageUrl` with a gradient placeholder fallback (remote images may
  be blocked inside the WKWebView origin). Downloads auto-load on completion
  (loadModel/loadIr by `model.kind`). Search is optional sugar — Logic gives
  the WebView no keyboard; chips + sort must carry browsing alone.
  Constraints from docs/research/tone3000-api.md: always `architecture=2` for
  NAM (omitting excludes A2), `format=ir` for IRs, page_size 25, debounce +
  per-session response cache C++-side only (no persistent catalog cache, ToS),
  TONE3000 branding + creator attribution always visible.

Knob primitive: rotary 270°, vertical drag (shift = fine), double-click = reset
to default, wheel nudge; Geist Mono tabular readout; gesture → begin/endGesture.

## Motion & design tokens

`ui/src/theme/tokens.css` implements vercel-design.md §6 verbatim (+ block accent
variables + dim/glow color-mix variants). `ui/src/theme/motion.ts` implements
motion-design.md tokens (durations, eases, springs) as exported constants. No
inline magic values in components — import tokens.

Performance guardrails (motion-design.md §guardrails) are review criteria, not
suggestions: transform/opacity only; no animated box-shadow/filter (pseudo-element
crossfade); motion values for meters/pan/zoom/knob-drag (no per-frame setState);
one rAF driver; `prefers-reduced-motion` respected for entrance/zoom/A-B.

## Build & verification

- `ui/`: `npm run dev` (mock bridge in browser), `npm run build` (vite build,
  `target: 'safari14'` — macOS 11 WKWebView), `npm run typecheck`, `npm run lint`.
- CMake: plugin target flips to `JUCE_WEB_BROWSER=1`, bundles `ui/dist` zip via
  `juce_add_binary_data` with an OUTPUT-based custom command running npm build;
  `TUBAMP_BUNDLED_UI=OFF` escape hatch skips npm and serves dev URL only.
  Console target `tubamp_smoke` unchanged; `tubamp_uishot` removed.
- Gate: `cmake --build build` green, `tubamp_smoke` passes, `auval -v aufx Tamp
  Tuba` passes, `npm run build && npm run typecheck` green, browser screenshot
  review of the mock-bridge UI.

## Out of scope

Preset browser overlay with search/tags. Split/mix (docs/SPLIT.md) covers exactly
ONE split region, no nesting, no more than two lanes, no dynamic split, no split/mix
instance pools, no amp2 tone stack, no per-lane FxHost pools beyond the existing 3
shared slots — see docs/SPLIT.md §6 for the full v1 scope line.
