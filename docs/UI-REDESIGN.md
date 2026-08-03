# tubamp UI/UX redesign — buildable signal path + modern dark UI

Goal: replace the fixed two-row panel grid with a **Cortex Control / TONEX-style**
interface: a horizontal, user-buildable signal chain of block tiles (drag to reorder,
add/remove, per-block bypass), a large parameter panel for the selected block, and a
restyled dark "high-tech" look. All existing functionality is preserved (presets, A/B,
TONE3000 browse, model/IR import, metering, settings).

**Hard constraints**
- All APVTS parameter IDs are frozen (AU compatibility, `auval -v aufx Tamp Tuba` must
  pass). No parameter is added or removed. Chain order is *state*, not a parameter.
- Real-time safety rules from `docs/DESIGN.md` still hold. No allocation, locks, or
  string work on the audio thread. Order reaches the audio thread as one
  `std::atomic<uint64_t>` (see `src/dsp/ChainOrder.h`, already written — the shared
  contract, do not modify it).
- Old saved state / presets without a chain order must load with the default order.

## File ownership (two implementers working in parallel — do not cross)

- **DSP agent** owns: `src/PluginProcessor.{h,cpp}`, `src/Parameters.{h,cpp}`,
  `src/dsp/*`, `src/library/PresetManager.{h,cpp}`, `tests/ProcessorSmokeTest.cpp`.
- **UI agent** owns: `src/ui/*` (rewrite/delete/create freely), `src/PluginEditor.{h,cpp}`.
- Neither touches `CMakeLists.txt` (sources are globbed; new/removed files under `src/`
  are picked up automatically).
- The UI agent codes against the processor API below *as specified* even though the DSP
  agent implements it concurrently.

## Processor API contract (DSP agent implements; UI agent consumes)

```cpp
// message thread only
chain::Order getChainOrder() const;          // current order (UI-facing)
void setChainOrder (const chain::Order&);    // publish lock-free to audio thread,
                                             // then invoke onChainChanged if set
std::function<void()> onChainChanged;        // editor subscribes (set/cleared like
                                             // library.onChanged); ALSO invoked after
                                             // any state restore that changes the order
                                             // (setStateInformation / applyStateVar /
                                             // preset load), on the message thread
```

Everything else already exists and keeps its signature: `loadModel/clearModel/
getLoadedModelPath`, `loadIr/clearIr/getLoadedIrPath`, `apvts`, `namEngine`, `library`,
`presets`, `tone3000`, `inputPeak/outputPeak`.

### Chain execution semantics (DSP agent)

- Fixed prologue: input trim gain → input peak meter. Fixed epilogue: DC blocker
  (always on) → output level gain → output peak meter.
- Between them, run blocks in the published order. Blocks absent from the order are
  skipped entirely; blocks present but `*_on == false` are bypassed **but their
  smoothers still `skip()`** exactly as today (comp makeup, amp in/out gains) so
  re-enabling doesn't jump.
- Per-block DSP is moved verbatim from the current `processBlock` — no retuning.
- Amp block keeps mono-collapse → NAM → stereo-expand internally, exactly as now.
- **Gate split rule (generalizes the current behavior):** at the gate's position run
  `measure()`. If the amp block appears *later* in the order **and** is enabled, defer
  `apply()` to the amp block's mono signal right after `namEngine.process` (before the
  amp-out gain), as today. Otherwise apply immediately at the gate's own position.
- Latency/staging logic (`applyStaging`, `setLatencySamples`) unchanged.
- Decode the packed order once per `processBlock` via `chain::unpackTo` (stack array,
  allocation-free).

### State (DSP agent)

- `getStateInformation`: add string property `chainOrder` (`chain::toString`) to the
  `TUBAMP` root. `setStateInformation`: parse with `chain::fromString` (tolerant;
  missing/garbage → default), publish, fire `onChainChanged`.
- `captureStateVar`/`applyStateVar` (A/B slots + presets): add `"chainOrder"` string
  the same way. Verify PresetManager round-trips it (it should flow through these).
- Update `tests/ProcessorSmokeTest.cpp`: set a non-default order (e.g. drive before
  comp, reverb before delay, gate removed), process blocks, assert finite/nonzero
  output; round-trip `getStateInformation`→`setStateInformation` and assert the order
  survives; assert default order when restoring legacy state without the property.

## Visual language (UI agent)

Inspiration: Neural DSP Cortex Control (block-tile grid, bottom parameter strip),
TONEX/AmpliTube (dark chrome, glowing accents). Flat, precise, dark. No skeuomorphism.

### Palette (define once in `TubampLookAndFeel` as `static constexpr` colours)

- Background base `#0C0E12`, elevated `#12151B`, panel `#161A21`, panel-hi `#1C222B`
- Stroke `#262E39`, stroke-hi `#333D4B`
- Text `#E6EAF0`, dim `#8B95A6`, faint `#5A6473`
- Brand accent (logo, preset pill, generic highlights): amber `#FF7A1A`
- Per-block accents — expose as `TubampLookAndFeel::blockAccent (chain::BlockId)`:
  gate `#A78BFA`, comp `#FBBF24`, drive `#F97316`, amp `#F43F5E`, cab `#2DD4BF`,
  eq `#60A5FA`, mod `#4ADE80`, delay `#818CF8`, reverb `#22D3EE`
- Meter gradient: `#34D399` → `#FBBF24` (−12 dB) → `#F87171` (−3 dB)

### LookAndFeel rewrite

- Rotary: 270° track arc (stroke colour, ~3 px), value arc in the slider's
  `rotarySliderFillColourId` (set per-panel to the block accent), small round thumb dot
  at the arc tip, dark face `#12151B` with 1 px stroke ring, needle from centre.
  Hover: value arc brightens slightly. Disabled: everything at 35 % alpha.
- Text buttons: rounded 6 px, `panel` fill, `stroke` border, hover `panel-hi`;
  toggled-on state fills with the accent at 18 % alpha + accent border + accent text.
- Power toggles: pill/dot style — filled accent circle with dark power glyph when on,
  outlined grey when off.
- ComboBox/PopupMenu/TextEditor/AlertWindow: dark, 6 px radius, accent highlight.
- Fonts: system default is fine; sizes — tile labels 10 px, knob labels 11 px
  (letter-spaced uppercase feel via `withExtraKerningFactor(0.08f)` where cheap),
  section titles 13 px semibold, model name 18 px bold.

### Layout (`PluginEditor`, 1120 × 700, fixed size; resizable is a stretch goal —
if attempted use a `ComponentBoundsConstrainer` with fixed aspect and min 950 × 600)

```
┌──────────────────────────────────────────────────────────────┐ 56 px HeaderBar
│ ● tubamp   [◀ Preset Name ▶] [save]        [A|B] [T3K] [⚙]  │
├──────────────────────────────────────────────────────────────┤ ~118 px SignalChainView
│  IN ─[GATE]─[COMP]─[DRIVE]─[AMP]─[CAB]─[EQ]─[MOD]─[DLY]─[REV]─[+]─ OUT │
├──────────────────────────────────────────────────────────────┤ flexible BlockParamPanel
│  ⬤ NAM AMP                              [power]              │
│  model row / knobs for the selected block                    │
├──────────────────────────────────────────────────────────────┤ 64 px footer
│ [trim knob][in meter]      model·latency      [out meter][level knob] │
└──────────────────────────────────────────────────────────────┘
```

Editor background: `#0C0E12` with a very subtle radial/vertical lightening toward the
top-centre (two-stop gradient, barely visible — depth, not decoration).

### SignalChainView (the centerpiece)

- Horizontal lane. Left anchor node **IN**, right anchor node **OUT** (small rounded
  squares with jack/arrow glyphs, `textDim`, not selectable). Between them: one tile
  per block in `getChainOrder()`, plus a trailing **[+]** tile.
- Connector: 2 px horizontal line through tile centres, `stroke-hi`, with the segment
  *entering the selected tile* tinted in that block's accent. Small triangular
  flow-chevron at the midpoint of each segment, `textFaint`.
- Tile: 60 × 60, rounded 12 px, `panel` fill, 1 px `stroke` border. Centre: 26 px
  vector icon (see icon spec) in the block accent when enabled, `textFaint` when
  bypassed. Top-right: 6 px power LED dot (accent when on, dark when off) —
  **clicking the dot toggles the block's `*_on` param without selecting it**.
  Below the tile: 10 px uppercase short name (`shortName` from `chain::blockInfos`),
  `textDim`; accent when selected.
- Selected tile: accent 2 px border + outer glow (`accent.withAlpha(0.25f)`, 3–4 px
  spread) + `panel-hi` fill.
- Bypassed tile: fill stays, icon/label greyed, small diagonal "off" feel via 45 %
  alpha. Still selectable and draggable.
- **Click** tile → select (notifies editor → param panel swaps).
  **Double-click** → toggle bypass. **Drag** horizontally → reorder: the dragged tile
  follows the mouse (slight scale-up + shadow), siblings shift with
  `juce::ComponentAnimator` (~120 ms) to open the gap; on drop call
  `processor.setChainOrder`. **Right-click** → PopupMenu: Enable/Bypass, Remove from
  chain (removal also available while a tile is selected via the param panel; removed
  ≠ disabled: removed blocks leave the order entirely).
- **[+] tile**: dashed 1 px border, `textDim` "+" glyph; click opens a PopupMenu of
  blocks *not currently in the chain* (icon-coloured item text if easy); choosing one
  appends it (then the user drags it into place) and selects it. Hidden when all nine
  blocks are in the chain.
- Subscribe to `processor.onChainChanged` (clear it in the destructor) and rebuild
  tiles; a 10 Hz timer refreshes power-LED/bypass visuals from the APVTS.
- Keep it robust: reorder math must clamp indices; dragging outside the lane cancels;
  no reentrancy from setChainOrder → onChainChanged → rebuild (guard flag).

### Block icons (`BlockIcons.h`, `juce::Path makeIcon (chain::BlockId)` in a 0–1 unit box)

Simple 2 px-stroke geometric glyphs: gate = two tall bars with a gap and a small
threshold tick; comp = two arrows converging on a horizontal line; drive = triangle
wave / clipped sine; amp = amp-head outline (rectangle, top handle, two knob dots);
cab = square with circle + centre dot (speaker); eq = three vertical faders at
different heights; mod = sine wave; delay = three diminishing vertical bars; reverb =
three concentric arcs radiating right. IN = input jack (circle + line), OUT = speaker
cone. Keep each ≤ 20 path ops.

### BlockParamPanel

One component that renders the selected block, with an accent-tinted header row:
20 px icon + display name (13 px semibold, letterspaced) + thin accent underline
(2 px, 48 px wide) + right-aligned power pill (ButtonAttachment to `*_on`) + a subtle
"remove from chain" `⌫`/menu affordance.

Body per block:
- **Generic blocks** (gate/comp/drive/eq/delay/reverb): a centred row of large knobs
  (72 px) with 11 px uppercase labels below and live value text (existing
  `TextBoxBelow`, restyled borderless). Same param→label map as the old panels:
  gate THRESH; comp THRESH/RATIO/ATTACK/RELEASE/MAKEUP; drive GAIN/TONE/LEVEL;
  eq BASS/MID/TREBLE; delay TIME/FDBK/MIX; reverb SIZE/DAMP/MIX.
- **Mod**: type ComboBox (Chorus/Phaser/Tremolo, `ComboBoxAttachment` to `mod_type`)
  + RATE/DEPTH/MIX knobs.
- **Amp**: port *all* behavior from the old `AmpPanel` (model name + live engine
  status, model combo + prev/next/clear, Import, TONE3000 browse with SafePointer
  guards and download progress bar, powered-by label, hint label, IN/OUT knobs,
  out-mode combo, slim slider visible only when `namEngine.isSlimmable()`), relaid
  out for the wider panel: left = model management, right = knobs + mode.
- **Cab**: port from old `CabPanel`: IR combo + prev/next/clear/import, LO CUT /
  HI CUT knobs.
- Set each knob's `rotarySliderFillColourId` to the block accent.
- When the selected block is removed from the chain, fall back to selecting the amp
  block if present, else the first block, else an empty-state label ("Add a block to
  get started").

### HeaderBar (restyle, same functionality)

Left: 8 px brand-accent dot + "tubamp" wordmark (18 px bold, white) . Centre-left:
preset pill (rounded 15 px group: ◀ | preset ComboBox borderless | ▶ | save glyph).
Right: A/B segmented pair (shift-click captures — keep tooltips), settings gear as a
vector glyph button (`juce::Path` gear, not text). Keep `presets.onPresetChanged`
subscription pattern and the Save dialog + Settings DialogWindow launch as-is
(restyle colours only).

### Footer

Input side: 44 px trim knob (brand accent) + vertical segmented meter (10 segments,
rounded 1 px gaps, gradient per palette, peak-hold tick decaying ~1.5 s). Centre:
`textFaint` 11 px info line (model SR · latency — as today). Output side mirrored.
Reuse/extend `PeakMeter` with the segmented style.

### Housekeeping

- Old `BlockPanel/AmpPanel/CabPanel/ModPanel` are superseded — fold their logic into
  the new components and delete the files (they're globbed; leaving dead components
  that reference removed APIs will break the build).
- `SettingsPanel` stays functionally as-is; recolour via the new LookAndFeel only.
- Keep every existing thread-safety pattern: SafePointer for async TONE3000
  callbacks, `onChanged`-style callbacks cleared in destructors, no blocking dialogs.
- 30 Hz editor timer for meters stays; chain view uses its own 10 Hz timer.

## Follow-ups explicitly out of scope for this pass

Parallel paths / splitter (QC-style rows), multiple NAM slots (capture stacking),
preset browser overlay with search/tags, resizable UI if not done as stretch.
