# tubamp React UI — implementation spec

The native JUCE editor is replaced by a JUCE 8 `WebBrowserComponent` (WKWebView)
hosting a Vite + React + TypeScript SPA. The main surface is a Figma/Miro-style
pan/zoom board where the user builds the signal chain, with the selected block's
editor in a band under it. Look: one restrained design system — neutral greys, one
accent whose hue follows the block's family (five hues at one lightness), motion
only where it is the signal or a value changing — specified in
`ui/DESIGN.md`, which is the authority for how anything looks or is arranged
(this file is the authority for behaviour and the bridge). Motion via the
`motion` library (motion.dev).

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
      footer/               # status bar: I/O meters, trim/level fields, engine status
      settings/             # settings sheet (t3k sign out, updates)
      update/               # in-plugin update: Settings rows, toasts, words
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

- Sliders (192): the 29 v1 ids — input_trim, output_level, gate_threshold,
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
  `mix_blevel`, `mix_apan`, `mix_bpan`, `mix_level`, and, appended for the
  reverb/delay engines (docs/REVERB-DELAY.md §1.4): `delay{,2,3}_lowcut|highcut|mod|
  color|duck` (15), then `reverb{,2,3}_predelay|decay|lowcut|highcut|mod` (15), then
  `cab_mix` (docs/NAM-IR.md §3, version hint 3), and,
  appended for the gate/comp/drive/EQ/mod engines (docs/FX-ENGINES.md §2.7.1, hint 4):
  `gate_release|hold|range|sc_hpf` (4), `comp{,2,3}_knee|mix|sc_hpf` (9),
  `eq{,2,3}_lowcut|highcut|mid_freq|mid_q|tilt|level|g100|g200|g400|g800|g1k6|g3k2|g6k4`
  (39), then `mod{,2,3}_width|tone|shape|wave` (12), and, appended for the second
  amp's own controls (version hint 5, docs/SPLIT.md §2): `amp2_cal_level`, `amp2_slim`,
  `amp2_eq_bass`, `amp2_eq_mid`, `amp2_eq_treble`, then the second cab's (hint 6,
  docs/SPLIT.md §2a): `cab2_lowcut`, `cab2_highcut`, `cab2_mix`.
- Toggles (39): the 13 v1 ids — gate_on, comp_on, drive_on, amp_on, cab_on, eq_on,
  mod_on, delay_on, reverb_on, amp_cal_input, fx1_on, fx2_on, fx3_on — plus every
  instance's bypass (`comp2_on`, `comp3_on`, `drive2_on`, `drive3_on`, `eq2_on`,
  `eq3_on`, `mod2_on`, `mod3_on`, `delay2_on`, `delay3_on`, `reverb2_on`,
  `reverb3_on`), `amp_eq_on`, and, appended for split/mix (docs/SPLIT.md §2):
  `amp2_on`, `split_on`, `mix_on`, `mix_bphase`, then, appended for the engines
  (docs/FX-ENGINES.md §2.7.1): `comp{,2,3}_auto_makeup`, `mod{,2,3}_color`, then
  (hint 5) `amp2_cal_input`, `amp2_eq_on`, then (hint 6) `cab2_on`.
  `amp_stereo` (docs/STEREO.md §1) is REMOVED — never shipped, superseded by `amp2` as
  an ordinary chain block.
- Combos (43): amp_out_mode, mod_type, mod2_type, mod3_type — mod is the only v1
  duplicable kind with a choice parameter — plus `delay_mode`/`delay2_mode`/
  `delay3_mode` (stereo chain, docs/STEREO.md §4) and `split_mode`
  (docs/SPLIT.md §2, choices FROZEN: Copy | L/R | X-Over), then, appended
  (docs/REVERB-DELAY.md §1.4): `delay_type`, `delay_sync`, `delay2_type`,
  `delay2_sync`, `delay3_type`, `delay3_sync`, `reverb_type`, `reverb2_type`,
  `reverb3_type`, then, appended (docs/FX-ENGINES.md §2.7.1, choices FROZEN):
  `gate_mode`, `comp{,2,3}_type`, `drive{,2,3}_type`, `eq{,2,3}_type|voice` (6), and
  `mod{,2,3}_chorus_mode|phaser_model|trem_voice|sync` (12), then (hint 5)
  `amp2_out_mode`. 274 parameters in all (`tests/golden/params-v6.tsv`).

JS uses `getSliderState(id)` etc. from the vendored frontend lib; wrap in hooks
(`useSliderParam`, `useToggleParam`, `useComboParam`) that subscribe to BOTH
`valueChangedEvent` and `propertiesChangedEvent` (properties arrive async) and
call `sliderDragStarted/Ended` around gestures (host automation touch).

**These lists are the whole relay surface.** A hosted plugin's parameters are NOT
relays and never enter `SLIDER_PARAM_IDS` / `paramMeta` / `useSliderParam` — see
§External AudioUnit slots below. 184/36/42 is the whole surface; a hosted plugin never
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
other block; `cab2` (docs/SPLIT.md §2a) is the cab's second instance in the same way,
with its own IR; `lane2` is a hidden STRUCTURAL token marking the lane-A/lane-B boundary
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
- `blockRecord(make)` — builds a `Record<BlockId, T>` from one factory over all 29
  tokens; every identity table (`BLOCK_INFO`, `BLOCK_GLYPH`) is written this way so
  an instance can never drift from its kind's name or glyph.

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
loadIrB(path: string): { error?: string }   // the second cab (`cab2`); same surface as loadIr
clearIrB(): void
importModel(target?: "amp" | "amp2"): { path?: string; name?: string; error?: string }
                                            // FileChooser (.nam); installs AND loads into
                                            // engine A ("amp", default) or B ("amp2"),
                                            // then modelChanged / modelBChanged
importIr(target?: "cab" | "cab2"): { path?: string; name?: string; error?: string }
                                            // FileChooser (.wav); installs AND loads into
                                            // the first cab ("cab", default) or the second,
                                            // then irChanged / irBChanged
loadPreset(path: string): { error?: string }
savePresetAs(): { error?: string }          // native AlertWindow text prompt (Logic kbd limitation), saves
deletePreset(path: string): { error?: string }
setPresetFavorite(path: string, favorite: boolean): void
abCapture(slot: 0 | 1): void
abRecall(slot: 0 | 1): void
t3kSignOut(): void
t3kStartSelectFlow(): void                  // async; results arrive as events
t3kDownloadModel(model: T3kModel, tone?: T3kTone): void
                                            // async; progress/completion as events. With
                                            // `tone`, C++ writes <file>.t3k.json (ToneSidecar:
                                            // toneId, modelId, title, modelName, creator, RAW
                                            // gear, format, size, architecture, license) on the
                                            // download thread BEFORE libraryChanged/t3kComplete
                                            // The tone's `imageUrl` is kept in the sidecar and
                                            // the photo itself saved as <file>.t3k.art (best
                                            // effort, https, <= 8 MB, must sniff as an image)
t3kEnsureArt(path: string, imageUrl: string | null): void
                                            // a library copy downloaded before photos were kept:
                                            // fetch its tone's photo (no-op when it has one, has
                                            // no sidecar or is outside the library). Always
                                            // resolves; the store calls it before re-loading an
                                            // installed copy picked in the browser
t3kSignIn(): void                           // standard OAuth flow; t3kStatus/t3kError follow
t3kBrowse(req: T3kBrowseRequest): T3kBrowseResult      // one catalog page (or {error})
                                            // req: { kind: "models"|"irs",
                                            //   shelf: "all"|"favorites"|"downloaded"|"created",
                                            //   sort: null|"best-match"|"trending"|"newest"
                                            //         |"downloads"|"oldest",   // null = API default
                                            //   query, gear: raw Gear|null, calibrated?, page }
t3kListModels(toneId: number, format?: "nam"|"ir"): { models?: T3kModel[]; error?: string }
t3kSetFavorite(toneId: number, favorite: boolean): { error?: string }
t3kPromptSearch(initial: string): { text: string | null }   // native AlertWindow search prompt
                                            // (keyboard-less hosts); null = cancelled
t3kInstalled(): { files: { path, toneId, modelId, format }[] }   // library files with a sidecar
openExternal(url: string): { error?: string }   // system browser; https://www.tone3000.com/ only

// --- external AudioUnit slots (fx1..fx3), see §External AudioUnit slots
fxListPlugins(): { plugins: FxPluginEntry[]; supported: boolean }
fxLoad(slot: FxSlotIndex, identifier: string): { error?: string }   // async; fxSlotChanged reports the outcome
fxClear(slot: FxSlotIndex): void
fxOpenEditor(slot: FxSlotIndex): { error?: string }   // the plugin's own native window
fxSetParam(slot: FxSlotIndex, index: number, value01: number): void
fxBeginGesture(slot: FxSlotIndex, index: number): void
fxEndGesture(slot: FxSlotIndex, index: number): void
fxWatchSlot(slot: FxSlotIndex | -1): void             // which slot gets fxParamValues; -1 = none

// --- EQ spectrum analyser, see docs/FX-ENGINES.md §2.8.1
eqWatch(instance: 0 | 1 | 2 | -1): void               // which EQ instance gets eqSpectrum; -1 = none
eqAnalyserSet(prefs: { on?, resolution?, speed? }): void  // stores the analyser's settings (saved with
                                                      // the plugin state); resolution 0|1|2 also sets
                                                      // the FFT size (2048 / 4096 / 8192 at 48 kHz)
                                                      // (the default when the editor opens)

// --- stereo width map, see `width` in UiState
ackNotice(kind: "monoHost"): void                     // the user has seen the mono-instance notice;
                                                      // it is not offered again in this instance
                                                      // (saved with the plugin state, not presets).
                                                      // widthChanged follows with notice: null

// --- window size + embedded hosted editor, see §Window size and embedding
getEditorSize(): EditorSizeLimits
setEditorSize(w: number, h: number): EditorSize            // clamped; returns what was applied
fxSetEmbedSlot(slot: FxSlotIndex | -1): { error?: string } // -1 unmounts; error => pop out instead
fxSetEmbedRect(x, y, w, h): void                           // the hole, in CSS px from the page origin
fxSetEmbedVisible(visible: boolean): void                  // hide without unmounting
fxSetEmbedMinWindow(w: number, h: number): void            // resize floor while embedded; grows
                                                           // the window once if smaller; 0,0 releases

// --- in-plugin update, see §In-plugin update
updateCommand(command: UpdateCommand): { error?: string }  // {} = understood (applied or not);
                                                           // {error} = unparseable. Outcomes
                                                           // arrive as updateChanged
```

```ts
interface UiState {
  chainOrder: string[]           // [] = deliberately empty chain (the "-" sentinel)
  chainRows: number[]            // row lengths partitioning chainOrder; [] = auto —
                                 // C++ keeps no layout it could not validate, and
                                 // the UI wraps for itself
  model: ModelInfo | null       // { path, name, sampleRateHz, loudnessDb?, inputLevelDbu?,
                                //   outputLevelDbu?, gearType?, includesCab,
                                //   isSlimmable, latencySamples, tone? }
                                //   `tone` = the file's TONE3000 sidecar: { toneId,
                                //   title, creator, gear, size, license, url, imageUrl }
                                //   `imageUrl` = the saved photo (<file>.t3k.art), served
                                //   by the plugin's own resource provider at
                                //   <root>t3k-art/<hex of the media path>-<mtime>, or null;
                                //   the amp editor shows it (AmpFace), else the default amp
  modelB: ModelInfo | null      // engine B, managed from the Amp 2 editor (`AmpBody
                                //   engine="b"`, docs/SPLIT.md §5). Same shape
                                //   as `model`, null when no B model is loaded.
                                //   `latencySamples` is the same combined scalar as
                                //   `model`'s — reported host latency is serial-section
                                //   latency + max(laneA, laneB) (docs/SPLIT.md §3)
  ir: IrInfo | null             // FileEntry + what the cab made of it: { path, name,
                                //   channels, sampleRateHz, seconds, reverbLike,
                                //   gainDb, truncated, image, tone? } — `image` is
                                //   "none" | "stereo" | "mono-file" | "mono-path"
                                //   (docs/NAM-IR.md §3)
  irB: IrInfo | null            // the second cab's IR (`cab2`), same shape as `ir`;
                                //   tracked whether or not `cab2` is in the chain
  width?: WidthView             // where the chain is mono and where stereo, and why
                                //   (src/dsp/WidthMap.h). Derived by C++ from the order and
                                //   the parameters, never measured: it holds in silence.
                                //   "stereo" = left and right carry different signals; one
                                //   lane panned alone is mono. Absent from older builds
  missingMedia: MissingMedia    // { model, modelB, ir, irB }: the path a restored session
                                //   or preset named and could not get, else null
                                //   (docs/NAM-IR.md §4)
  models: FileEntry[]
  irs: FileEntry[]
  presets: PresetInfo[]         // { name, path, favorite, tags }
  currentPresetName: string
  ab: { activeSlot: 0 | 1; aHasState: boolean; bHasState: boolean }
  t3k: { configured: boolean; authenticated: boolean; username: string | null }
  fxSlots: FxSlotState[]        // always 3 entries, indexed by slot
  eqAnalyser: { on: boolean, resolution: 0|1|2, speed: 0|1|2 }  // the EQ analyser's saved settings
                                                                 // (defaults: on, 1, 1); speed is the
                                                                 // UI's fall rate, Freeze is not saved
  editorSize: EditorSizeLimits  // { width, height, minWidth, minHeight,
                                //   maxWidth, maxHeight } — max = what fits the display
  fxEmbed: FxEmbedState         // { slot: FxSlotIndex | -1, width, height, error }
  fxSupported: boolean          // false in builds without JUCE_PLUGINHOST_AU;
                                // the FX panels then explain themselves
  update?: UpdateView           // same shape as updateChanged; absent from a build
                                // without the updater (read as { phase: { kind: "off" } })
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
type Width = "mono" | "stereo"
type WidthTurn = "keeps"      // out == in
               | "widens"     // mono in, stereo out, because of `why`
               | "folds"      // stereo in, mono out, because of `why`
               | "monoHost"   // would widen with two output channels; this instance has one
type WidthWhy = "pingPong" | "dualRatio" | "delayMod" | "diffuse"   // delay: mode, Dual ratio,
                                                                    // modulated lines, Diffuse type
              | "reverb" | "chorus" | "tremolo"                     // width > 0 (and mix > 0 where read)
              | "stereoIr"                                          // two-channel IR, no amp folds after it
              | "lanes"     // mixer: two different mono lanes panned apart (widens); split L/R
                            // of a stereo input, or a mixer with every lane hard to one side (folds)
              | "plugin"    // a live hosted AU on our width, fed mono (assumed to widen)
              | "amp" | "amp2" | "pluginMono"                       // folds
interface WidthStep {
  block: string        // chain token, e.g. "delay2", "split", "lane2", "mix"
  out: Width           // the wire leaving this block (split: lane A's entry; lane2: lane B's)
  outB?: Width         // split only: lane B's entry
  turn: WidthTurn
  why?: WidthWhy       // absent for keeps
}
interface WidthView {
  host: { in: 1 | 2; out: 1 | 2 }  // this instance's channel layout
  input: Width                      // stereo exactly when the host gives two input channels
  steps: WidthStep[]                // one per chainOrder entry, same order
  output: Width
  wouldBeStereo: boolean            // the same walk with two output channels is stereo
  notice: "monoHost" | null         // C++'s decision: show the one-time notice now
                                    // (out 1, wouldBeStereo, not yet acknowledged)
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
"meters"         { in: number, out: number, outL: number, outR: number }
                                                    // 30 Hz editor timer, linear peaks. out is
                                                    // the larger of outL/outR; a one-channel
                                                    // instance reports its channel as both
"dynMeters"      { gate: { state: -1|0|1|2, gr: number, level: number },
                   comp: [number, number, number] } // same tick, right after "meters"
                                                    // (docs/FX-ENGINES.md §2.8). gr and
                                                    // comp[i]: max gain reduction since the
                                                    // last tick, dB >= 0 (gate up to 90,
                                                    // Classic up to 120); level: max gate
                                                    // detector level, dBFS, floor -120;
                                                    // comp index = instance. Reset on read
                                                    // to 0 / -120 / 0, so a block that did
                                                    // not run reads idle. state is NOT reset:
                                                    // the last block's 0 closed / 1 hold /
                                                    // 2 open, -1 while the gate is absent,
                                                    // off or waiting for its re-entry reset
"chainChanged"   { chainOrder: string[], chainRows: number[] }  // onChainChanged incl.
                                                    // state restore
"libraryChanged" { models, irs }
"presetChanged"  { presets, currentPresetName, ab }
"modelChanged"   { model: ModelInfo | null }
"modelBChanged"  { modelB: ModelInfo | null }        // engine B, the Amp 2 editor's model
                                                     // (docs/SPLIT.md §5); same "poll
                                                     // and emit on change" reasoning as
                                                     // modelChanged
"irChanged"      { ir: IrInfo | null }               // also when only `image` moved
                                                     // (the chain changed around the cab)
"irBChanged"     { irB: IrInfo | null }              // the second cab, same rule
"widthChanged"   { width: WidthView }               // checked every meters tick, sent only when
                                                    // the map or the notice changed (and once
                                                    // after the editor opens)
"missingMediaChanged" { missingMedia: MissingMedia }
"t3kStatus"      { configured, authenticated, username }
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
"eqSpectrum"     { instance: 0|1|2, pre: number[256], post: number[256] }
                                                    // 30 Hz, only while eqWatch() names an
                                                    // instance (docs/FX-ENGINES.md §2.8.1).
                                                    // Point i at 20 * 1000^(i/255) Hz; value
                                                    // round((dB + 120) * 2), 0..240, dBFS (a
                                                    // full-scale sine reads 0 dB at its
                                                    // point). pre = into the EQ block, post =
                                                    // out of it after its bypass fader; mono.
                                                    // No smoothing or tilt (UI's job). No new
                                                    // samples since the last tick: one all-zero
                                                    // frame, then nothing until audio resumes
"editorSizeChanged" EditorSizeLimits                // the window changed size, INCLUDING when
                                                    // we changed it ourselves — one source of truth
"fxEmbedChanged" FxEmbedState                       // an embed mounted, unmounted, was refused,
                                                    // or reported a new size (can arrive twice)
"updateChanged"  UpdateView                         // the whole update snapshot on every change,
                                                    // progress included (<= 10 Hz); never a delta
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

### In-plugin update

Design and the C++ side: `docs/UPDATES.md`. The page never sequences an update: it
renders the snapshot and sends intents. Hydration (`UiState.update`) plus
`updateChanged` is the whole contract; nothing assumes a re-read on visibility, and a
dropped event costs nothing because the next one is whole. No URL, path, hash or HTTP
detail crosses the bridge in either direction: the release page is opened by C++
(`openReleasePage`), never built in the page, and `openExternal` keeps its
tone3000-only rule.

```ts
type UpdateVersion = string                 // "X.Y.Z"; the page only compares for equality
type UpdateFailure = "network" | "verify" | "noSpace" | "notWritable"
type UpdateManualReason = "systemDomain" | "unexpectedPath" | "readOnly"
                        | "noAtomicSwap" | "notAnAudioUnit"
type UpdateCheckProblem = "unreachable" | "unverified"   // only after a manual check

type UpdatePhase =
  | { kind: "off" }                                       // development build: no update UI
  | { kind: "idle"; problem?: UpdateCheckProblem }        // nothing newer known
  | { kind: "checking" }                                  // manual check running
  | { kind: "available"; version }
  | { kind: "downloading"; version; received: number; total: number }  // bytes
  | { kind: "installing"; version }                       // unpack, inspect, swap; not cancellable
  | { kind: "restartPending"; version }                   // on disk; the host still runs `running`
  | { kind: "failed"; version; reason: UpdateFailure }    // nothing changed; install retries
  | { kind: "manual"; version; reason: UpdateManualReason }  // newer exists, this copy can't
                                                          // replace itself

interface UpdateView {
  running: UpdateVersion        // the code running in this host
  hostName: string              // "Logic Pro", for "Restart Logic Pro to use it."
  autoCheck: boolean            // "Check for updates automatically"
  announce?: UpdateVersion      // toast this version once, then markAnnounced
  disclose: boolean             // first run: toast the daily-check disclosure, then markDisclosed
  systemCopy: boolean           // another tubamp is in /Library/Audio/Plug-Ins/Components
  phase: UpdatePhase
}

type UpdateCommand =
  | { type: "checkNow" }                                  // ignores the daily throttle
  | { type: "install"; version }                          // the version the user saw; ignored
                                                          // if the offer moved on
  | { type: "cancel" }                                    // downloading only
  | { type: "setAutoCheck"; enabled: boolean }
  | { type: "markAnnounced"; version }
  | { type: "markDisclosed" }
  | { type: "openReleasePage" }                           // C++ builds the URL
```

C++ ignores a command that means nothing in the current phase and pushes a fresh
`updateChanged`, so a stale page corrects itself. `announce` and `disclose` are
cleared C++-side for every editor, instance and host on the account. Both unions are
switched exhaustively in the UI (`features/update/words.ts`) and the mock, ending in
`unreachable(x: never)`, so a new phase or command fails `npm run typecheck`.

The C++ side lives in `src/update/Bridge.cpp` (`viewJson`, `parseCommand`), which
produces and reads exactly these shapes; `WebEditor` only converts the JSON text to and
from `juce::var`. `ui/src/bridge/update-contract.json` holds one sample of every phase,
failure, manual reason, check problem and command, plus commands C++ must refuse. Both
sides check it: `npm run typecheck` holds it to `types.ts`
(`update-contract.check.ts`), and `tubamp_update_tool test` holds `viewJson` and
`parseCommand` to it. Change a shape in `types.ts` and update the golden file with it, or
one of the two fails. C++'s three ways a download can fail to match the release
(integrity, archive, wrong bundle) all reach the page as `"verify"`. A build with
`TUBAMP_UPDATES=OFF` sends no `update` field and registers no `updateCommand`.

UI (`features/update/`): a toast "tubamp 0.4.0 is available" with **Update** (or
**Get the installer** in `manual`), once per version; a one-shot toast "tubamp checks
GitHub once a day for a new version." with **Turn off**. An install started from the
toast reports through a toast (progress, then installed or failed with its one action),
because whoever clicked it is not looking at Settings. The Settings sheet carries a
Version row (running version, the state in words, the one action the phase allows:
Update, Cancel, Try again, Get the installer or Check now; a 2px accent progress bar
while downloading; "What's new" → `openReleasePage`; the manual reason and the
system-copy warning as quiet lines) and the "Check for updates automatically" switch.
The Settings button carries a 6px accent dot while `available` or `manual` is offered
and the sheet has not yet shown that version this session.

Mock flags, combinable as `?update=a,b`: `available` (0.4.0 offered and announced),
`found` (Check now finds 0.4.0), `checkfail`, `disclose` (first run), `fail-download`,
`fail-verify`, `fail-space`, `fail-write`, `manual` (a /Library copy), `restart`,
`systemcopy`, `slow` (6× every step), `off`. Default: up to date. Update walks
downloading (progress) → installing → restartPending; Cancel works mid-download.

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

Fluid viewport (min 1000×600, see §Window size and embedding), a flat `--ds-bg`
window. Header (44) and status bar (36) are the only fixed bands; the board is
full-screen between them. The block editor is not a reserved band — it is a flush
296px band that opens on the board's bottom edge on selection and gives the space
back when nothing is selected (see the Panel bullet below); block tiles keep their
fixed face, because the board's own zoom is the density control.

**Look and arrangement (redesign of 2026-11, `ui/DESIGN.md`).** The bullets below
describe behaviour, and all of it still holds. Where they describe appearance —
per-block accent colours, power LEDs and pills, flow chevrons, knob-row bodies,
count badges, card and drawer sizes — `ui/DESIGN.md` supersedes them:

- **Tiles** are 148×48 text tiles: glyph, the block's short name, and one line
  saying what is in it (the type, the capture's or IR's title, the plugin's name).
  Bypass is a power icon button; a bypassed tile dims. Selected = accent ring.
  The glyph sits on a chip in the family's hue (`BLOCK_INFO[id].family`,
  `data-family` on the tile). Wires are plain 1.5px lines with a second, brighter
  stroke whose opacity follows the meters (input along the chain, output on the
  last stretch). In / Out are words at the wire's ends.
- **Add block** is a two-column picker in sections (Dynamics, Gain, Tone,
  Modulation and time, Plugins, Routing); a kind with no free instance is disabled.
  Removing a block or a split shows a toast with Undo (`features/board/undo.ts`,
  one `setChainOrder`). Zoom-to-fit and the first framing never exceed 100%.
- **Header:** wordmark, preset stepper + menu, "Save as", A/B with a pointer-only
  "Save to" menu, a "TONE3000" button that opens the browser, Settings.
- **Status bar** (`features/footer`): Input meter + trim field, the engine status
  once (`48 kHz · 1.9 ms`), Output field + meter. No knobs.
- **Editors** share one grammar (`features/panel/ui.tsx`, `display.tsx`): the
  block's type as a stepper beside its name in the band's header, a display that
  draws what the effect does from its own parameters (with handles for direct
  edits), then knobs — the two or three that define the effect large, the rest
  small. Every parameter has exactly one control. Below ~780px of band width
  (`data-narrow`) the display is dropped. `ui/DESIGN.md` §Editors has the table of
  what each editor shows. A dev-only page renders one editor on its own:
  `/editor-harness.html?block=delay&w=1180`.

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
  generic knob rows (gate/comp/drive/eq, any instance), mod (type combo +
  knobs, any instance), delay (`DelayBody`: TYPE/MODE/SYNC menus above ten knobs; with SYNC ≠ Off, TIME is
  dimmed via `KnobRow`'s `disabledKeys`; picking a type moves HI CUT, COLOR and DUCK
  to that type's starting points), reverb (`ReverbBody`: TYPE menu above nine knobs;
  picking a type moves DECAY and DAMP to that type's starting points — the tables live
  in `features/panel/typeDefaults.ts`, mirroring the C++ ones), amp (model mgmt + status + T3K + knobs + out-mode + slim when
  isSlimmable + a **Tone** section — Bass/Mid/Treble knobs on `amp_eq_bass/mid/treble`
  plus a power toggle on `amp_eq_on`, KnobRow-style; the amp has its own tone stack
  whether or not an `eq` block is in the chain). `amp2` is the amp kind's second
  instance (`KIND_INSTANCES.amp = ["amp", "amp2"]`, exactly two), named "Amp" like the
  first: the same `AmpBody block={token}` with the same controls, each bound to
  `${token}_*` (`amp2_out_mode`, `amp2_cal_input`, `amp2_eq_bass`, …). The tile's
  subtitle, the capture's name, tells the two apart. The picker has one "Amp" row that
  adds the first free instance. `ui/src/store/amps.ts` (`AMP_SLOTS`) is the one place
  that knows which bridge slot holds each amp's capture (`model`/`modelB`,
  `loadModel`/`loadModelB`, `missingMedia.model`/`.modelB`); "Import file" passes the
  token to `importModel(target)`. The lead is `AmpFace` — the capture's photo or the
  default amp, "Choose an amp" when empty; the amp itself and the ⋯ button open one
  menu: Browse TONE3000, Import file, the library, Remove capture; ‹ › step the
  library. "Dual amp" seeds the second amp from the first on creation only: its
  capture when it has none, and every control (`copyAmpSettings`). Where a sentence
  must say which amp (the TONE3000 target, load toasts), it says "Amp 1" / "Amp 2"
  only while both are placed, plain "Amp" otherwise), split (`SplitBody`: `split_mode` ParamMenu
  {Copy, L/R, X-Over} + `split_xover` knob, shown only in X-Over mode), mix
  (`MixBody`: A/B level + pan knobs, phase-B toggle, master `mix_level`), cab (`CabBody
  block={token}`: the IR and Low cut, High cut, Mix on `${token}_lowcut|highcut|mix`).
  `cab2` is the cab kind's second instance (`KIND_INSTANCES.cab = ["cab", "cab2"]`,
  exactly two), named "Cab" like the first and told apart by its IR's name on the tile;
  the picker's one "Cab" row adds the first free instance. `ui/src/store/cabs.ts`
  (`CAB_SLOTS`) is the one place that knows which bridge slot holds each cab's IR
  (`ir`/`irB`, `loadIr`/`loadIrB`, `missingMedia.ir`/`.irB`, `importIr(target)`). The
  lead is the amp's `AmpFace` with `kind="cab"`: the IR's TONE3000 photo
  (`ir.tone.imageUrl`, the same art pipeline as a capture's), else the default cabinet,
  "Choose an IR" when empty; the picture and ⋯ open one menu (Browse TONE3000, Import
  file, the library, Remove IR), ‹ › step the library. The "capture already has a cab"
  note is the first cab's only, since it reads the first amp's capture. fx1/fx2/fx3 (`fxSlotIndexOf`; plugin picker or loaded-plugin header +
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
- **TONE3000 browser (`features/t3k-browser/`)**: a fixed-width inspector drawer
  (`--t3k-w`, 400px — fixed while the rest is fluid: the row layout is designed at
  that width, and a wider window should give the board the space, not the catalog)
  docked to the right edge between the header and the status bar (the output
  meter stays in view while tones are auditioned), slide-in, NO backdrop — the
  board stays visible and interactive. While it is open App.tsx stamps
  `data-t3k-open` on the root, the block editor (`.dock`) ends at the drawer's
  left edge instead of running under it, and toasts shift left of it
  (`--t3k-inset`); the band's height and everything anchored to it are unchanged.
  Opened from the header's TONE3000 button as well as from the amp and cab
  editors. Rows are 56px (thumbnail, title, creator · gear, downloads; favourite
  and a Load → progress → Loaded / Retry control); the detail view lists the
  pack's files with the same control per file.
  - **Target.** Opened from AmpBody (`openT3kBrowser("models", block)`) or
    CabBody (`"irs"`); without a target, models go to the selected block when
    it is an amp, else `amp`. The header always says where a load lands: "Into
    Amp" / "Into Cab", and with both amps placed a segmented switch "Amp 1 |
    Amp 2" picks the engine. `selectT3kTarget` falls back to whichever amp is
    placed. IRs load into a cab the same way: `browser.cabTarget`, the cab the
    browser was opened from, and with both cabs placed a "Cab 1 | Cab 2" switch.
    "Loaded"/"in Amp 2" markers compare
    the target slot's path with the tone's files (sidecars from `t3kInstalled`
    + this session's downloads), placed amps only.
  - **Level 1 (browse).** Models/IRs segmented + Sort menu (Best match — only
    with a query, the default while searching —, Trending, Newest, Most
    downloaded, Oldest; plus "Calibrated only" for models on All, shown as a
    dot on the button); search field + keyboard button (native prompt
    `t3kPromptSearch`, because Logic gives the WebView no keyboard); shelf tabs
    All / Favorites / Recent (`/tones/downloaded`) / Mine (`/tones/created`)
    with the official empty-state copy; gear chips per kind from the real Gear
    enum (models: amp, amp-cab, pedal, outboard, experimental; IRs: cab,
    space). Search: 300 ms debounce, ≥ 2 chars (or empty), Enter submits at
    once, newer lists supersede older (list key = request minus page).
    Rows (64px): art, title, creator · gear · A2/IR · downloads, heart, and a
    one-click **Load** (`t3kLoadTone`): resolve files → default (NAM
    `standard`, else first; IR first) → reuse the library copy (sidecar
    modelId, else slugified name) or download → load into the target. States
    idle → resolving → % → Loaded / Failed + Retry (error on the meta line);
    one in flight per tone; the latest pick per target owns the slot (an older
    download that finishes later only lands in the library, with a toast).
    NAM tones with no A2 files show "No A2". Row body opens level 2.
    Paging: IntersectionObserver 320px ahead of the end, one page request per
    list at a time; a Load more / Retry row is the fallback.
  - **Level 2 (detail).** Banner with gear/format and "In Amp/Cab" pills
    ("Amp 1"/"Amp 2" with both amps placed), creator/stats/heart, primary "Load into …" (same default + states),
    description, make/tag chips, license label (when the API sends one), "Open
    on tone3000.com" (`openExternal`), files sorted default-first with
    size/architecture badges, "default" and "in library" notes, Get/Load →
    progress → Loaded per file.
  - **Memory.** Request (kind, shelf, sort, query, gear, calibrated), loaded
    pages (`t3kLists`, ≤ 12 lists), per-list scroll offset, the open detail
    tone, file listings and favorite overrides all live in the store for the
    editor's lifetime: reopening is instant and lands where the user was.
    Opening from another panel retargets kind/target, resets gear on a kind
    change and closes a detail of the other format. A sign-in/out clears it.
  - Persistent footer: what the target slot holds + "Powered by TONE3000".
    Unauthenticated: partnership splash with Sign in / key setup. Tone art:
    `imageUrl` with a gradient fallback.
  - Constraints from docs/research/tone3000-api.md: always `architecture=2`
    for NAM (omitting excludes A2), `format=ir` for IRs, page_size 25,
    in-memory session caches only (C++ 4-minute response cache keyed on the
    full request; profile lists fetched whole once per window), downloads only
    on an explicit click, branding + creator attribution always visible.
    Transport (Tone3000Client): 429 honours `Retry-After` (fails fast until it
    expires, "TONE3000 is limiting requests — try again in N s"), offline and
    timeouts get their own messages, 5xx/429 on refresh keep the session, one
    401 → refresh → retry.
  - Mock flags: `?t3kfail` (downloads die), `?t3kfavfail`, `?t3kslow` (6×
    latency), `?t3k429`, `?t3koffline`, `?t3kprompt=text` (scripted native
    prompt; bare = cancel).

Knob primitive: rotary 270°, vertical drag (shift = fine), double-click = reset
to default, wheel nudge; Geist Mono tabular readout; gesture → begin/endGesture.

## Motion & design tokens

`ui/src/theme/tokens.css` holds the design system's `--ds-*` tokens (surfaces,
lines, text, the accent and its per-family hues under `[data-family]`, status,
meters, plot ink, type scale, radii, spacing, motion) and the app metrics;
`ui/DESIGN.md` says how they are used. Live motion has three sources, all in
`ui/src/hooks`: `useMeters`, `useDynMeters` and `useSignal` (the input level's
last 8 s, for displays that show an effect answering what was played); displays
write them to the DOM through `useLive` and ease parameter jumps with `useEase`
(`features/panel/display.tsx`).
`ui/src/theme/motion.ts` exports the same timings and springs for `motion`
transitions; `ui/src/theme/words.ts` is the one place labels and values are
formatted (`paramLabel`, `paramValue`, `engineStatus`). No inline magic values in
components — a colour, font, radius, shadow or duration only through a token.
`node tools/design-lint.mjs src` (from `ui/`) fails on colour literals, gradients,
uppercase, letter-spacing, blur, off-token shadows, looping animation, off-scale
sizes and unicode icons.

Performance guardrails (motion-design.md §guardrails) are review criteria, not
suggestions: transform/opacity only; no animated box-shadow/filter (pseudo-element
crossfade); motion values for meters/pan/zoom/knob-drag (no per-frame setState);
one rAF driver; `prefers-reduced-motion` respected for entrance/zoom/A-B.

## Build & verification

- `ui/`: `npm run dev` (mock bridge in browser), `npm run build` (vite build,
  `target: 'safari14'` — macOS 11 WKWebView), `npm run typecheck`, `npm run lint`,
  `node tools/design-lint.mjs src`.
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
instance pools, no per-lane FxHost pools beyond the existing 3
shared slots — see docs/SPLIT.md §6 for the full v1 scope line.
