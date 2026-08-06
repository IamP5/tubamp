# Split Paths — Design Spec

Status: approved design, 2026-08-04. Branch: `feat/stereo-chain` (continues the stereo
work). Research basis: QC/Helix/BIAS FX/Tonocracy routing recon (2026-08-04) + codebase
capacity reads. User decisions locked in: (1) this feature SUPERSEDES the `amp_stereo`
toggle from docs/STEREO.md §1 — AMP 2 as a chain block is the one way to run dual amps;
(2) exactly ONE split region per chain, lanes always rejoin at the mixer, no nesting.

## What it is

Helix/QC-style parallel lanes expressed as ordinary chain tokens on the existing board:

```
[pre blocks...] → SPLIT ⇉ lane A blocks...   ⇉ MIXER → [post blocks...]
                         ⇉ lane B blocks...  ⇉
```

- `split`, `mix` are new singleton tiles (with panels); the lane boundary is a hidden
  structural token `lane2` (never rendered as a tile, never has a panel).
- `amp2` is a new singleton block tile: the second NAM engine (engine B) as a
  freely-placeable chain block — inside a lane, or anywhere serially.
- The flat order fully encodes the structure:
  `..., split, <laneA ids...>, lane2, <laneB ids...>, mix, ...`
  Empty lanes are legal (`split, lane2, mix` = passthrough fork).

## Invariants (chain-v2 + stereo-chain, unchanged)

- Chain order is STATE; packed into ONE atomic 128-bit word; audio thread decodes lanes
  from that word alone. No side-channel carries execution semantics (chainRows stays
  layout-only, tolerant, degrade-to-auto).
- APVTS append-only, kVersionHint2, literal ids, instance arrays. pathLock guards model
  paths. applyStateVar publishes chain before loads. pendingReset drain rule unchanged.
- parseOrder stays tolerant; structural validation degrades gracefully (below), never
  errors, never lets garbage reach the packed word.

## 1. ChainOrder.h changes (foundational — exact contract)

New BlockIds appended after `reverb3 = 23`:

```cpp
split = 24,   // token "split",  "Splitter",  "SPLIT",  enable "split_on"
lane2 = 25,   // token "lane2",  "Lane B",    "LANE B", enable "split_on" (never shown)
mix   = 26,   // token "mix",    "Mixer",     "MIX",    enable "mix_on"
amp2  = 27,   // token "amp2",   "NAM Amp 2", "AMP 2",  enable "amp2_on"
```

- `numBlockTypes` 24 → 28 (≤ 31 assert unchanged, still gates growth).
- **Decouple the length cap**: `inline constexpr int maxChainLength = 24;` — no longer
  derived from numBlockTypes (5 + 5*24 = 125 ≤ 128 stays true). Rewrite the adjacent
  comment: it is a chosen cap now, not an id-space consequence.
- Fix the `reverb3 == numBlockTypes - 1` static_assert → `amp2 == numBlockTypes - 1`,
  and keep a new assert pinning `split == 24` (persisted-token layout tripwire).
- **`instanceOf()` landmine**: the arithmetic `(index - numV1BlockIds) % 2 + 1` is wrong
  for ids > reverb3 — bound it: ids above `reverb3` return 0 (singletons). `kindOf()` /
  `instanceId()` / `numInstancesOf()` switches get the four new cases (each its own
  kind, 1 instance).
- All four get `blockInfos` entries (tokens above). `lane2` is furniture: the UI never
  renders it as a card; its BlockInfo exists so parse/pack round-trip it.
- New helpers in `tubamp::chain`:

```cpp
struct Structure { int splitAt = -1, lane2At = -1, mixAt = -1;
                   bool valid() const; };            // valid: -1s all or 0<=s<l<m
Structure findStructure (const Order&) noexcept;      // first occurrence of each
Order sanitizeStructure (Order);                      // enforce the grammar
```

`sanitizeStructure` rules (tolerant-degrade, in this priority):
1. Drop every occurrence of a structural id (`split`/`lane2`/`mix`) beyond its first
   (parseOrder's dedup already guarantees this — keep as belt).
2. If the full triple exists in order split < lane2 < mix → keep.
3. Otherwise remove ALL structural tokens (flatten to serial). A lone split, a mix
   before split, a missing lane2 — all flatten. `amp2` is NOT structural: it survives
   flattening like any block.

Every entry point runs it: `parseOrder`-consumers in PluginProcessor (`setChainOrder`,
both state-restore paths) call `sanitizeStructure` after parse, before publish. The
packed word therefore only ever holds a valid structure or none.

## 2. Parameters (append at END, kVersionHint2; REMOVE `amp_stereo`)

`amp_stereo` was never shipped (branch uncommitted) — delete it: Parameters.h/.cpp
entry, ParamPtrs, relay arrays (C++ + TS), paramMeta, AmpBody toggle. Delay/reverb
stereo modes and ALL model-B machinery (engines, natives, polling) stay.

New params (13), append in this exact order:

| id | type | range/choices | default |
|---|---|---|---|
| `amp2_on` | bool | | true |
| `amp2_input` | float dB | -24..24 | 0 |
| `amp2_output` | float dB | -24..24 | 0 |
| `split_on` | bool | | true |
| `split_mode` | choice | {"Copy", "L/R", "X-Over"} (FROZEN) | 0 |
| `split_xover` | float Hz | 100..4000 log | 800 |
| `mix_on` | bool | | true |
| `mix_alevel` | float dB | -60..12 | 0 |
| `mix_blevel` | float dB | -60..12 | 0 |
| `mix_apan` | float | -1..1 | 0 |
| `mix_bpan` | float | -1..1 | 0 |
| `mix_bphase` | bool | | false |
| `mix_level` | float dB | -60..12 | 0 |

Relay deltas vs current (81/27/7): sliders 81 −0 +8 = **89**, toggles 27 −1 +4 = **30**,
combos 7 +1 = **8**. Match amp2_input/output range+default to amp_input/amp_output's
existing spec (verify in Parameters.cpp — mirror exactly).

## 3. DSP execution (processBlock)

**Factor the per-id dispatch** out of the loop body into a member helper
`processChainBlock (chain::BlockId id, juce::AudioBuffer<float>& buf,
juce::dsp::AudioBlock<float> block, int numSamples, <the few loop-scope flags>)` so a
block can run on the main buffer or a lane scratch. Every case already takes a generic
AudioBlock except:
- FxHost: takes AudioBuffer& — pass the lane buffer; its `hostChannels != channels`
  gate passes because lane buffers are sized to the host channel count.
- Amp: refactor the mono-collapse path to collapse ITS TARGET BUFFER's channels into a
  caller-provided mono scratch, process, expand back to the target buffer. `amp` case
  uses engine A + amp_input/amp_output + ampEq; `amp2` case uses engine B +
  amp2_input/amp2_output and NO tone stack (users add an eq instance; note in docs).
  amp2's gains are calibrated like amp's — `amp2InputDb()`/`amp2OutputDb()` add engine
  B's own input-calibration and output-mode compensation, sharing the amp block's
  `amp_out_mode`/`amp_cal_input`/`amp_cal_level` knobs (amp2 has no duplicates of
  them). This is what keeps two lanes loudness-matched by default; without it lane B
  sits at the model's raw level, an arbitrary tens-of-dB imbalance.
  `amp2` gates on `namEngineB.hasLiveModel()` (pass-through otherwise, latency 0 —
  matches the latency rule below). Both engines' `applyStaging()` still run first-thing
  every block unconditionally.

**The stereo amp path from STEREO.md §1 is REMOVED** (superseded): the amp case goes
back to mono-collapse always (against its target buffer). Keep `applyStereoGain`
(renamed if clearer) — the materialized-ramp helper is reused by the mixer. The
`latencyRing` cluster is repurposed for LANE compensation (below); the per-channel
L/R engine compensation dies with the stereo path.

**Lane execution** (decoded once per block from the packed word via
`chain::findStructure` on the unpacked order — allocation-free, stack arrays):

1. Entries before `splitAt`: process on the main buffer (as today).
2. At `split`: snapshot into `laneScratch` (2ch × samplesPerBlock member, allocated in
   prepareToPlay, mirrors monoScratch pattern) per `split_mode`:
   - Copy: laneScratch = buffer (both lanes get the full signal).
   - L/R: buffer(lane A) ← ch0 on all its channels; laneScratch(lane B) ← ch1 on all
     its channels. Mono host: same as Copy.
   - X-Over: LinkwitzRiley 4th-order pair (two `juce::dsp::LinkwitzRileyFilter` members,
     prepared in prepareToPlay, reset via resetBlockInstance(split)): lane A ← lows,
     lane B ← highs, crossover at `split_xover` (setCutoffFrequency each block).
   - `split_on == false`: lane B is muted at the mixer (lane A processes normally;
     lane B blocks still run on laneScratch to keep their state warm — cheap and keeps
     re-enable click-free; the MUTE happens at the mix stage).
3. Entries in (splitAt, lane2At): main buffer. Entries in (lane2At, mixAt): laneScratch.
4. At `mix`: per-lane smoothed level×pan gains (equal-power pan: gL = cos((p+1)π/4),
   gR = sin((p+1)π/4)), lane B optionally phase-inverted, summed into the main buffer,
   then master `mix_level` ramp. All smoothers are NEW members (never call an existing
   smoother twice per block); ramps materialized once into monoScratch (idle here)
   exactly like the existing helper. `mix_on == false` → unity sum, params ignored
   (smoothers still advance). Mono host: pans inert (gains collapse to level only).
5. Entries after `mixAt`: main buffer.
6. No structure in the order → the loop is EXACTLY today's (single pass, main buffer).
   Zero-cost when unused; old sessions bit-identical.

**Lane latency compensation**: per-lane latency = Σ over that lane's entries of
(amp → engineA latency if amp_on; amp2 → engineB latency if amp2_on && hasLiveModel;
fxN → fxHost.latencyFor(slot)). At the mix stage, delay the LOWER-latency lane by the
difference through a per-lane compensation ring (two 2-ch rings, capacity 65536 — a
hosted linear-phase EQ or lookahead limiter in one lane reports tens of thousands of
samples, so 8192 would have left the lanes misaligned while the report claimed otherwise;
the reported figure is capped at the same number so the two always agree — the
repurposed latencyRing machinery incl. the primed-flag staleness rule). Reported host
latency = serial-section latency + max(laneA, laneB); `computeWantedLatency` and
`updateLatency` share the identical lane-aware rule (message-thread version derives
lane membership from uiChainOrder under chainLock).

**Gate defer generalization**: defer applies iff gate and amp sit in the SAME segment
(both pre-split, both post-mix, both in lane A, or both in lane B) with amp later and
both on. Cross-segment: gate applies at its own position (no defer). `amp2` NEVER
receives a deferred gate (single trigger/curve — document). This preserves today's
behavior exactly for structureless chains.

**pendingReset**: `resetBlockInstance` gains cases for split (X-over filters), mix
(smoothers snap), amp2 (nothing — engine state is managed by NamEngine). The existing
absent→present edge detection needs no change (present[] is lane-blind and that is
correct for it). `kMaxResetsPerCallback` stays 2.

## 4. State / serialization

Nothing new: the order string simply contains the new tokens, riding the existing
chainOrderV2 scheme (ids ≥ 12 already route there; v1-safe writer already emits "-").
`modelPathB` serialization from STEREO.md stays verbatim (it is amp2's model now).
chainRows stays layout-only. A/B + presets get structure for free via the order string.
Structural sanitize runs at every restore boundary (see §1) — a hand-edited preset with
a dangling `split` flattens to serial, never errors.

## 5. Board UI

Data contract: `BLOCK_IDS` grows to 28 mirroring the enum; `lane2` exists in the type
but is filtered from every card-rendering path. TS mirrors of `findStructure` /
`sanitizeStructure` live beside `normaliseChainRows` in the store; the UI must never
commit a malformed structure (C++ flatten is the backstop, not the norm).

Rendering: when the order contains a structure, the board renders the split region as a
lane container — SPLIT tile, then lane A's cards on an upper half-track and lane B's on
a lower half-track (shared column grid, half-height lane tracks), then MIX tile. The
container is one row-equivalent for outer wrap math and contributes its taller height to
the stage fit/pan box. Connectors: split fans out to both lane heads; both lane tails
converge on mix. Serial sections render exactly as today.

Drag rules (enforced in the drag machine against frozen geometry, mirrored in the
commit sanitizer):
- Ordinary blocks (incl. amp, amp2, fx slots) drop anywhere: serial sections or either
  lane track.
- SPLIT/MIX tiles: draggable only as a pair-preserving unit along the serial sections
  (v1 simplification: they may also simply be non-draggable — implementer's choice,
  note which in REACT-UI.md); deleting either deletes the structure (lanes flatten
  into the serial chain at the split's position, lane A first then lane B).
- Creating a split: an "Add split" entry in the existing add-block affordance inserts
  `split, lane2, mix` at the insertion point.
- `lane2` is never a drop target/tile; the UI recomputes its position from lane
  membership on every commit.

Panels: `SplitBody` (mode ParamMenu + xover knob, xover shown only in X-Over mode is
optional), `MixBody` (A/B level + pan knobs, phase B toggle, master level), `Amp2Body`
(model B management moved from AmpBody's stereo section: picker over the library list,
"Use model A", clear, status — plus amp2 in/out knobs). AmpBody loses its STEREO
section entirely. Mock bridge: mirror new params, add a malformed-structure preset
fixture (dangling split) to exercise the TS sanitizer, keep modelB state.

## 6. CPU / scope notes

amp + amp2 = the same 2 NAM instances the stereo amp used — no budget change. Lanes are
sequential on the audio thread (no threading); ScopedNoDenormals already covers both.
Out of scope v1 (document): nesting, >2 lanes, split/mix instance pools, dynamic split,
amp2 tone stack, per-lane FxHost pools beyond the existing 3 shared slots (each slot id
lives in exactly one lane at a time — already single-stream).

## 7. Tests (extend ProcessorSmokeTest)

- Structure parse/sanitize: valid triple survives round-trip; dangling split flattens;
  mix-before-split flattens; amp2 survives flattening.
- Structureless chains: output bit-identical to pre-split build (regression guard on a
  fixed impulse through the classic chain).
- Lane processing: split Copy/L-R/X-Over all finite on stereo + mono hosts; X-Over
  lanes sum flat (LR4 property) with mixer at unity.
- Dual amp: amp in lane A + amp2 in lane B, both engines loaded → finite, non-silent,
  and hard-panned mixer yields different L/R.
- Latency: serial+max(lane) reporting; lane compensation delays the shorter lane
  (impulse alignment test through an fx-latency mock or two different-rate models).
- State: order with structure round-trips get/set + capture/apply; amp_stereo absent
  from the param tree.
