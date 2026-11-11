# FX Engines — Build Spec (gate, compressor, drive, EQ, chorus, phaser, tremolo)

Status: spec, 2026-11-11. Branch base: `main` (worktree `claude/tubamp-effects-reverse-engineer-90c7f7`).
Implements the effect study in `docs/research/fx/` (README, per-effect `report.md` +
`verification.md`), with every verification correction applied.

**How to read this.** Everyone reads §1 (fences), §2 (architecture) and §3 (file
ownership). An engine engineer then reads only their own section in §4. The scaffold
engineer reads §5, integrators §6, the UI engineer §7. §8 lists the decisions this spec
took that the owner may want to revisit.

Tags used for every constant: `[study: path]` the study states it and verification did
not overturn it; `[ver: path]` a verification file corrected the study and this is the
corrected value; `[circuit: source]` a published schematic or component model;
`[fit: path]` fitted to measured data in the study's evidence; `[sim: §x]` a numpy model
of exactly the algorithm written here, run while writing this spec (the check script
must reproduce it); `[choice]` our own engineering choice, with the reason. Nothing is
copied from a vendor binary: the study's evidence describes structure only.

Paths are relative to the repo root. Line numbers refer to the base commit `bf81728`.

---

## 1. Scope and fences

### 1.1 In scope

Seven effects get new engines: **gate, compressor, drive, EQ, chorus, phaser, tremolo**
(the last three stay one block kind, `mod`, selected by the existing `mod_type`). The amp
block's own tone stack (`amp_eq_*`) gets the exact-flat fix and knob smoothing only.

### 1.2 Hard fences (each one is a mechanical check)

| # | Rule | How to check |
|---|---|---|
| F1 | Reverb and delay do not change. `src/dsp/Reverb.*`, `src/dsp/Delay.*`, `src/dsp/FxPrimitives.h`, `docs/REVERB-DELAY.md`, their parameter ids, ranges, defaults, choice lists, `kDelaySyncBeats`, `ReverbBody.tsx`, `DelayBody.tsx` and the reverb/delay entries of `typeDefaults.ts` are byte-identical to the base commit. Their audio is bit-identical. | `git diff --stat bf81728 -- src/dsp/Reverb.* src/dsp/Delay.* src/dsp/FxPrimitives.h docs/REVERB-DELAY.md ui/src/features/panel/ReverbBody.tsx ui/src/features/panel/DelayBody.tsx` is empty; golden cases `rv*` and `dl*` (§5.4) match. |
| F2 | The cab / IR block is untouched: `CabSim` (in `FxBlocks.h/.cpp`), `cab_*` parameters, `CabBody.tsx`. | `CabSim` declaration and definition text identical to the base (diff of `FxBlocks.*` shows no line inside `class CabSim` / `CabSim::`); golden case `cab0` matches. |
| F3 | Every existing parameter id, range, step, skew, default, choice list and version hint is frozen. New parameters are **appended** after the last line of `createParameterLayout()` that exists today (`src/Parameters.cpp:333`), with version hint **4**. | `tubamp_golden --params` (§5.4) dumps the layout; the first 166 entries equal the base dump exactly (id, name, range, default, hint, order). |
| F4 | Engines may include `FxPrimitives.h` read-only and must not add to it or to any other shared header. An engine's files are its own `.h/.cpp` pair (plus its fxbench file and check script). | §3 table; CI-style check: `git diff --name-only` of an engine branch lists only its owned files. |
| F5 | Classic (type 0) of every engine is today's algorithm bit-for-bit, except the two deliberate fixes of D2: (a) EQ / amp EQ with all three knobs at exactly 5.0 become an exact pass-through; (b) the drive's oversampling latency is reported and compensated. | golden cases (§5.4) with the expected-change list in §5.4.3. |

Out of scope: reverb, delay, cab, amp/NAM, split/mix, FX slots, presets UI.

---

## 2. Architecture

### 2.1 The engine shape

Each effect is one engine in its own pair of files, mirroring `src/dsp/Delay.h`:

| Effect | Files | Class | Params struct | Choice parameter |
|---|---|---|---|---|
| Gate | `src/dsp/Gate.h/.cpp` | `GateFx` | `GateParams` | `gate_mode` (`GateFx::Mode`) |
| Compressor | `src/dsp/Compressor.h/.cpp` | `CompFx` | `CompParams` | `comp_type` (`CompFx::Type`) |
| Drive | `src/dsp/Drive.h/.cpp` | `DriveFx` | `DriveParams` | `drive_type` (`DriveFx::Type`) |
| EQ | `src/dsp/Eq.h/.cpp` | `EqFx` | `EqParams` | `eq_type` (`EqFx::Type`), `eq_voice` |
| Chorus | `src/dsp/Chorus.h/.cpp` | `ChorusFx` | `ChorusParams` | `mod_chorus_mode` |
| Phaser | `src/dsp/Phaser.h/.cpp` | `PhaserFx` | `PhaserParams` | `mod_phaser_model` |
| Tremolo | `src/dsp/Tremolo.h/.cpp` | `TremoloFx` | `TremoloParams` | `mod_trem_voice` |

Every engine has this public surface (deviations: the gate's measure/apply split, §4.1;
the compressor's `skip`, §4.2; the drive's static `latencyFor`, §4.3.7):

- `prepare (const juce::dsp::ProcessSpec&)` — not real-time; allocates everything.
- `reset()` — audio-thread safe, no allocation; returns all audio state to fixed values.
- `setParameters (const XParams&)` — audio thread, once per block before `process()`;
  clamps every field to its documented range; never allocates.
- `process (juce::dsp::AudioBlock<float> block)` — in place, 1 or 2 channels, any
  `numSamples` from 1 up to `spec.maximumBlockSize` (a larger block must still produce
  finite output; it may process in sub-blocks).
- `int getLatencySamples() const noexcept` — integer, constant for a given
  (type, sample rate). Zero for every engine except `DriveFx` (§4.3).
- Meter getters where the effect has meters (gate, comp). Audio-thread only, plain
  floats, valid after the last `process()` / `measure()`.
- A frozen `Type` (or `Mode`/`Model`/`Voice`) enum with `numTypes`, a pimpl, and
  `inline constexpr` per-type starting-point arrays for knobs whose sensible start
  depends on the type.

**The pattern header** — every engine header follows this one exactly in layout,
comment style and naming. (`CompFx` is the full example because it has types, starting
points, a meter and the bypass `skip`.)

```cpp
#pragma once

#include <juce_dsp/juce_dsp.h>

#include <memory>

// The compressor block (docs/FX-ENGINES.md §4.2). One engine, five types:
//
//   Classic   juce::dsp::Compressor + smoothed makeup, exactly as before (type 0, frozen).
//   Clean     log-domain feed-forward, gain smoothed after the gain computer, linked.
//   Squash    log-domain feedback: the gain computer sees the output level (pedal style).
//   FET       Clean's detector with a two-stage release and a ratio-dependent threshold.
//   Opto      Clean's detector with a slow two-stage release and a wide knee.
namespace tubamp
{
/** Everything a compressor instance needs for one block, in the units Parameters.h documents. */
struct CompParams
{
    int type = 1;                 // CompFx::Type; out-of-range values clamp
    float thresholdDb = -20.0f;   // [-60, 0]
    float ratio = 4.0f;           // [1, 20]
    float attackMs = 5.0f;        // [0.1, 100]
    float releaseMs = 120.0f;     // [10, 1000]
    float makeupDb = 0.0f;        // [0, 24] manual makeup, added to auto makeup
    float kneeDb = 6.0f;          // [0, 24] total knee width (kCompKneeDefault)
    float mixPct = 100.0f;        // [0, 100] parallel mix, 100 = fully compressed
    float scHpfHz = 20.0f;        // [20, 300] detector high-pass; <= 20.5 = off
    bool autoMakeup = false;      // adds -gc(-10 dBFS) to the makeup
};

class CompFx
{
public:
    /** Order matches the comp_type choice list and is FROZEN: reordering it silently
        repoints every saved session. Append only. */
    enum class Type { classic = 0, clean = 1, squash = 2, fet = 3, opto = 4 };
    static constexpr int numTypes = 5;

    CompFx();
    ~CompFx();

    void prepare (const juce::dsp::ProcessSpec& spec);
    /** Clears detector and gain state. Does NOT touch the makeup smoother (Classic keeps
        today's behaviour: only prepare() re-primes it). */
    void reset();
    /** Audio thread, once per block before process() or skip(). */
    void setParameters (const CompParams& params);
    void process (juce::dsp::AudioBlock<float> block);
    /** Audio thread: the block is bypassed or absent this callback. Keeps the makeup
        smoother in step exactly like the old processor code did. */
    void skip (int numSamples) noexcept;

    int getLatencySamples() const noexcept { return 0; }
    /** Largest gain reduction applied during the last process(), dB >= 0, makeup excluded. */
    float getGainReductionDb() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CompFx)
};

// Where three knobs start for each type, indexed by CompFx::Type. A parameter has one
// default, which is the default type's entry; the editor applies the others when the user
// picks a type (docs/FX-ENGINES.md §2.7.4). Classic's entries equal the frozen defaults.

/** The knee is what most separates the characters: hard-ish for FET, very soft for Opto. */
inline constexpr float kCompKneeDefault[CompFx::numTypes]    { 6.0f, 6.0f, 6.0f, 4.0f, 12.0f };
/** Attack in ms: the knob means the same physical time in every type, but each type lives
    in a different range of it. */
inline constexpr float kCompAttackDefault[CompFx::numTypes]  { 5.0f, 5.0f, 2.5f, 0.8f, 10.0f };
/** Release in ms (FET/Opto: the fast stage; the slow stage follows it). */
inline constexpr float kCompReleaseDefault[CompFx::numTypes] { 120.0f, 120.0f, 150.0f, 55.0f, 60.0f };
} // namespace tubamp
```

### 2.2 Common contract (every engine)

- **Channels.** 1 or 2, as `spec.numChannels` says; the block's channel count may be
  lower than the spec's, never higher. Mono and stereo must both be tested.
- **Sample rates.** Correct from 22 050 Hz to 192 000 Hz. Every time constant is in
  seconds and every frequency in Hz; nothing is tuned in samples except where a section
  says so. Frequencies are clamped to `0.49 * fs` before any `tan()`.
- **Block size.** Any size from 1 to `maximumBlockSize`; results must not depend on the
  block size (except Classic paths, which keep today's per-block behaviour — the golden
  tool captures them at the block sizes it renders). New types that update coefficients
  every N samples keep their sub-block phase across `process()` calls.
- **Real-time safety.** No allocation, lock, system call, logging or exception on the
  audio thread (`reset`, `setParameters`, `process`, `skip`, `measure`, `apply`).
  `juce::AudioBuffer::setSize`, `std::vector::resize`, `new`, `std::function` copies are
  forbidden there. Verified by fxbench `--rt-check` (§5.3.4).
- **Finite output.** For any finite input and any parameter value (including values
  outside the documented range: `setParameters` clamps), output is finite. Recursive
  states pass through `fx::sanitise()` once per block (cheap) so a bad sample cannot
  live in a loop.
- **Non-finite input.** A NaN or infinite input sample must not latch any state or poison
  the rest of the block. The gate reads it as silence (X17), the compressor, drive and EQ
  replace it with 0 (§4.2.6, §4.3.3, §4.4.3 Addition 3, row E21), the chorus and phaser
  process it as 0 (CH20, P12), and the tremolo keeps it out of Bias's envelope and the
  Harmonic filters, so only the bad sample's own output is non-finite (N1). Classic paths
  are left as they were except where their section says so. The processor sanitises the
  input of every block it runs as well (processor: in progress).
- **No shared statics.** No mutable static or global state; two instances never
  interact.

### 2.3 How Classic stays bit-identical (decided once for all seven): MOVE, verbatim

The existing code is **moved verbatim into the engine** as its Classic path; nothing
wraps the old classes.

1. The engine engineer copies the Classic algorithm text into the engine's `.cpp`
   (an anonymous-namespace class `Classic<Effect>` or a section of the `Impl`), keeping
   **every expression, type (float vs double), operation order, JUCE object and call
   sequence** identical. Sources to copy:

   | Engine | Classic source (base commit) |
   |---|---|
   | Gate | `src/dsp/FxBlocks.cpp:25-30` (constants), `:222-344` (`NoiseGate::Impl`, prepare/reset/measure/apply), AudioDSPTools `dsp/NoiseGate.h` usage |
   | Comp | `src/PluginProcessor.cpp:1319-1331` (setters, `process`, makeup), `:38-49` (`applySmoothedGain`), `:893-898` (makeup prime), `:1096-1104` (absent-block skip); `juce::dsp::Compressor<float>` |
   | Drive | `src/dsp/FxBlocks.cpp:15-16, 32-39, 45-123` (`Drive`) |
   | EQ | `src/dsp/FxBlocks.cpp:19-23, 126-193` (`ToneStackEQ`) |
   | Chorus | `src/dsp/FxBlocks.cpp:349-415` (the chorus parts of `Modulation`), `juce::dsp::Chorus<float>` |
   | Phaser | `src/dsp/FxBlocks.cpp:349-424` (the phaser parts), `juce::dsp::Phaser<float>` |
   | Tremolo | `src/dsp/FxBlocks.cpp:364-405, 426-448` (the tremolo parts) |

   Where the old code applied a gain through `juce::AudioBuffer` (the compressor makeup),
   the Classic path wraps the block's channel pointers in a non-owning
   `juce::AudioBuffer<float> (ptrs, numChannels, numSamples)` — exactly as
   `processBlock` does for lane B (`src/PluginProcessor.cpp:1150-1155`) — and calls the
   copied `applySmoothedGain` unchanged, so JUCE's `approximatelyEqual` shortcuts in
   `applyGain`/`applyGainRamp` are preserved.

2. Bit-identity holds because the same source text compiled with the same flags in a
   different translation unit produces the same IEEE results (Apple clang's default
   `-ffp-contract=on` contracts within a statement only, so moving a statement keeps its
   FMA pattern; LTO inlining does not change contraction). Do not rewrite a Classic
   expression "equivalently", do not change float to double, do not hoist a constant.
3. During the parallel phase `FxBlocks.*` and `PluginProcessor.*` are **not touched** by
   engine engineers; the processor keeps running the old classes. Each engineer proves
   Classic identity offline with `tubamp_fxbench <effect> --classic-null` (§5.3), which
   runs the engine's Classic path and a **frozen reference copy** of the old code
   (`tools/fxbench/legacy/LegacyFx.{h,cpp}`, created by the scaffold from the base commit,
   namespace `tubamp::legacy`) on the same input and requires zero differing samples.
4. The integrator then deletes `Drive`, `ToneStackEQ`, `NoiseGate` from `FxBlocks.*`,
   turns `Modulation` into a dispatcher over the three mod engines, and removes the
   processor's `compressor` / `compMakeupLin` arrays (§6). The golden-render tool
   (captured before any change, §5.4) proves the whole plugin is still bit-identical.

New code may sit next to the Classic code in the same `.cpp`, but must not change a
single shared helper the Classic path uses.

### 2.4 Type switches without clicks

- Drive, EQ, phaser and tremolo own one `fx::EngineSwitcher` (from `FxPrimitives.h`, used
  read-only) exactly as `DelayFx` does (`src/dsp/Delay.cpp:508-576`): on a type change the effect
  fades to the untouched input over 20 ms, the incoming type is reset while silent, and
  fades back in over 20 ms. While steady the switcher costs one comparison and does not
  touch the audio, which is what keeps static Classic renders bit-identical.
- First block after `prepare()` or `reset()`: `switcher.snapTo (requestedType)` (the
  `snapEngine` pattern of `DelayFx`), so a session that starts on any type renders as if
  it had always been on it.
- Engines with a second choice inside a type (EQ `eq_voice` in Stack; the mod engines
  have one choice each) key the switcher on `type * 16 + subChoice`.
- **Exceptions — compressor and chorus.** The compressor does not use the switcher: a
  switch among Clean, Squash, FET and Opto hands the gain state over with no fade, and a
  switch to or from Classic crossfades both outputs over 40 ms (§4.2.6). The chorus keeps
  the outgoing mode sounding while the incoming line fills, then crossfades by measured
  power (§4.5.4). The tremolo and phaser use the switcher (their sync jumps are separate
  10 ms glides, §4.7.4 and §4.6.4).
- **Exception — gate.** A gate fading to "dry" would open for 40 ms. `GateFx` instead
  runs all detectors continuously and crossfades the two gain curves linearly over 20 ms
  (§4.1.8); while not switching, the active curve is used directly (no multiply by 1).
- **Bool switches inside a type** (`mod_color`, `comp_auto_makeup`) are not crossfaded
  by the switcher: `mod_color` goes through the switcher key (`voicing * 2 + color`);
  `comp_auto_makeup` fades the amount it adds through a 20 ms one-pole (target M or 0).

### 2.5 Where smoothing lives

Inside the engines, never in the processor. Rules for every new type:

- Every continuous parameter a new type reads goes through a smoother. Default: one-pole
  `fx::Smoother`, 20 ms time constant `[choice: matches the 20 ms ramps used across the
  chain (kGainSmoothingSeconds) and the study's references; study README "Parameter
  changes are ramped over about 20 ms"]`. Sections override this where they say so
  (comp threshold/ratio use 20 ms linear ramps; EQ uses two cascaded 5 ms one-poles).
- **Snap rule:** the first `setParameters()` after `prepare()` or `reset()` snaps every
  smoother to its target (Delay's `snapOnNextUpdate`). Exception: the compressor's
  makeup smoother, which only `prepare()` re-primes (Classic-exact, §4.2.6).
- Coefficient recomputation from smoothed values runs at a control rate of 16 or 32
  samples where a section says so, with per-sample linear interpolation of the
  coefficients where it says so.
- Choice parameters are never smoothed; they switch through §2.4.
- Classic paths keep today's (lack of) smoothing — **except Classic EQ and the amp EQ**,
  which gain knob smoothing because D7 asks for it (§4.4.3). Static Classic EQ renders
  stay bit-identical; automated ones change (that is the click fix).

### 2.6 Denormals, sanitising, reset

- Classic paths run under the processor's `juce::ScopedNoDenormals`
  (`src/PluginProcessor.cpp:948`), as today. fxbench wraps all processing in
  `juce::ScopedNoDenormals` too, so `--classic-null` compares like with like.
- **New types must be denormal-safe without FTZ**: every recursive state either adds
  `fx::kAntiDenormal` (1e-20f) or flushes `|s| < 1e-15` to 0 once per block. Check:
  fxbench `--no-ftz --bench` on a signal that decays into 30 s of silence costs at most
  1.5x the FTZ figure.
- **Reset determinism:** `reset()` sets every filter state, envelope, LFO phase,
  smoother ("snap on next update"), switcher ("snap on next block") and window/ring
  buffer to fixed values. No random seeds from time or addresses. Check:
  `--reset-repeat` renders input, resets, renders again; the two outputs are identical.
- LFO start phases after reset are specified per engine.
- **Reset on switch-on.** The processor calls `reset()` on a non-Classic engine when its
  block is switched back on (and when it re-enters the chain, through `pendingReset`), so
  a block never replays what it held when it left (processor: in progress). Each engine's
  `reset()` leaves nothing to replay: lines and filters cleared, gains started at unity or
  closed as the section says, and the wet path faded in where a section says so (chorus
  §4.5.4, tremolo §4.7.4, phaser §4.6.4). The fxbench keys that drive this are in
  `tools/fxbench/README.md` (EQ `reenable`, chorus `reset`, phaser `reset_on_enable`; the
  tremolo adapter does it itself).
- **Known step at switch-on.** The EQ's level, shelves and cuts still step when the block
  is switched on under signal; closing it needs a crossfade in the processor (§8).

### 2.7 Parameters

#### 2.7.1 Version hint, append order, naming

- New constant `kVersionHint4 = 4` in `src/Parameters.cpp` after `kVersionHint3`
  (`:38`), with the same comment pattern: hint 3 shipped with reverb/delay; a fresh hint
  sorts the whole new group after every shipped id in the AU parameter list.
- Ids follow `<block>_<key>`, instances `<kind><n>_<key>` (`comp2_knee`), literal
  constants in `Parameters.h`, instance-indexed `...Ids[maxInstances]` arrays, never
  string concatenation. Host names: `"<Kind name>[ n] <Key name>"` (`"Comp 2 Knee"`).
- **Append order** at the end of `createParameterLayout()` (after `src/Parameters.cpp:333`,
  before `return layout;`):
  1. Gate (singleton): `gate_mode`, `gate_release`, `gate_hold`, `gate_range`, `gate_sc_hpf`.
  2. Comp, instance 1, 2, 3, each: `type`, `knee`, `mix`, `sc_hpf`, `auto_makeup`.
  3. Drive, instance 1, 2, 3: `type`.
  4. EQ, instance 1, 2, 3, each: `type`, `lowcut`, `highcut`, `mid_freq`, `mid_q`,
     `tilt`, `level`, `voice`, `g100`, `g200`, `g400`, `g800`, `g1k6`, `g3k2`, `g6k4`.
  5. Mod, instance 1, 2, 3, each: `chorus_mode`, `phaser_model`, `trem_voice`, `sync`,
     `width`, `tone`, `shape`, `wave`, `color`.

**Count: 95 new parameters** — gate 5, comp 15, drive 3, EQ 45, mod 27. By kind: 64
floats, 6 bools, 25 choices. Relays become 183 sliders / 36 toggles / 42 combos.

- Float ranges use only the two range forms the UI already understands: linear
  `{min, max, step}` or `logRange (min, max, step)` (skew centre at the geometric mean,
  `Parameters.cpp:42-47`; `paramMeta.ts` `log: true`). No other skews.

#### 2.7.2 Choice lists are frozen

Every new choice list is written once as a `const char* const xxxNames[]` array in
`Parameters.h` (the reverb/delay pattern, `Parameters.h:319-327`) and a matching
`StringArray`. Append only. The engine enum is indexed by it and the engine header has a
`static_assert`-able `numTypes`; `Parameters.cpp` ties them with
`static_assert (std::size (xxxNames) == tubamp::XFx::numTypes)`.

#### 2.7.3 Defaults: new blocks start on the recommended type

A new choice parameter's **default is the recommended type** (gate Tight, comp Clean,
drive Screamer, EQ Parametric, chorus Wide, phaser Phase 90, tremolo Smooth). A block the
player adds — or any fresh instance — therefore starts on it. Old sessions and presets are
pinned to Classic by §2.7.5.

#### 2.7.4 Per-type starting points

As in reverb/delay: knobs whose sensible start depends on the type have a per-type table
in the engine header (`kXxxDefault[numTypes]`); a new parameter's default is the default
type's entry; the editor moves those knobs when the user picks a type in the menu
(`typeDefaults.ts`). Host automation and preset recall never go through the tables.

**Rule for existing (frozen) knobs:** their frozen defaults stay; the recommended type's
starting point is defined equal to the frozen defaults wherever this spec could make it
sensible (comp Clean, drive Screamer, chorus Wide), so a fresh block is already at its
starting point. The tables list Classic's entries as the frozen defaults.

#### 2.7.5 The legacy pin (new mechanism — none exists in this tree)

The base commit has **no** legacy pin: `pinLegacyEngineTypes` / `legacyEnginePins` were
removed with reverb/delay's Classic types (`docs/REVERB-DELAY.md` §4: absent ids simply
take their defaults). This spec adds one, owned by the integrator:

- **Rule.** When state is restored (host `setStateInformation`, presets and A/B through
  `applyStateVar`), for every new choice id in the table below whose **instance is
  present in the restored chain order** and whose id is **absent** from the restored
  state, set it to 0 (Classic) instead of its default. Instances not in the restored
  order take their defaults (a block the player adds later starts on the recommended
  type). A state that carries the id round-trips exactly.

  | Instance present in order | Ids pinned to 0 when absent |
  |---|---|
  | `gate` | `gate_mode` |
  | `comp`, `comp2`, `comp3` | `comp<n>_type` |
  | `drive`… | `drive<n>_type` |
  | `eq`… | `eq<n>_type` |
  | `mod`… | `mod<n>_chorus_mode`, `mod<n>_phaser_model`, `mod<n>_trem_voice` |

  No other new parameter needs pinning: Classic reads none of them.
- **Where.** `setStateInformation` (`src/PluginProcessor.cpp:2089`): parse the chain order
  (today `:2116-2125`) **before** `apvts.replaceState` (`:2101-2102`), then add a `PARAM`
  child `{id, value = 0}` to the param tree for each absent pinned id, then
  `replaceState`. The audio thread never sees the recommended type for a pinned block.
  `applyStateVar` (`:2215`): compute `chainState` (`:2253-2258`) before the parameter loop;
  the "absent means default" loop (`:2231-2236`) sets an absent pinned id straight to 0
  instead of its default, so here too the recommended type is never published.
  Implemented as one table, `kLegacyTypePins` (`src/PluginProcessor.h`: block kind →
  instance-indexed id array), which `writeFactoryPreset` (§8 D-2) also walks.
- `kStateVersion` becomes 3 (`src/PluginProcessor.cpp:36`) with a comment; the pin keys on
  absence of the id, never on the version number.

#### 2.7.6 The mod block parameter scheme (shared by chorus, phaser, tremolo)

One voicing choice per mod type (a choice list is frozen per id), plus five shared
controls reused where their meaning carries over, plus one tremolo-only control:

| id (inst. 1) | meaning | read by |
|---|---|---|
| `mod_chorus_mode` | chorus voicing: Classic, Wide, CE, Dimension, Tri | chorus |
| `mod_phaser_model` | phaser voicing: Classic, Phase 90, Small Stone | phaser |
| `mod_trem_voice` | tremolo voicing: Classic, Smooth, Optical, Bias, Harmonic | tremolo |
| `mod_sync` | tempo sync note value, Off = free `mod_rate` | every non-Classic voicing |
| `mod_width` | stereo: chorus wet width; tremolo spread = 180° × width | chorus (non-Classic), tremolo (non-Classic) |
| `mod_tone` | chorus wet low-pass; tremolo Harmonic crossover | chorus (non-Classic), tremolo Harmonic |
| `mod_shape` | tremolo Smooth skew / pulse duty; Bias knee | tremolo Smooth, Bias |
| `mod_wave` | tremolo Smooth waveform sine → triangle → pulse | tremolo Smooth |
| `mod_color` | Phase 90 script (off) / block (on); Small Stone Color | phaser (non-Classic) |

`mod_rate`, `mod_depth`, `mod_mix` keep their ids and meanings; each voicing's section
says whether it reads them. Rate multiplier: **not added** (§8, D-13) — sync covers fast
rates. Tempo sync is resolved by the integrator's `Modulation` dispatcher (§6.5): the
engines receive a rate in Hz and an optional song-position phase.

### 2.8 Meters (gate state + GR, compressor GR)

Same route as the level meters (`inputPeak`/`outputPeak`, `src/PluginProcessor.h:201`,
emitted by `WebEditor::timerCallback`, `src/WebEditor.cpp:1018-1024`, 30 Hz):

- **Engine side:** getters (`getGainReductionDb`, gate `getState`, `getDetectorLevelDb`)
  report the last block.
- **Processor side (integrator):** after each instance runs,
  `compGrDb[k]` and `gateGrDb`, `gateLevelDb` are max-accumulated with the
  `compare_exchange_weak` loop used for `inputPeak` (`publishMax` in
  `PluginProcessor.cpp`); `gateState` is a plain `store` of the last block's state. When a
  block is absent, bypassed or reset-pending, nothing is accumulated and `gateState`
  stores −1.
  ```cpp
  static constexpr float kDynMeterFloorDb = -120.0f;
  std::atomic<float> gateGrDb { 0.0f }, gateLevelDb { kDynMeterFloorDb };
  std::atomic<int>   gateState { -1 };               // -1 inactive, 0 closed, 1 hold, 2 open
  std::array<std::atomic<float>, params::maxInstances> compGrDb {};  // all 0
  ```
- **Editor:** one new event per timer tick, emitted right after `meters`. The gain
  reductions and the level are reset on read (`exchange` to 0 / −120 / 0), so a block that
  did not run since the last tick reads idle; `gateState` is a plain `load`, not reset
  (it is a lamp, not a peak). Opening the editor discards the accumulated backlog, as for
  the peaks:
  ```
  "dynMeters" { gate: { state: -1|0|1|2, gr: number /* dB >= 0 */, level: number /* dBFS */ },
                comp: [number, number, number] /* dB >= 0, index = instance */ }
  ```
  `level` is the gate's detector level in the threshold's own units (peak dBFS of the
  filtered sidechain for Tight/Smooth, peak of the mono sum for Classic), floor −120.
- **UI ballistics** (§7): instant rise, fall 20 dB/s `[choice]`; comp scale 0–20 dB,
  gate GR scale 0–60 dB.

### 2.9 Latency

Zero for every engine except `DriveFx`, whose oversampling types report
`std::lround (oversamplerLatency + adaaDelay)` (§4.3.7) and whose Classic reports
`std::lround (3.137…) = 3` without changing its audio (D2 b). The integrator adds drive
latency to `computeChainLatency` like the amp's (§6.3), and the gate's deferred curve is
delayed by the latency between gate and amp output for Tight/Smooth (§4.1.7).

### 2.10 CPU budget

Measured with `tubamp_fxbench <effect> --bench` (§5.3.4): stereo, 48 kHz, 64-sample
blocks, 10 s of pink noise at −18 dBFS, release build, on the development Mac (Apple
silicon). Figure = engine time / real time, per instance. Each section states a budget
per type; at 96 and 192 kHz the budget scales linearly with the rate unless the section
says otherwise.

### 2.11 Acceptance common to every engine

Every check script (`tools/fxbench/check_<effect>.py`) runs these first, for every type,
at 22 050, 44 100, 48 000, 96 000 and 192 000 Hz, mono and stereo, block sizes 1, 37,
512:

| Check | Pass |
|---|---|
| Finite | 20 s of extreme input (full-scale white noise, ±1 square at 20 Hz, a 1e-30 tail, then 0) with every float key at its min and max: no NaN/Inf; abs(out) < 1e3. |
| Block-size independence (new types) | output at block 1 and 37 equals block 512 within 1e-6 abs (or bit-exact where the engine is block-agnostic). |
| Reset determinism | `--reset-repeat`: identical. |
| RT safety | `--rt-check`: zero allocations inside audio calls. |
| Silence | 2 s of zeros after reset gives |out| < 1e-9 (tremolo/chorus/phaser: exactly 0 or < 1e-9). |
| Classic null | `--classic-null` on the standard music-like test input (§5.3.5) at 44.1/48/96 kHz, block 512 and 37: **0 differing samples** (EQ: except at all-5.0 settings, where the new output must equal the input exactly). |
| Denormals | `--no-ftz --bench` ≤ 1.5 × FTZ cost. |
| CPU | `--bench` within the section's budget. |

---

## 3. File ownership

No file has two parallel writers. "Scaffold" works first and alone; engine engineers
work in parallel after it; "integrator" and "UI" work after the engines land (the UI may
start on the mock bridge in parallel, touching only `ui/`).

| File | Owner | Notes |
|---|---|---|
| `docs/FX-ENGINES.md` | architect | engineers report deviations to the integrator, who edits |
| `src/dsp/Gate.h`, `src/dsp/Gate.cpp` | gate | stubs from scaffold |
| `src/dsp/Compressor.h`, `src/dsp/Compressor.cpp` | comp | |
| `src/dsp/Drive.h`, `src/dsp/Drive.cpp` | drive | |
| `src/dsp/Eq.h`, `src/dsp/Eq.cpp` | eq | |
| `src/dsp/Chorus.h`, `src/dsp/Chorus.cpp` | chorus | |
| `src/dsp/Phaser.h`, `src/dsp/Phaser.cpp` | phaser | |
| `src/dsp/Tremolo.h`, `src/dsp/Tremolo.cpp` | tremolo | |
| `tools/fxbench/fx_gate.cpp`, `check_gate.py` | gate | registration + checks |
| `tools/fxbench/fx_comp.cpp`, `check_comp.py` | comp | |
| `tools/fxbench/fx_drive.cpp`, `check_drive.py` | drive | |
| `tools/fxbench/fx_eq.cpp`, `check_eq.py` | eq | |
| `tools/fxbench/fx_chorus.cpp`, `check_chorus.py` | chorus | |
| `tools/fxbench/fx_phaser.cpp`, `check_phaser.py` | phaser | |
| `tools/fxbench/fx_tremolo.cpp`, `check_tremolo.py` | tremolo | |
| `tools/fxbench/main.cpp`, `FxBench.h`, `fxcheck.py`, `README.md` | scaffold | engines never edit; requests go to scaffold/integrator |
| `tools/fxbench/legacy/LegacyFx.h`, `LegacyFx.cpp` | scaffold | frozen copy of base-commit code; never edited after creation |
| `tools/golden/GoldenRender.cpp`, `tests/golden/fx-golden.tsv`, `tests/golden/expected-changes.tsv` | scaffold (capture), integrator (expected-changes updates only with owner sign-off) | |
| `CMakeLists.txt` | scaffold (fxbench, golden targets), integrator (anything later) | engine sources are picked up by the existing `GLOB_RECURSE src/*.cpp` (`CMakeLists.txt:145-147`); no engine needs a CMake edit |
| `src/Parameters.h`, `src/Parameters.cpp` | integrator | |
| `src/PluginProcessor.h`, `src/PluginProcessor.cpp` | integrator | |
| `src/dsp/FxBlocks.h`, `src/dsp/FxBlocks.cpp` | integrator | except `CabSim` (fence F2) |
| `src/library/PresetManager.cpp` | integrator | factory preset pin (§8 D-2) |
| `src/WebEditor.cpp`, `src/WebEditor.h` | integrator | relays, `dynMeters` event |
| `tests/ProcessorSmokeTest.cpp` | integrator | |
| `docs/REACT-UI.md` | integrator | relay counts, `dynMeters` contract |
| `ui/src/**` | UI | `bridge/paramMeta.ts`, `bridge/types.ts`, `bridge/mock.ts`, `features/panel/*` |
| `src/dsp/FxPrimitives.h`, `Delay.*`, `Reverb.*`, `docs/REVERB-DELAY.md` | nobody | fence F1 |

---

## 4. The seven engines

Each section is self-contained: read §1–§3 and your own section.

### 4.1 Gate — `GateFx` (`src/dsp/Gate.h/.cpp`)

Study: `docs/research/fx/gate/report.md` (R1–R5), corrected by its Verification status and
`verification.md` (V:31, V:36-40, V:42).

#### 4.1.1 Modes (frozen order)

| index | name | what |
|---|---|---|
| 0 | Classic | today's AudioDSPTools gate, bit-for-bit |
| 1 | Tight | hard gate: windowed-peak detector, 6 dB hysteresis, hold after a note stops (a dip or fade is ridden for up to 0.4 s), 0.2 ms linear opening, dB-linear release to `range` |
| 2 | Smooth | Tight's detector and state machine; when closed, a 3:1 downward expander instead of a jump to `range` |

Default for new blocks: **Tight**. Legacy pin: `gate_mode` → 0 (§2.7.5). The gate is a singleton.

#### 4.1.2 Parameters

Existing, unchanged: `gate_on` (bool), `gate_threshold` (−100…0 dB, step 0.1, default −80,
hint 1). New (hint 4):

| id | host name | range | step | skew | default | unit | read by |
|---|---|---|---|---|---|---|---|
| `gate_mode` | Gate Mode | Classic, Tight, Smooth | — | — | 1 (Tight) | — | all |
| `gate_release` | Gate Release | 5 – 500 | 0.1 | log | 30 (`kGateReleaseDefault[Tight]`) | ms (time for 60 dB) | Tight, Smooth |
| `gate_hold` | Gate Hold | 0 – 250 | 0.1 | linear | 6 (`kGateHoldDefault[Tight]`) | ms (the wait after a note stops, §4.1.5) | Tight, Smooth |
| `gate_range` | Gate Range | −90 – 0 | 0.1 | linear | −90 | dB (≤ −89.95 = mute) | Tight, Smooth |
| `gate_sc_hpf` | Gate SC HPF | 20 – 300 | 1 | log | 75 (`kGateScHpfDefault[Tight]`) | Hz, 4th-order Butterworth, 24 dB per octave (≤ 20.5 = off) | Tight, Smooth |

`gate_threshold` means, in Tight/Smooth, the **peak level of the filtered sidechain in
dBFS**; in Classic it keeps AudioDSPTools' mean-square reading (≈ 3 dB lower for a sine).
`[study: gate/report.md R1 "Threshold semantics"]`

Per-mode starting points (`Gate.h`): `kGateReleaseDefault[3] { 30, 30, 80 }` ms
`[study: R1 table, 30 ms Tight / 80 ms Smooth]`; `kGateHoldDefault[3] { 6, 6, 10 }` ms;
`kGateScHpfDefault[3] { 75, 75, 75 }` Hz (Classic's entries are unused). Hold was 10 ms
for every mode in the first draft; the engine measured 6 ms as the shortest hold that
rides a 41 Hz decay without chattering while muting 50 ms palm-mute gaps to −38 dB, and
the coordinator accepted it for Tight. With the adaptive wait of §4.1.5, hold means the
wait after a note stops, and 40 / 50 ms palm-mute gaps reach −31 / −51 dB (`Gate.h`).

Fixed (not parameters): opening is a linear rise from mute to full gain in 0.2 ms
(`kOpenSeconds`; it replaced the study's 0.5 ms for 60 dB dB-ramp, `[study: R1]`, with no
more splatter, check X16); hysteresis H = 6 dB `[study: R1, inside the 4.8–9.5 dB the
references use]`; detector window 12 ms `[study: R1 Nwin, half period of 42 Hz]`;
expander slope S = 3 `[study: R1]`.
Sidechain HPF default 75 Hz, 4th-order Butterworth `[choice, check_gate X20, G15: a low E
opens 1.7 dB over threshold (the 2nd-order 90 Hz filter: 3.9 dB) and 50 Hz hum is cut
14.3 dB; the study's figures were measured with 90 Hz, ver: gate/verification.md V:31]`.

```cpp
struct GateParams
{
    int mode = 1;               // GateFx::Mode
    float thresholdDb = -80.0f; // [-100, 0]
    float releaseMs = 30.0f;    // [5, 500] time for 60 dB of gain change
    float holdMs = 6.0f;        // [0, 250] (kGateHoldDefault[Tight])
    float rangeDb = -90.0f;     // [-90, 0]; <= -89.95 means mute (gain exactly 0)
    float scHpfHz = 75.0f;      // [20, 300], 4th-order Butterworth; <= 20.5 means off (kGateScHpfDefault[Tight])
};
```

#### 4.1.3 API (deviates from §2.1: measure/apply split)

```cpp
class GateFx
{
public:
    enum class Mode { classic = 0, tight = 1, smooth = 2 };
    static constexpr int numModes = 3;
    /** The processor's lane rings hold 65 536 samples, so no span it can align is longer. */
    static constexpr int kMaxCurveDelaySamples = 65535;

    void prepare (const juce::dsp::ProcessSpec& spec);  // sizes curve ring = maxBlock + kMaxCurveDelaySamples
    void reset();
    void setParameters (const GateParams& params);
    /** Tight/Smooth only: delay applied to the curve when it is applied elsewhere (deferred
        onto the amp output). Clamped to [0, kMaxCurveDelaySamples]. Classic ignores it. */
    void setCurveDelay (int samples) noexcept;
    /** Analyses `block` (mean of its channels) and computes this block's gain curve.
        Does not touch the audio. Blocks larger than maxBlockSize: no curve (pass ungated); the
        detector and the meters still follow them (Classic reads open, 0 dB). Non-finite input
        samples are read as silence in every mode. */
    void measure (const juce::dsp::AudioBlock<float>& block, int numSamples);
    void apply (juce::dsp::AudioBlock<float> block, int numSamples); // every channel
    void apply (float* mono, int numSamples);                        // one buffer
    int getLatencySamples() const noexcept { return 0; }
    int getState() const noexcept;               // 0 closed, 1 hold, 2 open (end of last measure)
    float getGainReductionDb() const noexcept;   // max over the last measured block, >= 0
    float getDetectorLevelDb() const noexcept;   // max detector level over the last block, dBFS
};
```
`apply` is a no-op if no curve exists for exactly `numSamples` (today's rule,
`FxBlocks.cpp:325, 339`).

#### 4.1.4 Classic (mode 0) — exact definition

`NoiseGate` moved verbatim (`src/dsp/FxBlocks.cpp:25-30, 222-344`): mono mean of all
channels → `::dsp::noise_gate::Trigger` with `TriggerParams (0.01, threshold, 0.1, 0.005,
0.01, 0.05)` → `::dsp::noise_gate::Gain` read back on a buffer of ones → linear curve
multiplied onto every channel. Includes `primeBuffers()` at prepare, the oversized-block
pass-through and `reset()` invalidating the curve only. Not delayed by `setCurveDelay`
(strict D2; §8 D-3). Meters for Classic: GR = −20·log10(min curve value in block),
clamped to [0, 120]; state 2 if the last curve value ≥ 0.891 (−1 dB), 0 if ≤ 0.01 (−40 dB),
else 1; level = 20·log10(max |mono| in block), floor −120.

#### 4.1.5 Tight and Smooth — algorithm

Per sample, on the mono mean `s` of the block's channels (`fs` = sample rate). A
non-finite `s` is read as 0 (a sample that is finite but huge is clamped to ±1e6, so it
still opens the gate):

```
sc   = HP4(s)  if scHpfHz > 20.5 else s     // two TPT SVF high-pass sections in series, a 4th-order
                                            // Butterworth, k = 2cos(pi/8) = 1.84776 and 2cos(3pi/8) = 0.76537,
                                            // g = tan(pi*min(fc, 0.49 fs)/fs), a1 = 1/(1+g(g+k)), a2 = g*a1, a3 = g*a2,
                                            // v3 = x-ic2; v1 = a1*ic1+a2*v3; v2 = ic2+a2*ic1+a3*v3;
                                            // ic1 = 2v1-ic1; ic2 = 2v2-ic2; y = x - k*v1 - v2
                                            // [study: gate/evidence/bb/proto.py:16-22], state in double
a    = |sc|
pk   = max of a over the last Nwin samples  // exact sliding-window maximum (van Herk / Gil-Werman),
                                            // Nwin = round(0.012 * fs)
raw  = same windowed max of |s|             // two more 12 ms windows, for the stop detector:
stp  = same windowed max of |s - LP1(s)|    // LP1 = one-pole low-pass at 2 * scHpfHz (when the filter is on)
open  = 10^(T/20);  close = open * 10^(-6/20)
ref  = max(pk, ref * 10^(-20 dB/s / fs), 1e-9)          // the recent peak: forgets 20 dB per second
stopped(w) = (w < close and w <= ref * 10^(-26/20))      // a fall of 26 dB or more from the recent peak
if stopped(raw) or stopped(stp):                          // the note has stopped
    if state == OPEN and ++c > Nhold:  state = CLOSED     // c counts samples; Nhold = round(hold * fs)
else:
    c = 0
    if pk >= open:          state = OPEN;  wait = 0
    elif state == OPEN:
        if pk >= close:     wait = 0                      // inside the hysteresis band
        else:               wait += 1 / (W + 1);  if wait >= 1: state = CLOSED
W = Nhold                      if pk <= ref * 10^(-26/20)   // deep fall
  = kRideSeconds * fs          if pk >= ref * 10^(-16/20)   // shallow fall, kRideSeconds = 0.4
  = log-linear between the two, in the fall in dB          // a dip or fade is ridden
target_dB = 0                                                 if OPEN
          = R                                                 if CLOSED and Tight
          = clamp(2 * (F - 20*log10(close)), R, 0)            if CLOSED and Smooth   (S-1 = 2)
          // F = floor estimate, dB of min(pk, raw) while closed (below)
gr_dB: closing    gr_dB -= min(gr_dB - target_dB, 60/(release*fs))              // dB-linear release
       opening    gain rises linearly by 1/(0.0002*fs) per sample to the target  // 0.2 ms, mute to full gain
gain  = (R <= -89.95 and gr_dB <= -89.95) ? 0 : 10^(gr_dB/20)
```
`R` = `rangeDb`. Smooth's floor estimate F is the dB peak level averaged with a time
constant of half the time since the gate closed (or since the level jumped by more than
6 dB, or since the gate was last open), up to 0.2 s: it lands on a new floor at once and
then holds still on hiss and hum (largest 10 ms gain move 0.33 dB, check X14). The
slew limiter **is** the smoothing for every gate parameter (threshold, range and hold
changes cannot click because `gr_dB` can only move at the release rate and the 0.2 ms
opening rate) `[study: R1 dB-linear ramp]`.

**Why the closing rule depends on the fall.** A wait that was a fixed hold made Tight
chatter on decaying chords and beating notes (49 of 328 verifier cases). Now a note that
stops (palm mute, a choked chord) falls 26 dB or more from its recent peak and closes
after the hold; a dip or fade of 16 dB or less is ridden for up to 0.4 s (a 1 Hz beat
with a 15 dB swing); between 16 and 26 dB the wait is interpolated. The stop detector
reads the unfiltered signal and a signal through a one-pole that keeps hum out, so the
sidechain filter's ringing (up to 10 ms after a loud low note) does not delay a stopped
note; neither reads more than 4.9 dB under the main filter, inside the 6 dB hysteresis,
so it never closes on a level that could reopen the gate (`Gate.cpp`, check X10 to X13).

**Detector change against the study (load-bearing):** the prototype held the peak for
12 ms and then let it fall with a 3 ms exponential; that fall costs 0.35 ms per dB of
level above threshold and up to 12 ms of jitter, which is why it closed in 30–39 ms at
−40 dB and 38–47 ms at −60 dB, missing its own 35 ms target `[ver: gate/verification.md
V:31, V:37]`. This spec uses an exact sliding-window maximum instead: after the last
crest leaves the window the detector reads the true current level at once, so the close
time no longer grows with level (the stop detector above keeps the sidechain filter's
ringing from adding to it) `[choice, sim: §4.1.10 numbers]`. Nwin = 12 ms still covers the
half period of 42 Hz so the held value cannot droop between half-cycles of a low note.

`state` for the meter: 2 if OPEN and pk ≥ open; 1 if OPEN and pk < open (hysteresis band
or holding); 0 if CLOSED. GR meter = max over the block of −gr_dB, clamped to [0, 90]
(90 when muted); level meter = max over the block of 20·log10(pk), floor −120.

**Start state after `reset()`:** CLOSED, `gr_dB = R`, the wait and stop counters 0, all
three windows empty, filter state 0, the floor estimate restarted, curve ring filled with
the corresponding floor gain (0 if mute). A gate that re-enters the chain starts closed
and opens on the first note within 1 ms (G8; the rise itself takes 0.2 ms).

#### 4.1.6 Lookahead

None `[study: R1 "Not recommended"]`. Latency 0.

#### 4.1.7 Deferred curve alignment (Tight/Smooth)

When the processor defers the gain onto the amp's mono output (`deferGateToAmp`,
`src/PluginProcessor.cpp:1064-1065`), the audio reaching `apply` is late by the latency of
everything between the gate's position and the amp output. The integrator calls
`setCurveDelay (D)` each block, with D = (NAM engine latency if the amp is on) + the
reported latency of every block strictly between the gate and the amp in the same
segment (drive instances, FX slots) (§6.1). `apply` then uses the gain computed D samples
earlier: `measure` writes this block's gains into the curve ring; `apply` reads
`ring[write - numSamples - D + i]`. With D = 0 (no resampling, nothing in between) the
output equals the undelayed curve exactly. Non-deferred `apply` always uses D = 0.
`[study: R4; ver: V:32, V:61 — size never measured, inferred from code]`

**As integrated, measured** (smoke "gate curve alignment", A2.nam, a 48 kHz model): the
deferred curve is delayed by exactly the reported span latency (27 samples at 44.1 kHz,
42 at 96 kHz). That figure is AudioDSPTools' `ResamplingContainer::GetLatency()`, a
buffer-fill count, and it is short of the resampler pair's true delay (phase delay of a
200 Hz tone through the amp, less the same at 48 kHz): 28.9 samples at 44.1 kHz, 45.0 at
96 kHz. The curve therefore opens 1.9 / 3.0 samples (0.04 / 0.03 ms) before the audio it
gates, against a 0.5 ms attack. The latency reported to the host has the same shortfall
and is frozen by the golden renders; closing the gap would need a measured group delay
from `NamEngine` used for D only (§8 D-22).

#### 4.1.8 Mode switches

All detectors run every block (Classic trigger + windowed detector, both cheap). On a mode
change the applied curve crossfades linearly from the old mode's curve to the new one's
over 20 ms; the old mode's state machine keeps running until the fade ends. Outside a
fade the active curve is used directly. After `reset()` the requested mode is snapped.

#### 4.1.9 Meters, latency, CPU

Meters as §4.1.3/§4.1.4. Latency 0. CPU budget: Tight/Smooth ≤ 0.10 % per instance,
Classic as today (one `log10` + one `pow` per sample, ≤ 0.25 %); all detectors together
≤ 0.35 %.

#### 4.1.10 Acceptance (`tools/fxbench/check_gate.py`)

Signals (from `gate/evidence/bb/gen.py`, rebuilt in `fxcheck.py`): tone bursts of 0.3 s
ending with a 1 ms raised-cosine fade, over digital silence unless stated. "Close time" is
measured on the **per-sample gain trace** (`--trace=gain`), from the end of the fade to the
first sample with gain ≤ −20 dB — not on windowed RMS, whose 24 ms window at 82 Hz adds up
to 12 ms (V:37). Each cell is the max over 8 start phases × 4 burst lengths
(0.3 s + {0, 0.37, 1.13, 2.9} ms). Defaults unless stated (Tight, release 30, hold 6
(Smooth: release 80, hold 10), range −90, HPF 75, 4th order). The G bounds were set from
a numpy model of the first §4.1.5 at 48 kHz `[sim]`; the model's values are in brackets
(they predate the adaptive wait and the 4th-order filter; the bars are what the script
enforces). The rows X1 to X20 below the table are the verifier-driven proofs.

| # | Check | Pass |
|---|---|---|
| G1 | Classic null (§2.11) incl. deferred apply on a mono buffer | 0 differing samples |
| G2 | Close time, 1 kHz, note −20 dBFS, thr −30/−40/−50/−60 | ≤ 35 ms each [31.8 / 31.9 / 33.3 / 34.6] |
| G3 | Close time, 1 kHz, note −6 dBFS, thr −30/−40/−50 | ≤ 36 ms [32.2 / 33.9 / 35.0] |
| G4 | Close time, 1 kHz, note −6 dBFS, thr −60/−70; note −20 dBFS, thr −70 | ≤ 44 ms [40.6 / 42.1 / 39.7] (sidechain filter ringing; with HPF off ≤ 35 ms) |
| G5 | Close time, 82.41 Hz, note −20 dBFS, thr −40/−60 | ≤ 50 ms [39.5 / 45.4] (study limit 50 ms) |
| G6 | Close time, 82.41 Hz, any of thr −30…−70 × note −6/−20 | ≤ 53 ms [max 50.2] |
| G7 | Close time never shorter than | ≥ 18 ms in every cell (no chopping of decays) |
| G8 | Open: 1 kHz −20 dBFS onset after silence, thr −40 | gain ≥ −1 dB within 1.0 ms [0.75] |
| G9 | Chug (`chug.wav`: 82.41 Hz + harmonics, 75 ms notes every 150 ms, white noise −66 dBFS RMS), thr −40, Tight | median gap gain (last 10 ms before each onset) ≤ −30 dB [−88]; first 10 ms of each note ≥ −1 dB [−0.63] |
| G10 | Same, Smooth (release 80) | median gap gain between −14 and −25 dB [−18.8]; first 10 ms ≥ −0.5 dB |
| G11 | Hover: 1 kHz at −40 dBFS ± 3 dB at 3 Hz, thr −40, 6 s | 0 crossings of −6 dB after 0.5 s [0] |
| G12 | Step-down from −20 dBFS to L, thr −40 | open (0 dB) 0.8 s later for L = −45, −45.5; closed (≤ −89 dB) for L = −46.5, −47 [same] |
| G13 | Smooth static: steady 1 kHz at −50/−55/−60 dBFS, thr −40 | −8.0 / −18.0 / −28.0 dB ± 0.5 [exact] |
| G14 | Range: Tight, range −20, closed | gain −20.0 ± 0.1 dB; range −90 → exactly 0 |
| G15 | SC HPF: 50 Hz hum −44 dBFS + chug, thr −40 (as written, thr −50 cannot be met by any gate: the chug's own noise peaks at −55 dBFS, over the −56 dBFS close level; that run is reported, not enforced) | HPF 75: median gap ≤ −30 dB; HPF off: gate stays open, median gap ≥ −1 dB (documents the benefit, V:42) |
| G16 | Deferred alignment (processor test, §6.1): 1 kHz burst through gate → amp on a model forced to resample (session 44.1 kHz) | gain edge and audio edge coincide within 1 sample. **As integrated:** the curve is delayed by exactly the reported latency (checked to the sample); against the audio it lands within 4 samples (measured 1.9 at 44.1 kHz, 3.0 at 96 kHz), the reported latency's own error (§4.1.7) |
| G17 | Rates | G2, G5, G9 also at 44.1 and 96 kHz, same bounds + 1 ms (36 ms and 51 ms) |
| G18 | Meter: thr −40, silence after a note | state 0 within 100 ms of the note end; 2 on the first sample above threshold; GR reads 90 when closed with range −90 |
| X1 | Smooth close: release 80, all 20 cells | after a note Smooth closes at most 1 ms later than Tight plus (hold difference + 20 ms × (80 − 30) / 60), i.e. the expander law never holds the gain up |
| X2 | Palm-mute gaps, Tight chug thr −40, 75 ms notes, gaps 50 / 75 / 100 ms | median gap gain ≤ −30 dB each |
| X3 | Decay not chopped: −6 dBFS exponential decays, 82.41 Hz and 1 kHz, tau 0.4 / 1.5 s, Tight and Smooth, thr −40 | gain ≥ −0.1 dB while the 4th-order-filtered envelope is at least 0.5 dB over the close level |
| X4 | No chatter: white noise −62…−46 dBFS RMS, thr −40 | at most 2 open/close changes in 5.5 s |
| X5 | Pick attack: first 5 ms of each chug note and of an abrupt 1 kHz onset (8 phases) | level lost ≤ 1 dB |
| X6 | Mode switches: all 6 transitions, 3 steady inputs, blocks 64 / 512 | added gain step per sample ≤ 1.5 × a full-scale 20 ms linear fade |
| X7 | Rates 22.05 and 192 kHz | G2 and G5 cells, bounds 36 ms and 51 ms |
| X9 | Low-note decay: 41.2 / 46.25 / 82.41 Hz with harmonics, tau 0.7 / 1.5 / 3 s, thr −40 / −55 | at most 2 open/close changes (one open, one close) |
| X10 | Chord decay: 14 verifier chords × thr −30…−65 step 5; the 4 hardest chords also at 22.05 / 44.1 / 96 / 192 kHz (`--quick`: 44.1 and 96 kHz) | at most one open and one close in every cell |
| X11 | Beating: AM notes (82.41 and 110 Hz, depth 0.2 / 0.33 / 0.5 / 0.7 = 3.5 to 15 dB, rate 1 / 3 / 7 / 15 / 30 Hz, thr −40); two strings a few hertz apart, thr −30…−60 | at most one open and one close |
| X12 | Single decays: 41.2…330 Hz × tau 0.15…2.5 s × thr −20…−70 | at most one open and one close |
| X13 | Palm-mute gaps by gain energy over the last 10 ms of each gap, Tight thr −40 | 50 ms gaps: median ≤ −36 dB, worst ≤ −30 dB; 40 ms gaps: median ≤ −20 dB. Also reported, not enforced: the 30 / 40 / 50 / 75 / 100 ms table and the gaps over 50 Hz hum |
| X14 | Smooth floor: noise, hum and buzz under thr −40, 44.1 / 48 / 96 kHz | largest gain move in 10 ms ≤ 0.5 dB, largest one-sample step ≤ 0.1 dB |
| X15 | Pick attack: picked notes 12 / 20 / 34 dB over thr −40, 82.41 / 196 / 1000 Hz | energy lost in the first millisecond ≤ 3 dB |
| X16 | Opening click: Tight, splatter at 82.41 / 200 / 1000 Hz | no more than the old 0.5 ms / 60 dB dB-ramp's |
| X17 | Non-finite input: NaN / ±Inf bursts in every mode and across switches, blocks 1 / 37 / 512, mono and stereo | gain equals the zero-substituted render bit for bit; meters finite |
| X18 | Oversized blocks: block 512 on a 256-sample prepare | audio unchanged; meters those of the same input in normal blocks (Classic: open, 0 dB) |
| X19 | Long curve delay: deferred, D = 8193 / 30000 / 65535 | bit-exact against the undelayed render, D samples later |
| X20 | Low-note threshold (reported, not enforced): steady sine needed to open at thr −40 | 41.2 / 73.42 / 82.41 / 110 Hz; a pure 41.2 Hz tone needs 15.4 dB over threshold (was 13.6), a low E 1.7 dB (was 3.9) |

check_gate.py ran 79 checks, 0 FAIL, at the commit that added X10 to X20 (`81fd404`). G16 in
the script is a proxy (`lag = delay`: the output must be the undelayed render, D samples
later, bit for bit, at D = 1 / 37 / 512 / 3000, blocks 64 / 512); the processor test is the
integrator's.

fxbench keys: `mode threshold release hold range sc_hpf delay deferred on lag prepare_block`
(`delay` = curve delay, applied with `apply` after `measure` as the processor does; `lag`
delays the deferred audio as the amp would; `prepare_block=N` re-prepares for N-sample
blocks, for X18).

### 4.2 Compressor — `CompFx` (`src/dsp/Compressor.h/.cpp`)

Study: `docs/research/fx/comp/report.md` R1–R6, corrected by its Verification status and
`verification.md` (V:48-68 detector hold, V:72-73 truncated tails, V:81 R4/R5).
The full header is the pattern header in §2.1.

#### 4.2.1 Types (frozen order)

| index | name | what |
|---|---|---|
| 0 | Classic | `juce::dsp::Compressor<float>` + smoothed makeup, exactly as today |
| 1 | Clean | log-domain feed-forward, windowed-peak detector, soft knee, gain smoothed after the gain computer, linked stereo |
| 2 | Squash | log-domain **feedback** (the gain computer sees the output level), pedal-style |
| 3 | FET | Clean detector + two-stage release + ratio-dependent threshold |
| 4 | Opto | Clean detector + slow two-stage release, wide knee |

Default for new blocks: **Clean**. Legacy pin: `comp<n>_type` → 0.

#### 4.2.2 Parameters

Existing, unchanged (all instances; hint 1 for instance 1, hint 2 for 2–3):
`comp_on`; `comp_threshold` −60…0 dB step 0.1 default −20; `comp_ratio` log 1…20 step
0.01 default 4 (":1"); `comp_attack` log 0.1…100 ms step 0.01 default 5; `comp_release`
log 10…1000 ms step 0.1 default 120; `comp_makeup` 0…24 dB step 0.1 default 0.

New (hint 4), instance 1 shown; instances 2 and 3 are `comp2_*` / `comp3_*`, host names
"Comp 2 …" / "Comp 3 …", identical ranges and defaults:

| id | host name | range | step | skew | default | unit | read by |
|---|---|---|---|---|---|---|---|
| `comp_type` | Comp Type | Classic, Clean, Squash, FET, Opto | — | — | 1 (Clean) | — | all |
| `comp_knee` | Comp Knee | 0 – 24 | 0.1 | linear | 6 (`kCompKneeDefault[Clean]`) | dB | Clean, Squash, FET, Opto |
| `comp_mix` | Comp Mix | 0 – 100 | 0.1 | linear | 100 | % | Clean, Squash, FET, Opto |
| `comp_sc_hpf` | Comp SC HPF | 20 – 300 | 1 | log | 20 | Hz (≤ 20.5 = off) | Clean, Squash, FET, Opto |
| `comp_auto_makeup` | Comp Auto Makeup | off/on | — | — | off | — | Clean, Squash, FET, Opto |

Existing knobs in new types are read **literally** (threshold dB, ratio, attack ms,
release ms, makeup dB). Classic reads only the six existing parameters. In the new types
the makeup knob follows the knob while the block runs (+12.00 dB for a 12 dB move), and
the **total makeup, manual plus auto, is capped at 48 dB** (`kMaxTotalMakeupDb`: auto
makeup alone reaches 47.5 dB at T −60, R 20, and manual adds up to 24, which would let an
unattacked onset pass 71.5 dB of gain).

Per-type starting points (`Compressor.h`, already in §2.1):

| type | knee dB | attack ms | release ms | source |
|---|---|---|---|---|
| Classic | 6 | 5 | 120 | frozen defaults |
| Clean | 6 | 5 | 120 | frozen defaults `[study: R1 "default 6 dB"]` |
| Squash | 6 | 2.5 | 150 | `[study: R4 sketch "attack 2.5 ms, release 150 ms", knee 6]` |
| FET | 4 | 0.8 | 55 | knee `[study: R5, tagged guess]`; attack inside R5's 0.2–2.5 ms; release = R5 τ1 at the knob minimum `[fit: comp/evidence 1176 t_a100_r100]` |
| Opto | 12 | 10 | 60 | knee `[study: R5, guess]`; attack top of R5's 1–10 ms; τ1 `[fit: LA-2A Gray t_comp_pr50]` |

```cpp
// CompParams: see §2.1.
```

#### 4.2.3 Classic (type 0) — exact definition

Moved verbatim from the processor (§2.3 table): per block, `setThreshold (threshold)`,
`setRatio (jmax (1, ratio))`, `setAttack (jmax (0, attack))`, `setRelease (jmax (0,
release))`, `process` (ProcessContextReplacing on the block); then
`makeup.setTargetValue (Decibels::decibelsToGain (makeupDb))` and `applySmoothedGain`
over the block through a non-owning `AudioBuffer` view. `skip(n)` = `setTargetValue` +
`makeup.skip (n)` (`PluginProcessor.cpp:1100-1102, 1330-1331`). Makeup smoother:
`juce::SmoothedValue<float>` (linear), `reset (fs, 0.02)` and
`setCurrentAndTargetValue (decibelsToGain (makeupDb))` on the first `setParameters`
after `prepare()` (`:893-898`); `reset()` resets the compressor only (`:923, 1833`).
Unlinked stereo, peak ballistics with time = knob/2π — all defects kept on purpose.

Classic meter: a **shadow** `juce::dsp::BallisticsFilter<float>` per channel with the same
attack/release/peak settings is fed the same input and the gain is computed with
`juce::dsp::Compressor`'s own formula (`env < thr ? 1 : pow (env / thr, 1/ratio - 1)`);
GR = max over the block and channels of −20·log10(gain). The audio path never sees it.

#### 4.2.4 Shared detector and gain computer (types 1–4)

Per sample. Non-finite input samples are zeroed before anything else (in every type, and
Classic's finite input stays bit for bit), and the detector and the gain see the input
clamped to ±1e6 (`kInputLimit`, +120 dBFS):

```
xd[ch] = HP1(x[ch])  if scHpf > 20.5 else x[ch]   // TPT one-pole high-pass on the detector only
                                                  // [study: R3, one-pole; Shadow Hills 90 Hz evidence]
d      = max over channels of |xd[ch]|            // linked stereo [study: R1]
held   = max of d over the last W samples         // exact sliding-window maximum (monotonic deque)
W      = round(clamp(0.25 * tauR1, 1 ms, 12.5 ms) * fs)   // tauR1 = release knob (s)
L      = 20*log10(max(held, 1e-6))                // floor -120 dB
```

**Why the window and not the study's 8 ms hold.** The study's hold re-armed only when a
crest was ≥ the held value; off the sample-locked grid the next crest is slightly lower,
the hold expires mid-slope and the gain ripples at beat frequencies; its 0.000 % THD was
an artefact of 100 Hz being 480 samples at 48 kHz `[ver: comp/verification.md V:48-68]`.
An exact sliding-window maximum longer than the half period always contains the last
crest, so the detector reads a constant for any steady tone above 1/(2W) — 40 Hz at the
12.5 ms cap, which covers the 41.2 Hz acceptance tone (half period 12.14 ms). For fast
releases the window shrinks (W ≤ τR/4) so release stays fast; LF ripple then returns, as
in every reference at fast settings. `[choice; sim: §4.2.9]`

Gain computer (knee width Wk = knee dB, slope s = 1/R − 1, T = threshold; o = level − T):

```
gc(o) = 0                         if 2o <= -Wk
      = s * (o + Wk/2)^2 / (2 Wk) if |2o| < Wk     (Wk > 0)
      = s * o                     otherwise
```
`[study: R1; literature: Giannoulis/Massberg/Reiss, JAES 2012]`. FET uses
T_eff = T + 2.6·log2(R/4) for R > 4 `[fit: R5, 1176 curve_ratio1/curve_ratio4]`.

Smoothing after the gain computer, in dB, one-pole branches (attack when more
reduction is asked):

```
aA = exp(-1/(tauA*fs));  aR = exp(-1/(tauR_eff*fs));  tauR_eff = tauR - W/fs
g  = (gc < g) ? aA*g + (1-aA)*gc : aR*g + (1-aR)*gc
```
`tauR_eff` subtracts the window so that the release t63 measured from the end of a note
equals the knob (the window adds W of hold first) `[choice: keeps "release = knob", the
defect the study set out to fix; W ≤ τR/4 so τR_eff ≥ 0.75 τR]`. FET and Opto subtract only
half of W (`kHoldCompensationTwoStage`): the R5 fits were made on a detector that already
held for 8 ms, and with all of W removed the curves land at 58 / 84 ms (FET t50 / t63) and
78 / 126 ms (Opto), below both reference bands (C14, C15). Time constants are the knob
values (no 2π) `[study: R1]`.

Threshold and slope `s` are ramped **linearly over 20 ms** per sample `[study: R6, Guitar
Rig ramps]`; knee and mix use 20 ms one-poles; makeup uses the shared
`SmoothedValue` (20 ms linear) stepped per sample with `getNextValue()`.

Output:

```
gainDb = g + makeupDb + (autoMakeup ? M : 0)      M = -gc(-10 - T_eff), recomputed every 16 samples,
                                                  limited so that makeupDb + M <= 48 (kMaxTotalMakeupDb)
wet    = x * 10^(gainDb/20)
y      = mix * wet + (1 - mix) * x               mix = comp_mix/100; when mix == 1 exactly, y = wet
```
Auto makeup `[study: R3, published −x_sc(0) rule at a −10 dBFS guitar peak; reference level tagged guess]`;
for Squash M uses the equivalent static curve (same formula). Auto makeup on/off fades
the amount M through a 20 ms one-pole (no step).

#### 4.2.5 Type algorithms

- **Clean (1).** §4.2.4 as written, one smoothing stage.
- **Squash (2) — log-domain feedback.** The gain computer is evaluated on the **output**
  level estimate `Lo = L + g` (input level plus the current gain), `o_fb = L + g − T`.
  `feedbackGain (o_fb, s, Wk)` returns the gain and the loop gain `lg = 1 − dgain/do_fb`:
  0 dB and `lg = 1` for `o_fb ≤ −Wk/2`; `gc = s·R·o_fb` and `lg = R` for `o_fb ≥ Wk/(2R)`;
  between, the knee solved so that the loop's steady state is exactly the textbook curve
  of the other types on the **input** level (the knee spans Wk, centred on the threshold;
  with `a = s/(2Wk)`, `c = o_fb + Wk/2`, `u = 2c / (1 + sqrt (1 + 4ac))`: `gc = c − u`,
  `lg = 1 / (1 + 2au)`). **Attack and release are both divided by the loop gain**,
  `g += (gc < g ? kA : kR) / lg · (gc − g)` (release with the window compensation), so
  the closed loop moves at the knobs' time constants at any ratio and never overshoots
  (the release used to run R times fast above the threshold; C10+, C26, C27). Steady
  state: g = (1/R − 1)(L − T) above the knee, i.e. the static curve is exactly ratio R.
  What stays feedback: after a drop the output falls under the threshold, where the loop
  is open and the gain heads for 0 dB, so a partial drop recovers sooner (printed in C26).
  **Change against the study:** the study fed the detector with the previous output
  sample (`|y[n−1]|`); with any detector lag that loop undershoots — its own prototype
  dipped to −53 dB before settling at −24.6 dB (`comp/evidence/bb/proto/squash_fb.tbA.tsv`
  `g_min_db`), and a numpy model of it with RMS/peak detectors dipped 16–80 dB at
  R 8–20. Closing the loop in the log domain removes the lag and keeps every feedback
  property the ear hears (ratio-dependent speed, soft-knee programme dependence)
  `[choice; sim: undershoot 0.00 dB for R 2–20, attack 0.1–20 ms]`. Ratio is the full
  1–20 knob (the study's 2–12 clamp was a guess). The study's "release × 4" scaling is
  dropped (guess; V:81).
- **FET (3).** Clean's detector and gain computer with T_eff; two smoothing stages sharing
  the attack: `g1` with τR1 = release knob, `g2` with τR2 = 6.4·τR1, output
  `g = g1 + w·(g2 − g1)` with the slow stage's share `w = 0.43·m²` (both τ reduced by W/2
  as above) `[fit: R5, 1176 Rev A t_a100_r100: t63 95 / 123 ms after 0.5 / 5 s]`.
- **Opto (4).** As FET with τR2 = 8.3·τR1 and `w = 0.46·m`, no threshold offset
  `[fit: R5, LA-2A Gray t_comp_pr50: 157 → 184 ms, ×1.17]`. Ratio is the full knob (R5's
  2–10 limit dropped: knobs mean what they say).

**Programme-dependent release (FET and Opto), delivered.** A memory `m` in [0, 1] charges
toward the depth of the gain reduction (`min (1, GR dB / 6)`) with a time constant of
0.28 s (FET) or 0.25 s (Opto) and discharges with 4 s; the slow stage's share `w` grows
with it. Steady compression is unchanged (both stages sit on the target); only the release
after it gets longer the longer and deeper it was. Measured: FET t63 66 / 97 / 122 ms
after 0.1 / 0.5 / 5 s bursts (the unit: 56 / 95 / 123 ms); Opto 141 → 162 ms from 0.5 to
5 s, ×1.15 (the unit: ×1.17) (`Compressor.cpp`, check C28). §8 D-18.

#### 4.2.6 Makeup, bypass, reset

One makeup smoother per instance, shared by all types. `skip(n)` (block off or absent)
advances it exactly as Classic; for new types it also snaps the threshold/slope ramps,
knee, mix to their targets. `reset()` clears detector window, HPF, g/g1/g2 and the
programme memory (to 0 dB and 0), the shadow ballistics and `juce::dsp::Compressor`, and
starts on the requested type; it does not touch the makeup smoother.

**Type switches (the compressor does not use `fx::EngineSwitcher`, §2.4).** Clean,
Squash, FET and Opto share the detector and the gain state, so a switch among them hands
the current gain reduction over with no fade and the new type moves from it with its own
ballistics (the uncompressed signal never passes: the old switch let +15 dB through for
40 ms). A switch to or from Classic crossfades both outputs over 40 ms
(`kCrossfadeSeconds`): the incoming new type is seeded at the reduction Classic was
applying and held while the detector window fills; an incoming Classic runs unheard until
its envelope has risen (four attack time constants, knob / 2π, plus 2 ms). A request
during a fade hands over within the incoming side, or reverses the fade from where it is.
While bypassed (`skip`), a switch completes without a fade.

#### 4.2.7 Meter, latency, CPU

GR = max over the block of −g (dB, makeup excluded, ≥ 0); Classic from the shadow filter.
Latency 0. CPU per instance: Classic as today (≤ 0.3 %); new types ≤ 0.35 % (one `log10`
and one `exp2` per sample, deque amortised O(1)).

#### 4.2.8 fxbench keys

`type threshold ratio attack release makeup knee mix sc_hpf auto_makeup on` (`on=0` calls
`skip()`); `--trace=gain` writes the per-sample gain in dB (−g, before makeup).

#### 4.2.9 Acceptance (`check_comp.py`)

Time metrics on the gain trace: t63 = time for the gain in dB to cover 63 % of its change.
Burst = 1 kHz, 0.5 s at −10 dBFS (and −15 dBFS) after and before silence; release measured
from the burst end. "Residual" = everything except the fundamental (least-squares fit of
the fundamental and DC over the last 1 s of a 2.5 s steady tone, −10 dBFS, at 48 kHz) —
**not** harmonic THD, which ignores the inharmonic sidebands the study's hold produced
(V:77). Acceptance frequencies are deliberately not divisors of the sample rate:
41.2, 82.4, 98 Hz (and 100 Hz for comparison with the study). Settings T −20, R 4 unless
stated. Model values in brackets `[sim]`. Rows below C20 (C3b onward, C21 to C28 and K) are
the verifier-driven proofs; the table lists every row id `check_comp.py` has, with its bar.

| # | Check | Pass |
|---|---|---|
| C1 | Classic null incl. makeup ramps and `skip` sequences (§2.11) | 0 differing samples |
| C2 | Classic meter: −10 dBFS 1 kHz, T −20, R 4, 5/120 | equals −20·log10 of the measured output/input envelope ratio within 0.1 dB |
| C3 | Clean static: −10 dBFS sine | GR 7.5 ± 0.3 dB [7.50]; slope ratio near threshold and at top 4.0 ± 0.1 |
| C4 | Clean attack, 5 ms knob | t63 5.0 ± 1.0 ms |
| C5 | Clean release, 120 ms knob, −10 and −15 dBFS bursts | t63 120 ± 6 ms, the two within 3 ms of each other |
| C6 | Clean residual, defaults (5/120) at 41.2/82.4/98/100 Hz | ≤ 0.05 % each [0.000 %] (today 2.40 % at 82.4, 2.03 % at 98) |
| C7 | Clean residual, 30/1000 and 1/50 | ≤ 0.05 % at all four frequencies [0.000 %] |
| C8 | Clean residual, fastest 0.1/10 | ≤ 6 % at 41.2, ≤ 2 % at 82.4, ≤ 1.2 % at 98 [4.93 / 1.32 / 0.76] (study no-hold 4.67 % at 100 Hz; 1176 fastest 3.53 %) |
| C9 | Clean, 1 kHz residual at defaults | ≤ 0.01 % |
| C10 | Squash onset, T −34, knee 6, R ∈ {2, 4, 8, 12, 20}, attack ∈ {0.1, 2.5, 20} ms | gain minimum during the burst ≥ settled − 0.5 dB [0.00]; attack t63 = knob ± 10 % (≥ 0.12 ms floor) [0.17 / 2.54 / 20.0] |
| C11 | Squash release (600 ms knob) | t63 = 600 ± 30 ms |
| C12 | Squash static slope, R 8 | 7.9 – 8.1 |
| C13 | Squash residual at 41.2/82.4/98 Hz (R 8, 2.5/600) | ≤ 0.05 % [0.000] |
| C14 | FET release t50/t63/t90 (T −24, R 6, knee 4, att 0.3, release knob 55) | 60–70 / 90–105 / 370–420 ms `[study: R5 bands]` |
| C15 | Opto release t50/t63/t90 (T −24, R 4, knee 12, att 2, release knob 60) | 80–95 / 130–150 / 650–750 ms `[study: R5 bands]` |
| C16 | FET/Opto residual at 41.2/82.4/98 Hz at their starting points | ≤ 0.05 % [0.000] |
| C17 | FET threshold offset: −1 dB point at R 4 / 8 / 16 | moves +2.6 ± 0.3 dB per doubling above 4 |
| C18 | Mix 50 % | output = mean(dry, wet render) within −80 dB |
| C19 | SC HPF 100 Hz, `fsteps` 40 Hz vs 1 kHz | GR at 40 Hz ≥ 5 dB less than at 1 kHz (one-pole estimate 6.3 dB, V:79) |
| C20 | Auto makeup, −10 dBFS sine, R ∈ {2, 4, 8, 20}, knee 6, every new type | output level within 0.5 dB of input |
| C21 | Automation (replaces the old line-at-k·fs/block test, which counted the modulation's own sidebands): each of 9 controls (threshold −40 ↔ −10, ratio 1 ↔ 20, attack 0.1 ↔ 100, release 10 ↔ 1000, knee 0 ↔ 24, mix 0 ↔ 100, makeup 0 ↔ 24, sc_hpf 20 ↔ 300, auto makeup off ↔ on) as a 10 Hz square wave per block, 4 new types, blocks 512 / 64 / 37, T −25 and each type's starting point. (a) 1 kHz tone: the loudest 21 ms frame of energy more than 3 kHz above the carrier | (a) ≤ −70 dBc |
| C21 | (b) 82.4 Hz bass note (row "C21 automation zipper"), where the fastest settings legitimately move the gain in a millisecond | the block render adds ≤ 3 dB to the same automation rendered at block 1 |
| C21b | Zipper isolated: threshold steps on block boundaries (2560-sample half period), blocks 512 / 64 against block 1, 4 new types | lines at k·fs/block in the difference ≤ −70 dBc |
| C22 | Type switch steps: every ordered pair of the 5 types (20), block 512, sustained −10 dBFS note | no 1 ms output level change > 3 dB |
| C22 | Classic → Clean crossfade (Clean at mix 0 makes the fade visible as a 15 dB rise), blocks 64 / 512 / 37 | ends 40 ± 2 ms after the switch |
| C22 | Switch level: 20 pairs × blocks 64 / 512 / 37, 1 kHz −10 dBFS, T −30, R 4 (15 dB of reduction) | level ≤ 1 dB over the larger of the pre-switch level and the new type's own settled level |
| C22 | Switch settles: same cells | 0.5 to 1 s later within 0.5 dB of a render that was always the new type |
| C22 | Switch click: same cells, 50 ms before to 0.5 s after | no frame's energy above carrier + 3 kHz over −70 dBc, unless one of the two types makes as much alone, in which case the switch adds ≤ 3 dB |
| C23 | Rates | C3, C4, C5, C6, C10 at 44.1, 96, 22.05 and 192 kHz, same bounds |
| C3b | Linked stereo: L −10 / R −30 dBFS, each new type | \|GR left − GR right\| ≤ 0.01 dB |
| C3c | GR meter: −10 dBFS, T −20, R 4, each new type | \|meter − 7.5\| ≤ 0.3 dB |
| C10+ | Squash stability: R 1 … 20 (10 values) × attack 0.1 … 100 (7) × knee 0 / 6 / 24, a −40 → 0 dBFS step and back to −20, at 22.05 kHz and 192 kHz | the gain moves monotonically each way: overshoot past the settled gain ≤ 0.05 dB |
| C24 | NaN / infinity: 8 NaN, 8 +inf, 8 −inf samples on the left channel (and the only channel), each type, sc_hpf off and 100, mono and stereo, blocks 512 / 37 | output and gain trace finite from one detector window (12.5 ms) after the first bad sample; level 1.5 s on within 0.1 dB of a clean render |
| C25 | Output bound: a fuzz case (makeup 24, auto makeup, T −60, R 17.4, bypass, type changes), a full-scale 41 Hz onset at attack 100 with maximum makeup and auto makeup in each new type, a note through a bypass with a type change for every type pair | \|out\| < 1000 for every case |
| C26 | Squash release: (a) 45 bursts to silence (levels −16 / −10 / 0 dBFS × R 2…20 × knee 0 / 6 / 24); (b) drops that leave the output above the threshold and beyond the knee (−4 → −6 / −8 / −10 / −16 dBFS) | t63 within 20 % of the 150 ms knob. Printed, not enforced: drops that take the output under the threshold recover sooner |
| C27 | Squash knee: R 4 / 8 / 20 / 1.5 with knees 6 / 6 / 24 / 12, GR of a steady tone at T − k/2, T, T + k/2, T + 10 | within 20 % of the textbook curve (0.1 dB where the curve is under 0.5 dB) |
| C28 | Programme release: FET (T −24, R 6, knee 4, attack 0.3, release 55) and Opto (T −24, R 4, knee 12, attack 2, release 60): t63 after 0.1 / 0.5 / 5 s bursts | t63 grows ≥ 5 % per step; the 5 s / 0.5 s ratio (and 0.1 s / 0.5 s for FET) within 20 % of the unit's (FET 123/95 and 56/95, Opto 184/157); a −20 dBFS burst releases faster than a −10 dBFS one; a 0.5 s burst 12 s after a 5 s one releases within 5 % of a fresh one |
| K | Per-control rows, 45: for each type, each control the type reads is moved while the block runs. Classic: threshold, ratio, attack, release, makeup; knee, mix, sc_hpf and auto_makeup (not read) must leave its output bit for bit unchanged. Each new type: threshold, ratio, attack, release, makeup, knee, mix, sc_hpf, auto_makeup | output level change in the right direction and within max (0.25 dB, 20 %) of what the type's curve predicts (the predicted change is at least 1 dB); for makeup, mix and auto makeup the GR change must also match (0 dB for makeup, within max (0.1 dB, 20 %)); attack and release: t63 ratio within 25 % of the knob ratio (1 → 20 ms, 50 → 400 ms). The row that exposed makeup doing nothing in four types |

Also printed, not pass/fail: the low-frequency residual split into harmonic distortion and
inharmonic sidebands for every type at its starting point and at the fastest settings
(41.2 / 82.4 / 98 / 110 Hz at 44.1 and 48 kHz), attack and release per type, and the
Classic crossfade timing at 44.1 kHz. `check_comp.py` ran 145 checks, 0 FAIL, at the commit
that added the K rows (`a27b550`). C1 in the script is two nulls: ramps and `skip`
sequences (blocks 512 / 37 / 1) and a mono `skip`-first sequence.

### 4.3 Drive — `DriveFx` (`src/dsp/Drive.h/.cpp`)

Study: `docs/research/fx/drive/report.md` R1–R7, corrected by its Verification status and
`verification.md` (V:64 Skreamer high-pass, V:76-88 alias figures, V:97 and V:110 sourcing,
V:109 latency).

#### 4.3.1 Types (frozen order)

| index | name | what | oversampled | latency (48 kHz) |
|---|---|---|---|---|
| 0 | Classic | today's 2x IIR + `tanh` + tone LPF + level, bit-for-bit | 2x (unchanged) | 3 (reported now, audio unchanged) |
| 1 | Screamer | TS-808-style: 720 Hz pre-clip high-pass, diodes in the op-amp feedback, clean path summed, active tone stage | yes | 5 |
| 2 | Distortion | RAT-style: op-amp gain with two RC legs, rail limit, 1 kΩ + diode pair to ground, RC filter | yes | 5 |
| 3 | Fuzz | Big-Muff-style: booster, sustain, two feedback-clipping stages, passive LP/HP tone blend | yes | 5 |
| 4 | Boost | clean gain + one-pole high cut | no | 0 |
| 5 | Treble | clean first-order high shelf (treble booster) | no | 0 |

Default for new blocks: **Screamer**. Legacy pin: `drive<n>_type` → 0.

#### 4.3.2 Parameters

Existing, unchanged: `drive_on` (default false for instance 1, true for 2–3);
`drive_gain` 0…36 dB step 0.1 default 12; `drive_tone` log 500…12000 Hz step 1 default
4000; `drive_level` −24…12 dB step 0.1 default 0. New (hint 4):

| id | host name | range | default | read by |
|---|---|---|---|---|
| `drive_type` (`drive2_type`, `drive3_type`; "Drive 2 Type"…) | Drive Type | Classic, Screamer, Distortion, Fuzz, Boost, Treble | 1 (Screamer) | all |

How each type reads the three existing knobs (d = `drive_gain`/36 ∈ [0,1],
T = ln(`drive_tone`/500)/ln 24 ∈ [0,1]) `[study: R1]`:

| type | `drive_gain` | `drive_tone` | `drive_level` |
|---|---|---|---|
| Classic | dB into `tanh` | LPF Hz | dB |
| Screamer | d → Drive pot | T → Tone pot | dB |
| Distortion | d → the stage's 1 kHz gain, 0 to 62 dB linear in d (§4.3.5) | T → Filter pot (1 = bright) | dB |
| Fuzz | d → Sustain divider, −78 (1 − d)² dB (§4.3.5) | T → Tone pot (0 = bass, 1 = treble) | dB |
| Boost | gain dB, literal | high-cut Hz, literal | dB |
| Treble | shelf gain dB, literal | shelf centre Hz, literal | dB |

The UI relabels the knobs per type (§7.3); the host still sees dB/Hz.

Per-type starting points (`Drive.h`, `kDriveGainDefault`, `kDriveToneDefault`,
`kDriveLevelDefault`):

| type | gain dB | tone Hz | level dB | note |
|---|---|---|---|---|
| Classic | 12 | 4000 | 0 | frozen defaults |
| Screamer | 12 | 4000 | 0 | = frozen defaults (§2.7.4): d = 0.33, T = 0.65 |
| Distortion | 18 | 2449 | −8.2 (L₂) | d = T = 0.5 |
| Fuzz | 18 | 2449 | 5.0 (L₃) | d = T = 0.5 |
| Boost | 12 | 12000 | 0 | |
| Treble | 12 | 3000 | 0 | centre near the Rangemaster's input corner and GR6's 3.6 kHz peak `[study: R7]` |

L₂, L₃ are set by the drive engineer by measurement: the level at which a −18 dBFS 1 kHz
sine at that row's gain and tone leaves at −12 dBFS peak ± 0.5 dB `[choice: a 6 dB push,
how a pedal sits in front of an amp]`; recorded in `Drive.h` (`kDriveLevelDefault`): L₂ = −8.2
dB and L₃ = 5.0 dB (L₃ was 4.6 dB before the Fuzz sustain law changed; `check_drive.py` prints
the level it needs and D16 checks it).

```cpp
struct DriveParams
{
    int type = 1;            // DriveFx::Type
    float gainDb = 12.0f;    // [0, 36]
    float toneHz = 4000.0f;  // [500, 12000]
    float levelDb = 0.0f;    // [-24, 12]
};
```

#### 4.3.3 Classic (type 0) — exact definition

`Drive` moved verbatim (`FxBlocks.cpp:45-123`): `juce::dsp::Oversampling<float>
(numChannels, 1, filterHalfBandPolyphaseIIR, true, false)` (2x, max quality, non-integer
latency); gain `SmoothedValue` stepped once per host sample; `std::tanh (x * gain)` in
float at 2x; down; `StateVariableTPTFilter` low-pass at `clampCutoff (tone)` set once per
block; level `SmoothedValue`. **New:** `getLatencySamples()` returns
`(int) std::lround (oversampling->getLatencyInSamples())` = 3 at every rate (3.137
samples, `[ver: drive/verification.md V:28, V:109]`). The audio is unchanged; the residual
0.137 sample is a comb null near 175 kHz, far out of band.

Non-finite input: `DriveFx::process` replaces every NaN and infinity in the block with 0
before any type (or the switcher's dry copy) sees it; finite samples are untouched, so the
Classic null still holds. A finite input near `FLT_MAX` can overflow Classic's float
half-bands: if Classic's output holds a non-finite sample, those samples become 0 and Classic
is reset (`clearNonFinite`, `Impl::run`). Where its output is finite it is passed on exactly.

#### 4.3.4 Shared engine for Screamer, Distortion, Fuzz

```
x  = in * V_REF                          // V_REF = 1 V per digital full scale [choice; study R1]
x  = clamp(x, ±1000 V)                   // a NaN fails the comparison and becomes 0 [choice: keeps any finite input finite]
x  = HP1(x, 15.9 Hz)                     // DC block, bilinear one-pole in double, base rate [study: R3, Yeh input coupling]
x↑ = upsample_N(x)                       // N from the rate table below
y↑ = TYPE(x↑)                            // all linear voicing, clipping, the clean path AND the tone stage at N·fs
y  = downsample_N(y↑)
y  = HP(y, 1.9 Hz)                       // Fuzz only: CB4 into the output bias divider (see Fuzz)
out = y * level / V_REF
```

The tone stage runs at N·fs, not at the base rate: at the base rate its bilinear warping
cost 1.2 to 1.4 dB at 10 kHz (48 kHz) against the 0.5 dB D8 allows (comment in
`Drive.cpp`, `runOversampled`).

- **Oversampling factor** by session rate `[choice: keeps the internal rate ≥ 176.4 kHz,
  which is what puts 4x at 48 kHz below the alias limit]`: N = 4 for fs < 80 kHz,
  N = 2 for 80 ≤ fs < 160 kHz, N = 1 for fs ≥ 160 kHz. **Distortion runs at twice that**
  (N = 8, 4, 2): its 67 dB op-amp stage into a rail makes the edge a near-ideal step, and
  at 4x even the second-order kernel leaves −67 dB of images for a 7.5 kHz tone
  (`Drive.cpp`, `factorFor`). `juce::dsp::Oversampling<float>`,
  `filterHalfBandPolyphaseIIR`, max quality, `useIntegerLatency = false`, one object per
  stage count (0 to 3), created in `prepare()`.
- **Why the linear voicing runs at N·fs:** the TS sums the clean signal with the clipped
  excess; doing both after the same upsampler keeps them aligned to the sample (the
  study's sketch added an undelayed base-rate `x` to a signal 4.4 samples late, which
  would comb-filter). Pre-clip filters at N·fs cost four biquads per input sample.
- **Clipper family** `[study: R2]`: antiparallel diode pair behind a resistance R,
  `u = g(v) = v + k·sinh(v/a)`, k = 2·Is·R. Solve for v per oversampled sample in double:
  ```
  v = sign(u) * min(|u|, a*asinh(|u|/k))            // upper bound start
  repeat 3: Halley step on f(v) = v + k*sinh(v/a) - u   // reaches double precision from 1e-6 to 1e5 V
  F1(u) = u*v - G(v),            G(v) = v*v/2 + k*a*(cosh(v/a) - 1)
  F2(u) = u*u*v/2 - u*G(v) + B(v)                      // B' = g^2/2, written so an error e in v moves F1, F2 by O(e^2)
  D(a, b) = (F2(a) - F2(b)) / (a - b)                  // mean of F1 over [b, a]
  y = 2 (D(u, u1) - D(u1, u2)) / (u - u2)              // u1, u2 the two previous inputs
  ```
  **Second-order ADAA** at N·fs (one oversampled sample of delay), not the first-order
  scheme this spec first had: at full gain the diodes are driven so hard that the edge is a
  step within a fraction of a sample, and a first-order kernel at 4x left images at −46 dB
  (Distortion, 7.5 kHz) to −57 dB (1.2 kHz) below 20 kHz (`Drive.cpp`, `DiodeClipper`). Where
  the divided differences are ill-conditioned (spans below 1e-5 (1 + mean |u|) V) the limits
  are used: an end-corrected trapezoid for D, and the previous v for y. Distortion's 1 kΩ
  curve below its rail is a fixed function, so a 1024-interval cubic Hermite table of v(u)
  gives the Halley start and one step replaces three.
- **Diode constants (all three types):** Is = 2.52 nA, n = 1.752, a = n·V_T =
  1.752 × 25.85 mV = 45.3 mV `[circuit: the standard SPICE model of the 1N4148/1N914
  (D1N4148: IS=2.52n N=1.752), which the TS-808 (1S1588/1N914), RAT (1N914) and Big Muff
  (1N914) use; this re-sources the study's decompiled values as V:126 asked]`.
- **Latency:** `lround (halfBandLatency(N) + clippers / N)`, `latencyFor` in `Drive.cpp`:
  the half-band cascade's low-frequency delay in base samples (0 / 3.1373 / 4.4327 / 4.9514
  for 0 to 3 stages) plus one oversampled sample per clipper (Screamer and Distortion 1, Fuzz 2,
  each divided by N). Per type and rate, from that formula:

  | rate | Screamer (N) | Distortion (N) | Fuzz (N) |
  |---|---|---|---|
  | 44.1 / 48 kHz | 5 (4) | 5 (8) | 5 (4) |
  | 88.2 / 96 kHz | 4 (2) | 5 (4) | 4 (2) |
  | 176.4 / 192 kHz | 1 (1) | 4 (2) | 2 (1) |

  Constant per (type, rate). (This spec first had 3 at 88.2/96 kHz and 1 at 176.4/192 kHz
  for every oversampled type: it predates the second-order clipper, the second Fuzz clipper
  and Distortion's doubled factor.)
- **Smoothing:** d, T and level each go through a `Glide`: two one-poles in series, each half
  the 20 ms section default (10 ms each, so 63 % at about 21 ms like the single 20 ms pole),
  without the corner a single pole puts at the start of a move; within 1e-6 (1 + |target|)
  of its target it lands on the target and reports settled `[choice: fx::Smoother in float
  stalled about 3e-5 short for good]`. While a glide runs, the coefficients **follow every
  base sample** (a pass of one base sample); once all glides have landed, work goes in
  passes of 32 base samples and nothing is recomputed. Held for 32 samples the coefficients
  stepped audibly (−59 dBc under a 98 Hz note). The second-order filters (Screamer PRE,
  tone stages) are **trapezoidal state-variable filters** (Zavalishin), the one-poles
  trapezoidal integrators: their states are the integrators', so retuning them while they
  run moves the response smoothly (a direct form retuned every sample left −54 dBc above
  3 kHz after a tone step under a 98 Hz note). Worst zipper over all knobs now −84.2 dBc
  (row Z). A change of series resistance re-evaluates the clipper's stored previous
  inputs on the new curve (`refresh`) so it does not put a step into the next difference.
- **Pot taper** for the Screamer's Drive pot and the Distortion's Filter pot:
  `A(p) = (81^p − 1)/80` (10 % of the resistance at mid travel) `[circuit: both are
  audio-taper ("A") pots; 81 is the common 10 %-at-half A-taper convention]`. The Distortion
  gain and the Fuzz sustain are not pot tapers: they are laws in dB (§4.3.5).
- **Non-finite input:** `DriveFx::process` zeroes NaN and infinity before the switcher; the
  pedal types also clamp to ±1000 V, and states outside 1e−15…1e12 are cleared once per block.

#### 4.3.5 Type algorithms

**Screamer (1)** — `[circuit: Ibanez TS-808 values as given in Yeh, Abel & Smith,
"Simplified, physically-informed models of distortion and overdrive guitar effects
pedals", DAFx-07; study: R3]`:

- R1 = 4.7 kΩ, Cz = 47 nF, Cc = 51 pF, R2 = 51 kΩ + 500 kΩ·A(d).
- Pre-clip excess: `PRE(s) = (R2/R1) · s R1 Cz/(1 + s R1 Cz) · 1/(1 + s R2 Cc)`
  (the op-amp's G(s) − 1, Zf = R2‖Cc over Zg = R1 + 1/sCz), bilinear at N·fs, prewarped at
  720 Hz. High-pass corner 1/(2π R1 Cz) = **720.5 Hz, fixed** — this is the circuit. (The
  study's "Skreamer 720 Hz" claim was wrong for Guitar Rig's classic Skreamer, whose corner
  is 587 Hz at its default Bass setting and moves with that knob `[ver: V:64]`; our type
  models the pedal, not that plugin, and its acceptance is against the circuit.)
- Clip: `u = PRE(x↑)`, v from the clipper with k = 2·Is·R2 (diodes across R2).
- Clean path: `y↑ = x↑[n−1] + v[n]` `[circuit: non-inverting stage, output = input + feedback-network voltage]`;
  the input is delayed by the one oversampled sample the ADAA adds, so the two stay aligned.
- TONE (base rate), the TS tone/volume stage `[circuit: TS-808 via Yeh: Rs 1 kΩ, Cs 0.22 µF,
  Ri 10 kΩ, Rf 1 kΩ, tone pot 20 kΩ linear, Rz 220 Ω, Cz2 0.22 µF]`, derived here (the
  study's expression left `Rr` undefined): with T' = clamp(T, 0.005, 0.995)
  `[choice: pot end resistance]`, Gl = 1/(T'·20k), Gr = 1/((1−T')·20k), G = Gl + Gr,
  gs = 1/Rs + 1/Ri, C = Cz2:
  ```
  H(s) = (N0 + N1 s) / (D0 + D1 s + D2 s^2)
  N0 = G/Rs                      N1 = C (G Rz + 1 + Rf Gr) / Rs
  D0 = gs G                      D1 = gs C (G Rz + 1) + Cs G + Gl C
  D2 = Cs C (G Rz + 1)
  ```
  (Op-amp node: Vout = Va·(1 + Rf·Gr·Yw/(G + Yw)), Yw = sC/(1 + s Rz C); input node loaded by
  Gl·Yw/(G + Yw).) T' → 1 is bright (+14.8 dB shelf), T' → 0 dark. Bilinear, c = 2·N·fs
  (the tone stage runs at the oversampled rate, §4.3.4), as a trapezoidal state-variable filter.
  Known limit, not fixed: the Screamer droops 0.84 dB at 10 kHz (commit `0af54cf`).

**Distortion (2)** — `[circuit: ProCo RAT; values confirmed in the ElectroSmash RAT analysis
(electrosmash.com/proco-rat: 47 Ω/2.2 µF → 1539 Hz, 560 Ω/4.7 µF → 60 Hz, 100 kΩ pots,
3.3 nF, 1N914, gain 1 + R/(47‖560) ≈ 2305) and cushychicken.github.io/ltspice-proco-rat
(LM308, ~67 dB, filter 32 kHz → 475 Hz)]`:

- Gain stage: `G(s) = 1 + Zf/Zg`, Zg = (47 + 1/sC1) ‖ (560 + 1/sC2), C1 = 2.2 µF, C2 = 4.7 µF;
  Zf = Rd ‖ (1/sCf), Cf = 100 pF `[circuit: madbeanpedals "Runt2020" schematic and BOM,
  a RAT clone: R4 47R + C6 2u2, R5 560R + C5 4u7, GAIN 100kA with C4 100pF across it, R6 1k,
  D1/D2 1n914, CUT 100kA in series with R7 1k5 into C8 3n3, LM308 with C3 30pF; cited in
  Drive.cpp]`. **The Distortion knob is a law in dB, not the pot's A taper:** the stage's
  1 kHz gain runs linearly in d from 0 dB (knob 0) to 62 dB (knob 36),
  `Rd = Z (G^d − 1)`, Z = |Zg(j·2π·1 kHz)|, G = 1 + 100 kΩ/Z `[choice]`. Through the A taper
  half the knob was already 42 dB and from a sixth of its travel up the output moved
  1.5 dB for 36 dB of input; now the middle is 31 dB (7.0 dB of output for 18 dB of input
  at the middle, was 2.6, commit `0af54cf`). Zf's pole is kept below 0.45·N·fs.
- Op-amp bandwidth: one-pole low-pass at f_gbw = min(GBW/|G(j·2π·1 kHz)|, 0.45·N·fs),
  recomputed with d, GBW = 1 MHz `[circuit: LM308 data sheet (Dec 1994), "Open Loop
  Frequency Response": with 30 pF compensation the gain crosses unity near 1 MHz, falling
  6 dB/oct]` (the study's 2.3 MHz was a fit to harness-only evidence, V:97, and is no
  longer used). LM308 slew limiting is not modelled `[choice]`.
- Rail: `u_r = clamp(u, −4.5 V, +4.5 V)` `[circuit: 9 V supply, op-amp biased at 4.5 V]`,
  folded into the clipper's ADAA: for |u| > 4.5 the composite function is v(4.5·sign u),
  `F1(u) = F1(4.5) + v(4.5)·(|u| − 4.5)` and F2 continues as its integral.
- Clipper: series 1 kΩ into the diode pair to ground, k = 2·Is·1 kΩ `[circuit: RAT 1 kΩ
  series resistor, same schematic]`. No clean path.
- TONE (at N·fs): one-pole low-pass, R = 1.5 kΩ + 100 kΩ·A(1 − T) (the CUT pot is an audio
  taper in the schematic; the spec assumed linear, the ends are the same),
  C = 3.3 nF → 32.2 kHz (T = 1) … 475 Hz (T = 0), clamped to 0.45·N·fs.

**Fuzz (3)** — `[circuit: Big Muff π early-version values, guitarscience.net "bigmuff.pdf"
Fig. 1 and §4 (tone network, notch at 900 Hz)]`:

- Booster: inverting, gain RB/RG1 = 470 k/39 k = 12.05 with CF1 = 470 pF across RB →
  low-pass 720.5 Hz.
- Sustain: a divider between booster and stage 1 `[circuit: "SUSTAIN B100K", PedalPCB
  MuffinFuzz]`. **The knob sets it in dB:** gain p = 10^(−78 (1 − d)² / 20), i.e. −78 dB at
  knob 0 to the full pot at knob 36 (−44 / −19.5 / −4.9 dB at the quarters) `[choice]`. Two
  clipping stages of 31 dB each saturate on anything the first passes, so the pedal follows
  the pick only where the divider keeps a note below the second stage's knee, near −60 dB
  for a −30 dBFS note; the floor puts a full strum there at knob 0. The middle stays fully
  saturated because D14 reads the tone stack through the clipped stages, which leaves it
  under 1 dB of dynamics (row Y2). The wiper's Thevenin resistance p (1 − p)·100 kΩ is added
  to the stage-1 source resistance Rs (10 kΩ) in the loop gain below.
- Two clipping stages, each: gain RB/RG = 470 k/10 k = 47 with CF = 470 pF across RB
  (low-pass 720.5 Hz) → clipper with diodes across RB, k = 2·Is·470 kΩ → coupling high-pass
  1 µF into 10 kΩ = 15.9 Hz (the booster is followed by the same coupling into stage 1).
  (The study's 1.3 kHz / 100 Hz / d^3.3 were fits; replaced by the circuit.)
- **Transistor loading** (guitarscience.net "bigmuff.pdf" eq. 9, 10, 14; RC 47 kΩ, RE 100 Ω,
  base divider 470 k/100 k, 9 V, β = 200, V_BE 0.65 V, I_C = 99.8 µA, r_π = β V_T / I_C =
  51.8 kΩ): each stage is `A(s) = α RB/Rs / ((1 + α + RB/RC) + (1 + α) s RB CF)`,
  α = β/(1 + r_π/Rs), still first order. The loading takes 1.0 dB (booster) and 2.5 dB
  (clipping stages) off the ideal gains and lifts the 720 Hz poles to 803 and 937 Hz.
- **Even harmonics:** each clipping stage's output is `v (1 + σ v / 2)` with
  σ = (r_π/Rs)/(1 + r_π/Rs) / ((1 + Tr) I_C RC) (0.0454 per volt for a clipping stage,
  Tr = α/(1 + RB/RC) the return ratio), from the collector current's dependence on its swing;
  the diodes keep |v| below a volt, where the next term is negligible. The asymmetry is derived
  from the bias, not set by hand (row A1).
- TONE (at N·fs), passive network `[choice: the 1975 "Ram's Head" stack, coda-effects.com
  "big-muff-tonestack-dealing-with-mids", 100 kΩ linear tone pot from muzique.com/lab/dtone.htm;
  the paper's 27 k/10 nF/4.7 nF/27 k stack dipped only 3.4 dB at 900 Hz, this one 6.1 dB]`:
  Vin feeds RT = 39 kΩ to node A with CT = 10 nF to ground (low-pass, 408 Hz) and CB = 4 nF to
  node B with RB = 22 kΩ to ground (high-pass, 1809 Hz); the tone pot Rm = 100 kΩ linear
  joins A and B through the wiper W, with 0.5 % end resistance, and the wiper is loaded by
  RL = 470 kΩ ‖ 100 kΩ (the output stage's bias divider, so the stack is no longer unloaded).
  With T = wiper position from A (bass) to B (treble), Gm_a = 1/(T Rm), Gm_b = 1/((1 − T) Rm):
  ```
  (Vin - VA)/RT = VA s CT + (VA - VW) Gm_a
  (Vin - VB) s CB = VB/RB + (VB - VW) Gm_b
  VW (Gm_a + Gm_b + 1/RL) = VA Gm_a + VB Gm_b
  Vout = VW
  ```
  Node W is eliminated, then A and B, giving a second-order H(s) (`updateTone`), bilinear,
  c = 2·N·fs, as a trapezoidal state-variable filter.
- **Output stage:** after the down-sampler, a 1.9 Hz high-pass (CB4 1 µF into RB41 470 k ‖
  RB42 100 k) takes away the DC the asymmetry makes; the rest of the output stage's gain is
  folded into `drive_level`.

**Boost (4):** `y = x · 10^(gain/20)`, then TPT one-pole low-pass at `drive_tone` Hz, level.
Linear, no oversampling, latency 0. `[study: R7, GR6 GainBoost flat, gain 30 dB × knob]`

**Treble (5):** first-order high shelf, 0 dB below, `drive_gain` dB above, geometric centre
f_c = `drive_tone`: `H(s) = (1 + s/ωz)/(1 + s/ωp)`, ωz = ωc/√G, ωp = ωc·√G,
G = 10^(gain/20); bilinear prewarped at ωc; level. Linear, latency 0. Built as
`x + (G − 1)(x − LP(x))` with the one-pole LP at ωp, so a gain move is a smooth multiply; gain,
tone and level glide as in §4.3.4 and the filter follows them every sample (a 32-sample staircase
zippered at −77 dBc). `[choice; study: R7
"first-order high-pass shelf"; GR6 treble booster +37 dB at 3.6 kHz vs +12 dB at 110 Hz]`

Not built (study "not recommended"): asymmetric SD-1 clipping, neural fuzz, capacitor
memory, sinc resampling.

#### 4.3.6 Type switches

`fx::EngineSwitcher` (§2.4). A switch between types with different latency changes the
reported latency (the host re-aligns at its next opportunity); the fade hides the step.

#### 4.3.7 Latency, CPU

Latency as the type table, exposed twice: `int getLatencySamples() const noexcept` (current
type, prepared rate) and `static int latencyFor (int type, double sampleRate) noexcept`, a pure
function the processor's latency rule can call from the message thread (§6.3). The type table
gives 48 kHz; the figures at every rate are in §4.3.4 (Classic 3 at every rate; Screamer 5 / 4 / 1,
Distortion 5 / 5 / 4, Fuzz 5 / 4 / 2 at 44.1–48 / 88.2–96 / 176.4–192 kHz; Boost, Treble 0).
CPU per instance (the factor adapts to the rate, so budgets
hold at every rate), as `BUDGETS` in `check_drive.py`: Classic ≤ 0.35 %; Screamer ≤ 2.0 %;
Distortion ≤ 3.5 % and Fuzz ≤ 4.0 % (raised from 2.0 / 3.0 % by the coordinator for
the anti-aliasing scheme, per the script's comment); Boost, Treble ≤ 0.05 %. After a knob move the CPU must return to
the settled figure (row C).

#### 4.3.8 fxbench keys

`type gain tone level on`; `--latency` prints the reported latency.

#### 4.3.9 Acceptance (`check_drive.py`)

Small-signal responses are measured at −70 dBFS input with swept sines and compared with
the analog transfer functions of §4.3.5 evaluated in numpy (scaled by the clipper's
small-signal gain 1/(1 + k/a)). Alias metric: 1 s periodic sine at f0, −6 dBFS, gain 36
(d = 1), tone at its brightest, 48 kHz; worst inharmonic component (any bin not within
±2 bins of a harmonic) below 20 kHz, relative to the fundamental, and the RSS sum of all
of them `[study: R2 metric; ver: V:77 "worst below 20 kHz", not "below f0"]`.

| # | Check | Pass |
|---|---|---|
| D1 | Classic null (§2.11) | 0 differing samples |
| D2 | Classic reported latency at 44.1/48/96/192 kHz | 3; measured phase delay at 1 kHz 3.14 ± 0.05 samples |
| D3 | Screamer reported latency | equals `lround` of the measured group delay at 1 kHz (gain 0, −60 dBFS, circuit phase divided out) at 22.05, 44.1, 48, 96, 192 kHz; 5 at 48 kHz |
| L | Distortion and Fuzz reported latency (the script's "L latency" rows) | same rule as D3 (Distortion gain 0 at −60 dBFS; Fuzz gain 18 at −120 dBFS); 5 at 48 kHz |
| D4 | Alias, Screamer, f0 ∈ {3830, 5000, 6100, 7919} Hz | worst ≤ −50 dB, sum ≤ −45 dB (today −21.4 / −20.8; tanh model of this scheme −53.8…−58.5, V:85) |
| D5 | Alias, Distortion and Fuzz, same f0 | worst ≤ −45 dB, sum ≤ −40 dB (hard-knee clipping; tanh hard-clip model −51.6 sum, study alias_schemes.tsv) |
| D6 | Alias at 1244 Hz, −6 dBFS, d = 1, all three | sum ≤ −80 dB `[study: R2 proof]` |
| D7 | D4–D6 at 44.1, 96, 192 kHz | same bounds |
| D8 | Screamer small-signal vs analog, d ∈ {0, 0.5, 1}, T ∈ {0, 0.5, 1}, 30 Hz–10 kHz | within 0.5 dB at 48 kHz (bilinear warping above) |
| D9 | Screamer pre-clip emphasis (input for 1 dB compression vs frequency, d = 0.5) | 110 Hz −16.4 ± 1.0 dB and 640 Hz −3.5 ± 0.7 dB relative to 2.5 kHz (first-order 720.5 Hz high-pass; GR6 Skreamer AU at Bass 0.5 reads −15.0/−2.7 with its 587 Hz corner, V:64) |
| D10 | Screamer response peak (d = 0.5, T = 0.5) | 650–800 Hz; 110 Hz and 5 kHz each −9 to −13 dB re peak |
| D11 | Screamer symmetry | H2 at 1 kHz −18 dBFS ≤ −60 dB |
| D12 | Distortion gain-stage small-signal vs analog, d ∈ {0.25, 0.5, 1} | within 0.5 dB 30 Hz–10 kHz; max gain at d = 1 within 1 dB of 1 + 100k/(47‖560) = 67.3 dB before the GBW pole |
| D13 | Distortion filter | T = 1: −3 dB above 15 kHz; T = 0: −3 dB at 475 ± 50 Hz |
| D14 | Fuzz | THD > 70 % at −18 dBFS, d = 0.5; at T = 0.5 the response at 900 Hz is ≥ 3 dB below the straight line (in dB over log f) joining its 220 Hz and 5 kHz values `[study: R6 proof]` |
| D15 | Boost / Treble | THD < 0.01 % at −6 dBFS out; Boost flat ± 0.1 dB 20 Hz–2 kHz with tone 12 kHz; Treble +gain ± 0.5 dB at 10·f_c, 0 ± 0.5 dB at f_c/10, 6 dB/oct between |
| D16 | Level | Distortion and Fuzz at their starting points: −18 dBFS 1 kHz in → −12 ± 1 dBFS peak out (Screamer's start is the frozen defaults and is exempt) |
| D17 | Tone sweep zipper: T 0 → 1 over 0.5 s, Screamer d = 0, −40 dBFS 1 kHz | non-harmonic content above 4 kHz ≤ −80 dBc |
| D18 | Split-lane alignment (processor test, §6.3): Classic drive at gain 0, −40 dBFS, lane A; lane B empty; summed | flat within 0.5 dB to 10 kHz, the tone filter's own response divided out (today: comb null near 7.38 kHz). Restated from 16 kHz: Classic's frozen 2x IIR delay rises from 3.14 samples at low frequency to 3.84 at 16 kHz, so a 3-sample wait cannot hold 0.5 dB there at 44.1/48 kHz (measured through the processor: −0.38 / −0.25 dB worst to 10 kHz, −8.2 / −3.9 dB at 16 kHz; 96 kHz −0.02 dB) |
| D18n | The same split-lane test for Screamer (gain 0, −60 dBFS), Distortion (gain 0, −60 dBFS) and Fuzz (gain 0, −120 dBFS), with the lag taken from `--latency`, at 44.1 and 48 kHz | flat within 0.5 dB to 10 kHz (16 kHz reported) |
| X1 | DC: Screamer, Distortion, Fuzz, Boost, Treble at gain 36, tone 12 kHz, 100 Hz −6 dBFS, 2 s | mean ≤ −80 dBFS |
| X2 | Gain step 0 → 36 dB at 1 s under a 1 kHz −18 dBFS sine, every new type | largest sample step at the move ≤ 1.1 × the largest step the steady signal makes at any gain in 0, 3 … 36 dB |
| X3 | Type switches 0-1-2-3-4-5-1 under a 220 Hz −12 dBFS sine at gain 24, tone 4000 | largest step in a switch ≤ 1.1 × the larger neighbouring steady step |
| Z | Knob zipper, every knob (gain, tone, level) of every new type: steps and 50 ms ramps both ways at block 64, under a 98 Hz note and 40–800 Hz noise at a level that keeps the circuit linear (Screamer −65, Distortion −70, Fuzz −110, Boost and Treble −12 dBFS) | worst frame's power above 3 kHz during the move ≤ −80 dBc re the settled output (worst now −84.2 dBc); the rest floor is reported |
| C | CPU one second after a move of all three knobs (30 s pink noise, `--bench`, min of 3) | ≤ 1.05 × the CPU of the settled values (the Treble shelf used to cost 23 % more after a gain move) |
| Y1 | Dynamics, guitar-like 110 Hz stack −30 → −12 dBFS peak, gain 0, Screamer, Distortion, Fuzz | output moves ≥ 12 dB for the 18 dB |
| Y2 | The same at gain 18: Screamer and Distortion | output moves ≥ 5 dB |
| Y2 (Fuzz, "sustain mid") | The same at gain 18, Fuzz | output moves ≤ 3 dB for the 18 dB: a Big Muff is a sustainer and D14 requires it saturated at half sustain, so it follows the pick only over the first part of the knob (0.7 dB measured, commit `0af54cf`) |
| Y3 | The same at gain 36, all three | output moves ≤ 6 dB |
| N1 | NaN and ±Inf runs in a 220 Hz sine, every type, blocks 1, 37, 512 | finite output, bit-identical to the render with the bad samples zeroed |
| N2 | One 3e38 sample, every type | finite output; from 0.9 s on (0.5 s after the sample) within 1e-3 of the clean render |
| A1 | Fuzz even harmonics | H2 at 1 kHz −18 dBFS (gain 18, tone 2449) between −60 and −30 dB; DC ≤ −80 dB re peak at gain 36, tone 500, 100 Hz −6 dBFS |
| A2 | Distortion symmetry | H2 at 1 kHz −18 dBFS, gain 0 / 18 / 36 ≤ −60 dB |

The script also prints, without a bar: the latency table, the alias table (also at 1, 2.5,
3.137, 5, 7.5 kHz, 44.1 and 48 kHz), harmonics, level against gain, pre-clip emphasis and level
curves. **Known limits, not fixed** (commit `0af54cf`): aliasing for input tones above 5 kHz
(Fuzz −43.1 dBc worst; an 8x factor would double the CPU) and the Screamer's 0.84 dB droop at
10 kHz. The D4 to D7 bars hold at the tones they name.

### 4.4 EQ — `EqFx` (`src/dsp/Eq.h/.cpp`)

Study: `docs/research/fx/eq/report.md` R1–R5, corrected by its Verification status and
`verification.md` (V:20 flat cause, V:30 stack taper, V:31 G1 citation, V:37 skip, V:65
smoother sketch, V:67 float-SVF numbers). Reference scripts:
`docs/research/fx/eq/evidence/scripts/design.py` (SVF, Orfanidis, Yeh stack, tilt, graphic)
and `ours.cpp` (zipper model).

`EqFx` also serves the amp block's own tone stack (`ampEq`), always in Classic.

#### 4.4.1 Types (frozen order)

| index | name | what |
|---|---|---|
| 0 | Classic | today's three float cookbook biquads (`ToneStackEQ`), plus knob smoothing and exact flat |
| 1 | Parametric | double-precision TPT state-variable core: low cut, bass shelf, sweepable mid bell with Q, treble shelf, tilt, high cut, level |
| 2 | Stack | interactive passive amp tone stack (Yeh–Smith), four voicings |
| 3 | Graphic | seven octave bells 100 Hz – 6.4 kHz, ±12 dB, constant Q |

Default for new blocks: **Parametric**. Legacy pin: `eq<n>_type` → 0. `eq_voice` list
(frozen): Tweed, Blackface, British, Top Boost.

#### 4.4.2 Parameters

Existing, unchanged: `eq_on`; `eq_bass`, `eq_mid`, `eq_treble` 0…10 step 0.1 default 5
(instances `eq2_*`, `eq3_*`; amp: `amp_eq_on`, `amp_eq_bass/mid/treble`). New (hint 4),
instance 1 shown, instances `eq2_*` / `eq3_*` ("EQ 2 …") identical:

| id | host name | range | step | skew | default | unit | read by |
|---|---|---|---|---|---|---|---|
| `eq_type` | EQ Type | Classic, Parametric, Stack, Graphic | — | — | 1 (Parametric) | — | all |
| `eq_lowcut` | EQ Low Cut | 20 – 400 | 1 | log | 20 (= off) | Hz | Parametric |
| `eq_highcut` | EQ High Cut | 2000 – 20000 | 1 | log | 20000 (= off) | Hz | Parametric |
| `eq_mid_freq` | EQ Mid Freq | 100 – 8000 | 1 | log | 425 | Hz | Parametric |
| `eq_mid_q` | EQ Mid Q | 0.5 – 8 | 0.01 | log | 0.7 | — | Parametric |
| `eq_tilt` | EQ Tilt | −6 – +6 | 0.1 | linear | 0 | dB (+ = brighter) | Parametric |
| `eq_level` | EQ Level | −12 – +12 | 0.1 | linear | 0 | dB | Parametric, Stack, Graphic |
| `eq_voice` | EQ Voice | Tweed, Blackface, British, Top Boost | — | — | 2 (British) | — | Stack |
| `eq_g100` | EQ 100 Hz | −12 – +12 | 0.1 | linear | 0 | dB | Graphic |
| `eq_g200` | EQ 200 Hz | same | | | 0 | dB | Graphic |
| `eq_g400` | EQ 400 Hz | same | | | 0 | dB | Graphic |
| `eq_g800` | EQ 800 Hz | same | | | 0 | dB | Graphic |
| `eq_g1k6` | EQ 1.6 kHz | same | | | 0 | dB | Graphic |
| `eq_g3k2` | EQ 3.2 kHz | same | | | 0 | dB | Graphic |
| `eq_g6k4` | EQ 6.4 kHz | same | | | 0 | dB | Graphic |

`[study: R2/R4/R5 table; eq_q_law (R6, optional) is not built]`. The existing bass/mid/treble
are read by Classic (as today), Parametric (dB per unit as Classic: 4, 3, 2) and Stack (pot
positions). Starting points: none — every type is neutral or "at noon" at the frozen
defaults, so no per-type table is needed.

```cpp
struct EqParams
{
    int type = 1;                 // EqFx::Type
    float bass = 5.0f, mid = 5.0f, treble = 5.0f;   // [0, 10], 5 = flat (Classic/Parametric), noon (Stack)
    float lowCutHz = 20.0f;       // [20, 400]; <= 20.5 = off
    float highCutHz = 20000.0f;   // [2000, 20000]; >= 19999 = off
    float midFreqHz = 425.0f;     // [100, 8000]
    float midQ = 0.7f;            // [0.5, 8]
    float tiltDb = 0.0f;          // [-6, 6]
    float levelDb = 0.0f;         // [-12, 12]
    int voice = 2;                // EqFx::Voice
    float graphicDb[7] {};        // [-12, 12] at 100, 200, 400, 800, 1600, 3200, 6400 Hz
};
```

#### 4.4.3 Classic (type 0) — exact definition and the two additions

Base: `ToneStackEQ` moved verbatim (`FxBlocks.cpp:19-23, 126-193`): three
`ProcessorDuplicator<IIR::Filter<float>, IIR::Coefficients<float>>` in series; bass
low-shelf 150 Hz Q 0.707, gain `decibelsToGain (4 * (v − 5))`; mid peak 425 Hz, Q 1.5 if
gain < 0 dB else 0.7, gain 3 dB/unit; treble high-shelf 1800 Hz Q 0.707, 2 dB/unit;
coefficients from `IIR::ArrayCoefficients<float>::make…` written into `*x.state`.
`design(v)` must be **the exact expressions** of `ToneStackEQ::setParameters` (same float
inputs, same calls), so a static session gets today's coefficients bit-for-bit.

Addition 1 — **knob smoothing** (D7) `[study: R1; ver: V:65]`: each knob value v_t goes
through two cascaded one-poles, a = 1 − exp(−1/(0.005·fs)):
`s1 += a·(v_t − s1); s2 += a·(s1 − s2)` per sample. A band is **moving** whenever
|v_t − s2| > 1e-4 (the study's sketch never set this flag, V:65). While any band moves the
block is processed in 16-sample sub-blocks with `coeffs = design(s2)` at each sub-block
start; when |v_t − s2| ≤ 1e-4 the band snaps `s1 = s2 = v_t`, designs `design(v_t)` once
and stops moving. First `setParameters` after `prepare/reset`: snap (no ramp). **A static
block is processed in a single call**, as today (on Intel `snapToZero` acts per call).
**A smoothing segment snaps only if its target has not moved:** when a ramp's last 16-sample
segment ends, the smoother snaps onto the value the coefficients now hold only if the knob's
target still equals it; if the knob was moved again meanwhile it keeps smoothing toward the new
target. (Before, it snapped to the new target while the filter held the old design and then
jumped: −8.2 dBFS of high-frequency content on a treble move under a low note, now −70 dBFS, row E24.)

Addition 2 — **exact flat** (D2 a) `[study: R2/§8; ver: V:20 cause = arm64 FMA]`: when all
three knob targets are exactly 5.0f and no band is moving, `process` returns without
touching the audio. On entering that state the three filters are `reset()` (their state at
flat is ≈ 0, so nothing audible is lost); on leaving it they start from zero state with
`design(s2)`. Any setting other than exactly 5.0/5.0/5.0 runs the filters exactly as
today (non-flat static renders stay bit-identical; the flat render becomes the input).

Addition 3 — **non-finite input** (all types): `EqFx::process` replaces every NaN and infinity
with 0 before any type or the switcher sees it (finite samples are untouched, so Classic stays
bit-identical). A finite sample near `FLT_MAX` can still overflow Classic's float biquads: if
the block's last output sample is non-finite, the three filters are `reset()` and the non-finite
samples become 0 (`guardOverflow`). The new types read their input through
`sanitisedInput`: |x| ≥ 1e6 or NaN is 0 (§2.2's `fx::sanitise` bound), and every recursive
double state outside 1e−15 … 1e12 (or non-finite) is zeroed once per block. Row E21.

`reset()` clears all four engines and makes the next block start on the requested type
outright (no switch in flight, nothing left to replay; row E23).

The amp block uses the same Classic path (smoothing + exact flat), called on the mono
buffer at `PluginProcessor.cpp:1263-1268`.

#### 4.4.4 Parametric (type 1)

SVF core in double, Simper/Zavalishin trapezoidal `[study: R2, design.py:49-76]`, one state
pair (ic1, ic2) per section per channel:

```
a1 = 1/(1 + g(g + k));  a2 = g a1;  a3 = g a2
v3 = x − ic2;  v1 = a1 ic1 + a2 v3;  v2 = ic2 + a2 ic1 + a3 v3
ic1 = 2 v1 − ic1;  ic2 = 2 v2 − ic2;  y = m0 x + m1 v1 + m2 v2
```
With A = 10^(dB/40), t = tan(π f/fs):

| section | f | Q | gain | g | k | (m0, m1, m2) |
|---|---|---|---|---|---|---|
| low cut | `eq_lowcut` | — | — | t | √2 | (1, −k, −1) |
| bass shelf | 150 Hz | 0.707 | 4·(bass − 5) dB | t/√A | 1/Q | (1, k(A−1), A²−1) |
| mid bell | `eq_mid_freq` | `eq_mid_q` (symmetric) | 3·(mid − 5) dB | t | 1/(Q A) | (1, k(A²−1), 0) |
| treble shelf | 1800 Hz | 0.707 | 2·(treble − 5) dB | t·√A | 1/Q | (A², k(1−A)A, 1−A²) |
| high cut | `eq_highcut` (≤ 0.45 fs) | — | — | t | √2 | (0, 0, 1) |

Order: low cut → bass → mid → treble → tilt → high cut → level `[study: R2]`. Shelf and bell
constants equal Classic's so Parametric at 10/10/10 matches Classic's boost response
(the bell's cut Q is symmetric, Classic's is not — intended).

- **Tilt** (first order, pivot 700 Hz `[study: R2, own constant]`): T = 10^(tilt/20),
  gp = tan(π·700/fs), `y = ((T + gp) x + (gp − T) x1 − (T gp − 1) y1)/(1 + T gp)`; −tilt dB
  at DC, +tilt dB at Nyquist, 0 dB at 700 Hz `[study: design.py:325-335]`.
- **Mid bell: Vicanek-matched at every frequency** (`matchedBell`; replaces the study's
  Orfanidis decramping above 2.5 kHz with a crossfade from 1.5 kHz, which was 1.9 dB off at
  44.1 kHz for wide bells and 13.8 dB at 22.05 kHz, row E20). The analog prototype is
  `(s² + (A/Q) s + 1)/(s² + s/(A Q) + 1)`, A = 10^(|dB|/40), matched after Vicanek, "Matched
  Second Order Digital Filters" (2016), peaking EQ: the poles are the analog poles by impulse
  invariance (ζ = 1/(2AQ), a1, a2 from e^(−ζ w0)); the numerator is the minimum-phase one whose
  magnitude equals the analog curve at DC, at f0 and at a third frequency fm. Vicanek's own
  third conditions (zero slope at f0, or the gain at Nyquist) leave 1.1 and 1.6 dB at Q 0.5 and
  22.05 kHz, so fm sits a fraction c of the way from f0 to Nyquist: c = 0.75 up to f0 = 0.45
  Nyquist, falling to 0.02 at 0.74 Nyquist (slope 2.5 per unit of f0/Nyquist), fm never above
  20 kHz `[own fit: minimax error against the analog bell over Q 0.5–8, 22.05–192 kHz]`. A cut
  is the boost turned upside down (exact for this prototype). The result is converted to SVF
  parameters (`fromBiquad`: g = √(d0/d2), k = d1/(d2 g), m0 = n2/d2, m1 = n1/(d2 g) − m0 k,
  m2 = n0/d0 − m0). **Exactly 0 dB gives m = (1, 0, 0)** with the design's own g, k, so a gain
  ramp away from 0 dB starts from the same poles; a non-finite or degenerate design falls back
  to the plain bell. Worst error 0.27 dB at 44.1 kHz for Q 0.5 to 4 (commit `fe2fee6`; the bar
  is E20). The same bell serves every Graphic band (§4.4.6).
- **Smoothing** `[study: R1/R2]`: every control (bass, mid, treble knob units; mid freq and
  cut frequencies in log2 Hz; mid Q in log2; tilt and level dB) through the two cascaded
  5 ms one-poles; every 16 samples compute the target (g, k, m0, m1, m2) for each section
  from the smoothed values and interpolate them linearly per sample over the next 16
  samples (a1..a3 derived per sample). Two poles are required: a single pole leaves
  −67 dBFS zipper because the output mix applies gain directly `[study: ours_zipper.tsv]`.
- **Exact flat and skipping:** bells and shelves at 0 dB give m = (1, 0, 0) — an exact
  pass-through while running, so they are never skipped. Low cut at ≤ 20.5 Hz, high cut at
  ≥ 19999 Hz, tilt at 0 dB and level at 0 dB are **skipped** once settled,
  and their state is zeroed on entering the skip (re-engagement starts from zero state,
  ramping from neutral) `[choice: V:37 — the study left re-engagement unspecified]`.
  Parametric at its defaults is therefore a bit-exact pass-through.
- **A control within 1e-3 dB of neutral counts as neutral** (`kNeutralDb`): the host hands a
  stepped range's default back with float noise (`eq_level` and the graphic bands read
  1.79e-7 dB, `eq_tilt` 8.94e-8 dB; measured host noise 1.8e-7 dB), which used to run the level
  and tilt stages and make a fresh block a near, not an exact, pass-through (risk D-23, now
  fixed in the engine). 1e-3 dB sits a thousand times above that noise and a hundred times
  below the smallest deliberate move, one 0.1 dB step `[choice]`. It applies to level, tilt,
  every graphic band, and the Parametric bass/mid/treble knobs (a knob whose gain
  4/3/2 dB per unit × (knob − 5) is under 1e-3 dB reads as exactly 5), in Parametric, Graphic
  and Stack level. Classic is exempt (its knobs read back exactly). Rows E22a, E22b.
- **Non-finite input** is silence for every type (§4.4.3, Addition 3).

#### 4.4.5 Stack (type 2)

Yeh–Smith closed-form third-order passive tone stack `[study: R4; Yeh & Smith, DAFx-06]`:
`H(s) = (b1 s + b2 s² + b3 s³)/(1 + a1 s + a2 s² + a3 s³)` with the coefficients of
`design.py:174-192` (`yeh_coeffs`, verified against an independent nodal solve to
0.000000 dB, `design_stack.tsv`) — **transcribe them from that file**, and the bilinear
digitisation of `design.py:234-239` (c = 2·fs). One third-order transposed direct form II
section in double. Knobs: t = treble/10 (linear), m = taper(mid/10), l = taper(bass/10),
taper(x) = (b^x − 1)/(b − 1), b = (1/0.3 − 1)² `[study: design.py:242-246, 261, 264; the
report prose says linear mid, the script and the verifier use the taper on mid too, ver: V:30]`.

| voice | R1 (treble) | R2 (bass) | R3 (mid) | R4 (slope) | C1 | C2 | C3 | makeup | source |
|---|---|---|---|---|---|---|---|---|---|
| 0 Tweed (5F6-A) | 250 k | 1 M | 25 k | 56 k | 250 pF | 20 nF | 20 nF | +8.9 dB | Yeh & Smith |
| 1 Blackface (AB763) | 250 k | 250 k | 10 k | 100 k | 250 pF | 100 nF | 47 nF | +15.1 dB | robrobinette.com (topology approximate, report:428) |
| 2 British (1959/2203) | 220 k | 1 M | 22 k | 33 k | 470 pF | 22 nF | 22 nF | +6.4 dB | caps ToneStack.cc |
| 3 Top Boost (AC30) | 1 M | 1 M | 10 k | 100 k | 50 pF | 22 nF | 22 nF | +13.6 dB | caps ToneStack.cc |

Makeup = minus the mean noon level over 80 Hz – 5 kHz on a log axis `[study: design_stack.tsv §3b]`,
then `eq_level`. Coefficients every 16 samples from the R1-smoothed knobs (no
interpolation). Fallback if the zipper check fails: a three-state trapezoidal state-space
form `[study: report:426, untested]`. Voice changes go through the switcher (key
`type·16 + voice`).

#### 4.4.6 Graphic (type 3)

Seven bells in series at 100, 200, 400, 800, 1600, 3200, 6400 Hz, ±12 dB, constant Q = √2
(half-gain bandwidth one octave) `[study: R5; Q is the study's own]`; every band is the SVF
bell of §4.4.4, Vicanek-matched (`matchedBell`). Smoothing
as Parametric. Bands within 1e-3 dB of 0 are exact (m = (1, 0, 0)); level skipped at 0 dB. No
ripple correction `[study: R5]`.

#### 4.4.7 Latency, CPU

Latency 0. CPU per instance: Classic as today (≤ 0.1 %, ≤ 0.15 % while moving); Parametric
≤ 0.30 %; Stack ≤ 0.15 %; Graphic ≤ 0.30 %.

#### 4.4.8 fxbench keys

`type bass mid treble lowcut highcut mid_freq mid_q tilt level voice g100 g200 g400 g800
g1k6 g3k2 g6k4 on reenable`; ramps with the timeline syntax (§5.3.2), e.g. `bass@0.5..1.0=5..10`.
`on=0` skips the block as the processor does; `reenable` (default 1) makes the adapter call
`reset()` when `on` returns to 1 on a non-Classic type, as the processor does (§2, §6: processor:
in progress); `reenable=0` replays the frozen state the old processor left (row E23).

#### 4.4.9 Acceptance (`check_eq.py`)

Zipper metric (study `ours zip` + `zip_an.py`): bass knob swept 5 → 10 in 0.5 s, 48 kHz,
−12 dBFS sine at 100 Hz; peak spectral content above 1 kHz (short-time FFT, Hann, 2048).

| # | Check | Pass |
|---|---|---|
| E1 | Classic null at non-flat static settings (bass 10/mid 0/treble 7; all 0; 5/5/5.1) | 0 differing samples |
| E2 | Classic and amp EQ at 5/5/5, white noise −12 dBFS, 44.1/96/192 kHz | output == input, 0 differing samples (today 99.7 % differ, −99.5/−89.5/−80.3 dB) |
| E3 | Classic zipper (100 Hz case) | ≤ −88 dBFS (today −62.5; two-pole biquad model −93.4) |
| E4 | Parametric at defaults vs input, 44.1/96/192 kHz | 0 differing samples |
| E5 | Parametric residual at bass 10, 82.4 Hz −20 dBFS, 192 kHz (vs ideal analog shelf) | ≤ −120 dBc (Classic −69.6) |
| E6 | Parametric at bass/mid/treble 10 vs the installed-AU response points (single band at 10; 44.1, 48, 96, 192 kHz) | +19.5 dB @50 Hz, **+10.0** @150 Hz, +15.0 @425 Hz, +5.0 @1.8 kHz, +10.0 @10 kHz, each ± 0.05 dB `[study: tubamp_fr.tsv]`. Restated: the study's +10.1 at 150 Hz was a misreading of the installed-AU table; a +20 dB shelf with its corner at 150 Hz gives half its gain there, and the legacy Classic reads +10.00 too |
| E7 | Parametric zipper (bass and mid-freq sweeps) | ≤ −90 dBFS (model −93.9) |
| E8 | Bell at high frequency (matched bell): mid 8 kHz, Q 1, +12 dB, 44.1 kHz | error vs analog ≤ 0.5 dB 20 Hz–20 kHz (cookbook 2.58); gain at Nyquist ≥ +1.5 dB (model 2.11) |
| E9 | Bell continuity (the 1.5–2.5 kHz blend no longer exists): mid freq swept 1.4 → 2.6 kHz at +12 dB, Q 1.4, 44.1 and 48 kHz | max step of the gain at the bell centre between 10-Hz-apart designs ≤ 0.05 dB; peak frequency strictly rising; Nyquist gain monotonic |
| E10 | Low/high cut | 12 dB/oct Butterworth: −3.0 ± 0.1 dB at the set frequency; at end stops bit-exact pass-through |
| E11 | Tilt ±6 dB | −tilt ± 0.1 dB at 20 Hz, +tilt ± 0.2 dB at 20 kHz (48 kHz), 0 ± 0.05 dB at 700 Hz |
| E12 | Stack vs `design.py` digital reference (same bilinear), 5×5×5 knob grid, every voice | ≤ 0.01 dB 20 Hz–20 kHz |
| E13 | Stack vs analog `yeh_H` | ≤ **1.7** dB to 10 kHz at 44.1 kHz; ≤ **0.35** dB at 96 kHz (above 10 kHz at 44.1 kHz up to 13.5 dB is expected, report gate valid to 10 kHz only). Restated from 1.6 / 0.3: E12 requires the engine to be `design.py`'s own bilinear transform, and that reference is itself 1.65 dB (44.1 kHz) and 0.31 dB (96 kHz) off the analog curve to 10 kHz on this grid, so the old bars sat below what E12 allows |
| E14 | Stack interaction: British, mid 0 → 10 | dip moves 852 → 603 Hz ± 3 % |
| E15 | Stack zipper (bass sweep) | ≤ −85 dBFS |
| E16 | Graphic single band +12 dB (each band) | peak 12.0 ± 0.1 dB at the centre; half-gain width 1.0 ± 0.05 octave; 6.4 kHz band error vs analog at 10/15/20 kHz ≤ 0.3 dB (44.1 kHz) |
| E17 | Graphic all bands 0 | bit-exact pass-through |
| E18 | Type and voice switch on a sustained chord | no step > 3 dB within 1 ms; fade 40 ± 2 ms |
| E19 | Rates | E2, E4, E6, E7 at 44.1, 96, 192 kHz |
| E20 | Parametric bell vs the analog prototype over Q ∈ {0.5, 0.7, 1, 1.4, 2, 3, 4, 8}, f0 100 Hz – 8 kHz (13 values), gains +15 (mid knob 10), −15 (knob 0), +6 (knob 7), −3 (knob 4) dB, 22.05, 44.1, 48, 96, 192 kHz, 20 Hz to min(20 kHz, 0.45 fs) | at each rate: worst error for Q 0.5 to 4 < 1 dB; for Q 8 no worse than the old design (0.415 / 0.067 / 0.055 / 0.013 / 0.003 dB at 22.05 / 44.1 / 48 / 96 / 192 kHz). Verification had found 1.91 dB at Q 0.5, 8 kHz, 44.1 kHz, and 13.5 dB at 22.05 kHz |
| E21 | Non-finite input: a 10-sample run of NaN, +Inf or −Inf in a 220 Hz sine, and one finite 3e38 sample; Classic 10/5/7 and flat, Parametric all sections and defaults, Graphic and Graphic flat, Stack in four voices; blocks 1, 37, 512; 1 and 2 channels | never a non-finite output sample; NaN and Inf renders equal, bit for bit, the render with zeros in their place; after 3e38, within 1e-4 of the clean render 100 ms later. (One NaN had latched Classic, Parametric, Stack and non-flat Graphic for good) |
| E22a | Neutral controls plus host float noise (tilt 8.94e-8, level ±1.79e-7, bass 5.00000048, mid 4.99999952, treble 5.00000048, lowcut 20.0000019 Hz, highcut 19999.998 Hz, every graphic band 1.79e-7; fresh, and after moving away and back), at 44.1, 96, 192 kHz, 1 and 2 channels; Stack level at noise against level 0 | output equals input bit for bit; Stack level 1.79e-7 equals level 0 bit for bit |
| E22b | Smallest step: 0.1 dB on level, tilt, each graphic band; 0.1 knob on bass; 48 kHz | each within 0.005 dB of the step (level ±0.1, band ±0.1 at its centre, tilt ∓0.1 at 20 Hz, bass 5.1 gives +0.4 dB at 20 Hz) |
| E23 | Re-enable after bypass: process, 1 s not called, `reset()`, process (fxbench `on=0` then `on=1`), +12 dB low boosts under a 98 Hz −12 dBFS note; blocks 1, 37, 512 | silent input at the return gives silence (the frozen state used to replay); held note: first sample within 0.1 of the dry sample (re peak), no step larger than the settled output's own, no overshoot above +0.5 dB in the first 100 ms |
| E24 | Classic retarget: a knob moved again in a ramp's last segment (treble and mid, blocks 1, 37, 77, 100) | click at the retarget ≤ the click of the same move from rest + 3 dB |

### 4.5 Chorus — `ChorusFx` (`src/dsp/Chorus.h/.cpp`)

Study: `docs/research/fx/chorus/report.md` R1–R6, corrected by its Verification status and
`verification.md` (V:42 Wide clamp, V:46 CE rate, V:53 Dimension target, V:68 mix law, V:74
depth law provenance). Runs when `mod_type` = Chorus; the integrator's `Modulation`
dispatcher (§6.5) calls it.

#### 4.5.1 Modes (frozen order)

| index | name | what |
|---|---|---|
| 0 | Classic | today's `juce::dsp::Chorus` (one sine voice shared by both channels), bit-for-bit |
| 1 | Wide | four voices in two quadrature pairs, triangle LFO, depth in cents |
| 2 | CE | one bucket-brigade-style voice per channel, fixed 2.7 ms sweep, bent triangle |
| 3 | Dimension | two antiphase lines fed from the mono sum, slow triangle |
| 4 | Tri | three lines at 120°, slow sine + fixed 6 Hz shimmer |

Default for new blocks: **Wide**. Legacy pin: `mod<n>_chorus_mode` → 0.

#### 4.5.2 Parameters

Existing (read as stated): `mod_type` (Chorus/Phaser/Tremolo), `mod_rate` log 0.05–10 Hz
default 1, `mod_depth` 0–1 default 0.4, `mod_mix` 0–1 default 0.35. New mod parameters
(hint 4; instance 1 shown, `mod2_*`/`mod3_*`, "Mod 2 …" identical) — this table is the
same in §4.5, §4.6 and §4.7:

| id | host name | range | step | default | unit | chorus reads |
|---|---|---|---|---|---|---|
| `mod_chorus_mode` | Mod Chorus Mode | Classic, Wide, CE, Dimension, Tri | — | 1 (Wide) | — | yes |
| `mod_phaser_model` | Mod Phaser Model | Classic, Phase 90, Small Stone | — | 1 (Phase 90) | — | no |
| `mod_trem_voice` | Mod Trem Voice | Classic, Smooth, Optical, Bias, Harmonic | — | 1 (Smooth) | — | no |
| `mod_sync` | Mod Sync | Off, 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2, 1/2., 1/1, 2/1, 4/1 | — | 0 (Off) | LFO cycle length | non-Classic |
| `mod_width` | Mod Width | 0 – 1 | 0.001 | 1.0 | — | non-Classic |
| `mod_tone` | Mod Tone | 0 – 1 | 0.001 | 0.5 | — | non-Classic |
| `mod_shape` | Mod Shape | 0 – 1 | 0.001 | 0.5 | — | no |
| `mod_wave` | Mod Wave | 0 – 1 | 0.001 | 0.0 | — | no |
| `mod_color` | Mod Color | off/on | — | on | — | no |

What each mode reads (UI dims what is not read):

| mode | rate | depth | mix | sync | width | tone |
|---|---|---|---|---|---|---|
| Classic | Hz (clamped 0.01–100, as today) | JUCE depth | JUCE linear mix | – | – | – |
| Wide | Hz | cents law | mix law | ✓ | ✓ | ✓ |
| CE | Hz (swing scaled above 2.86 Hz) | sweep fraction | mix law | ✓ | ✓ | ✓ |
| Dimension | Hz | swing fraction | mix law | ✓ | ✓ | ✓ |
| Tri | Hz (slow LFO) | cents law | mix law | ✓ | ✓ | ✓ |

Per-mode starting points (`Chorus.h`: `kChorusRateDefault`, `kChorusDepthDefault`,
`kChorusMixDefault`; width not moved):

| mode | rate Hz | depth | mix | source |
|---|---|---|---|---|
| Classic | 1.0 | 0.4 | 0.35 | frozen defaults |
| Wide | 1.0 | 0.4 | 0.35 | = frozen defaults (§2.7.4): 4.8 cents |
| CE | 0.8 | 0.7 | 0.5 | inside the pedal's 0.43–2.86 Hz `[study: brigade_chorus.tsv]`; CE-1 sums dry and wet equally `[study: R2]` |
| Dimension | 0.25 | 0.8 | 0.5 | the first Dimension preset point `[study: R3]` |
| Tri | 0.6 | 0.5 | 0.5 | slow ensemble `[choice]` |

```cpp
struct ChorusParams
{
    int mode = 1;              // ChorusFx::Mode
    float rateHz = 1.0f;       // resolved by the dispatcher: mod_rate, or the synced rate [0.01, 20]
    float depth = 0.4f;        // [0, 1]
    float mix = 0.35f;         // [0, 1]
    float width = 1.0f;        // [0, 1]
    float tone = 0.5f;         // [0, 1]
    double syncPhase = -1.0;   // >= 0: LFO phase in cycles at this block's first sample (song position lock)
};
```
When `syncPhase ≥ 0` the engine sets its master LFO phase to it at block start (voices keep
their offsets) **through a dip** (§4.5.4 "Transitions"), not by sliding. Classic ignores
rate sync and `syncPhase` (the dispatcher passes `mod_rate` to Classic, §6.5).
`process()` treats a non-finite input sample as 0 and writes the 0 back (all modes).
`reset()` clears every line, starts every smoother at its target, and in modes 1–4 passes
the dry at once and fades the wet in as the lines refill; the processor calls it when a
bypassed Wide/CE/Dimension/Tri block is switched back on (processor: in progress).

#### 4.5.3 Classic — exact definition

From `Modulation` (`FxBlocks.cpp:349-415`): `juce::dsp::Chorus<float>` prepared with the
spec, `setCentreDelay (7.0f)`, `setFeedback (0.0f)`; per block `rate = jlimit (0.01, 100,
rateHz)`, `depth = jlimit (0, 1, …)`, `mix = jlimit (0, 1, …)`, `setRate/setDepth/setMix`,
`process` (ProcessContextReplacing). `reset()` = `chorus.reset()`.

#### 4.5.4 Shared machinery (modes 1–4)

```
per voice set:  xf = LP2_bw(HP1(x, f_hp), f_lp * 2^(2*(tone - 0.5)))   // TPT SVF LP (k = sqrt 2), clamp 0.45 fs
                write xf into the voice's line (fx::DelayBuffer, read-only use of FxPrimitives.h)
per voice:      u   = shape(frac(p + offset))                          // unipolar 0..1
                u_s = onepole(u, fc = max(8 Hz, 20 * rate))           // corner rounding [study: shared design]
                d   = d0 + D * u_s                                     // seconds, per sample, never stepped
                y   = line.readHermite(d * fs)                         // 4-point Catmull-Rom [study]
triangle:  u = 1 - |2 frac(p) - 1|     (u = 0 at p = 0; peak slope 2 f D)
sine:      u = 0.5 - 0.5 cos(2 pi p)   (peak slope pi f D)
cents law: c = Cmax * depth^2;  k = 2^(c/1200) - 1
           triangle D = k/(2 f), sine D = k/(pi f);  D = min(D, D_max)
mix law:   dry = min(1, 2 - 2m),  wet = min(1, 2m)                    [choice: dry at unity to the midpoint]
width:     M = (wL + wR)/2, S = (wL - wR)/2, wL' = M + width S, wR' = M - width S
out_ch  =  dry * x_ch + wet * w'_ch
```
- The mix law is tubamp's own: Choral, which the study credited, measures dry = 1 − mix/2
  `[ver: V:68]`.
- `Cmax`: Wide 30 cents — the measured maximum of the CE-1 model (29.3 cents at 2.859 Hz)
  `[fit: chorus/evidence/brigade_chorus.tsv]`; square law `[choice: fine control at low
  depth]`. (V:74 notes the study took 30·x² from Choral's code; this spec grounds the
  ceiling in the measured CE-1 instead and owns the taper.)
- **Low-rate limit, written into the spec** `[ver: V:42]`: the constant-cents law needs a
  swing that grows as 1/f; `D_max` caps it, so below `f_min = k/(2·D_max)` (triangle) the
  detune falls in proportion to the rate. This spec raises `D_max` from the study's 8 ms to
  **20 ms** `[choice]`: at full depth the 30-cent law then holds down to 0.44 Hz (the study's
  8 ms held only above 1.09 Hz), at depth 0.5 (7.5 cents) down to 0.11 Hz; below, the
  detune is 1200·log2(1 + 2 f D_max). Line length: d0 + D_max + 4 samples ≥ 34 ms at the
  prepared rate.
- Smoothing: rate, depth, mix, width, tone through 20 ms one-poles per sample (`kSmoothSeconds`);
  D recomputed every 16 samples from the smoothed values (linear in between); filters'
  coefficients every 16 samples (TPT is modulation-safe).
- **Transitions keep the pitch** (`Chorus.cpp`: `kPitchAllowance`, `kDipSeconds`,
  `kSettleSeconds`, `kQuietSeconds`, `kSyncSlackCycles`). A voice's pitch is the slope of its
  delay, so while settings move every voice delay is **slope-limited to the largest steady
  peak slope of the settings involved plus 4 cents** (`2^(4/1200) − 1`). A change that this
  limit cannot finish within 200 ms (`kSettleSeconds`) of the knob stopping is a step: the
  wet fades out over 10 ms, every delay moves to its new law in the silence, and the wet fades
  back in over 10 ms (`kDipSeconds` per side). A knob counts as stopped after 80 ms without
  a move (`kQuietSeconds`), so a ramp is followed, not dipped. A sync phase that differs from
  the running LFO by more than 1e-3 cycles (`kSyncSlackCycles`: transport start, locate,
  loop, note-value change) is honoured through the same dip, never by a slide.
- Stereo/mono: Wide and CE run one voice set per input channel (the right voices read the
  right input's line); Dimension and Tri feed their lines from the mono sum (L + R)/2. A
  1-channel block computes both outputs and writes their mean, except **Dimension: a mono
  instance plays line A alone** (the stereo left channel). The mean of its two antiphase
  lines would have no detune; line A alone keeps the 3.5 cents of the stereo instance.
- LFO start after reset: master phase p = 0 (u = 0, shortest delay), offsets as listed.
  `reset()` clears the lines (§4.5.2); the wet fades in as they refill, the dry is never
  interrupted.
- **Mode switches do not use `fx::EngineSwitcher` (§2.4).** The outgoing mode keeps sounding
  while the incoming mode's line fills (until it has been written for its longest delay,
  `reachSamples`, plus 4 samples). The two wets are then crossfaded over 20 ms (`kSwitchSeconds`), scaled by their measured short-term powers
  (two cascaded one-poles of 4 ms, run for 10 ms before the fade starts:
  `kSwitchPowerSeconds`, `kSwitchMeasureSeconds`) so that the wet power moves straight from
  one mode's to the other's whatever their correlation (a chord gives −0.3 to +0.45 between
  modes; a plain equal-power fade would dip or bump by up to 1.6 dB). Measured: 0.69 dB
  level bump, click −102 dBFS.

#### 4.5.5 Mode algorithms

- **Wide (1)** `[study: R1]`: pair 1 d0 = 6 ms, rate f, voices L (offset 0) and R (offset
  0.25 = 90°); pair 2 d0 = 10 ms, rate f·√2, offsets 0.5 (L) and 0.75 (R)
  `[choice: start the pairs half a cycle apart]`, gain 0.7. Triangle, cents law with each
  pair's own rate, D_max 20 ms. `wL = (v1L + 0.7 v2L)·0.819`, likewise R
  (0.819 = 1/√(1 + 0.7²), power-normalises two uncorrelated voices `[choice]`).
  HP 60 Hz, LP 9 kHz.
- **CE (2)** `[study: R2; ver: V:46]`: one voice per channel, R offset 0.25. Bent triangle
  `u' = u·(1 + b(u − 1))`, b = 0.1 (peak slope ×1.1) `[fit: brigade_chorus.tsv — 29.3 cents
  at 2.859 Hz and 4.5 at 0.428 Hz with a 2.7 ms sweep; the study's b = 0.25 had no fit (V:48)
  and would give 33 cents]`. `d = 2.95 ms + 2.7 ms·depth·s(f)·(u' − 0.5)`,
  `s(f) = min(1, 2.86/f)`: **above the pedal's top rate of 2.86 Hz the swing shrinks as
  1/f**, so the detune never exceeds its 2.86 Hz value (≈ 29 cents) — the fixed sweep gave
  91 cents at 10 Hz `[ver: V:46; choice of fix: keeps the knob live to 10 Hz]`. Wet filter:
  first-order HP 130 Hz; two-pole LP 4.6 kHz plus one-pole LP 9 kHz (tone scales both LP
  corners) `[fit: study R2 to brigade_fr.tsv; ~3 dB thin at 45 Hz, V:47]`.
- **Dimension (3)** `[study: R3; ver: V:53]` (mono instance: line A alone, §4.5.4): lines A and B from the mono sum; one triangle,
  `uB = 1 − uA` (antiphase, measured in Choral's Dimension mode, R − L = 179.9°);
  `d = 8.5 ms + 2.5 ms·depth·(2u − 1)`; wL = A, wR = B. HP 80 Hz `[study: Anwander SDD-320]`,
  LP 10 kHz `[study: tagged guess]`. The literature's ±2.0/±2.5/±1.5 ms at 0.25/0.25/0.5 Hz
  give **3.5 / 4.3 / 5.2 cents for a triangle** (the study's 5–8 cents used the sine formula
  for a triangle, so its first preset failed its own band, V:53); acceptance below uses the
  triangle values.
- **Tri (4)** `[study: R4]`: three lines from the mono sum, d0 = 7 ms; slow sine at rate f,
  offsets 0, 1/3, 2/3, cents law with Cmax = 20 cents (sine form, D_max 20 ms); plus a fast
  sine fixed at 6 Hz with the same offsets and a fixed 3-cent swing
  (D_fast = (2^(3/1200) − 1)/(π·6)); `d = d0 + D·u_slow + D_fast·u_fast`;
  wL = 0.7(v1 + v2), wR = 0.7(v2 + v3). HP 80 Hz, LP 6 kHz.

#### 4.5.6 Latency, CPU, fxbench

Latency 0. CPU per instance: Classic as today; Wide ≤ 0.5 %; CE ≤ 0.25 %; Dimension ≤ 0.25 %;
Tri ≤ 0.4 %; during a mode switch (two modes running) the two modes' budgets added (CH23).
fxbench keys: `mode rate depth mix width tone sync_phase on`; test hooks `reset` (any change
of its value calls `reset()` at that block, as the processor does when the block comes back)
and `solo=<voice>` routes one voice to both outputs at mix 1 (implemented as
`ChorusFx::setDebugSolo (int)`, a no-op when −1; allowed because it costs one branch per
block).

#### 4.5.7 Acceptance (`check_chorus.py`)

Pitch is measured with a 1 kHz sine probe (not the author's click method, which reads up to
1.2 cents high, V:12) through one soloed voice at mix 1; instantaneous frequency from the
analytic signal (numpy FFT Hilbert), 95th percentile of |deviation| in cents over ≥ 3 LFO
cycles. Tolerances from the verifier: pitch 1.5 cents, phase 7°, level 0.5 dB (V:8).
Model values in brackets are closed-form.

| # | Check | Pass |
|---|---|---|
| CH1 | Classic null (§2.11) | 0 differing samples |
| CH2 | Wide pitch, depth 0.5, rates 0.25 / 1 / 3 Hz (pair 1) | 7.5 ± 1.5 cents each [7.5 / 7.5 / 7.5] |
| CH3 | Wide pitch, depth 1 | 30 ± 2 cents at 0.5, 1, 3 Hz; at 0.25 Hz 17.2 ± 1.5 cents [1200·log2(1 + 2·0.25·0.020)] — the documented low-rate limit |
| CH4 | Wide stereo | R − L LFO phase 90 ± 10°; band-noise correlation < 0.93 at width 1, ≥ 0.9995 at width 0; mono-sum loss ((L + R)/2 against one channel) not above the decorrelated-wet prediction, at mix 0.5 and 0.35 |
| CH5 | CE sweep, depth 1, rate ≤ 2.86 Hz | 1.6 – 4.3 ms ± 0.15 ms; delay crest 1.6 – 1.8 |
| CH6 | CE pitch | 4 – 6 cents at 0.43 Hz [4.4]; 25 – 31 cents at 2.86 Hz [29.2]; ≤ 31 cents at every rate up to 20 Hz [≤ 29.2] |
| CH7 | CE wet response vs `brigade_fr.tsv` after level alignment | within 2 dB, 100 Hz – 8 kHz |
| CH8 | Dimension | R − L 180 ± 10°; 3.0 – 4.0 cents at 0.25 Hz / depth 0.8 [3.46]; 4.7 – 5.7 cents at 0.5 Hz / depth 0.6 [5.19]; mono-sum loss not above the prediction |
| CH9 | Tri | three wet taps visible in the impulse cross-correlation; slow components 120 ± 15° apart; a 6 Hz component of 2 – 4 cents |
| CH10 | Tone | wet −3 dB point moves one octave down at tone 0 and up at tone 1 (± 0.1 octave) |
| CH11 | Clicks | mode switch, rate and depth steps during a 220 Hz −12 dBFS sine: after a 2 kHz high-pass the output never exceeds −60 dBFS `[study: R6]` |
| CH12 | Interpolation | 10 kHz sine through Wide at depth 0.3, tone 1: amplitude ripple ≤ 0.7 dB (Catmull-Rom predicts 0.53, linear 2.0) |
| CH13 | Sync | `sync_phase` given per block at 120 BPM, 1/4: LFO period 0.5 s ± 0.1 %, phase 0 lands on the beat ± 1 ms |
| CH14 | Rates | CH2, CH5, CH6, CH8 at 44.1 and 96 kHz |
| CH8-rates | Dimension detune at depth 1 | at each of 1, 2, 3, 5, 10, 16, 20 Hz at most the 0.5 Hz value + 0.2 cents |
| CH4/8-width | Mono-sum remedy | Wide, CE, Dimension, Tri at mix 0.5, width 0.8 / 0.5 / 0: (L + R)/2 within 1 dB of one channel |
| CH-mono | Mono instance | output equals the stereo mean for the same mono input within 1e-6 (Wide, CE, Tri), and the wet is above −20 dB of the input; **Dimension: equals the stereo left channel** within 1e-6 (the mean of two antiphase lines has no detune) |
| CH15 | Transients: rate, depth, width, tone steps and ramps, 16 random steps 25 / 60 ms apart; 997.3 and 3301.7 Hz carriers, every soloed voice, blocks of 64 | where a voice is sounding its \|deviation\| stays within 5 cents of the largest steady peak of the settings involved; back inside the new settings' peak + 5 cents 250 ms after the change; dips reported (≤ 10 ms per side by design) |
| CH15-rates | CH15's hardest four cases at 22.05 and 192 kHz, blocks 1 / 512 / 4096, mono and stereo | same bar as CH15 |
| CH16 | Sync jumps: transport start, locate, loop, note-value change | pitch bound and settle as CH15; LFO within 2 % of a cycle of the requested phase afterwards; 220 Hz −12 dBFS at mix 0.5 and 1: 2 kHz high-passed peak ≤ −60 dBFS |
| CH17 | Bypass then `reset()` (block off 1 s, back on) | silent input: output ≤ −120 dBFS (no replay of the old lines); 220 Hz −12 dBFS at mix 0.35 and 0.5, mono and stereo: restart peak after 2 kHz high-pass ≤ −60 dBFS |
| CH18 | Mode switch between Wide, CE, Dimension, Tri (every pair, mix 0.35 / 0.5 / 1) | under a chord, the 20 ms level averaged over six switches each way stays within 1.5 dB of the span between the levels before and after; a 220 Hz sine shows no click (2 kHz high-pass ≤ −60 dBFS) |
| CH19 | Mono instance is still a chorus | pitch deviation (\|IF\| p95, mix 1; 220.5, 997.3, 3301.7 Hz) at least half the stereo left channel's, and the comb swing on pink noise at least half as deep |
| CH20 | Non-finite input (NaN, +Inf, −Inf sample, and a run of 100 NaNs), every mode, mono and stereo, blocks 37 and 512 | output bit-identical to the same input with zero samples; one 1e30 sample gives finite output back on the clean render 0.2 s later within −100 dB |
| CH21 | Onset after `reset()` (sustained 220 Hz −12 dBFS) | 2 kHz high-passed peak ≤ −60 dBFS at mix 1, and at mix 0.5 under the dispatcher's 20 ms linear fade |
| CH22 | `--rt-check` and `--reset-repeat` through switches, dips, sync jumps, bypass and reset, 22.05 – 192 kHz, blocks 1 / 37 / 512 / 4096, mono and stereo | exit 0, finite output |
| CH23 | CPU while two modes alternate every 60 ms (Wide+Tri, CE+Dimension, Wide+CE, Dimension+Tri, Wide+Classic) | the two modes' §4.5.6 budgets added (Classic: 1.2 × the legacy figure) |

Check script at commit `08da770`: 71 checks, 0 FAIL.

### 4.6 Phaser — `PhaserFx` (`src/dsp/Phaser.h/.cpp`)

**Build spec: `docs/research/fx/phaser/voicings.md`, ported exactly; numeric reference
`docs/research/fx/phaser/prototype/phaser_ref.py`** (where the two disagree the prototype
wins, voicings.md:7-9). Confirmed by `verification-refit.md`; the round-3 numbers were
re-tracked by the coordinator only (report.md:9). Ignore `report.md` §7 code and proof bounds
(stale: sqrt law, κ 0.36, 6 Hz gate, feedback 0.80).

#### 4.6.1 Models (frozen order)

| index | name | `mod_color` off | `mod_color` on |
|---|---|---|---|
| 0 | Classic | today's `juce::dsp::Phaser` (6 stages, sine), bit-for-bit (color ignored) | same |
| 1 | Phase 90 | script logo: four stages, no feedback | block logo: stage-2 feedback β with lag L |
| 2 | Small Stone | Color off | Color on: lower sweep, shaped wet input, delay-free feedback |

Default for new blocks: **Phase 90** with `mod_color` **on** (block). Legacy pin:
`mod<n>_phaser_model` → 0.

#### 4.6.2 Parameters

The shared mod table (identical to §4.5.2; read by the phaser where marked):

| id | host name | range | step | default | phaser reads |
|---|---|---|---|---|---|
| `mod_chorus_mode` | Mod Chorus Mode | Classic, Wide, CE, Dimension, Tri | — | 1 | no |
| `mod_phaser_model` | Mod Phaser Model | Classic, Phase 90, Small Stone | — | 1 (Phase 90) | yes |
| `mod_trem_voice` | Mod Trem Voice | Classic, Smooth, Optical, Bias, Harmonic | — | 1 | no |
| `mod_sync` | Mod Sync | Off, 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2, 1/2., 1/1, 2/1, 4/1 | — | 0 | Phase 90, Small Stone |
| `mod_width` | Mod Width | 0 – 1 | 0.001 | 1.0 | no (one LFO for both channels, voicings.md) |
| `mod_tone` | Mod Tone | 0 – 1 | 0.001 | 0.5 | no |
| `mod_shape` | Mod Shape | 0 – 1 | 0.001 | 0.5 | no |
| `mod_wave` | Mod Wave | 0 – 1 | 0.001 | 0.0 | no |
| `mod_color` | Mod Color | off/on | — | on | Phase 90, Small Stone |

Existing: `mod_rate` (Hz, literal, both voicings: the Speed / Rate knob); `mod_depth` and
`mod_mix` are **ignored** by Phase 90 and Small Stone, which run at the stock depth and mix
the fits were made at (prototype `depth = 0.5`, `mix = 0.5`, i.e. scale factors 1) — the UI
dims both `[study: voicings.md:288-298, report R2]`. Classic reads rate, depth, mix as today.
Default `mod_color = on` `[choice: both "on" positions are the better-validated fits —
Phase 90 block 0.38–0.72 % rms trajectory error, Small Stone Color-on 1.1 % against 1.9 %
off; the script Phase 90 has no measured reference, report.md:10]`.

Starting points (`Phaser.h`, `kPhaserRateDefault`): Classic 1.0 Hz; Phase 90 1.07 Hz
(GR6 noon, 1.071); Small Stone 0.32 Hz (GR6 noon, 0.316) `[study: voicings.md]`.

```cpp
struct PhaserParams
{
    int model = 1;             // PhaserFx::Model
    float rateHz = 1.0f;       // resolved by the dispatcher [0.01, 20]
    float depth = 0.4f;        // Classic only
    float mix = 0.35f;         // Classic only
    bool color = true;         // Phase 90: block; Small Stone: Color
    double syncPhase = -1.0;   // >= 0: raw LFO phase at block start (song-position lock)
};
```

#### 4.6.3 Classic — exact definition

From `Modulation`: `juce::dsp::Phaser<float>` prepared with the spec,
`setCentreFrequency (600.0f)`, `setFeedback (0.0f)`; per block rate/depth/mix clamped as
the chorus (`FxBlocks.cpp:371-405`), `setRate/setDepth/setMix`, `process`.

#### 4.6.4 Phase 90 and Small Stone — algorithm

Implement voicings.md §"Shared building block", §"Voicing 1: Phase 90" and §"Voicing 2:
Small Stone" line by line, matching `phaser_ref.py` (`phase90_break_hz`, `run_phase90`,
`smallstone_break_hz`, `run_smallstone`, `rc_triangle`, `lfo`). Summary of the constants
(all from the prototype, tags as there):

- **Stage:** TPT one-pole, g = tan(π fc/fs), G = g/(1 + g), lp = v + s with v = (x − s)G,
  s ← lp + v; allpass P = 2·lp − x; Phase 90 op-amp stage A = −P. The voicings are analog
  transfer functions (Laplace variable s, one-pole breaks in Hz) and the fits are to the analog
  response. **The four stage coefficients are not one shared `tan` per sample.** `prepare()`
  solves, at 257 sweep positions (`kTableN` = 256 intervals, the position w from −1 to +1), the
  TPT coefficients whose digital notches land on the analog ones, and stores them in four
  tables (Phase 90 script, Phase 90 block, Small Stone off, Small Stone on; five columns, the
  fifth the Phase 90 lag). The audio thread only interpolates linearly in the table
  (`lookup`); there is no `tan` in `process()`. The solve (prototype `stage_g`):
  `pairCoefficients` gives two coefficients g_a < g_b, used for stages (a, b, a, b), whose
  chain has the analog phase at two matched frequencies (closed form; the higher one capped at
  0.25·fs, `kMatchCap`). Script and Small Stone off: pair at tan(π/8)·fc and fc/tan(π/8).
  Small Stone on: the same pair times one factor that puts notch 1 of the whole loop on the
  analog notch (Newton, `kNotchEps` = 1e-4). Phase 90 block: stage 2 and the lag at the
  analog notch 1, stages 3 and 4 a pair matched at notches 1 and 2, then three unknowns
  solved by Newton so notch 1, notch 2 and notch-1 depth match. Result: the notches, and so
  the sweep, are the same at every sample rate (§4.6.6 P9). Feedback loops are solved in
  closed form (no unit delay).
- **LFO:** `rc_triangle` halves `s(w) = (1 − e^{−κw})/(1 − e^{−κ})`; phase 0 = sweep bottom
  (output −1). Free-running phase accumulates `rate/fs` per sample in double; with
  `syncPhase ≥ 0` the phase is set at block start. A song-position jump is still honoured,
  but the sweep does not step: the difference between the old and the new sweep position
  (above 1e-6, `kGlideThreshold`) is faded out over 10 ms (`kGlideSeconds`) with a raised
  cosine, so the break frequency glides to the new phase's position. The first block after
  `prepare()` or `reset()` sets its phase with no glide.
- **Phase 90** (`P90_*`): C = 47 nF, R_fix = 22 kΩ `[circuit]`; leg R(u) = (RS^m + R_ch^m)^{1/m},
  R_ch = R1·e^{−γu}, RS = 916.8 Ω, R1 = 1777.7 Ω, γ = 0.8952, m = 3.876 `[fit]`;
  fc = (1/R_fix + 1/R(u))/(2πC); κ = −0.007; gate one-pole 3.542 Hz on the LFO before the
  law, k = 1 − exp(−2π·3.542/fs), state starting at the first LFO value. Script: wet = A⁴.
  Block: β = 0.3957, lag one-pole 14746 Hz (analog) in the feedback path, the per-sample loop of
  voicings.md. Mix: out = M·(0.5·x + 0.5·wet), M the fixed make-up below.
- **Small Stone** (`SS_*`): κ = 0.702; fc = √(f_lo f_hi)·(f_hi/f_lo)^{v/2}. Color off:
  f_lo 347.7, f_hi 8931.5 Hz, no feedback, dry 0.75, wet 0.50. Color on: f_lo 99.4,
  f_hi 3068.3 Hz, feedback c = 0.7548 around all four stages through F(s), wet input through
  Ha(s), poles f_p1 49.8, f_p2 267.9 Hz, zeros f_zf 12.0, f_za 12.7, f_zb 862.1 Hz,
  dry 0.1318, wet 0.4395; TPT discretisation of voicings.md (Ha and F as two first-order
  sections each; the chain's p⁴ is now the product of the four per-stage factors
  `p[0]·p[1]·p[2]·p[3]`, and `y4 = (p⁴(xs + c·bF) + K)/(1 − c·aF·p⁴)`, divisor ≥ 0.24).
- **Make-up (fixed per voicing, `[choice]` in voicings.md, "Level"):** Phase 90 script
  ×1.365 (+2.70 dB), block ×1.189 (+1.50 dB), Small Stone off ×1.059 (+0.50 dB), Small Stone on
  ×1.0 (0 dB); `kP90ScriptMakeup`, `kP90BlockMakeup` and `StoneVoicing::makeup` in `Phaser.cpp`.
  The Small Stone's dry and wet constants above are multiplied by it. Classic is unchanged.
  Switching Classic to Phase 90 now gains about 1.5 dB (voicings.md).
- No oversampling, no saturation, no output filter `[study: voicings.md]`. One LFO for both
  channels; per-channel stage states. Non-finite input samples are replaced by 0 before the
  engine sees them (Classic included; finite input is untouched, so the Classic null holds).

**Decision — oversampling (Small Stone Color, and any voicing): not done.** Internal 2x
needs up/down-sampling filters, i.e. latency, and D6 allows latency only for drive. The
notch-matched discretisation above removes what the 2x would have bought for the notches
(the fits are to the analog response and the notches land on it at 44.1 to 192 kHz). What it
cannot fix is the magnitude response away from the notches: four first-order sections at
44.1 or 48 kHz cannot follow an analog chain's phase near 10 kHz, so the frozen responses
between 48 and 192 kHz differ on the slopes above 3 kHz (§4.6.6 P9, "frozen response",
failing; §8 D-8). The Color-on resonance at the sweep top also sits +2.7 % high at 48 kHz
(3288 against 3203 Hz analog) for the same reason [voicings.md]. The remaining gaps against
Guitar Rig 6 (Small Stone Color off: notch-1 ends 2.2 to 3.1 % low, notch 2 at the bottom
about 4.4 % high; Phase 90 block notch 2, not a target, 1.6 to 3.3 % high at the sweep top;
Color on: notch-1 minimum +2.8 % at 1.78 Hz, inside the tracker's own +6.2 % bias) are small
against the 0.41 to 1.25 % rms trajectory fits (`compare.txt`).

#### 4.6.5 Latency, CPU, fxbench

Latency 0 (`getLatencySamples()` returns 0). CPU per instance: Classic as today; Phase 90
≤ 0.4 %; Small Stone ≤ 0.5 % (`BUDGETS` in `check_phaser.py`; per sample one table
interpolation of four or five coefficients plus up to eight one-poles, no `tan`; the four
tables, 257 entries each, are built in `prepare()`, which
takes under 10 ms [voicings.md]). fxbench keys:
`model rate depth mix color sync_phase on reset_on_enable freeze_u`; the last two are test
hooks (`freeze_u` calls `PhaserFx::freezeSweep`, a test hook that holds the sweep position in
[−1, 1], −1 the bottom; NaN releases it).

`reset()` clears every state: filters, feedback, the LFO (phase 0 = sweep bottom), the gate
smoothing, the sync glide, the model switcher and the parameter smoothers, so the next block
is a fresh engine's. The processor calls it when a Phase 90 / Small Stone block is switched
back on (§2.6, §6.5; processor: in progress). Non-finite input samples are replaced by 0 at the
top of `process()`, for every model.

#### 4.6.6 Acceptance (`check_phaser.py`)

| # | Check | Pass |
|---|---|---|
| P1 | Classic null (§2.11) | 0 differing samples |
| P2 | Parity with `phaser_ref.render()` (imported from the prototype): 4 s of pink noise −18 dBFS plus an impulse train, each voicing × color, rates 0.143, 1.071, 3.72, 9.76 Hz (Phase 90) and 0.1585, 0.316, 1.778 Hz (Small Stone), at 44.1, 48, 88.2, 96, 192 kHz (`--quick`: 48 kHz only) | rms(C++ − ref)/rms(ref) ≤ −80 dB; max abs difference ≤ 1e-4 (reported per voicing: Phase 90 script, Phase 90 block, Small Stone off, Small Stone on) |
| P3 | Frozen-fc table: the engine at a fixed sweep position (adapter key `freeze_u` = −1 and +1, `PhaserFx::freezeSweep`) reproduces `evidence/fit/spec_check.txt` (notch 1/2 frequencies and levels, peak, DC at 44.1, 48, 88.2, 96, 192 kHz); the impulse response's last eighth holds under 1e-12 of its energy | frequencies ± 0.1 %, levels ± 0.05 dB (e.g. block 48 kHz: 419.4 / 1934 Hz, −29.18 / −15.27 dB at the bottom; 1589.9 / 7481 Hz, −25.27 / −18.46 dB at the top; the same notch frequencies at every rate) |
| P4 | Trajectory against GR6 (`evidence/fit/trajectories.tsv`, `itrack.py`, 48 kHz), regression guard only (`--quick`: the noon rates) | Phase 90 block ≤ 0.8 % rms, ≤ 1.4 % max over the six rates; Small Stone off ≤ 2.0 % rms; on ≤ 1.2 % rms (current values `compare.txt`: block 0.41 to 0.70 % rms, max 1.18 %; off 1.24 to 1.25 % rms; on 0.80 to 0.85 % rms; the Small Stone rows have no max bar) |
| P5 | Rate: three rows. (a) "rate (audio)": the notch-1 period tracked from the P4 renders; (b) "rate (LFO)": the phase trace of Phase 90 and Small Stone at 0.01, 0.143, 1.071, 3.72, 9.76, 20 Hz, block 1, 48 kHz; (c) "sync (CH13)": 120 BPM, 1/4 note, song starting at beat 0.3, blocks 64 and 512, both models, Color on | (a), (b) period error < 0.01 %; (c) period 0.5 s ± 0.1 %, phase-0 timing error ≤ 1 ms |
| P6 | Stability: Phase 90 block and Small Stone Color on, 30 s at 10 Hz of full-scale uniform noise, at 22.05, 48, 192 kHz | finite, \|out\| < 4 |
| P7 | Model / color switch on a chord (five switches, at 48 kHz blocks 64 and 512, 44.1 kHz blocks 512 and 37, 96 kHz block 384), and Classic with a color toggle | no step > 3 dB within 1 ms; fade 40 ± 2 ms; the Classic color toggle bit-identical to no toggle |
| P8 | depth/mix independence | Phase 90 and Small Stone output bit-identical for any `depth`/`mix` values (0/0, 1/1, 0.13/0.91, and ramps) |
| P9 | Rate independence, against each voicing's own 48 kHz render at 44.1, 88.2, 96, 192 kHz (`--quick`: 44.1, 48, 96). Three rows per voicing: **trajectory** (notch 1 at the noon rate, tracked like P4), **frozen notches** (notches ≤ 10 kHz at sweep positions −1, −0.5, 0, 0.5, 1; same notch count) and **frozen response** (\|H\| from 20 Hz to 10 kHz where the 48 kHz response is within 20 dB of its peak) | trajectory ≤ 0.3 % rms; notches ≤ 0.2 %; response ≤ 0.1 dB. **Trajectory and notch rows are met** (voicings.md: 0.05 % Phase 90, 0.17 / 0.15 % Small Stone off / on rms; notches within 0.16 %). **The frozen-response row currently FAILS: 1.06 to 1.69 dB against 0.1 dB, an open item (a follow-up is in progress; §8), not met** |
| P10 | Sync jumps: a 220 Hz tone at −12 dBFS, rate 1 Hz, block 64, 48 kHz; a song-position jump at sweep phase 0, 0.25, 0.5, 0.75 of +0.05, +0.25, +0.5, −0.3 cycle, per voicing (16 jumps) | the largest tick above 2 kHz ≤ −60 dBFS (Kaiser FIR highpass residual) |
| P11 | Re-enable, driven as the processor will drive it: a loud signal, no `process()` for 1 s (`on` 0), then `reset()` (adapter key `reset_on_enable=1`) and processing again; silent input and continuing signal, at 48 kHz block 64 and 44.1 kHz block 512 | silent input peaks ≤ −60 dBFS; a continuing signal differs from a fresh engine's output by ≤ −60 dBFS |
| P12 | Non-finite input: NaN, +Inf, −Inf samples in either channel and a 64-sample NaN burst; Classic and the four voicings, blocks 64 and 512 | output finite and bit-identical to the render with zeros in place of the bad samples |
| finite (bad keys) | `model` 7 and −3, `rate` 1e9, −4, NaN, +Inf, `sync_phase` NaN and 1e12, Classic with NaN depth/rate and −Inf mix | output finite, \|out\| < 1e3 |

P9 to P12 are new with the sample-rate, sync-jump, re-enable and non-finite fixes. P11
depends on the processor calling `reset()` on re-enable (processor: in progress); the adapter
key stands in for it until then.

### 4.7 Tremolo — `TremoloFx` (`src/dsp/Tremolo.h/.cpp`)

Study: `docs/research/fx/tremolo/report.md` R1–R6, corrected by its Verification status and
`verification.md` (V:31-32, V:44 edge splatter, V:51-53 stereo power). Numeric reference:
`docs/research/fx/tremolo/prototype/tremolo_proto.py`.

#### 4.7.1 Voices (frozen order)

| index | name | what |
|---|---|---|
| 0 | Classic | today's sine gain LFO with the dry/wet mix, bit-for-bit |
| 1 | Smooth | shaped LFO (wave, skew), real depth law with make-up, 120 Hz gain smoother |
| 2 | Optical | lamp pulse into a two-rate photocell lag, −20 dB floor at full depth |
| 3 | Bias | depth falls as the playing gets louder (push-pull bias model) |
| 4 | Harmonic | low and high bands modulated in antiphase |

Default for new blocks: **Smooth**. Legacy pin: `mod<n>_trem_voice` → 0.

#### 4.7.2 Parameters

The shared mod table (identical to §4.5.2):

| id | host name | range | step | default | tremolo reads |
|---|---|---|---|---|---|
| `mod_chorus_mode` | Mod Chorus Mode | Classic, Wide, CE, Dimension, Tri | — | 1 | no |
| `mod_phaser_model` | Mod Phaser Model | Classic, Phase 90, Small Stone | — | 1 | no |
| `mod_trem_voice` | Mod Trem Voice | Classic, Smooth, Optical, Bias, Harmonic | — | 1 (Smooth) | yes |
| `mod_sync` | Mod Sync | Off, 1/32, 1/16T, 1/16, 1/8T, 1/16., 1/8, 1/4T, 1/8., 1/4, 1/2T, 1/4., 1/2, 1/2., 1/1, 2/1, 4/1 | — | 0 | voices 1–4 |
| `mod_width` | Mod Width | 0 – 1 | 0.001 | 1.0 | voices 1–4: stereo spread = 180° × width; **a mono instance ignores it** (runs as width 0) |
| `mod_tone` | Mod Tone | 0 – 1 | 0.001 | 0.5 | Harmonic: crossover = 300 Hz · 2^(2(tone − 0.5)) (150 – 600 Hz) |
| `mod_shape` | Mod Shape | 0 – 1 | 0.001 | 0.5 | Smooth: skew/duty s = 0.1 + 0.8·shape; Bias: knee = −24 + 24·shape dBFS |
| `mod_wave` | Mod Wave | 0 – 1 | 0.001 | 0.0 | Smooth: 0 sine, 0.5 triangle, 1 pulse |
| `mod_color` | Mod Color | off/on | — | on | no |

Existing: `mod_rate` (Hz) and `mod_depth` are read by every voice (new voices through the
depth law below); `mod_mix` is read by Classic only — new voices ignore it and the UI dims
it (the mix of two identical copies is a second depth knob that also changes level,
`[study: R1; README defects table: 1.6 dB of tremolo and +2.9 dB of level at defaults]`).
`[study: R1-R5; the trem_spread / trem_shape / trem_wave / trem_sync ids of the report are
the shared mod_width / mod_shape / mod_wave / mod_sync here; trem_mult is dropped, §8 D-13]`.

Per-voice starting points (`Tremolo.h`: `kTremRateDefault`, `kTremDepthDefault`,
`kTremWidthDefault`):

| voice | rate Hz | depth | width | note |
|---|---|---|---|---|
| Classic | 1.0 | 0.4 | 1.0 | frozen defaults (width unused) |
| Smooth | 4.0 | 0.6 | 0 | |
| Optical | 5.0 | 0.7 | 0 | prototype's 5 Hz reference point |
| Bias | 5.0 | 0.75 | 0 | prototype's bias table at depth 0.75 |
| Harmonic | 4.0 | 0.8 | 0 | `[choice]` |

```cpp
struct TremoloParams
{
    int voice = 1;             // TremoloFx::Voice
    float rateHz = 1.0f;       // resolved by the dispatcher [0.01, 20]
    float depth = 0.4f;        // [0, 1]
    float mix = 0.35f;         // Classic only
    float width = 1.0f;        // [0, 1] spread = 180 deg * width
    float tone = 0.5f;         // [0, 1] Harmonic crossover
    float shape = 0.5f;        // [0, 1] Smooth skew/duty, Bias knee
    float wave = 0.0f;         // [0, 1] Smooth waveform
    double syncPhase = -1.0;   // >= 0: LFO phase at block start (song-position lock)
};
```

#### 4.7.3 Classic — exact definition

From `Modulation` (`FxBlocks.cpp:364-405, 426-448`): `dryGain = sqrt(1 − mix)`,
`wetGain = sqrt(mix)`, `phaseInc = rate/sampleRate`, per sample
`lfo = 0.5 + 0.5·sin(2π·tremPhase)`, `modGain = 1 − depth·(1 − lfo)`,
`out = x·dryGain + x·modGain·wetGain` on every channel, phase wrap at ≥ 1; rate clamped
0.01–100, depth and mix 0–1; `reset()` sets `tremPhase = 0`. Same float types and order.

#### 4.7.4 Shared rules for voices 1–4

- **Depth law** `[study: R1 — an engineering choice with no reference behind it, report.md:12]`:
  `floor = (1 − depth)²`, `gain = (floor + (1 − floor)·w)·10^(3·depth/20)` with w the voice's
  shape (1 = loud). Depth 0.5 → floor −12.0 dB, crest +1.5 dB; depth 1 → silence at the dip,
  crest +3.0 dB (the +3 dB make-up keeps loudness, TONEX crest 1.4142).
- **Gain smoother:** one-pole at 120 Hz on the final gain, coefficient 1 − exp(−2π·120/fs)
  (τ 1.3 ms, 10–90 % edge 2.9 ms) `[study: R1, choice]`; Optical uses its own lag instead.
- Depth ramped by a 20 ms one-pole before the law; rate, width, tone, shape, wave by 20 ms
  one-poles.
- **Gains are held as their distance from unity** (`smoothToward`): near 1 a float one-pole
  stalls a few ulps short of its target, so depth 0 could never return to exact
  transparency. With the distance decaying to nothing, **depth automated to 0 is bit-exact**
  (X7; X3 for a fresh engine).
- **Non-finite input** (`fx::sanitise`): a NaN or Inf sample is kept out of Bias's envelope
  and Harmonic's filter state, so it spoils at most its own output sample (N1).
- **Stereo spread** `[study: R3; ver: V:51-53]`: right LFO phase `phR = frac(ph + spread/360)`,
  spread = 180°·width; loudness law `w' = w^(1 − spread/360)` applied to both channels'
  shape before the depth law. **Smooth only**: Optical's lamp is lit or dark, so the law
  leaves it unchanged, and Bias and Harmonic take no loudness exponent (coordinator
  decision, `Tremolo.cpp`, `check_tremolo.py` header), so their summed power is constant at
  spread 180° (X5, bar 0.2 dB). What is guaranteed — and nothing more — is listed in the acceptance table: at
  spread 180° and depth 1 the summed power is constant; at lower depths it ripples up to
  0.63 dB; at 90° the sum still swings 11.5 dB; and the exponent turns a 180° sine into a
  rectified half-sine whose smoothed floor is −38.7 dB, so "floor below −60 dB" holds only
  at spread 0.
- **Sync:** rate from the dispatcher; with `syncPhase ≥ 0` the phase is set at block start
  so the dip lands on the grid `[study: R4]`. Rate multiplier: none (§8 D-13). **A
  song-position jump** (transport start, locate, loop: the new phase differs from the running
  one by more than 1e-3 cycles, `kJumpCycles`) **crossfades the gain target from the old
  phase's LFO to the new one's over 10 ms with a raised cosine** (`kJumpFadeSeconds`); the
  120 Hz smoother alone left a slope step that clicked at −44 dBFS on a low note (S1). A jump
  during a fade restarts the fade from the newer phase. Smaller corrections apply outright.
- LFO start after reset: phase 0 (Smooth starts at the dip, as the prototype).
- **`reset()`** clears every state; voices 1–4 start with their knobs at the last values sent
  and **every gain at unity**, so the first block glides from the input into the tremolo
  through the gain smoother instead of stepping to the dip (the reference starts at y0 = 1).
  Every `setParameters()` until the next `process()` snaps the knobs. The processor calls it
  on a non-Classic voice when the block is switched back on (processor: in progress;
  R1 checks that the output then equals a freshly prepared engine's, bit for bit).
- **Mono blocks ignore Width in every voice** (`updateWidth (w, stereo)`): the left-channel
  LFO only, as at width 0 (M1).

#### 4.7.5 Voice algorithms (constants as `tremolo_proto.py`)

- **Smooth (1)** `[study: R1, proto :142-166]`:
  ```
  s     = 0.1 + 0.8*shape
  w0    = ph < s ? 0.5*ph/s : 0.5 + 0.5*(ph - s)/(1 - s)     // skew: crest at ph = s
  sine  = 0.5 - 0.5*cos(2*pi*w0);   tri = 1 - |2*w0 - 1|
  pulse = frac(ph - s/2) < s ? 1 : 0                         // centred on the crest
  w     = wave <= 0.5 ? lerp(sine, tri, 2*wave) : lerp(tri, pulse, 2*wave - 1)
  gain  = onepole120( (floor + (1 - floor)*w') * 10^(3*depth/20) )
  ```
- **Optical (2)** `[study: R2, proto :193-203]`: lit = (ph ≥ 0.75) (lamp on a quarter of the
  cycle); target = 1 − 0.9·depth·lit (floor −20 dB at depth 1); gain follows target with a
  one-pole whose corner is 31.83 Hz (τ 5 ms) while falling and 7.96 Hz (τ 20 ms) while
  rising, chosen by the direction of change; start 1.0. Ignores wave and shape; no make-up.
  Spread shifts the right channel's lamp phase.
- **Bias (3)** `[study: R5, proto :267-288]`: u = 0.5 + 0.5·cos(2π ph) (1 = dip);
  env = peak follower of the block's mono sum (attack 5 ms, release 150 ms);
  q = env/knee_lin, knee_lin = 10^((−24 + 24·shape)/20) (default −12 dBFS);
  `gain = sqrt((1 − depth·u + q)/(1 + q))`, then the 120 Hz smoother. Clean gain, no
  waveshaper (the amp supplies the distortion). The name rests on an inferred reading of
  Guitar Rig's AC30 (V:67).
- **Harmonic (4)** `[study: R6, proto :315-331]`: L = TPT one-pole low-pass at f_x,
  H = x − L, f_x = 300·2^(2(tone − 0.5)) Hz; u = 0.5 − 0.5·cos(2π ph);
  `out = sqrt(2/(2 − depth))·(sqrt(1 − depth·u)·L + sqrt(1 − depth·(1 − u))·H)`;
  the two band gains pass the 120 Hz smoother. Literature-only voice (no installed
  reference); judge by ear before shipping (report.md:386).

#### 4.7.6 Latency, CPU, fxbench

Latency 0. CPU per instance: Classic as today; Smooth, Optical, Bias ≤ 0.10 %; Harmonic
≤ 0.15 %. fxbench keys: `voice rate depth mix width tone shape wave sync_phase on`; on
`on` going from 0 to 1 with a voice other than Classic the adapter calls `reset()` before the
next block, as the processor will; `--trace=gain` writes the left (and right) gain.

#### 4.7.7 Acceptance (`check_tremolo.py`)

Steady 1 kHz sine at −18 dBFS, 4 Hz unless stated; envelope from `tremenv.py`
(study evidence/tools) or the gain trace. Verifier tolerances: rate 0.1 %, gain 0.005,
edge 0.3 ms, depth 0.15 dB.

| # | Check | Pass |
|---|---|---|
| T1 | Classic null (§2.11) | 0 differing samples |
| T2 | Parity with `tremolo_proto.py` per voice (Smooth at wave 0/0.5/1 and shape 0.2/0.5/0.8; Optical; Bias; Harmonic), spread 0 and 180 | rms difference ≤ −80 dB re output |
| T3 | Smooth depth 1, wave 0 | crest +3.0 ± 0.2 dB; floor ≤ −60 dB (spread 0); loud half 0.50 ± 0.01 |
| T4 | Smooth depth 0.5 | floor −12.0 ± 0.5 dB |
| T5 | Smooth wave 1 (pulse) | 10–90 % edges 2.9 ± 0.5 ms; loud fraction = s ± 0.01 (shape 0.5 / 0.25 / 0.75); splatter 3–6 kHz around the carrier ≤ **−58 dBc at worst-case carrier phase** (8 carrier phases incl. cosine, carriers 997 / 1001.5 Hz, and a dense sweep of 64 phases at 1 kHz plus off-grid carriers) — the study's −64.8 dBc held only with the carrier at a zero crossing on every edge; worst case at 1 kHz is −59.1 dBc `[ver: V:32, V:44]`. **Splatter by carrier frequency** (worst of 16 carrier phases, the band is carrier + 2 … 5 kHz and for a low carrier also catches the edge's lower sideband, which adds coherently, +3.3 dB at 40 Hz; the incoherent mean is −61.6 dBc flat): carriers 40 / 48 / 64 / 84 / 112 / 148 / 196 / 248 / 332 / 440 / 660 / 1000 / 1320 / 2000 Hz must stay at or below −55.5 dBc from 40 Hz, −55.8 from 82 Hz, −56.4 from 200 Hz, −58.0 from 500 Hz |
| T6 | Depth step 0 → 1 between blocks | largest per-sample gain step ≤ 0.005 [0.0014] |
| T7 | Optical, 5 Hz, depth 1 | fall 11 ± 1.5 ms; rise 44 ± 5 ms; loud fraction 0.70 ± 0.03; floor −20 ± 1 dB; envelope identical within 0.1 dB from −36 to −6 dBFS |
| T8 | Stereo, spread 0 | L and R identical (null) |
| T9 | Stereo, Smooth sine, spread 180, depth 1 | envelope correlation < −0.9; summed power L² + R² ripple ≤ 0.2 dB [0.00] |
| T10 | Stereo, spread 180, depth 0.25 / 0.5 / 0.75 | summed power ripple ≤ 0.7 dB [0.63 / 0.59 / 0.21] (no constant-power guarantee below depth 1) |
| T11 | Stereo, spread 90 | right leads by 90 ± 2°; (no power guarantee: swing ≈ 11.5 dB) |
| T12 | Stereo floor, spread 180, depth 1 | floor ≤ −36 dB re crest [−38.7] (documents the exponent's cusp) |
| T13 | Sync: 120 BPM, 1/8 | 4.000 Hz ± 0.1 %; gain minimum within 2 ms of each eighth |
| T14 | Bias, 440 Hz, depth 0.75, knee −12 dBFS | depth 5.5 ± 0.5 dB at −40 dBFS; 1.25 ± 0.4 dB at −6 dBFS |
| T15 | Harmonic | depth 0: null against dry ≤ −100 dB; depth 1: 82 Hz and 2 kHz sines each swing ≥ 10 dB with envelope correlation < −0.9; pink-noise rms ripple ≤ 3.5 dB |
| T16 | Voice switch mid-note | no step > 3 dB within 1 ms; fade 40 ± 2 ms |
| T17 | Rates | T3, T5, T7, T13 at 44.1 and 96 kHz |
| X1 | Rate accuracy, Smooth, 0.1 / 0.5 / 2 / 4 / 10 / 20 Hz | within 0.1 % |
| X2 | Depth law across the knob (0.1 … 0.97) against `tremolo_proto.voice_smooth` | floor and crest within 0.15 dB |
| X3 | Depth 0 on music, voices 1–4 | transparent to ≤ −100 dB (a fresh engine; X7 is after automation) |
| X4 | Zipper under block-rate automation of depth, rate, wave, shape (each voice's own knobs) | residue lines at k·fs/512 ≤ −60 dBc |
| X5 | Stereo summed power: Smooth table (T9/T10 bars); Bias at spread 180 (−40 / −18 / −6 dBFS, depth 0.5 / 0.75 / 1); Harmonic at spread 180 (1 kHz and 100 Hz + 2 kHz, depth 0.5 / 0.8 / 1) | Smooth as T9/T10; Bias and Harmonic ripple ≤ 0.2 dB |
| M1 | Mono instance: width 0 / 0.37 / 1, every voice | bit-identical (max \|diff\| 0); Smooth at depth 1 and width 1 reaches the same floor as stereo width 0 (≤ −60 dB, equal within 0.01 dB) |
| R1 | Re-enable after bypass (`on` 0 then 1, `reset()` between), voices 1–4 | (a) nothing of an old note returns: ≤ −140 dBFS; (b) with the input still playing and knobs moved while bypassed, output from the re-enable sample on equals a freshly prepared engine's, 0 samples differ; (c) the gain step across the re-entry ≤ 1 − exp(−2π·120/fs) per sample |
| S1 | Song-position jump (0.1 / 0.25 / 0.5 / 0.75 cycle, six points of the cycle, 82.4 and 220 Hz at −12 dBFS, each voice at its starting depth and at 1) | peak above 2 kHz from 5 ms before to 120 ms after the jump ≤ −60 dBFS, or what the voice puts there on its own without a jump (Optical's lamp edges), whichever is higher |
| X7 | Depth automated to 0 (ramp 1.0–1.5 s, and Harmonic's step 1 → 0) | output equals the input bit for bit from 2.5 s on |
| N1 | NaN / Inf bursts in the input, every voice, block 512 | no extra non-finite output samples (only the bad input samples' own); 0.5 s after the last bad sample the output equals the clean render's (≤ −100 dB rms) |

Check script at commit `2b847b9`: 86 checks, 0 FAIL.

---

## 5. Scaffold work list (one engineer, in this order, before anyone else starts)

### 5.1 Golden renders FIRST, on the unchanged base

1. Add `tools/golden/GoldenRender.cpp` and a `tubamp_golden` console target in
   `CMakeLists.txt`, built exactly like `tubamp_smoke` (core sources, headless defines,
   `CMakeLists.txt:253-270`). No plugin source is touched.
2. Build Release on the development Mac (arm64) and run
   `tubamp_golden --capture tests/golden/fx-golden.tsv [--model <the smoke model .nam>]`
   and `tubamp_golden --params tests/golden/params-base.tsv`.
3. Commit these three files alone ("golden captured on bf81728"). Every later check
   compares against them. Hashes are architecture- and build-type-specific: verify on the
   same arch, Release.

The tool (§5.4) is part of the scaffold's deliverable.

### 5.2 Frozen legacy reference and engine stubs

4. `tools/fxbench/legacy/LegacyFx.h/.cpp`: verbatim copies, from the base commit, of
   `Drive`, `ToneStackEQ`, `NoiseGate`, `Modulation` (`src/dsp/FxBlocks.*`, with their
   file-local constants and helpers) and a `LegacyComp` class holding
   `juce::dsp::Compressor<float>` + makeup `SmoothedValue` + `applySmoothedGain`, with
   `setParameters/process/skip` reproducing `PluginProcessor.cpp:1319-1331, 893-898,
   1096-1104` exactly. Namespace `tubamp::legacy`. Never edited afterwards.
5. Stub engines: `src/dsp/{Gate,Compressor,Drive,Eq,Chorus,Phaser,Tremolo}.{h,cpp}` with
   the exact public API, Params struct, enums, `numTypes` and starting-point arrays of
   §2.1 and §4.x, and pass-through bodies. They compile into the plugin through the
   existing glob but nothing calls them yet. From here on each pair belongs to its engineer.

### 5.3 `tubamp_fxbench`

6. `tools/fxbench/FxBench.h` — the registration interface (scaffold-owned, frozen once
   engineers start):
   ```cpp
   namespace fxbench
   {
   /** Parameter values for one block, after the timeline has been applied. */
   struct Args
   {
       std::map<std::string, double> values;
       double get (const char* key, double fallback) const;
       bool has (const char* key) const;
   };

   class Effect
   {
   public:
       virtual ~Effect() = default;
       virtual const char* typeKey() const = 0;          // "type", "mode", "model", "voice"
       virtual void prepare (double sampleRate, int maxBlock, int numChannels) = 0;
       virtual void reset() = 0;
       virtual void setParams (const Args&) = 0;          // every block, before process()
       virtual void process (juce::AudioBuffer<float>& io, int numSamples) = 0;
       virtual int latency() const { return 0; }
       virtual int traceChannels() const { return 0; }   // > 0 if --trace is supported
       virtual void trace (float* const* dst, int numSamples) { juce::ignoreUnused (dst, numSamples); }
       virtual std::string meterRow() const { return {}; } // one TSV row per block for --meters
       /** The frozen pre-change implementation, for --classic-null (fed the same Args). */
       virtual std::unique_ptr<Effect> makeLegacy() const { return nullptr; }
   };

   struct Registration
   {
       const char* name;   // "gate", "comp", "drive", "eq", "chorus", "phaser", "tremolo"
       const char* help;   // the effect's keys, printed by --help
       std::unique_ptr<Effect> (*make)();
   };

   bool registerEffect (const Registration&);   // called from a static initialiser
   } // namespace fxbench

   #define FXBENCH_REGISTER(NAME, CLASS, HELP)                                              \
       static const bool fxbenchRegistered_##CLASS = fxbench::registerEffect (               \
           { NAME, HELP, [] () -> std::unique_ptr<fxbench::Effect> { return std::make_unique<CLASS>(); } })
   ```
   The `chorus`, `phaser`, `tremolo` adapters drive their engine directly (not through
   `Modulation`); their legacy is `legacy::Modulation` with the matching `Type`. The gate
   adapter calls `measure` then `apply` (key `deferred=1` applies to a mono copy through
   `apply (float*, n)`, key `delay=<n>` sets the curve delay).
7. `tools/fxbench/main.cpp` — the CLI:
   ```
   tubamp_fxbench <effect> <in.wav | gen:SPEC> <out.wav> [key=value ...] [options]
     key=value              engine parameter, units as Parameters.h
     key@T=value            step at T seconds
     key@T0..T1=V0..V1      linear ramp, evaluated once per block
     gen:SPEC               sine:<hz>:<dBFS> | noise:white|pink:<dBFS> | impulse | music
                            (with --sr=<Hz> --seconds=<s> --channels=<1|2>)
     --block=N              default 512
     --trace=<path.wav>     per-sample trace (gain etc.), Effect::trace
     --meters=<path.tsv>    per-block Effect::meterRow
     --latency              print Effect::latency
     --classic-null         render with typeKey=0 and makeLegacy(); print the number of
                            differing samples and the first index; exit 1 if any differ
     --reset-repeat         render, reset, render the same input; exit 1 if not identical
     --rt-check             global operator new/delete hooks armed only around
                            setParams/process; exit 1 on any allocation
     --no-ftz               do not wrap processing in juce::ScopedNoDenormals (default: wrap)
     --bench                stereo pink noise, 48 kHz, block 64 unless overridden, 10 s;
                            print ns/sample and % of real time (min of 5 runs)
   ```
   Output WAVs are 32-bit float. `main.cpp` owns WAV I/O, the timeline, the options; each
   `fx_<effect>.cpp` only maps keys to its Params struct and implements `Effect`.
8. Seven `tools/fxbench/fx_<effect>.cpp` stubs registering their (stub) engines; from here
   on each belongs to its engineer.
9. CMake: target `tubamp_fxbench` (console app like `tubamp_fxrender`, `CMakeLists.txt:277-293`)
   with `file (GLOB CONFIGURE_DEPENDS tools/fxbench/*.cpp tools/fxbench/legacy/*.cpp)` plus
   `src/dsp/{Gate,Compressor,Drive,Eq,Chorus,Phaser,Tremolo}.cpp`; links `nam_core` (the gate's
   Classic path uses AudioDSPTools), `juce_dsp`, `juce_audio_formats`. Static registration
   works because the objects are linked directly, not from a static library. No engineer
   edits CMake: a new `fx_*.cpp` is picked up by the glob.
10. `tools/fxbench/fxcheck.py` (numpy only, `python3 -I` safe): locate the binary
   (`$FXBENCH` or `build/tubamp_fxbench_artefacts/Release/tubamp_fxbench`), `run(effect,
   inp, keys, opts) -> (out, trace)`, WAV read/write, generators (sine, burst with raised-
   cosine fade, chug, hover, step, pink, white with fixed seed, the music input of §5.3.5),
   analyses (least-squares fundamental residual, harmonic/inharmonic alias spectrum, swept-
   sine response, t50/t63/t90 on a trace, analytic-signal pitch track, envelope, correlation,
   short-time zipper metric), and `common_checks(effect, types)` implementing §2.11. Reuse
   `tools/re/sig.py` where it already does the job.
11. Seven `tools/fxbench/check_<effect>.py` skeletons calling `common_checks` and printing a
   PASS/FAIL table, exit status 1 on any failure. `tools/fxbench/README.md` documents all of it.

#### 5.3.5 The standard test input

`gen:music` (also used by the golden tool, same generator in C++ and Python, fixed LCG
`x = x·1664525 + 1013904223 mod 2³²`, seed 12345): 6 s, stereo — 0–2 s plucked notes
(82.41, 110, 196 Hz, harmonics 1/0.6/0.4/0.25, 5 ms attack, 0.4 s decay, right channel 3 ms
later), 2–3 s the chord plus a white-noise burst at −30 dBFS, 3–4 s 1 kHz sine at −20 dBFS,
4–4.5 s white noise at −30 dBFS, 4.5–6 s silence. Peak −6 dBFS.

### 5.4 The golden-render tool (`tubamp_golden`)

- `--capture <file>`, `--verify <file> [--expected tests/golden/expected-changes.tsv]`,
  `--params <file>` (dump of the parameter layout: index, id, name, range, step, skew,
  default, version hint), optional `--model <path.nam>` (adds the `M` cases; the file's
  SHA-256 is recorded), optional `--ir <path.wav>` (adds `cabIR`).
- Each case: a fresh `TubampAudioProcessor`; `setChainOrder`; parameters set with
  `setValueNotifyingHost (convertTo0to1 (v))`; **in `--verify`, every new type/mode/voicing
  id that exists in the layout (§2.7.5 table) is first set to 0 (Classic)**, because a fresh
  instance now defaults to the recommended types; `prepareToPlay (fs, block)`; render
  `gen:music`; hash with `juce::SHA256` over the interleaved float bytes; record
  `getLatencySamples()` after the last block.
- File row: `case  fs  block  sha256  latency`.

#### 5.4.1 Cases

Rates 44 100, 48 000, 96 000 at block 512 for every case; cases marked ★ also at 22 050 and
192 000 and at 48 000 with blocks 1 and 37. "On" flags are set for the blocks in the chain
(`drive_on` defaults to false for instance 1).

| case | chain | settings |
|---|---|---|
| `empty` ★ | `-` | — |
| `gate0` ★, `gate1` | gate | threshold −80 / −40 |
| `gateamp` | gate, amp | threshold −40 (no model: exercises the deferred mono apply) |
| `comp{1,2,3}_def` (★ for 1), `_fast`, `_slow` | comp k | defaults; thr −40 R 20 att 0.1 rel 10 makeup 12; thr −30 R 2 att 30 rel 1000 makeup 3 |
| `compoff` | comp | `comp_on` false, makeup 6 (exercises `skip`) |
| `drive{1,2,3}_def` (★ for 1), `_hot`, `_dark` | drive k | 12/4000/0; 36/12000/−6; 0/500/0 |
| `eq{1,2,3}_flat` (★ for 1), `_shape`, `_cut`, `_near` | eq k | 5/5/5; 10/0/7; 0/0/0; 5/5/5.1 |
| `amp_noeq`, `ampeq_flat`, `ampeq_shape` | amp | `amp_eq_on` off; on at 5/5/5; on at 8/3/6 |
| `mod{1,2,3}_{cho,pha,tre}_{def,fast,slow}` (★ for mod1 def) | mod k | `mod_type` 0/1/2 × (1/0.4/0.35; 10/1/1; 0.05/0.1/0.5) |
| `dl{1,2,3}_t{0..4}` | delay k | type t, defaults (fence F1) |
| `rv{1,2,3}_t{0..4}` | reverb k | type t, defaults (fence F1) |
| `cab0` | cab | no IR (`cabIR` with `--ir`) |
| `classic` ★ | `chain::classicOrder()` | defaults, `drive_on` true |
| `splitA` | split, drive, lane2, mix | drive on, gain 0 |
| `splitAB` | split, drive, lane2, drive2, mix | both on, gain 0 |
| `ampM`, `gateampM` (with `--model`) | amp; gate, amp | at 44 100 the model resamples (NAM latency > 0) |

#### 5.4.3 Expected changes (`tests/golden/expected-changes.tsv`)

Default for every case: audio identical, latency identical. Exceptions (the only ones):

| case | audio | latency |
|---|---|---|
| `eq{1,2,3}_flat` | must equal the `empty` case's hash at the same fs/block (exact pass-through, D2 a) | same |
| `ampeq_flat` | must equal `amp_noeq` | same |
| `gateamp`, `ampM`, `gateampM` | must equal this build's render of the same case with `amp_eq_on` off: `amp_eq_on` defaults on at 5/5/5, so these chains carry a flat amp EQ (D2 a) | same |
| `classic` | must equal this build's render with `amp_eq_on` and `eq_on` off (a flat amp EQ and a flat EQ block, D2 a) | 0 → 3 (D2 b) |
| `drive*`, `splitAB` | identical | 0 → 3 (D2 b) |
| `splitA` | must differ (lane B now waits 3 samples for lane A) — checked by smoke D18 | 0 → 3 |

Anything else that differs is a regression. Editing this file needs the owner's sign-off.
The audio rules `off:<id>,…` (equal to the same case rendered with those ids at 0) and
`differs` were added to express the rows above; for the four `off:` cases the off renders were
also captured with the processor as it stood before the EQ/drive integration and are identical
to this build's, so each case equals the old plugin with its flat EQs removed. `tubamp_golden
--legacy-check` meets the same two fixes: its flat cases (`eq_flat`, `ampeq_flat`) compare
with the legacy chain without the EQ, and `eq_steps` compares only up to its first knob step
(D-4 smoothing). The mod cases that change `mod_type` (`mod_typechange`, `mod3_typesteps`,
`mod_absent`) meet the D-5 crossfade: leaving the fade's samples out would not do, because the
always-on DC blocker carries any difference on, so the legacy side runs the old class once per
type, resets the incoming one where the old block reset everything and crossfades as
`Modulation` does; the processor must equal that everywhere, and that must equal the old
single-instance jump everywhere outside the fades (checked before the DC blocker, reported per
case). Every other case is bit-identical to `tools/fxbench/legacy`.

---

## 6. Integration work list

Integrators work after the engines pass their check scripts. Line numbers are the base
commit's. Do the common part first, then one effect at a time, running `tubamp_golden
--verify` and `tubamp_smoke` after each.

### 6.0 Common

| File:line | Change |
|---|---|
| `src/Parameters.cpp:38` | add `kVersionHint4 = 4` with the hint comment |
| `src/Parameters.cpp` includes | include the seven engine headers (like `Delay.h`/`Reverb.h`, used for default tables) |
| `src/Parameters.h:246` → | new id constants, grouped as §2.7.1, each with a units comment |
| `src/Parameters.h:301` → | `...Ids[maxInstances]` arrays for every per-instance id |
| `src/Parameters.h:327` → | frozen name arrays + `StringArray`s: `gateModeNames`, `compTypeNames`, `driveTypeNames`, `eqTypeNames`, `eqVoiceNames`, `modChorusModeNames`, `modPhaserModelNames`, `modTremVoiceNames`, `modSyncNames` |
| `src/Parameters.cpp:333` → | the append loops of §2.7.1 (gate, comp ×3, drive ×3, eq ×3, mod ×3) before `return layout;` (`:335`); `static_assert`s tying name arrays to `numTypes`; defaults from the engine tables |
| `src/PluginProcessor.h:448` → | `ParamPtrs`: one pointer (or per-instance array) per new id |
| `src/PluginProcessor.cpp:228-324` | resolve the new pointers in the constructor |
| `src/PluginProcessor.h:486-497` | `NoiseGate gate` → `GateFx gate`; `compressor` + `compMakeupLin` → `std::array<CompFx, 3> comp`; `std::array<Drive,3>` → `std::array<DriveFx,3>`; `std::array<ToneStackEQ,3>` → `std::array<EqFx,3>`; `ToneStackEQ ampEq` → `EqFx ampEq`; `Modulation` stays (now the dispatcher) |
| `src/PluginProcessor.h:201` → | meter atomics of §2.8 |
| `src/PluginProcessor.cpp:36` | `kStateVersion = 3` |
| `src/PluginProcessor.cpp:2089-2171`, `:2215-2303` | the legacy pin of §2.7.5 in both restore paths |
| `src/library/PresetManager.cpp:196-205` | `writeFactoryPreset`: set every new type id to 0 before capture (§8 D-2) |
| `src/WebEditor.cpp:39-118` | append relays: sliders (+64), toggles (+6: `comp_auto_makeup` ×3, `mod_color` ×3), combos (+25), in Parameters.cpp append order |
| `src/WebEditor.cpp:120-122` | `static_assert`s → 183 / 36 / 42 with updated reasons |
| `src/WebEditor.cpp:413-414`, `:1018-1024` | reset the new meter atomics on editor open; emit `dynMeters` (§2.8) in `timerCallback` |
| `src/WebEditor.h:43` | relay count comment |
| `docs/REACT-UI.md:59-107, 284` | relay lists and counts; `dynMeters` event contract |

### 6.1 Gate

| File:line | Change |
|---|---|
| `PluginProcessor.cpp:805-806` | `gate.prepare (spec)` (prepare resets) |
| `PluginProcessor.cpp:919` | `gate.reset()` in `releaseResources`, and in `resetBlockInstance` on re-entry: the gate is now drained through `pendingReset` like every block and runs as bypassed while pending (`gateRuns`). Classic's reset only drops its curve, so Classic is unaffected (legacy case `gate_absent`) |
| `PluginProcessor.cpp:1064-1065` → | after `deferGateToAmp` (now computed after the reset drain): D = Σ `blockLatency (order[i])` for i from `gateIndex + 1` through `ampIndex`; `blockLatency` is the per-entry term `computeChainLatency` sums (amp, amp2 with a live model, FX slots), so the amp's own latency and anything between share one rule; `gate.setCurveDelay (deferGateToAmp ? D : 0)`. Drive latency is in `blockLatency` (§6.3), so the host report, the lane ring and D all count it |
| `PluginProcessor.cpp:1299-1310` | fill `GateParams` from `gate_threshold`, `gate_mode`, `gate_release`, `gate_hold`, `gate_range`, `gate_sc_hpf` (`gateParams()`); `setParameters`; `measure (block, n)`; if not deferred `apply (block, n)`; publish meters; `gateState = −1` is stored in `processBlock` whenever the gate is absent, off or pending |
| `PluginProcessor.cpp:1254-1255` | unchanged call `gate.apply (mono, numSamples)` |
| `tests/ProcessorSmokeTest.cpp` | new: gate modes render finite; deferred alignment G16 (with the smoke model at 44.1 kHz); pin rule for `gate_mode` |

Invariants: the trigger always measures at the gate's own position; the reduction is
deferred only when the amp is later in the same segment and on
(`PluginProcessor.cpp:1059-1065`); the amp's tone stack stays after the gate gain on the
mono path (`:1257-1268`); Classic is never delayed.

### 6.2 Compressor

| File:line | Change |
|---|---|
| `PluginProcessor.cpp:813-814` | `comp[k].prepare (spec);` (no separate reset) |
| `PluginProcessor.cpp:893-898` | delete (makeup now inside `CompFx`, primed by its first `setParameters` after `prepare`) |
| `PluginProcessor.cpp:923, 1833` | `comp[k].reset()` |
| `PluginProcessor.cpp:1096-1104` | absent instance: build `CompParams` (`compParams (k)`), `comp[k].setParameters (cp); comp[k].skip (numSamples);` |
| `PluginProcessor.cpp:1312-1335` | on: `setParameters` + `process (block)` + publish GR; off or pending: `setParameters` + `skip`. The makeup now goes through the block's channels, not the host `AudioBuffer` (same channels: input and output layouts are equal) |
| tests | type round-trip and pin; `skip` keeps makeup in step (render with comp toggled on/off mid-stream equals the golden behaviour for Classic); every type renders finite |

Invariant: each makeup smoother advances exactly `numSamples` per callback whatever the
block's state (`:1094-1104, 1328-1332`).

### 6.3 Drive

| File:line | Change |
|---|---|
| `PluginProcessor.cpp:816, 924, 1837` | prepare / reset with `DriveFx` |
| `PluginProcessor.cpp:1338-1351` | `DriveParams` from gain, tone, level, type (`driveParams (k)`) |
| `blockLatency` (the per-entry term `computeChainLatency` sums and the gate's curve delay uses) | for drive instances: `latency = isOn (pp.driveOn[k]) ? DriveFx::latencyFor ((int) valueOf (pp.driveType[k]), preparedSampleRate) : 0` (the amp rule: only when the block runs). `latencyFor` is a pure static function of (type, rate) so the message thread (`updateLatency`) can call it without touching the audio-thread object. `preparedSampleRate` is an atomic the processor stores in `prepareToPlay`: `getSampleRate()` is only what the host last announced (0 when `prepareToPlay` is called directly, as the console tools do) |
| `PluginProcessor.cpp:1730`… | the lane ring (`compensateLaneLatency`) then aligns drive lanes automatically |
| tests | reported latency with drive on/off and per type; D18 split-lane flatness; `splitA`/`splitAB` expectations |

`DriveFx` therefore has, besides §2.1: `static int latencyFor (int type, double sampleRate) noexcept;`
(`getLatencySamples()` returns `latencyFor (currentType, preparedRate)`).

Invariant: latency is reported from the audio thread when it changes
(`PluginProcessor.cpp:1034-1045`) and from `updateLatency()`; both use
`computeChainLatency`.

### 6.4 EQ and amp EQ

| File:line | Change |
|---|---|
| `PluginProcessor.cpp:817, 824-825, 925, 932, 1841` | prepare / reset with `EqFx` |
| `PluginProcessor.cpp:1414-1426` | `EqParams` from the instance's 15 ids + bass/mid/treble (`eqParams (k)`) |
| `PluginProcessor.cpp:1263-1268` | `ampEq` is prepared mono (`numChannels` 1) and fed `EqParams` with `type = 0` and the three `amp_eq_*` knobs; process on the mono block, still after the deferred gate |
| tests | `runAmpEqTests` (`ProcessorSmokeTest.cpp:575`) extended: flat amp EQ == amp EQ off exactly |

### 6.5 Mod block (chorus, phaser, tremolo) — `Modulation` becomes the dispatcher

`src/dsp/FxBlocks.h/.cpp` (integrator). As built — `Modulation` keeps its name and its place
in `FxBlocks`, so the processor's prepare/reset call sites are unchanged:

```cpp
/** Note values for mod_sync in quarter-note beats per LFO cycle; index 0 = Off. Order
    matches modSyncNames and is FROZEN (FxBlocks.cpp static_asserts the two sizes). */
inline constexpr double kModSyncBeats[] { 0.0, 0.125, 1.0/6, 0.25, 1.0/3, 0.375, 0.5, 2.0/3,
                                          0.75, 1.0, 4.0/3, 1.5, 2.0, 3.0, 4.0, 8.0, 16.0 };

struct ModParams
{
    int type = 0;                 // Modulation::Type (mod_type)
    int chorusMode = 1, phaserModel = 1, tremVoice = 1;
    float rateHz = 1.0f, depth = 0.4f, mix = 0.35f;
    int sync = 0;                 // index into kModSyncBeats
    float width = 1.0f, tone = 0.5f, shape = 0.5f, wave = 0.0f;
    bool color = true;
    double bpm = 120.0;           // host tempo; <= 1 (or none) means 120, as for the delay
    double ppqPosition = 0.0;     // host position at the block's first sample
    bool playing = false;         // the host is playing and gave ppqPosition (a flag, not
                                  // ppq < 0: hosts report negative positions in pre-roll)
};

class Modulation   // holds ChorusFx, PhaserFx, TremoloFx and a scratch copy for the crossfade
{
public:
    enum class Type { chorus = 0, phaser = 1, tremolo = 2 };
    struct Timing { float rateHz; double syncPhase; };
    static Timing resolveTiming (const ModParams&, bool classic) noexcept;
    static int crossfadeSamples (double sampleRate) noexcept;   // 20 ms
    void prepare (const juce::dsp::ProcessSpec&);
    void reset();
    void setParameters (const ModParams&);
    void process (juce::dsp::AudioBlock<float>);
};
```
- Rate resolution (`resolveTiming`): Classic voicings always get `mod_rate` and
  `syncPhase = −1` (bit-identity). Otherwise sync Off is `mod_rate`, free-running; a note
  value is `rate = bpm / (60·beats)` at the host tempo, with the delay's fallback (no tempo,
  or ≤ 1 BPM → 120), and `syncPhase = frac(ppqPosition / beats)` while the host plays and
  gives a position, else −1 (free-running at the synced rate). A synced rate outside the
  engines' [0.01, 20] Hz is clamped **and runs free**: a clamped LFO cannot follow the song
  position (1/32 above 150 BPM, 1/16T above 200, 4/1 below 9.6).
- `mod_type` switch — **a crossfade, not `fx::EngineSwitcher`.** On a change the incoming
  engine is reset in that block and runs from there, exactly where the old block reset
  everything and jumped; the outgoing engine keeps running for 20 ms on a copy of the input
  with the last parameters it had (free-running), and the output moves linearly from it to
  the incoming one over `crossfadeSamples()`. When the fade ends the output is the old
  block's bit for bit. The switcher's fade through dry was not used: it resets the incoming
  engine 20 ms after the request, so that type would run 20 ms behind the old jump for good.
  A second change inside a fade cuts the fade short (a new one starts from the engine that
  was coming in), so every type still starts where the old block started it. A block larger
  than `prepare()` promised completes the switch outright (the old jump). Snap on the first
  block after prepare/reset. Only the running engines get `setParameters`, as the old block
  only set the active type; each engine fades its own voicing and colour switches and is
  never reset for them.
- `processBlock` reads the play head once (`HostTransport { bpm, ppqPosition, playing }`) and
  hands it to `processChainBlock`; the delays take `bpm` from it, the mod blocks all three.

| File:line | Change |
|---|---|
| `FxBlocks.h:114-134`, `FxBlocks.cpp:349-449` | replace the old `Modulation` with the dispatcher above; delete `Drive`, `ToneStackEQ`, `NoiseGate` (`FxBlocks.h:15-112`, `FxBlocks.cpp:15-30, 45-193, 222-344`) and the AudioDSPTools include; keep `DcBlocker`, `CabSim` byte-identical |
| `PluginProcessor.cpp:818, 926, 1845` | prepare/reset unchanged in form |
| `PluginProcessor.cpp:1428-1442` | `modParams (k, transport)` fills `ModParams` from the 4 existing + 9 new ids, raw, and the transport |
| `PluginProcessor.cpp:965-973` | capture tempo, `ppqPosition` and `isPlaying` once per callback (`HostTransport`) |
| tests | smoke `runModEngineTests`: every new voicing (knob steps, voicing and colour switches) through the processor equals the engine alone; sync 1/8 at 120 BPM for CE, Phase 90 block and Smooth, host playing and stopped: equal to the engine alone at 4 Hz with `frac(ppq/0.5)` or free-running, output repeats every 250 ms (residual ≤ −60 dB; a 0.1 % tempo error exceeds it), Smooth's dips within 2 ms of each eighth; 120 → 150 BPM followed; no play head = 120 BPM; clamped 1/32 at 200 BPM free at 20 Hz; Classic ignores sync; instances 2/3 drive their own ids; voicing and `mod_type` switches on P7's chord ≤ 3 dB per 1 ms; the crossfade is exactly the engines alone with a linear 20 ms fade. `runModRtTests`: three mod blocks cycling types, voicings, colours, sync, transport, bypass and re-entry allocate nothing. `--legacy-check` mod cases: see §5.4.3. The voicing-id pin is `runLegacyPinTests` |

### 6.6 Invariants that must not regress (whole integration)

- APVTS ids, ranges, defaults, order of the first 166 parameters (golden `--params` diff).
- The audio thread never allocates or locks; every instance is prepared eagerly in
  `prepareToPlay` (`:808-821`); blocks larger than promised still run (lanes run flat,
  gate passes ungated).
- Re-entry reset through `pendingReset`, at most two per callback; a pending block
  processes as bypassed (`:1067-1092`). A non-Classic engine is `reset()` when its block is
  switched back on, and the input of every running block is sanitised against NaN and
  infinity before the engine sees it (§2.6, §2.2) (processor: in progress).
- `applyStateVar` publishes the chain order before any synchronous model/IR loading;
  `setStateInformation` publishes immediately and defers `adoptChainOrder` (`:2127-2170`).
  The legacy pin must not reorder these.
- Latency counts only blocks that run; FX slots count while bypassed; the lane ring aligns
  the shorter lane (`:1732-1797`).
- Chain order is state, never a parameter; `chainLock` is a leaf lock.
- Reverb/delay untouched (F1); cab untouched (F2).
- `tubamp_smoke` passes at 44.1, 48, 96 kHz (the 22.05/32 kHz split-path failure is
  pre-existing on `main`).

---

## 7. UI work list (`ui/` only)

Constraints (REACT-UI.md, project memory): **no keyboard input** in the WebView under Logic
(no text fields — menus, knobs, toggles only); **no `color-mix()`**; **Safari 14 floor**
(`vite.config.ts` target `safari14`: no `:has()`, no container queries, no flex `gap`
reliance for layout-critical spacing, no `aspect-ratio`); meters via motion values, no
per-frame `setState`; right-click stays suppressed. Iterate with `cd ui && npm run dev`
(mock bridge).

### 7.1 Bridge

- `bridge/paramMeta.ts`: append an `FX_LAYOUT` after `PRO_LAYOUT` (`:332-337`) in
  Parameters.cpp append order: `GATE_PRO` (5), `COMP_PRO_1` (5) + instances 2/3 via
  `instanceEntry`, `DRIVE_PRO_1` (1) + instances, `EQ_PRO_1` (15) + instances, `MOD_PRO_1` (9)
  + instances; frozen choice arrays (`GATE_MODES`, `COMP_TYPES`, `DRIVE_TYPES`, `EQ_TYPES`,
  `EQ_VOICES`, `CHORUS_MODES`, `PHASER_MODELS`, `TREM_VOICES`, `MOD_SYNCS`) copied from
  `Parameters.h`; `log: true` exactly where §4 says log. Update the totals comment
  (183 / 36 / 42).
- `bridge/types.ts`: append the 64 slider, 6 toggle, 25 combo ids to `SLIDER_PARAM_IDS`,
  `TOGGLE_PARAM_IDS`, `COMBO_PARAM_IDS` in `WebEditor.cpp` order; add to `BridgeEventMap`:
  `dynMeters: { gate: { state: -1 | 0 | 1 | 2; gr: number; level: number }; comp: [number, number, number] }`.
- `bridge/mock.ts`: emit `dynMeters` from the existing 30 Hz meter driver (`:798-835`):
  gate level follows the fake note envelope in dBFS, state from `gate_threshold` with 6 dB
  hysteresis, GR 0 when open / −`gate_range` when closed; comp GR ≈ max(0, (level −
  threshold)·(1 − 1/ratio)) per instance present in the chain.

### 7.2 Panel bodies and menus

`features/panel/index.tsx` `BlockBody` (`:171-190`): route `gate` → `GateBody`, `comp` →
`CompBody`, `drive` → `DriveBody`, `eq` → `EqBody` (instance-aware like `ModBody`).
Every type/voicing menu is a `ParamMenu` whose `onPick` applies that type's starting points
(`applyStartingPoints`, `typeDefaults.ts`). Knob sets per type below; "dim" uses
`KnobRow`'s `disabledKeys`; "hide" swaps the spec list (KnobRow takes any spec array).

| Block | Menu row | Knobs (per type) | Extra |
|---|---|---|---|
| Gate | MODE | Classic: THRESH (others hidden). Tight/Smooth: THRESH, RELEASE, HOLD, RANGE, SC HPF | gate meter (§7.4) |
| Comp ×3 | TYPE | Classic: THRESH, RATIO, ATTACK, RELEASE, MAKEUP. Others: + KNEE, MIX, SC HPF | AUTO toggle (dim in Classic); GR meter |
| Drive ×3 | TYPE | three knobs, labels per type: Classic GAIN/TONE/LEVEL; Screamer DRIVE/TONE/LEVEL; Distortion DIST/FILTER/LEVEL; Fuzz SUSTAIN/TONE/LEVEL; Boost GAIN/HI CUT/LEVEL; Treble BOOST/FREQ/LEVEL | — |
| EQ ×3 | TYPE (+ VOICE menu in Stack) | Classic: BASS, MID, TREBLE. Parametric (snug, 9): LO CUT, BASS, MID, FREQ, Q, TREBLE, TILT, HI CUT, LEVEL. Stack: BASS, MID, TREBLE, LEVEL. Graphic: seven vertical `ParamSlider`s (100 … 6.4k) + LEVEL | — |
| Mod ×3 | TYPE, then the current type's voicing menu (`<block>_chorus_mode` / `_phaser_model` / `_trem_voice`), SYNC | RATE, DEPTH, MIX, WIDTH, TONE, SHAPE, WAVE — shown/dimmed per the voicing tables in §4.5.2, §4.6.2, §4.7.2 (hide what no voicing of the current type reads; dim what the current voicing ignores); RATE dims when SYNC ≠ Off | COLOR toggle, phaser only, labelled BLOCK (Phase 90) or COLOR (Small Stone), dim in Classic |

`knobSpecs.ts`: add the new ids to the base rows where the "all knobs" list is needed
(gate, comp, mod), and export per-type spec lists for the bodies (written once per base and
mapped onto instances by the existing `KNOB_SPECS` mechanism, `:113-123`).

### 7.3 Starting points (`typeDefaults.ts`, mirrors the engine headers)

```ts
/** Classic, Tight, Smooth. */           export const GATE_STARTING_POINTS  = { release: [30, 30, 80], hold: [6, 6, 10], sc_hpf: [75, 75, 75] };
/** Classic, Clean, Squash, FET, Opto. */ export const COMP_STARTING_POINTS  = { knee: [6, 6, 6, 4, 12], attack: [5, 5, 2.5, 0.8, 10], release: [120, 120, 150, 55, 60] };
/** Classic … Treble. */                  export const DRIVE_STARTING_POINTS = { gain: [12, 12, 18, 18, 12, 12], tone: [4000, 4000, 2449, 2449, 12000, 3000], level: [0, 0, -8.2, 5.0, 0, 0] };
/** Classic, Wide, CE, Dimension, Tri. */ export const CHORUS_STARTING_POINTS = { rate: [1, 1, 0.8, 0.25, 0.6], depth: [0.4, 0.4, 0.7, 0.8, 0.5], mix: [0.35, 0.35, 0.5, 0.5, 0.5] };
/** Classic, Phase 90, Small Stone. */    export const PHASER_STARTING_POINTS = { rate: [1, 1.07, 0.32] };
/** Classic, Smooth, Optical, Bias, Harmonic. */
export const TREM_STARTING_POINTS = { rate: [1, 4, 5, 5, 4], depth: [0.4, 0.6, 0.7, 0.75, 0.8], width: [1, 0, 0, 0, 0] };
```
(L2 = −8.2 and L3 = 5.0 dB are in `Drive.h`; `typeDefaults.ts` matches every `k…Default[…]`
table in the engine headers exactly.) Picking a voicing applies
that voicing's entries; picking a `mod_type` applies the entries of that type's currently
selected voicing. Reverb/delay entries are untouched (F1).

### 7.4 Meters

- `GateMeter` (gate panel): a state lamp (closed / hold / open, three block-accent shades,
  no `color-mix()` — precomputed CSS variables), a horizontal detector-level bar
  (−90 … 0 dBFS) with a marker at `gate_threshold` and a second marker 6 dB lower (close
  point, Tight/Smooth only), and a GR bar 0 … 60 dB. Hidden state −1 = greyed.
- `CompMeter` (comp panel, per instance): GR bar 0 … 20 dB, reading `comp[instance]`.
- Ballistics: instant rise to the reported value, fall 20 dB/s; motion values driven from
  the `dynMeters` subscription.

### 7.5 Relay counts

The UI's relay arrays must match `WebEditor.cpp` exactly: 183 sliders, 36 toggles,
42 combos; a typecheck-time assertion on the array lengths mirrors the C++ `static_assert`.

---

## 8. Open risks and decisions taken (owner may revisit)

| # | Item | Decision taken (recommended default) |
|---|---|---|
| D-1 | **No legacy-pin mechanism exists** in this tree (removed with reverb/delay Classic, REVERB-DELAY.md §4), so D2's "existing legacy pin" cannot be reused. | New pin (§2.7.5): absent type ids of instances present in the restored chain become Classic; absent instances get the recommended type. |
| D-2 | Factory presets are captured by the running build, so a fresh install would write them with the recommended types while existing installs keep old files (pinned to Classic). | Write factory presets with every new type id = 0 so they sound the same everywhere; a later preset refresh can adopt the new types deliberately. |
| D-3 | Gate deferred-curve alignment (study R4) would change Classic renders whenever the NAM model resamples. | Alignment for Tight/Smooth only; Classic stays unaligned (strict D2). |
| D-4 | D7 asks for knob smoothing in every EQ type incl. Classic and the amp EQ. | Done; static Classic renders stay bit-identical, automated ones change (the click fix). Not listed as a D2 exception because static bit-identity holds. |
| D-5 | `mod_type` switches today reset and jump instantly. | The dispatcher crossfades over 20 ms from the outgoing type to the incoming one, which is reset where the old block reset it (§6.5, not through the switcher); static renders, and everything after a fade, are unchanged. |
| D-6 | Drive latency: JUCE's integer-latency mode would add a fractional delay and change Classic audio. | Report `lround` (Classic 3; Boost/Treble 0; Screamer and Fuzz 5 at 48 kHz, 4 at 96, then 1 and 2 at 192; Distortion 5, 5 and 4; `latencyFor` in `Drive.cpp`) and leave the audio alone; residual ≤ 0.44 sample (measured for the first design, not re-measured). The first version of this row said 3 at 96 kHz and 1 at 192 for every oversampled type; that was the first-order, single-clipper design. |
| D-7 | Drive latency counted only while `drive_on`, like the amp; automating `drive_on` or `drive_type` moves the reported latency mid-playback. | Follow the amp precedent (D2 "like the amp"). Alternative: the FX-slot rule (count while bypassed and delay the bypassed signal) — not chosen because it would delay a bypassed Classic drive. |
| D-8 | Small Stone Color at 2x internal rate. | Not done (latency forbidden by D6; never rendered, needs a refit). Revisit with GR6 renders at 44.1/96 kHz. Update: still not done, and no longer needed for the notch positions: the stage coefficients are solved per sweep position and tabulated in `prepare()`, so the notches land on the analog ones at every rate (§4.6.4). |
| D-9 | Study's Squash (sample-delayed feedback) undershoots by 30–60 dB at fast attack. | Replaced by log-domain feedback (§4.2.5): same static curve and feedback timing, no undershoot. |
| D-10 | Study's 8 ms detector hold is unsupported (V:48-68). | Exact sliding-window maximum, W = clamp(τR/4, 1, 12.5 ms), release compensated so t63 = knob. Model residual 0.000 % at 41.2/82.4/98 Hz at defaults. |
| D-11 | Tight gate missed 35 ms at −60 dB (V:31). | Sliding-window detector; acceptance written per threshold × level cell (§4.1.10). The sidechain HPF's ringing still adds up to ~8 ms at −60/−70 dB thresholds with −6 dBFS notes; turning the HPF off removes it. |
| D-12 | Chorus gaps (V:42/46/53). | Wide D_max 20 ms with the low-rate limit written down; CE swing ∝ 1/f above 2.86 Hz and bend 0.1 (fitted); Dimension acceptance on triangle values. |
| D-13 | Tremolo rate multiplier (study R4). | Not added: keeps the shared set small; `mod_sync` reaches 16 Hz at 120 BPM (1/32) and the free knob 10 Hz. |
| D-14 | RAT 100 pF feedback cap, 1 kΩ clipper resistor and pot tapers; Big Muff tone pot value — not confirmed by an opened source. | Values used are the widely published ones; the drive engineer confirms each against a schematic and cites it in `Drive.cpp`, or tags it `[choice]`. RAT GBW 2.3 MHz is a fit on harness-only data — revisit by ear. Update: `Drive.cpp` now uses 1 MHz (`kRatGbw`, LM308 data sheet, §4.3.5); the 2.3 MHz fit is dropped. |
| D-15 | Phaser `mod_depth`/`mod_mix` ignored by Phase 90 / Small Stone (stock-only). | Kept stock (fits are only valid there); UI dims them. The prototype's depth/mix scaling exists if the owner wants them live later. |
| D-16 | EQ adds 45 parameters (Graphic alone 21). | Kept: the study recommends Graphic; dropping it would save 21 ids and one UI layout. |
| D-17 | Gate threshold means peak dBFS in Tight/Smooth but mean-square in Classic (≈ 3 dB apart for a sine); old sessions pinned to Classic are unaffected. | Documented in the UI as the meter's level bar uses the active mode's units. |
| D-18 | FET/Opto give one release shape regardless of burst length (V:81); no program dependence. | Accepted for v1; a burst-length-dependent τ2 is future work. Update: delivered (a27b550): FET and Opto releases lengthen with programme length (§4.2.5, check C28). |
| D-19 | Tremolo depth law, 120 Hz smoother, Optical constants, Bias fit and Harmonic voice have no installed reference (report.md:12, :386). | Built as specified; parity with the prototype is enforced; Harmonic and Bias need a listening pass before release. |
| D-20 | Golden hashes depend on arch and build type. | Capture and verify on the development Mac, arm64, Release. |
| D-21 | `mod_color` default on means a new Phase 90 starts as the block-logo (resonant) voicing. | Chosen because it is the validated fit; switch the default to off if the owner prefers the script sound. |
| D-22 | The NAM resampler's reported latency is 1.9 (44.1 kHz) / 3.0 (96 kHz) samples short of its true delay (§4.1.7), so a deferred Tight/Smooth curve leads the audio by that much, and the host's delay compensation is off by the same amount. | Left as is: D uses the reported latency as specified, and the host figure is frozen by the golden renders. A measured group delay from `NamEngine` (probe at model load), used for D only, would close it for the gate. |
| D-23 | `eq_tilt`, `eq_level` and the seven `eq_g*` read their default 0 back from the parameter as 8.9e-8 / 1.8e-7 dB on arm64 (exactly 0.1f × 120 − 12 with the product unrounded, as a fused multiply-add in JUCE's `snapToLegalValue` gives; visible in `tests/golden/params-v4.tsv`), so a fresh Parametric or Graphic block in the plugin runs its level/tilt/band stages instead of the exact 0 dB paths E4/E17 test on the engine. Measured through the processor: within −144 dBFS of the input at 44.1 kHz (−150 at 48, −162 at 96). | Left as is: the processor passes values raw and the layout is frozen. Closing it means the engine treating \|dB\| below 1e-5 as 0 (engine owner), never a processor-side rounding. Classic and the amp EQ are unaffected (their knobs read back exactly). Update: fixed in the engine (fe2fee6): a control within 1e-3 dB of neutral counts as neutral (host noise measured at 1.8e-7 dB; rows E22a, E22b), not 1e-5 as written above. |
| D-24 | **Verification round, 2026-11-11 (seven `dsp:` commits after the integration).** The engines were fixed against the verification reports; this table keeps the earlier decisions above and adds what changed. | Gate (81fd404): the close rule depends on the fall from the recent peak; hold is the wait after a note stops (§4.1.5). Open: a pure 41.2 Hz tone now needs 15.4 dB over threshold to open (was 13.6), the price of the 4th-order 75 Hz sidechain filter. |
| D-25 | Compressor type switches (a27b550). | `fx::EngineSwitcher` dropped (§2.4, §4.2.6): switches among Clean, Squash, FET and Opto keep the gain state with no fade; to or from Classic both outputs crossfade over 40 ms (+0.01 dB bump). Total makeup is capped at 48 dB. |
| D-26 | Drive limits left open (0af54cf). | Aliasing for input tones above 5 kHz (Fuzz worst −43.1 dBc, per the commit; 8x oversampling would double the CPU) and the Screamer's 0.84 dB droop at 10 kHz are known and not fixed. The Fuzz mid-knob row Y2 is a sustain row (output moves at most 3 dB for 18 dB of input) because D14 requires the Muff saturated at half sustain. |
| D-27 | EQ block switched on under signal (fe2fee6). | Level, shelves and cuts still step. Open: needs a crossfade in the processor, not in the engine. |
| D-28 | Chorus mode switches and mono Dimension (08da770). | A crossfade by measured power replaces `fx::EngineSwitcher`; a mono Dimension instance plays line A alone, so CH-mono compares with the stereo left channel, not the mean (the mean of two antiphase lines has no detune). |
| D-29 | Tremolo (2b847b9). | A mono instance ignores Width in every voice; `reset()` starts gains at unity; a song-position jump crossfades over 10 ms; the stereo loudness law is Smooth only (the earlier text said it applied to Optical, Bias and Harmonic as well, which the code never did). |
| D-30 | **Known limit: the phaser's level between notches depends on the sample rate above 3 kHz.** After the notch-matched discretisation (§4.6.4) the notches and trajectories are rate independent, but the frozen magnitude response at 48 kHz differs from 192 kHz by 1.06 to 1.69 dB (script 1.69, block 1.40, Small Stone off 1.06, on 1.61), inside the top 3 dB of the response at 3.4 to 5.5 kHz at the top of the sweep. The 48 kHz render is the one off the analog response (up to 1.80 dB; 192 kHz is within 0.11 dB): four first-order stages lag the analog cascade by 4.5 to 7.3° near 10 kHz (`docs/research/fx/phaser/evidence/fit/rate_limit.txt`). | Left as is; row P9 "frozen response" enforces 1.8 dB as the structure's limit, not 0.1 dB. Tried and measured: four free stage coefficients per position reach only 0.57 to 1.45 dB and give up exact notches. A minimum-phase correction on the wet output closes it (0.06 to 0.28 dB with a 3 dB rise above 10 kHz, 0.03 to 0.12 dB with 6 dB) but lifts the wet above 10 kHz at every rate, which neither the pedals nor the Guitar Rig models do. Owner's call to revisit: that correction, or 2x internal oversampling at 44.1 / 48 kHz (D-8). |
| D-31 | The processor side of the contract is being implemented. | Calls `reset()` on a non-Classic engine when its block is switched back on, and sanitises non-finite input (§2.2, §2.6, §6.6), each marked "(processor: in progress)" for the coordinator to clear. |

---

## Appendix A — parameter count

| Effect | New ids per instance | Instances | Total | floats / bools / choices |
|---|---|---|---|---|
| Gate | `mode release hold range sc_hpf` | 1 | 5 | 4 / 0 / 1 |
| Comp | `type knee mix sc_hpf auto_makeup` | 3 | 15 | 9 / 3 / 3 |
| Drive | `type` | 3 | 3 | 0 / 0 / 3 |
| EQ | `type lowcut highcut mid_freq mid_q tilt level voice g100 g200 g400 g800 g1k6 g3k2 g6k4` | 3 | 45 | 39 / 0 / 6 |
| Mod | `chorus_mode phaser_model trem_voice sync width tone shape wave color` | 3 | 27 | 12 / 3 / 12 |
| **Total** | | | **95** | **64 / 6 / 25** |

Relays after integration: 119 + 64 = 183 sliders, 30 + 6 = 36 toggles, 17 + 25 = 42 combos.
Layout size: 166 + 95 = 261 parameters.

## Appendix B — sources opened while writing this spec

- Study: `docs/research/fx/README.md`; `gate|comp|drive|eq|chorus|phaser|tremolo/report.md`
  (Verification status, Recommendations), `verification.md`, `phaser/voicings.md`,
  `phaser/prototype/phaser_ref.py`, `phaser/verification-refit.md`,
  `tremolo/prototype/tremolo_proto.py`, `eq/evidence/scripts/design.py`.
- ProCo RAT: ElectroSmash RAT analysis (electrosmash.com/proco-rat, via search summary) and
  cushychicken.github.io/ltspice-proco-rat.
- Big Muff π: guitarscience.net/papers/bigmuff.pdf (Fig. 1 component values, §4 tone stage).
- 1N4148/1N914: the standard SPICE diode model (IS = 2.52 nA, N = 1.752).
- TS-808: Yeh, Abel, Smith, DAFx-07, as transcribed in the drive study (R3).
- `[sim]` figures: numpy models of §4.1.5 and §4.2.4–4.2.5 run on 2026-11-11 at 48 kHz
  (not committed; the check scripts reproduce them).
