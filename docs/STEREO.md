# Stereo Chain — Design Spec

Status: approved design, 2026-08-03. Implementation branch: `feat/stereo-chain`.
Research basis: `docs/research/nam-a2.md` (A2 CPU/format), NAM stereo recon (maintainer
confirms mono-only core, NeuralAmpModelerPlugin issue #108; stereo = dual mono instances,
~2x CPU, no weight sharing — each instance re-parses and owns its Eigen tensors).

## Scope

1. ~~**Dual-NAM stereo amp**~~ — **SUPERSEDED by docs/SPLIT.md** (see §1 below). Engine B
   ships as the freely-placeable `amp2` chain block instead of a mode on the amp block;
   the `amp_stereo` parameter and the amp's per-channel path never shipped and are gone.
2. **Stereo delay modes** — the delay block gains a mode switch: Stereo (today's
   behavior), Ping-Pong (cross-feedback), Dual (independent R time via ratio), plus a
   wet width control.
3. **Reverb width** — expose `juce::Reverb`'s already-wired `width` field as a param.

Out of scope (deliberate, note in docs): amp pan/spread/polarity switches, tape delay
mode, reverb pre-delay/freeze, stereo mod. (Parallel chain paths were out of scope here
and are the subject of docs/SPLIT.md.)

## Invariants that must not regress (from chain-v2)

- Chain order is STATE, never a parameter. APVTS ids frozen; new params are APPEND-ONLY
  at the end of `createParameterLayout()` with `kVersionHint2`, literal id constants in
  `Parameters.h`, instance-indexed arrays (never string concat). (SPLIT.md does add
  BlockIds — appended, with the packed-word asserts as the tripwire.)
- `loadedModelPath` (and new `loadedModelPathB`) guarded by `pathLock`.
- `applyStateVar` publishes chain order BEFORE synchronous model/IR loads.
- Latency reported change-detected from processBlock AND updateLatency(); NAM latency
  counts only when the block is present + on.
- Gate split rule: measure at gate position, defer apply to post-NAM iff amp later+on
  (SPLIT.md §3 narrows "later" to "later in the same segment").
- The `monoScratch` mono-collapse path is the ONLY amp path: both amp blocks collapse,
  process, expand.
- Reset-on-reentry via pendingReset; audio thread never allocates or locks.

## 1. Dual-NAM stereo amp — SUPERSEDED (docs/SPLIT.md)

The stereo MODE on the amp block never shipped: `amp_stereo` is deleted (parameter,
relays, UI toggle) and the amp block collapses to mono always, as it always did. Engine
B is now reached through the `amp2` chain block — a second NAM block the user places
anywhere, most usefully inside one lane of a split. Two engines either way, same CPU;
what changed is that the routing is chain order instead of a hidden per-channel path.
The DSP contract for both amp blocks lives in docs/SPLIT.md §3.

Everything below this line survived the supersession and is still the contract:

### Parameters / state (unchanged, engine B is amp2's)

- Non-param state `modelPathB` (string), a sibling of `modelPath` threaded through ALL
  FOUR serialization sites: `getStateInformation`, `setStateInformation`,
  `captureStateVar`, `applyStateVar` (missing key = empty = no B model; old state loads
  unchanged). Member `loadedModelPathB` under the existing `pathLock`.
- Processor API: `loadModelB(file)`, `clearModelB()`, `getModelInfoB()` mirroring the
  A-side methods. Second `NamEngine namEngineB` member. `collectGarbage()` mirrored.
  `setSlimSize()` fans out to BOTH engines (single UI control).
- `namEngineB.applyStaging()` runs first-thing every block alongside A's, whatever the
  chain contains — a staged swap must never sit stale because amp2 is out of the chain.
- `hasLiveModel()` is an audio-thread-safe query (std::atomic<bool>, published by
  applyStaging / clear). It is deliberately NOT `hasModel()`, which is the UI's "loaded
  or staged" question and goes true at loadModel() time: gating DSP on that would run
  one block through an engine whose live model is still null. The amp2 block gates on
  `hasLiveModel()`, and its latency contribution uses the identical test.

### Loading rules (unchanged)

- `loadModelB` is message-thread-only, same contract as `loadModel` (synchronous IO +
  prewarm + staged swap). No dspData sharing across engines (parse twice; acceptable —
  load is a UI action, and NAM core offers no weight sharing anyway).
- Clearing model A does NOT clear B; an empty B just makes amp2 a pass-through.
- Model B failing validation surfaces the same error string path as A.

### What was removed with the stereo path

- The `amp_stereo` parameter and its relay/UI entries.
- The amp block's per-channel branch (engine A on L, engine B on R) and its
  `applyStereoGain` two-channel ramp helper.
- The L/R engine latency compensation. The `latencyRing` cluster it used is repurposed
  for per-LANE compensation in SPLIT.md §3 — same ring, same primed-flag staleness rule,
  two of them now.
- The stereo term in `computeWantedLatency`; latency is now serial + max(lane A, lane B).

## 2. Stereo delay modes

### Parameters (per instance x3: `delay`, `delay2`, `delay3`)

- `delay_mode` / `delay2_mode` / `delay3_mode` — AudioParameterChoice
  `{"Stereo", "Ping-Pong", "Dual"}`, default 0 (Stereo = exactly today's DSP).
  Choice list is FROZEN once shipped — do not reorder/append later without care.
- `delay_ratio` / … — float, 25..200 (%), default 100. R time = L time x ratio/100.
  Used in Dual mode only (visible always, inert otherwise is fine for v1 UI).
- `delay_width` / … — float 0..1, default 1. Wet-signal M/S width (1 = unchanged,
  0 = mono wet). Applies in all modes; stereo hosts only.

### DSP (`DelayFx` in FxBlocks)

`setParameters(timeMs, feedback, mix, mode, ratioPct, width)`. Two smoothed delay
lengths: `delaySamplesL` (existing) and `delaySamplesR` (= L x ratio in Dual, = L
otherwise). Process becomes sample-major when channels == 2 and mode != Stereo:

- **Stereo** (mode 0): keep the existing per-channel loop verbatim — bit-identical
  output for old sessions.
- **Ping-Pong**: input seeds line L summed to mono; cross-feedback:
  ```
  dl = pop(0, tL); dr = pop(1, tL)          // both lines at the L time
  push(0, inMono + dr * fb)                  // R line output crosses back to L
  push(1, dl * fb)                           // L line output crosses to R
  wetL = dl; wetR = dr
  outCh = dry*ch + wet*mix                   // per channel dry stays true stereo
  ```
  First repeat lands L at t, then R at 2t, alternating; `feedback` sets decay.
- **Dual**: per-channel self-feedback exactly like Stereo but line R runs at
  `tR = tL * ratio` (each channel seeds its own line — preserves stereo input).
- **Width**: post-wet M/S scale on the wet contribution only:
  `mid = (wl+wr)/2, side = (wl-wr)/2 * width` → `wl' = mid+side, wr' = mid-side`.
- **Mono host (channels == 1)**: all modes collapse to the existing single-line path.
- Mode changes need no reset (stale echoes decay naturally); the pendingReset
  re-entry rule is unchanged.

## 3. Reverb width

- `reverb_width` / `reverb2_width` / `reverb3_width` — float 0..1, default 1
  (today's hardcoded value → bit-identical by default).
- `ReverbFx::setParameters(size, damping, mix, width)` writes `params.width`.
- juce::Reverb sums input to mono internally and decorrelates via per-channel comb
  banks; width crossfades wet1/wet2 — no structural work.

## 4. Bridge / UI

Relay additions (C++ `WebEditor.cpp` arrays + static_assert counts, TS
`SLIDER_PARAM_IDS`/`TOGGLE_PARAM_IDS`/`COMBO_PARAM_IDS` in the SAME append order,
`paramMeta.ts` entries):

- Sliders +9: delay_ratio x3, delay_width x3, reverb_width x3  → 72 → 81
- ~~Toggles +1: amp_stereo → 26 → 27~~ — SUPERSEDED: the toggle is removed again by
  docs/SPLIT.md §2, which counts from the 27 this line produced.
- Combos  +3: delay_mode x3                                     → 4 → 7

UiState: add `modelB` (same ModelInfo shape as `model`, null when empty); poll it in
`timerCallback` alongside `pollModelAndIr`. New natives: `loadModelB(path)`,
`clearModelB()`. Keep `loadModel`'s signature untouched (A stays default target).

UI components:

- ~~**AmpBody**: a STEREO toggle + Model B row~~ — SUPERSEDED by docs/SPLIT.md §5: the
  model-B management (library picker, "Use model A", clear, status) moves to `Amp2Body`
  and AmpBody loses the section entirely. Slim-size stays a single control on AmpBody
  (it applies to both engines).
- **DelayBody** (new, modeled on ModBody): `${token}_mode` ParamMenu + KnobRow. Extend
  `BASE_KNOB_SPECS.delay` with ratio + width knobs; instances inherit via blockRecord.
- **Reverb**: extend `BASE_KNOB_SPECS.reverb` with width (generic KnobRow, no body).
- Mock bridge: mirror new params + modelB state so `npm run dev` keeps working.

## 5. CPU note (from research)

Dual A2 inference is linear 2x with no shared-compute discount, but A2-Full is 30-40%
cheaper than A1-Standard with the fast path (`NAM_ENABLE_A2_FAST`, on by default);
an Apple M-series machine runs ~64 A2-Full instances real-time, so stereo dual-amp is
~1/32 of headroom on target hardware. Slim-size control scales both sides together.
Denormals: `ScopedNoDenormals` already covers the whole processBlock — includes B.

## 6. Tests

Extend `tests/ProcessorSmokeTest.cpp`:
- ~~stereo amp path: amp_stereo on + two engines~~ — SUPERSEDED by docs/SPLIT.md §7
  (dual amp = amp in lane A + amp2 in lane B).
- state round-trip: modelPathB survives capture/apply and get/setStateInformation;
  absent key loads as empty.
- delay modes: each mode processes stereo + mono buffers finite; mode 0 output matches
  pre-change behavior for a fixed impulse (regression guard).
