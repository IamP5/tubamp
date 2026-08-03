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

- Sliders (29): input_trim, output_level, gate_threshold, comp_threshold,
  comp_ratio, comp_attack, comp_release, comp_makeup, drive_gain, drive_tone,
  drive_level, amp_input, amp_output, amp_cal_level, amp_slim, cab_lowcut,
  cab_highcut, eq_bass, eq_mid, eq_treble, mod_rate, mod_depth, mod_mix,
  delay_time, delay_feedback, delay_mix, reverb_size, reverb_damping, reverb_mix
- Toggles (13): gate_on, comp_on, drive_on, amp_on, cab_on, eq_on, mod_on,
  delay_on, reverb_on, amp_cal_input, fx1_on, fx2_on, fx3_on
- Combos (2): amp_out_mode, mod_type

JS uses `getSliderState(id)` etc. from the vendored frontend lib; wrap in hooks
(`useSliderParam`, `useToggleParam`, `useComboParam`) that subscribe to BOTH
`valueChangedEvent` and `propertiesChangedEvent` (properties arrive async) and
call `sliderDragStarted/Ended` around gestures (host automation touch).

**These lists are the whole relay surface.** A hosted plugin's parameters are NOT
relays and never enter `SLIDER_PARAM_IDS` / `paramMeta` / `useSliderParam` — see
§External AudioUnit slots below. The 29 sliders stay 29.

### Chain blocks

Twelve block types (`chain::BlockInfo`, `BLOCK_IDS` in `bridge/types.ts`): the nine
built-ins `gate, comp, drive, amp, cab, eq, mod, delay, reverb`, plus the three
external-AudioUnit slots `fx1, fx2, fx3` ("FX Slot 1..3", short name "FX 1".."FX 3").
The fx blocks are absent from the default order — they only appear in the chain once
the user adds them from the [+] picker — and an fx block with no plugin assigned is a
pass-through, so it is legal to have one in the chain forever without loading anything.
Everything else about them is ordinary block behaviour: drag to reorder, remove, and
a `fxN_on` power LED wired to a normal automatable APVTS toggle.

### Native functions (all return JSON; names verbatim)

```ts
getUiState(): UiState                       // full hydration snapshot
setChainOrder(tokens: string[]): void
loadModel(path: string): { error?: string }
clearModel(): void
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
```

```ts
interface UiState {
  chainOrder: string[]
  model: ModelInfo | null       // { path, name, sampleRateHz, loudnessDb?, inputLevelDbu?,
                                //   outputLevelDbu?, gearType?, includesCab,
                                //   isSlimmable, latencySamples }
  ir: FileEntry | null          // { path, name }
  models: FileEntry[]
  irs: FileEntry[]
  presets: PresetInfo[]         // { name, path, favorite, tags }
  currentPresetName: string
  ab: { activeSlot: 0 | 1; aHasState: boolean; bHasState: boolean }
  t3k: { configured: boolean; authenticated: boolean; username: string | null }
  fxSlots: FxSlotState[]        // always 3 entries, indexed by slot
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
"chainChanged"   { chainOrder: string[] }           // onChainChanged incl. state restore
"libraryChanged" { models, irs }
"presetChanged"  { presets, currentPresetName, ab }
"modelChanged"   { model: ModelInfo | null }
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

Auto-selected when `window.__JUCE__` is absent. Full fake `UiState` (the 9 built-in
blocks in the chain plus three fx slots, `fxSupported: true` with a fake plugin list
and one occupied slot with parameters, a loaded model with metadata, 6 presets,
library entries, t3k configured+authed),
param states with real ranges/skew from `docs/research/current-ui-inventory.md`,
fake 30 Hz meters (musical envelope), simulated t3k select/download flows with
progress, latency ~90 samples. Dev-only code path; tree-shaken out is NOT required
(guarded at runtime), but keep it in a separate chunk if trivial.

## UX structure (the board is the app)

Fixed 1120×700 viewport, `--bg-app` with subtle radial wash, dot-grid stage.

- **Board (centerpiece, ~60% height)**: pan/zoom stage (wheel = zoom to cursor,
  ctrl/cmd+wheel fine zoom, space-drag / middle-drag / two-finger = pan,
  double-click empty = zoom-to-fit; all springs per motion.ts). IN and OUT
  terminal nodes; block cards between them in chain order joined by connectors
  (SVG, animated flow chevrons; segment entering the selected card tinted with
  its accent). Block card: ~112×80, icon + short name + power LED (click LED =
  bypass toggle without select), accent treatments per vercel-design.md.
  Interactions: click select, drag to reorder (manual drag + layout animations,
  pick-up scale/shadow, siblings reflow, drop settle spring, ESC/out-of-bounds
  cancel), kebab menu on card (Bypass/Enable, Remove) — NO contextmenu (Logic
  crash risk; suppress globally), [+] node at lane end when blocks remain →
  Vercel-style picker menu of absent blocks. Zoom-to-fit + zoom % control,
  bottom-right. Empty chain → empty-state with "Add a block".
- **Param dock (bottom, slides over board bottom edge)**: selected block's panel;
  content morph via AnimatePresence popLayout; accent header (icon, name, power
  pill, remove affordance). Bodies per current-ui-inventory.md: generic knob rows
  (gate/comp/drive/eq/delay/reverb), mod (type combo + knobs), amp (model mgmt +
  status + T3K + knobs + out-mode + slim when isSlimmable), cab (IR mgmt + cut
  knobs), fx1/fx2/fx3 (plugin picker or loaded-plugin header + "Open editor" +
  a generic knob grid over `FxSlotState.params` — see §External AudioUnit slots;
  never `useSliderParam`). Selection fallback: amp → first → none (but newly added block is
  force-selected). Selection is UI-local state only.
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
- **TONE3000 browser (`features/t3k-browser/`)**: a 380px inspector drawer
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

## Out of scope (unchanged)

Parallel paths/splitter, multiple NAM slots, preset browser overlay with
search/tags, resizable UI (fixed 1120×700; the board's own zoom covers density).
