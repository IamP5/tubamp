# Current native JUCE UI — exhaustive functional inventory

Purpose: a checklist the React rewrite must satisfy behaviorally. Organized by
component, cross-referenced to source. "WORKAROUND" = a JUCE quirk-fighting
detail the React port should drop; "DOMAIN" = real behavior that must be kept.

Overall shell: `src/PluginEditor.h/.cpp`
- Fixed size window: **1120 x 700**, not resizable in code shown.
- Layout top-to-bottom: HeaderBar (56px) → SignalChainView (118px) → BlockParamPanel
  (remaining area minus footer, reduced by 16,14 margin) → footer (64px band).
- Footer band painted as an elevated plate (`bgElevated`) with a 1px top stroke line
  (`footerBounds.withHeight(1)`), drawn in the editor's own `paint()`, not a child
  component.
- Background: flat `bg` fill + a very subtle radial-ish gradient glow from
  top-center (bg.brighter(0.08)) down to `bg` at 85% height — pure decoration, not
  behavior; can be reproduced as a CSS radial-gradient or dropped.
- Editor owns a single shared `TubampLookAndFeel` instance, set/cleared in
  ctor/dtor. DOMAIN concept: all block-accent colors, meter colors, fonts route
  through this one style object — for React this is just a shared theme/tokens file.
- `juce::TooltipWindow` with 700ms hover delay attached to the editor — every
  tooltip text noted below should surface as a native-feeling tooltip with roughly
  that delay in the web UI.
- Footer controls (not owned by HeaderBar/ChainView/ParamPanel):
  - **IN TRIM** rotary knob → `params::inputTrim` (dB, [-24,24], default 0, step 0.1).
    No visible text box (`NoTextBox`), but `setPopupDisplayEnabled(true, true, this)`
    shows a floating value bubble while dragging/hovering — DOMAIN: must reproduce a
    transient value readout during drag.
  - **OUT LEVEL** rotary knob → `params::outputLevel` (dB, [-60,12], default 0, step 0.1).
    Same popup-display behavior.
  - Labels "IN TRIM" / "OUT LEVEL" under each knob, 10px kerned bold, `textDim` color.
  - `PeakMeter inputMeter, outputMeter` — thin vertical meters flanking each knob
    (10px wide `.reduced(0,6)` slot), see PeakMeter section.
  - `infoLabel` centered, 11px, `textFaint`: live text built each timer tick (see
    Timers below).
- **Timer**: editor runs a **30 Hz** `juce::Timer` (`startTimerHz(30)`), each tick:
  1. `inputMeter.pushLevel(processorRef.inputPeak.exchange(0.0f))` — DOMAIN: peak
     value is read-and-reset atomically each tick; the meter only ever sees
     "peak since last poll" (~33ms windows at 30Hz).
  2. Same for `outputMeter` / `outputPeak`.
  3. Rebuilds `infoLabel` text: if `namEngine.hasModel()`, prefix
     `"Model SR {sampleRate int} Hz   ·   "` (middot via UTF8 codepoint, NOT a
     plain "·" char literal — WORKAROUND for JUCE mojibake, irrelevant in JS/HTML
     which is UTF-8 native), always followed by `"Latency {latency} smp"` where
     latency = `processorRef.getLatencySamples()`.
- Wiring done once in the ctor (not re-subscribed): `chainView.onSelectionChanged`
  → `paramPanel.setBlock(id)`; `paramPanel.onRemoveBlock` → `chainView.removeBlock(id)`.
  Initial `paramPanel.setBlock(chainView.getSelection())` because SignalChainView
  resolves its own initial selection in its own constructor (see below) — DOMAIN
  ordering: selection resolution happens chain-view-first, panel mirrors it.

---

## HeaderBar (`src/ui/HeaderBar.h/.cpp`)

Layout (from `resized()`, area reduced 20px each side horizontally):
- Right-anchored, in order from the right edge: settings gear (32×32 slot, drawn
  30×30 centered) → 16px gap → B button (36×36 slot, drawn 36×28) → A button
  (immediately left of B, no gap, `ConnectedOnRight`/`ConnectedOnLeft` visually
  fuses A|B into one segmented pair) → preset pill.
- Preset pill: fixed X start = **170px** from left, width = `min(380, available)`
  where `available = max(160, slotAButton.getX() - 24 - 170)` — i.e. pill never
  collides with the A|B pair; height 30px, vertically centered. Painted as a
  rounded-rect panel with 1px stroke border (radius = half height, full pill shape).
  Inside the pill (reduced 4,3): prevButton (24px) | preset combo (flex) |
  nextButton (24px) | saveButton (24px), i.e. **order is prev, combo, next, save**
  reading left→right but save/next are laid out via `removeFromRight` so visually
  it's `[prev][combo…][next][save]`.
- Brand mark: 8×8 accent dot with a soft outer glow (accent @30% alpha, expanded
  3.5px) centered at x=24, plus "tubamp" wordmark at x=38..158, 18px bold kerned
  font, `text` color.
- Background: vertical gradient `panel`→`bgElevated`, 1px bottom stroke.

Controls & param/state wiring:
- **presetCombo** (`juce::ComboBox`, borderless style via `styleCombo(...,true)`):
  tooltip "Preset". `onChange` → `selectPreset(getSelectedId() - 1)`. IDs are
  1-based (`item id = index+1`); index -1 means "no selection".
- **prevButton / nextButton** (`IconButton`, chevron glyphs, no plate): tooltips
  "Previous preset" / "Next preset". `stepPreset(-1)` / `stepPreset(+1)`.
- **saveButton** (`IconButton`, save glyph, colours textDim/accent): tooltip
  "Save preset". Opens the save-name AlertWindow (see below).
- **slotAButton / slotBButton** (`juce::TextButton`, non-toggle-on-click —
  `setClickingTogglesState(false)`, state is driven programmatically):
  - Click (no shift) → `proc.presets.recallSlot(0 or 1)`.
  - Shift-click → `proc.presets.captureToSlot(0 or 1)` (checked via
    `juce::ModifierKeys::currentModifiers.isShiftDown()` at click time, not a
    drag-modifier check).
  - After either action, `updateAbButtons()` runs immediately (don't wait for the
    poll timer).
  - Tooltip on both: **"Recall A - shift-click to capture"** / same for B.
  - `buttonOnColourId` = accent; toggle state reflects `proc.presets.getActiveSlot()`.
- **settingsButton** (`IconButton`, gear glyph, inset 0.22): tooltip "Settings".
  `onClick` → `openSettings()`.

Timer: HeaderBar runs its own `juce::Timer` at **2 Hz**, calling
`updateAbButtons()` — i.e. A/B toggle highlight is polled every 500ms in addition
to the immediate update after a click, in case the active slot changes elsewhere
(e.g., preset load resets it). React equivalent: derive A/B active state reactively
from processor/store state instead of polling — this whole timer is a
**WORKAROUND** for JUCE's lack of reactive bindings.

`refreshPresets()` (also the callback for `proc.presets.onPresetChanged`, set in
ctor, cleared in dtor):
- Guarded by `updatingCombo` flag so the combo's own `onChange` doesn't re-fire
  while it's being repopulated (**WORKAROUND** — in React, driven by state won't
  need this).
- Clears combo, adds `presets[i].name` with id `i+1`, tracks `selectedId` = id of
  the entry whose `.name == getCurrentPresetName()`.
- `setSelectedId(selectedId, dontSendNotification)`.
- If nothing matched (`selectedId == 0`, e.g. unsaved/renamed current state):
  `setTextWhenNothingSelected(currentName)` — DOMAIN: shows the current preset
  name as placeholder text even though it's not a literal combo item (handles the
  "modified since load" / A|B-recalled-unsaved-state case).

`selectPreset(index)`:
- No-ops while `updatingCombo` is true.
- If index is a valid preset index, `proc.presets.loadPreset(presets[index].file)`.

`stepPreset(delta)`:
- No-op if preset list empty.
- Finds current index by name match (`getCurrentPresetName()`), wraps around
  (`(currentIndex + delta + size) % size`) — DOMAIN: prev/next always wraps,
  never clamps.

`savePreset()`:
- `juce::AlertWindow` modal with a text editor pre-filled with
  `getCurrentPresetName()`, "Save" (Enter) / "Cancel" (Escape) buttons.
- Uses `juce::Component::SafePointer<HeaderBar>` guard before touching `this` in
  the async callback (**WORKAROUND** for the dialog outliving the editor — in
  React this is irrelevant, a controlled modal component handles it naturally).
- On Save with non-empty trimmed name: `proc.presets.savePreset(name)`.
  DOMAIN: empty/whitespace-only name is silently ignored (no save, no error).

`openSettings()`:
- Constructs `SettingsPanel` and launches via `juce::DialogWindow::LaunchOptions`:
  title "Settings", `dialogBackgroundColour = bg`, native title bar, **not
  resizable**, `launchAsync()`. DOMAIN: settings is a separate top-level modal
  window (own LookAndFeel instance owned by SettingsPanel itself), not an inline
  panel — decide in React whether to keep as a real modal/dialog vs. inline;
  functionally it must behave modally (blocks interaction with main editor until
  closed, but is not literally OS-modal).

---

## SignalChainView (`src/ui/SignalChainView.h/.cpp`)

Concept: horizontal, user-buildable lane: `IN — [tile]…[tile] — [+] — OUT`, tiles
draggable to reorder. 60×60 tile face inside a 72×84 slot (6px pad each side for
selection glow + 14px bottom label strip). Node badges (IN/OUT) are 34×34.
Gap between all lane elements = 8px.

### Data flow / lifecycle
- Ctor: `order = proc.getChainOrder()`, `rebuildTiles()`, `ensureValidSelection()`.
- Subscribes `proc.onChainChanged` → `syncFromProcessor()` **guarded by
  `applyingOrder`** (see reentrancy guard below). Cleared in dtor along with
  `animator.cancelAllAnimations(false)`.
- `syncFromProcessor()`: re-pulls `proc.getChainOrder()`, `rebuildTiles()`,
  `ensureValidSelection()` — full rebuild, used for preset/state loads.
- **State-poll timer id 0 @ 100ms** (`stateTimer`): calls `refreshFromParams()`
  which re-reads each tile's enabled state from the APVTS raw parameter (does NOT
  rebuild tiles, just refreshes bypass visuals) — this is how bypassing a block
  from elsewhere (e.g. automation, host, A/B recall) shows up in the tile grid.
  **WORKAROUND** for JUCE's lack of reactive param subscriptions; React should
  subscribe/derive this reactively instead of polling every 100ms.
- **Animation timer id 1 @ 16ms** (`animTimer`): runs only while
  `animator.isAnimating()` or a drag is in progress (`dragIndex >= 0`); started on
  demand via `startAnimationTimer()`, self-stops in `timerCallback` once neither
  condition holds. Drives `repaint()` each frame during reflow animations —
  DOMAIN concept to keep: tiles animate to new slots over **120ms** with a linear
  step size of 1.0/probability 0.0 (`animator.animateComponent(tile, target, 1.0f,
  120, false, 1.0, 0.0)`), i.e. a plain 120ms tween, no easing curve or bounce.

### Reentrancy guard (WORKAROUND, drop in React)
`applyingOrder` boolean set via `ScopedValueSetter` around `proc.setChainOrder(order)`
inside `publishOrder()`. Because `setChainOrder()` calls `onChainChanged`
synchronously and the view already holds the authoritative just-published order,
the guard prevents re-entering `syncFromProcessor()` from its own publish. In a
React/unidirectional-state architecture this class of bug can't occur the same
way (no synchronous re-entrant callback loop needed) — omit the guard, just don't
call the "load order" action from inside the "order changed, so save order"
handler, or dedupe by identity/equality.

### Tile (per-block square)
- Tooltip: `"{displayName} - drag to reorder, double-click to bypass"`.
- Visual states: `selected`, `enabled`(=powered on), `hovered`, `dragging`.
  - `selected`: 3-layer outer glow rings (accent @ 0.24/i alpha, i=3..1,
    expanded 1.6*i px, radius 12+1.6*i), face fill `panelHi`, 2px accent border.
  - Not selected: face fill `panel` (or `panelHi` if hovered), 1px stroke
    (`strokeHi` if hovered else `stroke`).
  - `dragging`: extra drop-shadow rect behind the face (black @35% alpha, offset
    (0,3), expanded 1px, radius 13).
  - Disabled (bypassed): icon alpha 0.45, diagonal strike-through line drawn
    corner-to-corner inset 13px (bottom-left to top-right), label/icon colour
    dims to `textFaint`/`textDim`.
  - Icon: `icons::drawUnitPath` of `icons::makeIcon(blockId)`, colour = accent
    when enabled else `textFaint`, inset 17px into the 60×60 face, stroke 2px.
  - **Power LED**: 16×16 hit zone at top-right of the face (`face.right-19,
    face.y+3, 16, 16`), visually a 6px dot. Lit (enabled): accent-filled dot with
    a soft accent-@28%-alpha halo (expanded 3px). Off: `bg`-filled dot with
    `strokeHi` 1px ring.
  - Bottom label strip (14px tall): `shortName` (e.g. "GATE"), 10px kerned bold,
    colour accent if selected else textDim(enabled)/textFaint(disabled).
- Mouse handling (`Tile::mouseDown/mouseDrag/mouseUp/mouseDoubleClick`):
  - `mouseDown`: `suppressGesture` resets to false each press.
    - If down-position is inside the **LED hit box** → set
      `suppressGesture = true`, call `owner.toggleBypass(blockId)`, **return**
      (no selection change, no drag start).
    - Else if `e.mods.isPopupMenu()` (right-click / ctrl-click on macOS) →
      `suppressGesture = true`, `owner.showBlockMenu(blockId, this)`, return.
    - Else → `owner.selectBlock(blockId)` (plain left click selects; does NOT
      suppress drag — a click-then-drag on the body still becomes a reorder drag).
  - `mouseDrag`: forwarded to `owner.tileDragged(*this, e)` unless
    `suppressGesture` (i.e. LED or context-menu presses never turn into drags).
  - `mouseUp`: forwarded to `owner.tileDragEnded` unless suppressed.
  - `mouseDoubleClick`: `owner.toggleBypass(blockId)` unless suppressed —
    DOMAIN: double-click anywhere on the tile body (not just the LED) toggles
    bypass, redundant with the LED single-click but is the documented affordance
    from the class doc comment.
  - Hover: `mouseEnter/mouseExit` just toggle a `hovered` repaint flag (visual only).

### AddTile ("+")
- 44×44 dashed-outline square, tooltip "Add a block to the chain".
- Hover: fills `panel` background, brightens stroke/glyph to `strokeHi`/`text`.
- Visible only when `order.size() < chain::numBlockTypes` (9) — i.e. hidden once
  every block type is already in the chain.
- `mouseUp` (not mouseDown) inside its bounds → `owner.showAddMenu()`.

### Selection
- `selectBlock(id)`: no-op if already selected; else sets `selection`, calls
  `updateTileStates()` (repaints tile selected/enabled flags), `repaint()`, then
  fires `onSelectionChanged(selection)`.
- `ensureValidSelection()` — DOMAIN fallback rule, must be preserved exactly:
  1. If current selection is unset OR the selected block is no longer in the
     chain → recompute:
     a. If chain contains `BlockId::amp` → select **amp** (Amp is the
        "default focus" block).
     b. Else if chain is non-empty → select `order.front()` (first block in
        chain order).
     c. Else → no selection (empty chain → empty state).
  2. If the recomputed value equals current selection, just resync tile states
     (no event fired, avoids redundant callback).
  3. Otherwise updates `selection`, repaints, and fires `onSelectionChanged`.
- Called after every structural chain mutation: `rebuildTiles`+`syncFromProcessor`
  (via ctor/subscription), `removeBlock`. NOT called after `appendBlock` (which
  explicitly `selectBlock(id)`s the newly appended block instead — i.e. adding a
  block always selects it immediately, bypassing the amp-priority fallback rule).

### Bypass toggling
- `isBlockEnabled(id)`: reads `proc.apvts.getRawParameterValue(enableParamId)
  ->load() > 0.5f` (defaults true if param missing).
- `setBlockEnabled(id, value)`: proper gesture wrap —
  `beginChangeGesture()` / `setValueNotifyingHost(1|0)` / `endChangeGesture()` —
  DOMAIN: must be a single automatable gesture, not a raw value poke, so DAW
  automation lanes/undo see a clean start/end.
- `toggleBypass(id)`: flips via the above, then `updateTileStates()` immediately
  (doesn't wait for the 100ms poll).

### Context menu (right-click a tile) — `showBlockMenu`
- Section header = block's `displayName`.
- Item 1: **"Bypass"** (if currently enabled) or **"Enable"** (if currently
  bypassed) — dynamic label matching current state.
- Separator.
- Item 2: **"Remove from chain"**.
- `juce::Component::SafePointer<SignalChainView>` guard in the async menu
  callback (**WORKAROUND** for the view being destroyed while the menu is open —
  irrelevant to a React component tree, drop it).
- Min width 180px, anchored to the tile (`target`).

### Add menu (click "+") — `showAddMenu`
- Lists every `chain::blockInfos` entry **not already in the chain** (already-
  present block types are omitted, not shown-disabled).
- Each item's `itemID = (int)info.id + 1` (1-based), `item.colour =
  TubampLookAndFeel::blockAccent(info.id)` — DOMAIN: each menu row is tinted with
  that block's own accent color.
- Selecting an item calls `appendBlock(id)`.
- SafePointer guard (same WORKAROUND note as above).

### Append / Remove
- `appendBlock(id)`: no-op if already present or chain already at
  `numBlockTypes` (9) cap. Otherwise pushes to end of `order`, `publishOrder()`,
  `rebuildTiles()`, then explicitly `selectBlock(id)` (new block is always
  auto-selected).
- `removeBlock(id)`: erase from `order` if present, `publishOrder()`,
  `rebuildTiles()`, `ensureValidSelection()` (falls back per the priority rule
  above, does NOT auto-select anything specific).

### Drag-to-reorder (`tileDragged` / `tileDragEnded`)
- Drag doesn't "start" until horizontal movement exceeds a **5px** threshold
  (`std::abs(e.getDistanceFromDragStartX()) < 5` early-returns) — small-jitter
  clicks never trigger a reorder.
- On drag start: records `dragIndex`, `dragGrabOffsetX = e.getMouseDownX()`
  (grab point within the tile), snapshots `orderBeforeDrag = order` (for
  cancel-restore), `tile.setDragging(true)`, `tile.toFront(false)`, starts the
  16ms animation timer.
- Dragged tile's X is clamped to `[laneBounds.getX(), laneBounds.getRight() -
  tileW]` (can't be dragged outside the lane bounds horizontally); Y is fixed to
  its slot's row Y minus 3px (a slight "lift" offset while dragging).
- **Nearest-slot-wins** reorder logic: compute `centreX` of the dragged tile,
  find the slot index with the closest `slots[i].getCentreX()` (ties favor lower
  index due to strict `<` comparison), clamp to valid range. If nearest !=
  current dragIndex, splice the moved block in `order` to that position, move
  the `tiles` array element to match, update `dragIndex`, and
  `layoutLane(true)` (animated reflow of the *other* tiles out of the way — the
  dragged tile itself is excluded from the target-bounds loop while
  `i == dragIndex`).
- Drag end: if the release point (relative to the view, in local coords) is
  outside `getLocalBounds().expanded(24, 48)` → **cancel**: restore
  `order = orderBeforeDrag` and `reorderTilesTo(orderBeforeDrag)` (re-sorts the
  `tiles` array back to match without touching `proc`, i.e. no publish happens).
  DOMAIN: dropping far outside the lane (24px horizontal / 48px vertical
  tolerance) is the cancel gesture; releasing inside that expanded rect
  (even outside the strict lane bounds) commits.
  Otherwise, if `order != orderBeforeDrag`, `publishOrder()` (single publish at
  drag end, not per-hover-swap).
  Always ends with `tile.setDragging(false)`, `dragIndex = -1`,
  `layoutLane(true)` (final animated settle).
- `reorderTilesTo(desired)`: pure `tiles` OwnedArray reshuffle to match a given
  order (used only for the cancel path — a "just fix visual order, no publish"
  helper).

### Publishing order
- `publishOrder()`: wraps `proc.setChainOrder(order)` in the `applyingOrder`
  guard (see reentrancy section). DOMAIN: order is NOT an APVTS parameter — it's
  published via a dedicated processor method (`setChainOrder`), and comes back
  through `proc.onChainChanged`, not a parameter listener. Serialization format
  (`chain::toString`/`fromString`, `pack`/`unpack` to a lock-free `uint64_t` for
  the audio thread) is processor/DSP-side and out of scope for the UI itself, but
  the UI's mental model — "chain order is a single opaque published value, not a
  per-block index" — should carry over to the React state design (e.g. a single
  order array in a store, not derived per-tile from index props).

### Connector line / arrows (paint)
- Elements drawn left→right: IN node, each tile face (in current `order`), the
  "+" tile (if visible), OUT node.
- Between consecutive elements with a gap (`to > from`): a 2px horizontal line;
  colored with the *next* element's selected-block accent (@85% alpha) if that
  next element is the selected tile, else `strokeHi`.
  DOMAIN nuance: the accent highlight applies to the segment **feeding into**
  the selected tile, not the one leaving it.
- If the gap is ≥14px, draws a small flow-arrow glyph (`icons::makeFlowArrow()`)
  centered in the gap, `textFaint`, 7×7 box, 1.4px stroke.
- IN/OUT nodes: 8px-radius rounded squares, `panel` fill + `stroke` border,
  centered glyph (`makeInputIcon`/`makeOutputIcon`, textDim, inset 9px, 1.6px
  stroke), caption "IN"/"OUT" below at the same baseline Y as tile labels
  (`rowLabelY`), 9px kerned bold, `textFaint`.

### Lane centering math
- `laneBounds = getLocalBounds().reduced(16, 0)`.
- Total lane content width = `2*nodeSize + n*tileW + (showAdd?addW:0) +
  laneGap*(elementCount-1)` where `elementCount = 2 + n + (showAdd?1:0)`.
- The whole row is horizontally centered in `laneBounds`
  (`x = laneBounds.centreX - total/2`), i.e. the lane re-centers itself as blocks
  are added/removed (it does not grow from a fixed left edge).
- Vertically: `top = laneBounds.centreY - tileH/2`; `rowCentreY` and
  `rowLabelY` derived from that.

---

## BlockParamPanel (`src/ui/BlockParamPanel.h/.cpp`, 905 lines)

The big single-block editor. Structure: accent-tinted header (56px) + a body that
switches per-block-type. **Both `AmpBody` and `CabBody` sub-components are
constructed once in the panel's ctor and kept alive for the panel's whole
lifetime** (never destroyed on block switches — only shown/hidden via
`setVisible`), specifically so an in-flight TONE3000 download always has
somewhere to land even if the user navigates to a different block mid-download.
**DOMAIN, must keep the underlying behavior** (an async download must not be
silently lost if the user clicks to a different tile) even though in React this
would naturally be modeled as global/store-level download state rather than
component lifetime tricks.

### Header (56px band)
- Accent wash gradient behind the header only (accent@10%→0% left-to-right,
  fading out at 60% of header width), 1px bottom stroke.
- Header icon (`icons::makeIcon(block)`) at x=20, 20×20, accent-colored, 1.8px stroke.
- `titleLabel`: block `displayName`, 13px kerned bold, positioned at (48,14,320,20).
- Accent underline: 48×2px rect at (48, 38) under the title.
- **powerButton** (`juce::ToggleButton` styled via `makePowerPill`, tick colour =
  accent): bound via `ButtonAttachment` to `chain::infoFor(block).enableParamId`
  — i.e. the SAME `*_on` parameter the tile's LED and context menu drive. Tooltip
  "Enable or bypass this block". Pill shows "ON"/"OFF" caption per
  `TubampLookAndFeel::makePowerPill`/`drawToggleButton` (confirmed via grep:
  draws literal text "ON" / "OFF"). Positioned at header-right, 84×28 pill.
- **removeButton** (`IconButton`, trash glyph, colours `textFaint`→`text`, inset
  0.24): tooltip "Remove this block from the chain". `onClick` fires
  `onRemoveBlock(*block)` if a block is selected — the panel does NOT remove the
  block itself, it delegates to the owner (which is `chainView.removeBlock`, see
  PluginEditor wiring). Positioned rightmost, 28×28.
- Header/power/remove all hidden (`setVisible(false)`) when no block is selected;
  `emptyLabel` ("Add a block to get started", centered, 15px, `textFaint`) shown
  instead, occupying the full body area.

### Per-block body dispatch (`setBlock`, `resized`)
- `block` optional held; on every `setBlock(id)` call: resets `powerAttachment`
  first (avoids a stale attachment pointing at the wrong param while other state
  updates), recomputes `accentColour` (`blockAccent(*block)` or the flat header
  `accent` fallback when empty) and `headerIcon`.
- Visibility flags: `isAmp`, `isCab`, `isMod` computed; `ampBody`/`cabBody`/
  `modTypeCombo` visibility set accordingly (all three are mutually exclusive
  add-on bodies; every other block type just gets the generic knob row).
- **Cab special case**: `if (isCab) cabBody->refreshIrList();` — refreshes the IR
  combo every time the Cab block is (re)selected, because a preset load could
  have swapped the IR file while the panel was showing a different block.
- If a block is selected: sets title text/colour, re-creates `powerAttachment`
  bound to that block's enable param, and calls `buildKnobs(knobSpecsFor(*block),
  accentColour)` (rebuilds the generic knob row every switch — even for Amp/Cab/
  Mod, which for Amp yields an empty knob list since Amp knobs live in AmpBody).
- If no block: `knobs.clear()`.
- Always ends `resized(); repaint();`.

### Knob specs per block (`knobSpecsFor`) — id → label, in display order
- gate: `gate_threshold` → "THRESH"
- comp: `comp_threshold`→"THRESH", `comp_ratio`→"RATIO", `comp_attack`→"ATTACK",
  `comp_release`→"RELEASE", `comp_makeup`→"MAKEUP"
- drive: `drive_gain`→"GAIN", `drive_tone`→"TONE", `drive_level`→"LEVEL"
- cab: `cab_lowcut`→"LO CUT", `cab_highcut`→"HI CUT" (plus the IR row from CabBody)
- eq: `eq_bass`→"BASS", `eq_mid`→"MID", `eq_treble`→"TREBLE"
- mod: `mod_rate`→"RATE", `mod_depth`→"DEPTH", `mod_mix`→"MIX" (plus modTypeCombo)
- delay: `delay_time`→"TIME", `delay_feedback`→"FDBK", `delay_mix`→"MIX"
- reverb: `reverb_size`→"SIZE", `reverb_damping`→"DAMP", `reverb_mix`→"MIX"
- amp: **empty list** — all amp controls live in `AmpBody`.

Each generic knob: `juce::Slider` (RotaryHorizontalVerticalDrag,
**TextBoxBelow** — unlike the footer/AmpBody knobs, these DO show a numeric
value box under the rotary), styled via `TubampLookAndFeel::styleKnob(slider,
accentColour)`, 11px kerned bold micro-label under it, bound via
`SliderAttachment` to the param id. No custom `textFromValueFunction` is set
anywhere in this file or in `TubampLookAndFeel.cpp` — the value-box text uses
JUCE's default slider formatting (derived from the parameter's `NormalisableRange`
interval / suffix label set in `Parameters.cpp`, e.g. "12.0 dB", "4.00 :1", one
decimal typical for a 0.1 interval range, more for log-range fine intervals).
React must reproduce equivalent default formatting per param (see Parameters
table below — suffix label + numeric precision implied by each param's `interval`).

Knob-row layout (`layoutKnobRow`): each knob cell is 100px wide, total row
width = `knobs.size() * 100`, horizontally+vertically centered in the available
body area (capped at `knobCellH` = 72(knob) + 18(textbox) + 4(gap) + 14(label) =
108px tall). Label at bottom (14px), 4px gap, knob fills the rest.

### Mod-type combo (visible only for `mod` block)
- `modTypeCombo`, items = `params::modTypeChoices` = ["Chorus","Phaser","Tremolo"],
  bound via `ComboBoxAttachment` to `params::modType`. Positioned centered, 180×30,
  above the generic knob row (RATE/DEPTH/MIX).

### AmpBody (the NAM amp management + level controls)
Two-column layout: left = model management (fixed content height clamp 200px,
vertically centered in the left column), right = levels (content height clamp
220px, vertically centered), separated by a 1px vertical divider line
24px inset from the right column's left edge.

**Left column ("MODEL" caption, 10px faint kerned):**
- `modelNameLabel` (18px bold, left-justified) — text/colour driven by the
  **4Hz internal timer** (see below), NOT by a static bind.
- Combo row: `modelCombo` (borderless-off style, "Select a model..." placeholder)
  + `modelPrevButton`/`modelNextButton`/`modelClearButton` (32px `IconButton`s
  with a plate, chevron/chevron/cross glyphs; tooltips "Previous model"/"Next
  model"/"Clear the loaded model").
- Button row: `importButton` ("Import...", 104px wide TextButton) +
  `browseButton` ("Browse TONE3000", 168px, `buttonOnColourId`=accent) +
  `poweredByLabel` ("Powered by TONE3000", 11px faint, 150px wide) — DOMAIN:
  attribution text is always visible next to Browse, not conditional.
- `progressBar` (8px tall `juce::ProgressBar` bound to a local `double progress`
  member) — hidden unless a TONE3000 download is in flight.
- `hintLabel` (11px dim) below the progress bar — dual purpose: shows the
  "add your TONE3000 key" nudge OR the latest TONE3000 error message (see
  timer/browse logic below); both routes reuse the same label.

**Right column ("LEVELS" caption):**
- `inputKnob`/`outputKnob`: rotary **with TextBoxBelow**, styled with amp accent,
  bound to `params::ampInput` (dB [-20,20]) / `params::ampOutput` (dB [-40,40]).
  Labels "IN"/"OUT" (11px kerned, centred).
- `modeCombo`: bound to `params::ampOutMode` (choice, contract-fixed id/order —
  Raw=0/Normalized=1/Calibrated=2, items from `params::ampOutModeChoices`,
  `addItemList(choices, 1)` so item ids are 1-based matching choice index+1).
  Label "MODE" (left-justified micro-label, 46px wide).
- `slimSlider`: `LinearHorizontal`, `NoTextBox`, track colour = amp accent, bound
  to `params::ampSlim` ([0,1], step 0.01). Label "SLIM" (left-justified).
  **Visibility gated**: `slimSlider.setVisible/slimLabel.setVisible(slimmable)`
  and `slimSlider.setEnabled(slimmable)` where `slimmable =
  proc.namEngine.isSlimmable()` — **contract-fixed DSP API**: true only when the
  currently loaded model implements the A2 "slimmable" interface. This is
  DOMAIN, not a UI decision — must be reproduced by querying whatever the
  React-side processor bridge exposes as the equivalent flag, polled/derived per
  the currently loaded model, not assumed from file type or user choice.

**AmpBody 4Hz internal timer** (`startTimerHz(4)` in ctor) — drives:
1. `modelNameLabel` text, rebuilt only if changed (avoids redundant repaint
   churn): if a model path is loaded (`proc.getLoadedModelPath()` non-empty),
   text = `{basename without ext}` + (if `namEngine.hasModel()`) middot +
   `{sampleRate/1000 rounded to 0 decimals} kHz` + middot + `"active"`, else
   (loaded-but-not-live) middot + `"NOT RUNNING"`. If nothing loaded: text =
   `"No model - load a NAM capture"`. Colour: `text` if a path is loaded, else
   `textDim`. DOMAIN: reports the **live engine state**, not just "a path was
   requested" — surfaces the case where a model failed to reach the audio
   thread (shows "NOT RUNNING" instead of silently looking loaded).
2. If `loaded != lastLoadedPath` (a preset/state load swapped the model behind
   the UI's back), calls `refreshModelList()` to resync the combo selection —
   **WORKAROUND for JUCE's lack of reactive state binding**; React should
   subscribe to model-loaded state directly instead of polling for path changes.
3. `hintLabel` visibility = `!configured && !downloading` where `configured =
   proc.tone3000.isConfigured()`; when shown and `!configured`, text is forced
   to `"Add your TONE3000 publishable key in Settings to browse."` every tick
   (i.e. this specific text is authoritative whenever not configured, overriding
   any leftover error text) — **note**: an error message set by a failed browse
   only persists as long as `configured` stays true; if `!configured` on the
   next tick, it's overwritten back to the key-nudge text.
4. `progressBar.setVisible(downloading)`.
5. `slimSlider`/`slimLabel` visibility+enabled per `isSlimmable()` (see above).

**Model combo (`refreshModelList`)**: guarded by `updatingCombo`
(WORKAROUND, same pattern as HeaderBar's preset combo). Populates from
`proc.library.getModels()` (sorted by name per ModelLibrary contract), 1-based
ids, selects the id whose file's full path matches `proc.getLoadedModelPath()`
(0/none if no match e.g. nothing loaded or a manually-imported/TONE3000 file not
yet in the refreshed list — but note browse/import flows call `refreshModelList`
after install so this should resolve). Placeholder "Select a model...".

**modelComboChanged()**: ignored while `updatingCombo`; else loads
`models[selectedId-1].file` via `proc.loadModel(...)`; on non-empty error string,
shows a blocking `AlertWindow::showMessageBoxAsync` (Warning icon, title "Model
load failed", body = error).

**stepModel(delta)**: no-op if library empty. Finds current index by matching
`getLoadedModelPath()`; if no current model, starting index is `0` for
delta>0 or `size-1` for delta<0 (i.e. "next" from nothing starts at the first
model, "prev" from nothing starts at the last) — **NOT the same as HeaderBar's
wrap rule**, note this asymmetry. If there IS a current model, wraps normally:
`(current + delta + size) % size`. Same load-error alert as above. Always calls
`refreshModelList()` after, regardless of success/failure.

**clearModelClicked()**: `proc.clearModel()` then `refreshModelList()`.

**importClicked()**: `juce::FileChooser("Import NAM model", {}, "*.nam")`,
async, open+selectFiles mode. On a valid file result: `proc.library.importModel
(file, error)`; if the returned installed file doesn't exist, shows a Warning
alert "Import failed" / error; else attempts `proc.loadModel(installed)`, and if
THAT errors, shows a separate "Model load failed" alert. Note: **does not call
`refreshModelList()` explicitly here** — relies on `proc.library.onChanged` firing
(see BlockParamPanel ctor: `proc.library.onChanged` fans out to both
`ampBody->refreshModelList()` and `cabBody->refreshIrList()`).

**browseClicked() — TONE3000 select flow**:
- If `!proc.tone3000.isConfigured()`: sets `hintLabel` text to the key-nudge
  message and forces it visible, returns immediately (no network call attempted).
- Else: captures `juce::Component::SafePointer<AmpBody> safeThis` (**WORKAROUND**:
  `Tone3000Client` runs on a background thread and delivers via
  `MessageManager::callAsync` at an arbitrary later time, possibly after this
  panel/AmpBody has been destroyed — every callback below null-checks
  `safeThis` first before touching anything. In React this maps to: don't update
  state on an unmounted component / cancel in-flight requests on unmount, but no
  literal SafePointer pattern is needed.)
- `proc.tone3000.startSelectFlow(onToneSelected, onError)`:
  - `onToneSelected(toneModels)`: if `safeThis==nullptr` return; if
    `toneModels.models.isEmpty()` return silently (**no user-facing message for
    "tone had zero models"** — worth flagging as a possible UX gap to fix or
    knowingly reproduce). Else builds a `PopupMenu` listing each model's `.name`
    (1-based item ids), shown anchored to `browseButton`. Selecting one:
    - Sets `downloading = true`, `progress = 0.0`, shows the progress bar.
    - `proc.tone3000.downloadModel(model, ModelLibrary::getModelsDir(),
      onProgress, onComplete, onError)`:
      - `onProgress(p)`: `progress = (double)p` (drives the `ProgressBar`'s bound
        double directly — no smoothing/animation beyond what `ProgressBar`
        itself does).
      - `onComplete(savedFile)`: `downloading=false`, hide progress bar; if the
        file exists, `refreshModelList()` → `proc.loadModel(savedFile)` →
        `refreshModelList()` again (**called twice**: once before load so the
        new file is at least visible in the combo, once after so the
        now-loaded selection highlights correctly — reproduce this exact
        double-refresh sequencing, not just a single refresh, since the load
        can itself be async-adjacent in timing).
      - `onError(message)`: `downloading=false`, hide progress bar, sets
        `hintLabel` text to the raw error message and shows it.
  - `onError(message)` (top-level select-flow failure/cancel): sets `hintLabel`
    text to the raw message, shows it. No distinction is made in the UI between
    "user cancelled the browser flow" and "network/auth error" — both just
    surface as whatever string the client produced.
- **Multi-model download**: NOTE the flow as coded only downloads **one** model
  per menu selection (user picks exactly one item from the popup, one
  `downloadModel` call). There is no batch/multi-select UI in this file despite
  the task prompt's phrase "multi-model download behavior" — confirm this is
  intentional (a tone can have multiple architecture-2 model variants, e.g.
  standard/lite/feather/nano, and the popup menu is exactly how the user picks
  ONE variant to download, not a batch operation). No further multi-model logic
  exists to reproduce beyond "the picker can list N models, only one gets fetched".

### CabBody (IR row)
- `irCombo` (borderless-off style, "No IR loaded" placeholder) +
  `irPrevButton`/`irNextButton`/`irClearButton` (32px plated IconButtons,
  chevron/chevron/cross, tooltips "Previous IR"/"Next IR"/"Clear the loaded IR")
  + `importButton` ("Import...", tooltip "Import an impulse response (.wav)").
  Row order right-to-left via `removeFromRight`: import(100) | clear(32) |
  next(32) | prev(32) | combo(flex). Caption "IMPULSE RESPONSE" above (10px
  faint kerned). Content width capped at 620px, centered.
- `refreshIrList()`: same `updatingCombo`-guarded pattern as models; populates
  from `proc.library.getIrs()`, selects by matching `proc.getLoadedIrPath()`,
  placeholder "No IR loaded".
- `irComboChanged()`: guarded, loads `irs[selectedId-1].file` via
  `proc.loadIr(...)` — **note**: unlike AmpBody's model combo, this does not
  surface a load-error alert on combo selection (no `if (error...)` check here,
  only in `stepIr`/`importClicked`) — likely an inconsistency to flag, but
  reproduce as-is unless asked to fix.
- `stepIr(delta)`: same "no current → next starts at 0 / prev starts at last;
  else wraps" rule as `stepModel`. Shows a Warning alert "IR load failed" on
  error. Always `refreshIrList()` after.
- `clearIrClicked()`: `proc.clearIr()` then `refreshIrList()`.
- `importClicked()`: `FileChooser("Import impulse response", {}, "*.wav")`;
  on valid result, `proc.library.importIr(file, error)`; if installed exists,
  `proc.loadIr(installed)`; **always** `refreshIrList()` at the end regardless of
  success (unlike AmpBody's import, which relies on the `onChanged` callback and
  shows an alert on failure — CabBody's import shows **no alert at all** on
  either import failure or load failure, just silently refreshes the list).

Below CabBody (56px row) sits the generic knob row for `cab_lowcut`/`cab_highcut`
(LO CUT / HI CUT), laid out via the shared `layoutKnobRow`.

---

## SettingsPanel (`src/ui/SettingsPanel.h/.cpp`)

Standalone `DialogWindow` content, 380×320 fixed size, own `TubampLookAndFeel`
instance (set in ctor, cleared in dtor — **kept alive exactly as long as the
dialog**, a WORKAROUND-flavored detail specific to JUCE's LookAndFeel ownership
model; in React this is just "use the shared theme," no lifetime concern).

Fields, top to bottom (16px margins, sequential `removeFromTop` layout):
1. `titleLabel` "TONE3000", 16px bold, 28px row.
2. 12px gap.
3. `keyLabel` "Publishable key", 18px row.
4. `keyEditor` (`juce::TextEditor`, 28px row): pre-filled with
   `proc.tone3000.getClientId()`; placeholder text "t3k_pub_..." in `textDim`
   colour when empty.
5. 10px gap.
6. Button row (32px): `applyButton` "Save" (100px) + 8px gap +
   `signOutButton` "Sign out" (100px).
   - Save: `proc.tone3000.setClientId(keyEditor.getText().trim())`, then
     `statusLabel` text = **"Saved."**
   - Sign out: `proc.tone3000.signOut()`, then `statusLabel` text =
     **"Signed out."**
7. 8px gap, `statusLabel` (18px row, `textDim` colour) — shows whichever of the
   two literal strings above was last triggered; empty otherwise (no initial
   text set).
8. 16px gap.
9. `calibrationTitleLabel` "Amp Calibration", 16px bold, 28px row.
10. 10px gap.
11. `calibrateInputButton` (`juce::ToggleButton`, label "Calibrate input", 24px
    row) — bound via `ButtonAttachment` to raw string id **`"amp_cal_input"`**
    (= `params::ampCalInput`, but referenced as a literal in this file per the
    "contract-fixed id, avoid depending on the header" comment — WORKAROUND/
    convention note only, functionally identical to using the constant; React
    should just use the shared param-id constant, no need to preserve the
    string-literal indirection).
12. 8px gap, `calLevelLabel` "Input level (dBu)", `textDim`, 18px row.
13. `calLevelSlider` (`LinearHorizontal`, **TextBoxRight** 56×20), bound via
    `SliderAttachment` to raw id **`"amp_cal_level"`** (= `params::ampCalLevel`,
    dBu [-60,60], default 12, step 0.1), 28px row.

No footer/close button drawn explicitly — dialog closes via the native title
bar close box or Escape (`escapeKeyTriggersCloseButton = true` set in
`HeaderBar::openSettings`).

---

## PeakMeter (`src/ui/PeakMeter.h/.cpp`)

- 10 vertical segments (`numSegments`), range **-60 dB to +6 dB**
  (`minDb`/`maxDb`), 2px gaps between segments, each segment individually
  rounded (1.5px radius).
- `pushLevel(linearPeak)` — called externally at 30Hz (from PluginEditor's timer)
  with a **linear** peak value that the caller is expected to have
  read-and-reset from an atomic (so it's "peak since last poll", not a running
  max) — DOMAIN framing to preserve: the meter itself doesn't know about the
  atomic-exchange semantics, it just receives a fresh instantaneous linear peak
  each call and applies its OWN decay on top:
  - `displayLevel = max(linearPeak, displayLevel * 0.75)` — **75% decay
    per pushLevel call** (i.e. per ~33ms tick at the 30Hz caller rate — the decay
    constant is tick-rate-dependent, not time-normalized, so if React drives
    `pushLevel` at a different rate the visual fall-off speed changes; either
    keep the same 30Hz cadence or convert the 0.75-per-33ms decay into a
    proper time-based exponential decay, e.g. ~time-constant ≈ 33ms/ln(1/0.75) ≈
    115ms, for correctness at other frame rates).
  - **Peak-hold**: `holdLevel`/`holdStartMs` tracked with `Time::getMillisecondCounterHiRes()`.
    If `displayLevel >= holdLevel`, hold snaps up immediately and its start-time
    resets (hold always tracks the loudest recent peak). Once `displayLevel`
    falls below `holdLevel` for **more than 1500ms**, hold starts decaying at
    `holdLevel * 0.90` per subsequent `pushLevel` call (again tick-rate-dependent,
    same conversion caveat as above) but never below the current `displayLevel`
    (`max(displayLevel, holdLevel*0.90)`).
- Rendering: `positionForDb` linearly maps dB→[0,1] over [-60,6] and clamps.
  Segment `i` (0=bottom) represents the dB range centered at
  `minDb + (maxDb-minDb)*(i+0.5)/10`; its colour is chosen by that **segment's own
  center dB**, not the current level: **≥ -3dB → `meterHigh` (red, `#f87171`)**,
  **≥ -12dB → `meterMid` (amber, `#fbbf24`)**, **else → `meterLow` (green,
  `#34d399`)**. A segment is "lit" if `level > i/10` (unlit segments render at
  10% alpha of their own colour, dark background segment first
  (`bg`-fill) under both). Lit segments get an extra glow rect (colour @25%
  alpha, expanded 1px, radius 2px).
- Peak-hold tick: a 2px-tall horizontal bar drawn at the hold position (only if
  `hold > 0.001`), coloured by `colourForDb(gainToDecibels(holdLevel))` (the
  ACTUAL hold level's colour, not the segment-center convention used for the
  ladder) at 90% alpha.

---

## TubampLookAndFeel (`src/ui/TubampLookAndFeel.h/.cpp`) — design tokens

Palette (hex, all as design tokens for the React theme):
- `bg` #0c0e12 (window base), `bgElevated` #12151b (bands/knob faces/panel
  chrome), `panel` #161a21 (tiles/controls), `panelHi` #1c222b (hover/selected),
  `stroke` #262e39, `strokeHi` #333d4b, `text` #e6eaf0, `textDim` #8b95a6,
  `textFaint` #5a6473, `accent` #ff7a1a (brand amber/orange).
- Meter stops: `meterLow` #34d399 (green), `meterMid` #fbbf24 (amber),
  `meterHigh` #f87171 (red) — thresholds at -12dB and -3dB (see PeakMeter).
- `blockAccent(BlockId)` — per-block identity colour used everywhere a block
  appears (tile, icon, header wash, knob arcs, power LED). Exact values
  (`src/ui/TubampLookAndFeel.cpp`):
  | block | hex |
  |---|---|
  | gate | `#a78bfa` (violet) |
  | comp | `#fbbf24` (amber) |
  | drive | `#f97316` (orange) |
  | amp | `#f43f5e` (rose) |
  | cab | `#2dd4bf` (teal) |
  | eq | `#60a5fa` (blue) |
  | mod | `#4ade80` (green) |
  | delay | `#818cf8` (indigo) |
  | reverb | `#22d3ee` (cyan) |
  Fallback (unknown id) is the flat brand `accent` (#ff7a1a).
- Fonts: `plainFont(height, bold)` and `kernedFont(height, bold)` (extra
  letter-spacing for uppercase micro-labels/section titles) — both presumably
  wrap a single system/embedded typeface; check `.cpp` for the actual family if
  React needs a literal font-family match (not extracted in this pass).
- Toggle buttons drawn as "power pill" (`makePowerPill`) literally render the
  text **"ON"** / **"OFF"** inside the pill shape (confirmed via
  `drawToggleButton`'s `g.drawText(on ? "ON" : "OFF", ...)`).
- Custom drawing overrides: rotary slider (arc-style knob with accent fill),
  linear slider, button background, toggle button, tick box, combo box (+ text
  positioning), popup menu background, text editor background/outline, progress
  bar, alert box, plus font getters for labels/combo/popup/button/alert. All of
  this is purely visual styling to replicate in CSS/component variants — no
  additional interactive behavior beyond what's already described per-component
  above.

---

## BlockIcons.h / IconButton.h

- `BlockIcons.h` (292 lines) is a pure vector-glyph library: `icons::makeIcon
  (BlockId)` returns a unit-square `juce::Path` per block type, plus standalone
  glyphs (`makeChevronGlyph(bool pointingRight)`, `makeSaveGlyph`,
  `makeGearGlyph`, `makePlusGlyph`, `makeFlowArrow`, `makeInputIcon`,
  `makeOutputIcon`, `makeTrashGlyph`, and locally-defined crosses in
  BlockParamPanel.cpp for the clear buttons) plus `drawUnitPath(g, path, area,
  strokeWidth)` (scales/strokes a unit-space path into a target rect) and
  `addLine` (helper for building glyphs from 0..1 coordinates). For React: needs
  an equivalent icon set — either literally port these as SVG paths (unit
  0..1 coordinate space maps directly to SVG `viewBox="0 0 1 1"`) or swap for an
  icon library, as long as one distinct icon exists per `BlockId` plus the
  chrome glyphs listed.
- `IconButton.h`: a `juce::Button` subclass that paints a glyph instead of text,
  optional rounded "plate" background, configurable inset/stroke-width/colours
  (normal vs hover, brightened further on press). Used throughout for chevrons,
  save/gear/trash/clear-cross buttons. In React this maps to a generic
  `<IconButton glyph disabled? plate? />` component — no additional hidden
  behavior beyond hover/press colour states and disabled-state alpha (0.35).

---

## Parameters — full table (`src/Parameters.h/.cpp`)

All ids are frozen strings (AU compatibility) — the React UI must use these
exact ids when talking to whatever bridge exposes the processor's parameter
state. `kVersionHint = 1` for every `ParameterID`. Range is `{min, max,
interval}`; "log" = `logRange()` (skew centred at the geometric mean
`sqrt(min*max)`, natural feel for freq/time/ratio controls) — visually this
means the knob's mid-position corresponds to that geometric-mean value, not the
arithmetic mean.

**Block enables** (bool, default noted):
| id | default |
|---|---|
| `gate_on` | true |
| `comp_on` | true |
| `drive_on` | **false** (only block off by default) |
| `amp_on` | true |
| `cab_on` | true |
| `eq_on` | true |
| `mod_on` | true |
| `delay_on` | true |
| `reverb_on` | true |

**Input/Output**
| id | range | default | unit |
|---|---|---|---|
| `input_trim` | [-24, 24] step 0.1 | 0 | dB |
| `output_level` | [-60, 12] step 0.1 | 0 | dB |

**Gate**
| `gate_threshold` | [-100, 0] step 0.1 | -80 | dB |

**Compressor**
| `comp_threshold` | [-60, 0] step 0.1 | -20 | dB |
| `comp_ratio` | log [1, 20] step 0.01 | 4 | :1 |
| `comp_attack` | log [0.1, 100] step 0.01 | 5 | ms |
| `comp_release` | log [10, 1000] step 0.1 | 120 | ms |
| `comp_makeup` | [0, 24] step 0.1 | 0 | dB |

**Drive**
| `drive_gain` | [0, 36] step 0.1 | 12 | dB |
| `drive_tone` | log [500, 12000] step 1 | 4000 | Hz |
| `drive_level` | [-24, 12] step 0.1 | 0 | dB |

**Amp (NAM)** — mirrors official NAM plugin knob-for-knob
| `amp_input` | [-20, 20] step 0.1 | 0 | dB |
| `amp_output` | [-40, 40] step 0.1 | 0 | dB |
| `amp_out_mode` | choice {Raw=0, Normalized=1, Calibrated=2} | Normalized(1) | — |
| `amp_cal_input` | bool | false | — |
| `amp_cal_level` | [-60, 60] step 0.1 | 12 | dBu |
| `amp_slim` | [0, 1] step 0.01 | 0 | — (0=cheapest, 1=full quality) |

**Cab**
| `cab_lowcut` | log [20, 500] step 1 | 80 | Hz |
| `cab_highcut` | log [2000, 20000] step 1 | 8000 | Hz |

**Tone stack** (NAM knob units, 0-10, 5=flat/noon)
| `eq_bass` | [0, 10] step 0.1 | 5 | (150 Hz low shelf, 4 dB/unit) |
| `eq_mid` | [0, 10] step 0.1 | 5 | (425 Hz peak, 3 dB/unit) |
| `eq_treble` | [0, 10] step 0.1 | 5 | (1800 Hz high shelf, 2 dB/unit) |

**Modulation**
| `mod_type` | choice {Chorus=0, Phaser=1, Tremolo=2} | Chorus(0) | — |
| `mod_rate` | log [0.05, 10] step 0.001 | 1 | Hz |
| `mod_depth` | [0, 1] step 0.001 | 0.4 | — |
| `mod_mix` | [0, 1] step 0.001 | 0.35 | — |

**Delay**
| `delay_time` | log [20, 2000] step 0.1 | 420 | ms |
| `delay_feedback` | [0, 0.95] step 0.001 | 0.35 | — |
| `delay_mix` | [0, 1] step 0.001 | 0.25 | — |

**Reverb**
| `reverb_size` | [0, 1] step 0.001 | 0.5 | — |
| `reverb_damping` | [0, 1] step 0.001 | 0.5 | — |
| `reverb_mix` | [0, 1] step 0.001 | 0.25 | — |

Note: label/suffix strings above ("dB", "ms", ":1", "Hz", "dBu") are what
`AudioParameterFloatAttributes().withLabel(...)` sets — this is what JUCE's
default slider text box appends after the numeric value. React should append
the same suffix strings for parity, with decimal precision inferred from each
param's `interval` (e.g. 0.1 → 1 decimal place typically; JUCE's default
formatting derives digits from the interval's magnitude — verify exact rounding
behavior empirically against the built plugin if pixel/text-exact parity
matters, since no explicit `setNumDecimalPlacesToDisplay` calls were found in
this codebase, i.e. all decimal formatting is JUCE's implicit default, not a
hand-specified format string).

---

## Chain order & block identity (`src/dsp/ChainOrder.h`)

- 9 block types, fixed enum order in `BlockId`: gate, comp, drive, amp, cab, eq,
  mod, delay, reverb — this enum order is ALSO the "add menu" ordering when
  multiple blocks are missing from the chain simultaneously (menu iterates
  `blockInfos` in this fixed order, not the current chain order), and is the
  default full-chain order (`defaultOrder()`).
- `BlockInfo` per type: `token` (persisted short id, e.g. "gate"), `displayName`
  ("Noise Gate"), `shortName` (tile caption, "GATE"), `enableParamId`
  ("gate_on") — table already exhaustively covered inline in the SignalChainView
  and BlockParamPanel sections above; this is the canonical source.
- DOMAIN: order is explicitly **not** an APVTS parameter — it's serialized as a
  comma-joined token string (`"gate,comp,drive,amp,cab,eq,mod,delay,reverb"`),
  with a special sentinel `"-"` (`emptyChainToken`) distinguishing "user removed
  every block" from "no chain data present / old preset" (which falls back to
  `defaultOrder()`). `fromString` tolerantly drops unknown tokens and
  de-duplicates (keeps first occurrence). This distinction matters for React's
  state-restore logic: an explicitly-empty saved chain must restore as empty,
  not silently repopulate with the default 9 blocks.
- Packing (`pack`/`unpack`) to a `uint64_t` (4 bits count + 4 bits per entry,
  up to 9 entries) is purely an audio-thread lock-free transport detail — no UI
  relevance, omit from the React port entirely (the web UI will talk to
  whatever bridge/IPC layer exists, not a raw bitpacked integer).

---

## Cross-cutting behavior notes for the React rewrite

1. **Polling timers to replace with reactive state**: PluginEditor 30Hz meter
   pull, HeaderBar 2Hz A/B-highlight poll, SignalChainView 100Hz *(sic, actually
   100ms = 10Hz)* param-bypass poll, AmpBody 4Hz model-status poll. All exist
   because JUCE's APVTS doesn't push change notifications to arbitrary UI code
   without per-parameter `Listener` boilerplate; a React app with a proper
   store/selector architecture (or a WebSocket/message-bridge push model) should
   replace ALL of these with direct subscriptions, EXCEPT the meter's ~30Hz
   redraw cadence, which is a real animation/refresh-rate concern to keep (or
   drive from `requestAnimationFrame` instead of a fixed 30Hz timer).
2. **SafePointer / null-checks in async callbacks** are pure JUCE-lifetime
   WORKAROUNDs (component-outlives-async-callback protection) — irrelevant in
   React where callbacks should check "is this still mounted"/AbortController
   patterns idiomatically instead, or better, hold TONE3000 download state in a
   store that outlives any single component's mount cycle (mirroring the
   "AmpBody/CabBody constructed once, kept alive" trick, but done properly via
   app-level state rather than component lifetime).
3. **`updatingCombo` reentrancy flags** (HeaderBar preset combo, AmpBody model
   combo, CabBody IR combo) are WORKAROUNDs for JUCE ComboBox's onChange firing
   during programmatic repopulation — a controlled `<select>`/listbox driven
   from state won't have this problem.
4. **`applyingOrder` reentrancy guard** in SignalChainView — WORKAROUND, see
   dedicated section above.
5. **Real domain behavior that must transfer literally, not just "similar
   enough"**:
   - amp-priority / first-block / empty selection fallback rule
     (`ensureValidSelection`).
   - append-always-selects vs remove-uses-fallback asymmetry.
   - shift-click A/B capture-vs-recall semantics + exact tooltip text.
   - prev/next preset wrap-around vs prev/next model/IR "no-current-selection"
     asymmetric start point.
   - bypass toggle via LED / double-click / context-menu all mapping to the
     same `*_on` parameter with proper begin/end change-gesture semantics.
   - drag-to-reorder's 5px start threshold, nearest-slot-wins snapping, and the
     24×48px cancel-zone tolerance on release.
   - empty-chain "-" sentinel vs missing-data default-order fallback.
   - TONE3000 configured/unconfigured gating of the Browse button and the
     specific nudge/error text swapping logic in `hintLabel`.
   - `isSlimmable()` gating of the SLIM control, and `hasModel()` vs
     "path loaded but engine not running" distinguishing the amp status text.
   - PeakMeter's peak-since-last-poll display decay + 1500ms hold + 90%-per-tick
     hold decay curve, and the color-by-segment-center vs color-by-actual-level
     distinction between the segment ladder and the hold tick.
6. **Processor state restore**: no dedicated "on restore" UI hook exists beyond
   the callbacks already covered — `proc.onChainChanged` (chain order),
   `proc.presets.onPresetChanged` (preset name/list), `proc.library.onChanged`
   (model/IR file lists), and the various polling timers picking up parameter
   value changes indirectly. A full state restore (host loading a saved
   session) is expected to fire `onChainChanged` (rebuilds the tile lane) and
   let the normal per-block `SliderAttachment`/`ButtonAttachment`/
   `ComboBoxAttachment` bindings pick up restored parameter values
   automatically (JUCE APVTS attachments are inherently reactive to host state
   changes) — the React equivalent needs an explicit "full resync" action
   dispatched on session/state-load rather than relying on N independent
   widget-level bindings magically catching up, since React won't have the
   same automatic two-way `AudioProcessorValueTreeState` binding.
