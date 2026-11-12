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
  are left as they were except where their section says so. `processBlock` also zeroes
  every non-finite sample of the plugin input, on every channel, before anything reads it,
  so one NaN cannot latch the always-on DC blocker. The same pass (`zeroAbsurd`) zeroes any
  sample of magnitude 1e6 or more (+120 dBFS, no audio signal; one near FLT_MAX would
  overflow in the input trim): at the input, before the trim, and again at the output
  before the DC blocker, so a non-finite or absurd value made inside the chain (a hosted
  plugin, the neural model) becomes silence there and the next callback starts clean.
  Anything below 1e6 passes bit for bit. There is no limiter: levels above full scale pass
  unclipped and finite (compressor makeup up to 48 dB, Small Stone Color on up to +13.6 dB
  over a full-scale input, §8 D-39).
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

None of the seven engines uses `fx::EngineSwitcher` (from `FxPrimitives.h`); only `DelayFx`
and `ReverbFx` do (`Delay.cpp`, `Reverb.cpp`). The switcher fades to the untouched input, and
that let the dry guitar through: +19 dB over the louder type for 40 ms on a drive with its level
at −20 dB, +11.5 dB on the phaser, +15 dB on the compressor. Each engine now keeps both types
(or voicings) running and never lets the input itself pass. Every row below is a rule in the
engine and a row in its check script.

- **Drive (§4.3.6), EQ (§4.4.3), phaser (§4.6.4).** The incoming type starts from reset and
  runs unheard so its states settle (drive 10 ms, EQ 50 ms, phaser 30 ms). Then the two outputs
  cross over (drive 20 ms, EQ 40 ms raised cosine, phaser 40 ms raised cosine) with gains
  that come from the short-term powers of the two outputs and their correlation, so the level
  moves on a straight line between the two types' own levels. The fade has no corner at either
  end. A request that arrives during a switch waits for it to end (the EQ takes a reversal
  back, §4.4.1). The phaser also waits, after its unheard settle, for a moment where the two
  outputs will not cancel (§4.6.4).
- **Tremolo (§4.7.4).** Both voices run on one LFO (the phase carries over, Classic included)
  and their outputs crossfade over 40 ms; the incoming voice joins its own steady trajectory.
- **Chorus (§4.5.4).** The outgoing mode keeps sounding while the incoming mode's line fills.
  The two whole outputs then crossfade over 20 ms, per channel, in two bands, from a moment
  where they will not cancel.
- **Compressor (§4.2.6).** A switch among Clean, Squash, FET and Opto hands the gain state over
  with no fade. A switch to or from Classic crossfades both outputs over 40 ms, with the
  detector and the gain computer live from the first sample.
- **Gate (§4.1.8).** A gate fading to "dry" would open for 40 ms. `GateFx` runs all detectors
  continuously and crossfades the two gain curves linearly over 20 ms; while not switching,
  the active curve is used directly (no multiply by 1).
- First block after `prepare()` or `reset()`: the engine starts on the requested type, with no
  switch in flight (the `snapEngine` pattern of `DelayFx`), so a session that starts on any type
  renders as if it had always been on it. While steady, none of these paths touches the audio,
  which is what keeps static Classic renders bit-identical.
- Choices inside a type take part in the same crossfade: the EQ's `eq_voice` in Stack (two
  Stacks run during a voice change), the phaser's `mod_color` (§4.6.4: "Model and Color
  switches"). The `mod_type` switch between effects is the dispatcher's, not an engine's
  (§6.5, §8 D-5, D-32). A `mod_type` request made during the dispatcher's crossfade waits for it
  to end, then fades from the type reached to the newest request; requests in between are
  dropped.
- **Bool switches inside a type** (`comp_auto_makeup`) fade the amount they add through a 20 ms
  one-pole (target M or 0).

### 2.5 Where smoothing lives

Inside the engines, never in the processor. Rules for every new type:

- Every continuous parameter a new type reads goes through a smoother. Default: one-pole
  `fx::Smoother`, 20 ms time constant `[choice: matches the 20 ms ramps used across the
  chain (kGainSmoothingSeconds) and the study's references; study README "Parameter
  changes are ramped over about 20 ms"]`. Sections override this where they say so
  (comp threshold/ratio use 20 ms linear ramps; EQ uses two cascaded 5 ms one-poles, three for the
  Classic knobs, a Parametric bell's gain and Q and the Graphic gains, §4.4.3, §4.4.4).
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
- **Reset on switch-on.** The processor calls `reset()` on an engine when its block starts
  again, whatever its type (`engineStarts`: the block ran before, stopped, and holds audio
  from then; Classic included, which replayed stale audio when it was switched back on
  before the second round). A block that re-enters the chain is reset through
  `pendingReset`. A block never replays what it held when it left. A state restored while the
  last callback's output was silence resets entering and retyped blocks and snaps every fader
  at once, so such a render is the same in either order of restore and prepare (§8 D-52). A
  restore that lands on sound changes the blocks exactly as the same change by hand does. Each engine's
  `reset()` leaves nothing to replay: lines and filters cleared, gains started at unity or
  closed as the section says, and the wet path faded in where a section says so (chorus
  §4.5.4, tremolo §4.7.4, phaser §4.6.4). The fxbench keys that drive this are in
  `tools/fxbench/README.md` (EQ `reenable`, chorus `reset`, phaser `reset_on_enable`; the
  tremolo adapter does it itself).
- **Crossfade at switch-on and switch-off.** For all five block kinds (gate, compressor,
  drive, EQ, modulation), every type, Classic included, goes in and out of the
  signal through `BypassFader` (`src/dsp/FxBlocks.h`): the block's input and the engine's
  output are crossfaded over 10 ms (the fade is a linear ramp of the wet amount, applied as
  a smoothstep `w * w * (3 - 2 * w)` so neither end has a corner). A block that comes back
  after a reset first runs the engine unheard for 10 ms (`restart()`, the settle time)
  while its filters fill from zero, then fades in over 10 ms: 20 ms from the reset to full
  wet. A fade out keeps the engine running for its 10 ms (`needsEngine`). The input is
  delayed by the engine's latency so both sides line up (non-zero for drive only, §2.9).
  The fader's input delay moves through a `DelayGlide` (read with 4-point Lagrange
  interpolation, exact at whole samples), so it slides over 10 ms from where the tap is
  whenever the delay changes: when the type of a bypassed drive changes, and for a Classic
  drive, whose rule is unchanged (3 samples, counted only while it runs, §2.9). Its input
  path slides between 0 and 3 samples while the engine is unheard (settling on, and after
  its fade off), so the fade always meets the engine aligned. This replaces the earlier
  rule that Classic types switch exactly as before: Classic toggles no longer burst or pop
  (old: up to +6.0 dBFS on a hot drive, -8.1 dBFS EQ, -14 to -28 dBFS chorus, phaser,
  tremolo; now at most -73 dBFS under the fade, §8 D-42). Static Classic renders stay
  bit-identical. This closes the EQ step that D-27 listed (worst switch-on pop -119.9
  dBFS, was -7.2 dBFS).

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
delayed by the latency between gate and amp output for Tight/Smooth (§4.1.7). A non-Classic
drive counts its latency whenever it is in the chain, bypassed or waiting for its reset
included, and holds its input back by the same amount while it does not run (`BypassFader`,
§2.6), so toggling it never changes the plugin's latency. A Classic drive keeps the amp's
rule: it counts only while it runs and is not waiting for its reset, because delaying a
bypassed Classic drive would change old sessions (§8 D-7). Latency is computed after the
reset drain, so what is reported is what the block delays in that callback.

**Who reports it.** `processBlock` never calls `setLatencySamples`. It stores the figure it
applies in an atomic (`audioLatency`, with the generation it read before decoding the
chain). Every change the latency depends on (the chain, the state, a model, a latency
parameter), on any thread, only bumps `latencyGeneration`, so no figure computed before a change
is reported after it. `parameterChanged` does nothing else. It no longer asks JUCE which thread
it is on, which took a lock on the audio path. The 20 ms timer (`kLatencyTimerMs`) is the only
caller of `reportLatency`, the one place that calls `setLatencySamples`, once per tick at most and
only when the figure changed. While audio runs it reports the audio thread's figure from a
callback that saw the last change. The audio counts as stopped after no callback for two blocks
and 15 ms, kept between 40 and 250 ms (`audioIdleMs`; a stopped audio thread is noticed within
about 60 ms). The message thread then computes the figure itself from the chain and the
parameters, once per change. The hold of the larger figure during a drive type switch (§8 D-42)
ends with the last queued drive switch. For a drive the audio thread counts what the drive
applies (`driveReported`, from `DriveTimeline`, §6.3), a switch or slide taken at its larger
end. In the gap before the host learns of a change, audio stays continuous and the split lanes
stay aligned with each other; the host's compensation is off by the change (at most 5 samples
for a drive) for at most about 20 ms plus the host's own delay in acting on the report.

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
| 1 | Tight | hard gate: windowed-peak detector, 6 dB hysteresis, closing rule version 5.1 (a stop closes after the hold; a decay or a beat trough is ridden, and the gain is at the range `hold` + 250 ms after the level last was over the open level, §4.1.5), opening that follows the onset (0.2 ms linear for a pick up to a 5 ms smoothstep for a swell), dB-linear release to `range` |
| 2 | Smooth | Tight's detector and state machine; when closed, a 3:1 downward expander instead of a jump to `range` |

Default for new blocks: **Tight**. Legacy pin: `gate_mode` → 0 (§2.7.5). The gate is a singleton.

#### 4.1.2 Parameters

Existing, unchanged: `gate_on` (bool), `gate_threshold` (−100…0 dB, step 0.1, default −80,
hint 1). New (hint 4):

| id | host name | range | step | skew | default | unit | read by |
|---|---|---|---|---|---|---|---|
| `gate_mode` | Gate Mode | Classic, Tight, Smooth | — | — | 1 (Tight) | — | all |
| `gate_release` | Gate Release | 5 – 500 | 0.1 | log | 30 (`kGateReleaseDefault[Tight]`) | ms (time for 60 dB) | Tight, Smooth |
| `gate_hold` | Gate Hold | 0 – 250 | 0.1 | linear | 10 (`kGateHoldDefault[Tight]`) | ms (the wait after a note stops, §4.1.5) | Tight, Smooth |
| `gate_range` | Gate Range | −90 – 0 | 0.1 | linear | −90 | dB (≤ −89.95 = mute) | Tight, Smooth |
| `gate_sc_hpf` | Gate SC HPF | 20 – 300 | 1 | log | 75 (`kGateScHpfDefault[Tight]`) | Hz, 4th-order Butterworth, 24 dB per octave (≤ 20.5 = off) | Tight, Smooth |

`gate_threshold` means, in Tight/Smooth, the **peak level of the sidechain in dBFS**, read as
the lower of the filtered and the unfiltered 12 ms peak (§4.1.5); in Classic it keeps AudioDSPTools' mean-square reading (≈ 3 dB lower for a sine).
`[study: gate/report.md R1 "Threshold semantics"]`

Per-mode starting points (`Gate.h`): `kGateReleaseDefault[3] { 30, 30, 80 }` ms
`[study: R1 table, 30 ms Tight / 80 ms Smooth]`; `kGateHoldDefault[3] { 6, 10, 10 }` ms;
`kGateScHpfDefault[3] { 75, 75, 75 }` Hz (Classic's entries are unused). **Hold is 10 ms for
Tight and Smooth** (6 for Classic's unused entry, and the `gate_hold` parameter's default is 10).
Hold was 10 ms for every mode in the first draft; versions 1 to 4 used 6 for Tight, the shortest
hold that rode a 41 Hz decay without chattering. Version 5 takes 10 again: with 6 the short beat
dips of low chords near the threshold read as stops (version 5: 11 of 256 chord cases chatter at
hold 6, 1 at hold 10; version 5.1, verifier's g1 script on the merged build: 2 of 256 at hold 6,
0 at hold 10), while every 50 ms palm-mute gap still closes (row X13: median −43.4 dB). Under the
closing rule of §4.1.5 (version 5.1) hold is the wait after a **stop**, a fall of the level from
the open level to the close level within 50 ms, at any floor depth: check A2 shows the wait
moves one for one with the knob over 0 to 250 ms (largest error 1.3 ms), and with 10 ms 240 of
240 palm-mute gaps close over floors 12 to 40 dB under the notes (A3); 50 ms gaps over a −40
dBFS threshold reach −43.4 dB and 40 ms gaps −23.4 dB, not fully closed (X13, §8 D-61). Slow
falls and beat troughs are ridden whatever the hold, and the deadline of §4.1.5 is `hold` +
250 ms.

Fixed (not parameters): **opening** follows the onset, `kOpenRatio` 0.5: the rise lasts half
the time the level took to climb the hysteresis band, from 0.2 ms (`kOpenMinSeconds`) up to
5 ms (`kOpenMaxSeconds`). At 0.2 ms the rise is linear, which keeps a pick whole (version 2's
only shape; it replaced the study's 0.5 ms for 60 dB dB-ramp, `[study: R1]`); from twice that
(0.4 ms) it is a smoothstep, blended between, so a swell opens without a click. Hysteresis
H = 6 dB `[study: R1, inside the 4.8–9.5 dB the references use]`; detector window 12 ms
`[study: R1 Nwin, half period of 42 Hz]`; expander slope S = 3 `[study: R1]`.

Sidechain HPF default 75 Hz, 4th-order Butterworth `[choice, check_gate X20, F rows: a low E
reads 1.7 dB under its level, 50 / 60 Hz hum 14 / 8 dB; the 2nd-order 90 Hz filter of the first
draft read a low E 3.9 dB under and hum 11 / 8 dB; the study's figures were measured with
90 Hz, ver: gate/verification.md V:31]`. It is a guitar default. What a pure tone at −40 dBFS
threshold needs, in dB over the threshold, to open the gate (rows F, printed, not enforced;
"steady" is a tone that rises over 0.3 s, "abrupt" one that starts at a crest and must still
hold the gate open 0.4 s later):

| sidechain HPF | pure tone | steady: dB over threshold to open | abrupt onset: dB over threshold |
|---|---|---|---|
| 75 Hz (default) | 30.87 Hz | +30.1 | +30.9 |
| 75 Hz | 41.2 Hz | +20.8 | +20.9 |
| 75 Hz | 82.41 Hz | +1.7 | +1.7 |
| 40 Hz | 30.87 Hz | +9.5 | +9.5 |
| 40 Hz | 41.2 Hz | +2.6 | +2.6 |
| 40 Hz | 82.41 Hz | +0.1 | +0.1 |
| off (knob at 20) | 30.87 / 41.2 / 82.41 Hz | +0.1 | +0.1 |

(Tight, threshold −40 dBFS; check `F bass, level to open`, printed and not enforced, measured
on the merged build. "Steady" rises over 0.3 s; "abrupt" starts at a crest and must hold the gate
open 0.4 s later; version 3 read the abrupt tone at +24.9 / +15.4 / +0.2 dB for 30.87 / 41.2 /
82.41 Hz with 75 Hz.) A decaying dull note shuts the gate when its true peak is still, over the
threshold, +15.6 / +10.8 / +6.1 dB (30.87 / 41.2 / 55 Hz) with 75 Hz, +4.8 / +0.4 / −2.0 dB with
40 Hz and −1.9 / −2.0 / −2.2 dB with the filter off: the closing rule rides it, then the deadline
closes it. Hum reads, against the fundamental's amplitude with the 12 ms window: 75 Hz filter −3.9 dB at 50 Hz
and −1.9 dB at 60 Hz; 40 Hz filter +2.5 / +3.2 dB; filter off +3.2 / +3.2 dB.

For bass use the knob's minimum (exactly off) or about 40 Hz.

```cpp
struct GateParams
{
    int mode = 1;               // GateFx::Mode
    float thresholdDb = -80.0f; // [-100, 0]
    float releaseMs = 30.0f;    // [5, 500] time for 60 dB of gain change
    float holdMs = 10.0f;       // [0, 250] (kGateHoldDefault[Tight])
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

This is **version 5.1** of the closing rule (`Gate.h` states it, `Gate.cpp` implements it).
Method: versions 1 to 4 were rejected by independent verification (chatter on decaying chords,
palm mutes left open, a 30 s latch on a noise floor, chord chatter). Version 5 (`bd1f69a`) was
specified from an offline model validated against the engine and implemented to match a Python
reference within 0.1 dB (measured worst 0.0138 dB on 153 signals, implementer's run). Version 5.1
(`8a1081a`) amends it after four rows failed on version 5 (X4 noise at the threshold, A5 palm mutes
near the hiss, B1 and B2 chord clicks, I3 for Smooth); the changes are listed under "What 5.1
changed" below.

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
pk   = max of |sc| over the last Nwin samples     // exact sliding-window maximum (van Herk / Gil-Werman),
                                                  // Nwin = round(0.012 * fs); the filtered peak
raw  = same windowed max of |s|                   // the unfiltered peak
lev  = min(pk, raw)  if the filter is on else pk  // the level the state machine reads
open  = 10^(T/20);  close = open * 10^(-6/20)
```

**The level.** The state machine reads the lower of the filtered and the unfiltered windowed
peak. The 4th-order filter rings for tens of ms after a note stops (up to 45 ms more after a
loud low note over a deep floor); the unfiltered signal stops with the note. On a guitar note
the unfiltered peak is the higher, so the hysteresis stays whole; hum keeps the filtered peak
lower. This replaces version 2's separate stop detector (check X10, B1).

**The closing rule.** What closes the gate is how fast the level fell through the 6 dB
hysteresis band (from `open` to `close`), not how far it fell, because the noise floor decides
how far a stopped note can fall. Two clocks run: the stop rule below, and a deadline that caps
everything. In the order a reader needs them:

1. **Open level and band.** `lev >= open` opens the gate (but see reopening, point 9). Between
   `close` and `open` the gate stays open and nothing counts, except the in-band stop (point 5).
   Under `close` the stop clock runs.
2. **The hold (a stop).** Let `fall` be the time since `lev` was last over `open`. If `lev` falls
   under `close` with `fall` of 50 ms or less (`kFastFallSeconds`), it is a **stop**, at any floor
   depth. The gate closes once `lev` has stayed under `close` for `hold + extend` (`extend`:
   point 3). A hand stop crosses the band in 1 to 44 ms whatever the floor.
3. **Riding remembered troughs (`extend`).** While the gate is open the engine remembers dips of
   the level: a fall of 6 dB (`kDipDb`) under the level's own 50 ms maximum that comes back to
   within 3 dB (`kBackDb`) of it. These are a chord's partials beating 15 to 40 times a second,
   which the 12 ms window cannot fill. **Only dips the gate rode open are remembered** (a dip in
   which the gate closed is not: a palm-mute gap over a floor inside the band would teach every
   later gap as a trough to ride). Dips under 12.5 ms (`kDipMinSeconds`, one period of 41.2 Hz
   minus the window: a low note's own crest gaps) are not remembered, and one longer than 200 ms
   is dropped. `extend` is `1.25 × (longest remembered dip) + 1 ms`, at most 150 ms
   (`kExtendMaxSeconds`); the memory forgets 40 ms per second. The dip tracker restarts at each
   opening and waits until the level is back over half its 50 ms maximum, so a note's own attack
   is no dip.
4. **The slow fall that becomes a stop.** If `fall` is over 50 ms the fall is a decay or a beat
   trough: the wait is `max (hold + extend, min (1 s, 2 × fall))` (`kRideRatio` 2,
   `kRideMaxSeconds` 1 s), counted from where the level stops falling. It restarts whenever `lev`
   makes a new low 1 dB under the previous mark, and everything restarts if `lev` climbs 6 dB back
   from its lowest point. When `lev` drops 6 dB under its own 50 ms maximum the ridden fall
   turns into a stop from there: the wait becomes `hold + extend`. The gate closes at the latest
   1 s after the crossing.
5. **The in-band stop.** A stop onto a floor just under the threshold never crosses `close`. So
   inside the band, a level 6 dB under the peak of the **current excursion** over the open level
   (measured within 50 ms, not against a louder earlier note's maximum) and 6 dB deeper than any
   trough already ridden (`kBandMarginDb`; the trough depth forgets 10 dB per second) is a stop. It
   closes after `hold` plus one observed beat period (0 on a steady note, at most 150 ms), **unless
   the level climbs back to 5 dB under that peak first** (`kBandCancelDb`; shallow fast beats spend
   about the hold between 6 and 3 dB under their crest).
6. **The beat period.** Successive dips 6 dB under the level's maximum (re-armed within 3 dB of
   it) are timed; two intervals in a row within 25 % of each other and 25 to 150 ms long set the
   beat period (faster is a low note's own crests). It forgets 40 ms per second, and **every close
   forgets it** (a palm-mute gap is not a beat; a 55 Hz muted note's own decay otherwise showed a
   steady 62 ms "beat" that held later gaps open).
7. **The deadline.** Whatever the stop rule says, the gain is at the range `hold + 250 ms`
   (`kDeadlineSeconds`; 1.5 × the release if that is slower, i.e. a release over 167 ms) after the
   level last was over the open level. The cap on the gain stays at 0 dB until the last 80 ms,
   then falls `−90 dB × u²` (u from 0 to 1 over those 80 ms: gentle, then steep), so chord
   troughs up to about `hold + 190 ms` keep the gain over −6 dB and the floor is heard at most
   `hold + 250 ms`. **Smooth only falls inside that window**: while the cap is under 0 dB its
   gain is held at or under its own previous value and under the cap, because a fresh closed
   estimate landing low on one instant of the level would otherwise slew back up and read as a
   reopening.
8. **The stop tail.** Under −60 dB a release falls at most 30 dB per 40 ms
   (`kStopTailFromDb`, `kStopTailDbPerSecond` 750). A beat trough read as a stop therefore costs
   a partial dip and the crest reopens the gate: at the default 30 ms release the gain reaches
   −60 dB in 30 ms and the range 40 ms later.
9. **Reopening after a deadline close.** After the deadline has closed the gate, a level over
   `open` counts as over it only if it is also **6 dB over the lowest floor estimate since that
   close** (`kReopenRiseDb`). Why a rise: noise whose 12 ms peak reads 1 to 3 dB under the
   threshold rides into the band, the deadline closes the gate, and the next peak over `open`
   reopened it at once (version 5: 12 changes in 5.5 s, 80 in 30 s; the median reading 2 dB under
   the threshold); a floor's peaks cannot reach its own estimate + 6 dB, a pick or a new note can.
   Why the lowest estimate and not the running one: the floor estimate follows the level with a
   200 ms lag, so a rise slower than about 30 dB per second never gets 6 dB ahead of it. Judged
   against the running estimate, swells of 2, 3 and 5 s after a natural decay never opened the gate
   (model, coordinator's measure); against the lowest estimate since the close, which only moves
   down while the gate is closed, all ten opened 4 ms after the level crossed `open` (the engine, row
   X21: over −1 dB at most 4.0 ms after the reading crosses `open`, 20 cases). A level that
   does not pass the test counts as under `open` everywhere: the deadline clock, the ride estimate
   and the state machine.
10. **No deadline before the first opening after a reset.** Tight is at the range already; Smooth,
    never having opened, has nothing to ride and follows its expander as version 3 did (without
    this its gain would sit at the range from the deadline on).
11. **Opening** follows the onset: the rise lasts half the time the level took to climb from
    `close` to `open`, between 0.2 ms and 5 ms (§4.1.2).
12. **Smooth's closed estimate** (point 7's ride estimate, the dB level averaged with a time
    constant of half the time since the level fell under `open`, up to 0.2 s): at a close it
    **continues** that estimate instead of restarting on one instant of the level, and Smooth
    follows its expander law at once until the deadline (after it, at most 1 dB per 10 ms).

```
target_dB = 0                                                 if OPEN
          = R                                                 if CLOSED and Tight
          = clamp(2 * (F - 20*log10(close)), R, 0)            if CLOSED and Smooth   (S-1 = 2)
          // F = floor estimate, dB of lev while closed (below)
gr_dB: closing    gr_dB -= min(gr_dB - target_dB, 60/(release*fs))              // dB-linear release; at most 750/fs under -60 dB
       opening    gain rises over the onset-dependent length, linear at 0.2 ms to smoothstep from 0.4 ms
       deadline   gr_dB = min(gr_dB, deadline_dB(sinceOpen))                    // point 7
gain  = (R <= -89.95 and gr_dB <= -89.95) ? 0 : 10^(gr_dB/20)
```
`R` = `rangeDb`. **Smooth** under the threshold: the floor estimate F is the dB level averaged
with a time constant of half the time since the level fell under the open level (continued
across the close, point 12), up to 0.2 s (`kFloorTauMaxSeconds`); once the deadline window is
over, the expander gain follows its target **at most 1 dB per 10 ms** (`kFloorSlewDbPerSecond`
100), so a floor that jumps or wobbles moves no faster. Right after a reset the estimate and the
gain take the level as it comes until the window has filled once (no floor yet to protect).
Version 2 let the gain move 12.4 dB in 10 ms on a +5 dB floor step; now the largest 10 ms move
is 1 dB (check E1) and 0.33 dB on hiss and hum (check X14).

**Range and the other parameters.** `range` is smoothed per sample, a one-pole in dB with a
10 ms time constant (`kRangeSeconds`; it snaps once within 0.001 dB), so a range automation
cannot step at block rate (check G1: largest gain step 0.00004 per sample against 0.0121 in
version 2; bar 0.001). The slew limiter and the release rate **are** the smoothing for
every other gate parameter (threshold, hold and sidechain changes cannot click because
`gr_dB` can only move at the release rate and the opening rate) `[study: R1 dB-linear ramp]`.

**Why the closing rule depends on how fast the level fell.** A wait that was a fixed hold made
Tight chatter on decaying chords and beating notes (49 of 328 verifier cases). Version 2 waited
according to how far the level had fallen from a recent peak. The noise floor decides how far a
stopped note can fall, so with the floor within about 22 dB of the notes Tight stayed open
418 to 430 ms after a stop, closed 0 of 8 palm-mute gaps, the hold knob stopped meaning the
wait, and the remembered peak brought chatter back behind a louder note. A hand stop takes 1 to
44 ms to cross the band whatever the floor, a decay at 60 dB per second takes 100 ms, so the
band time tells them apart. Version 3 rode a slow fall for up to 1 s and could latch open on a
floor that kept the level in the band (a 30 s latch, verifier 3); the deadline replaced that.

**What 5.1 changed** (against version 5):
- Reopening after a deadline close needs the 6 dB rise (point 9).
- Only dips the gate rode open teach trough depth and length (point 3); the closed-trough memory
  and the gentle tail of version 3 are gone.
- The dip tracker restarts, blocked, at each opening (point 3).
- The in-band stop measures from the current excursion's peak and cancels 5 dB under it, was 3
  dB under the 50 ms maximum (point 5).
- Version 4's stop tail replaces the gentle tail (point 8).
- Every close forgets the beat period (point 6).
- Smooth's closed estimate continues the ride estimate (point 12).

**Stated costs and limits (version 5.1; §8 D-59 to D-64).**
- A slowly decaying note is faded out `hold + 250 ms` after it falls under the open level, even
  if it is still inside the 6 dB band (D-60).
- Palm-mute gaps of 40 to 52 ms at hold 10 end at −18 to −23 dB, not fully closed (D-61).
- Four cases where a loud note is stopped 0.1 s after a quiet chord that sits about 6 dB over the
  threshold keep one or two more dips than the chord alone (D-62).
- After a deadline close a note less than 6 dB over a floor that sits inside the band does not
  reopen the gate (D-63).
- A decay with the threshold 3 dB over the hiss: 346 ms in one of 76 cases (D-64).
- A stop on a chord whose partials beat fast waits up to 150 ms more (`extend`).
- Fast, deep beat troughs (a trough that falls as fast as a stop) dip partway and the crest
  reopens the gate; none reaches the range within 60 ms of its start (rows B, C).
- The sidechain filter's cost on bass: see the table in §4.1.2 (rows F) and §8 D-35.

The trade-off, from check C (two tones 110 and 196 Hz beating, troughs 6, 12 and 20 dB,
thresholds −40 and −55 dBFS, decaying through the threshold), as the deepest dip of the gain and
its length (coordinator's full run on the merged build; −240 dB is the range, mute):

| trough | 0.5 Hz | 1 Hz | 2 Hz | 4 Hz | 8 Hz | 15 Hz |
|---|---|---|---|---|---|---|
| 6 dB | no dip | no dip | no dip | no dip | no dip | no dip |
| 12 dB | −240 dB, 2555 ms | −240 dB, 1133 ms | −240 dB, 627 ms | no dip | no dip | no dip |
| 20 dB | −240 dB, 185 ms | −240 dB, 100 ms | −240 dB, 592 ms | no dip | no dip | no dip |

The dips are at 0.5 to 2 Hz, where a trough is long enough for the gate to reach the range; none
reaches it within 60 ms of its start and none clicks on reopening (the bar). Beats of 4 Hz and
faster are ridden. In row X10 to X12 and B1 no case dips at the range inside 60 ms (§4.1.10).

**Detector change against the study (load-bearing):** the prototype held the peak for
12 ms and then let it fall with a 3 ms exponential; that fall costs 0.35 ms per dB of
level above threshold and up to 12 ms of jitter, which is why it closed in 30–39 ms at
−40 dB and 38–47 ms at −60 dB, missing its own 35 ms target `[ver: gate/verification.md
V:31, V:37]`. This spec uses an exact sliding-window maximum instead: after the last
crest leaves the window the detector reads the true current level at once, so the close
time no longer grows with level (the lower-of-two reading above keeps the sidechain
filter's ringing from adding to it) `[choice, sim: §4.1.10 numbers]`. Nwin = 12 ms still covers the
half period of 42 Hz so the held value cannot droop between half-cycles of a low note.

`state` for the meter: 2 if OPEN and the filtered peak `pk` ≥ open; 1 if OPEN and `pk` < open
(hysteresis band or waiting); 0 if CLOSED. GR meter = max over the block of −gr_dB, clamped to
[0, 90] (90 when muted); level meter = max over the block of 20·log10(pk), floor −120.

**Start state after `reset()`:** CLOSED, `gr_dB = R`, the wait and fall counters 0, the dip
memories and the beat period empty, no excursion peak, no deadline close recorded (so no
reopening test), no deadline until the first opening, all three windows empty, filter state 0,
the floor and ride estimates restarted, curve ring filled with the corresponding floor gain (0 if
mute). A gate that re-enters the chain starts closed and opens on the first note within 1 ms (G8:
0.188 ms to −1 dB).

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
(0.3 s + {0, 0.37, 1.13, 2.9} ms). Defaults unless stated (Tight, release 30, hold 10
(Smooth: release 80, hold 10), range −90, HPF 75, 4th order). The figures in brackets are
**measured** on the merged build (version 5.1) by the coordinator's full run of
`check_gate.py` unless a figure says otherwise; the bars are what the script enforces. The G
bounds were first set from a numpy model of the first §4.1.5 at 48 kHz `[sim]`. Rows I1 to I5
state the closing rule as invariants on seeded random input; rows A to E and R are the
verifiers' signals; rows TA to TD score the targets of the version 5 specification on seeded
draws; rows X are the other proofs. Rows whose name starts with `info` and rows marked INFO
print their figures and never count as FAIL.

| # | Check | Pass |
|---|---|---|
| G1 | Classic null (§2.11) incl. deferred apply on a mono buffer | 0 differing samples |
| G2 | Close time, 1 kHz, note −20 dBFS, thr −30/−40/−50/−60 | ≤ 35 ms each [31.6 / 31.8 / 31.9 / 31.9] |
| G3 | Close time, 1 kHz, note −6 dBFS, thr −30/−40/−50 | ≤ 36 ms [31.8 / 31.9 / 31.9] |
| G4 | Close time, 1 kHz, note −6 dBFS, thr −60/−70; note −20 dBFS, thr −70 | ≤ 44 ms [31.9 / 32.0 / 31.9] (sidechain filter ringing; with HPF off ≤ 35 ms, measured the same three figures) |
| G5 | Close time, 82.41 Hz, note −20 dBFS, thr −40/−60 | ≤ 50 ms [31.8 / 31.9] (study limit 50 ms) |
| G6 | Close time, 82.41 Hz, any of thr −30…−70 × note −6/−20 | ≤ 53 ms [max 32.0] |
| G7 | Close time never shorter than | ≥ 18 ms in every cell (no chopping of decays) [min 30.2, Tight + Smooth, 40 cells] |
| G8 | Open: 1 kHz −20 dBFS onset after silence, thr −40 | gain ≥ −1 dB within 1.0 ms [0.188] |
| G9 | Chug (`chug.wav`: 82.41 Hz + harmonics, 75 ms notes every 150 ms, white noise −66 dBFS RMS), thr −40, Tight | median gap gain (last 10 ms before each onset) ≤ −30 dB [−73.8, worst −73.7]; first 10 ms of each note ≥ −1 dB [−0.01] |
| G10 | Same, Smooth (release 80) | median gap gain between −14 and −25 dB [−19.7, worst −17.8]; first 10 ms ≥ −0.5 dB [−0.00] |
| G11 | Hover: 1 kHz at −40 dBFS ± 3 dB at 3 Hz, thr −40, 6 s | 0 crossings of −6 dB after 0.5 s [0, Tight and Smooth] |
| G12 | Step-down from −20 dBFS to L, thr −40 (L = −45 … −47 lands in the band, so it is a stop) | closed for L = −45, −45.5, −46.5, −47: −20 dB within 35 ms, then mute [32.0 ms each] |
| G13 | Smooth static: steady 1 kHz at −50/−55/−60 dBFS, thr −40 | −8.0 / −18.0 / −28.0 dB ± 0.5 [−8.03 / −18.03 / −28.03] |
| G14 | Range: Tight, range −20, closed | gain −20.0 ± 0.1 dB; range −90 → exactly 0 [−20.000 dB; 0] |
| G15 | SC HPF: 50 Hz hum −36 dBFS + chug, thr −40 | HPF 75: median gap ≤ −30 dB [−60.6]; HPF off: gate stays open, median gap ≥ −1 dB [0.0] (documents the benefit, V:42). INFO: hum −44 dBFS, inside the band, holds nothing open now (HPF 75 and off both −73.7 dB); thr −50 is not enforced, no gate can meet it (the chug's own noise peaks at −55 dBFS, over the −56 dBFS close level; HPF 75 −44.3 dB, off 0.0 dB) |
| G16 | Deferred alignment (processor test, §6.1): 1 kHz burst through gate → amp on a model forced to resample (session 44.1 kHz) | gain edge and audio edge coincide within 1 sample. **As integrated:** the curve is delayed by exactly the reported latency (checked to the sample); against the audio it lands within 4 samples (measured 1.9 at 44.1 kHz, 3.0 at 96 kHz), the reported latency's own error (§4.1.7). The script's proxy (`lag = delay`) is bit-exact at D = 1 / 37 / 512 / 3000, blocks 64 / 512 |
| G17 | Rates | G2, G5, G9 also at 44.1 and 96 kHz, same bounds + 1 ms (36 ms and 51 ms) [31.6 to 31.9 ms; chug −73.8 / −73.7 dB] |
| G18 | Meter: thr −40, silence after a note | state 0 within 100 ms of the note end [21.8 ms]; 2 on the first sample above threshold [sample 1]; GR reads 90 when closed with range −90 [90] |
| G1 (range) | Range smoothing: closed, range ramped −90 → −10 dB, blocks 64 and 512 | largest gain step ≤ 0.001 per sample [0.00004; version 2: 0.0121 at block 512] |
| I1 | Bounded close: 360 seeded cases (Tight's release, both modes; floors that wobble or pulse, decays, stops, beats, chords, steps; every rate, block size, channel count, threshold, hold, release and range) and 179 Smooth cases | under the open level for hold + 250 ms the gain is over the range by at most 0.01 dB [0.000]; Smooth over its expander law by at most +0.1 dB [+0.00] |
| I3 | Reopen over the open level only: the same cases | no rise of the gain under the open level after an opening [0.00e+00]; Smooth never over max(release, expander law) [0.00e+00] |
| I4 | Stop closes: 240 random hand stops from any level (inside the band too), onto a floor 8 to 90 dB under or a threshold 3 to 4 dB over the floor, 44.1 to 192 kHz, blocks 1 / 37 / 512, mono and stereo | −20 dB within 12 + hold + release/3 + 3 ms of the end of the fall [worst 37.4 against 41.6; from inside the band worst margin +8.3 ms; threshold 3 to 4 dB over the floor +4.2 ms] |
| I5 | No click: the same 360 cases | the gain never falls faster than the release knob or the deadline's quadratic at that depth, nor rises faster than the 0.2 ms linear opening [0.00e+00 dB per sample; 0.00e+00 per sample] |
| X1 | Smooth close: release 80, all 20 cells | after a note Smooth closes at most 1 ms later than Tight plus (hold difference, 0) + 20 dB × (80 − 30) / 60 ms (16.7 ms), i.e. the expander law never holds the gain up [−0.02 ms; worst 48.6 ms at 1 kHz −6 dBFS thr −70] |
| X2 | Palm-mute gaps, Tight chug thr −40, 75 ms notes, gaps 50 / 75 / 100 ms | median gap gain ≤ −30 dB each [−46.7 (worst −46.4) / −73.8 (−73.7) / −120.0 (−120.0)] |
| X3 | Decay not chopped: −6 dBFS exponential decays, 82.41 Hz and 1 kHz, tau 0.4 / 1.5 s, Tight and Smooth, thr −40 | gain ≥ −0.1 dB while the 4th-order-filtered envelope is at least 0.5 dB over the close level [0.000 dB] |
| X4 | No chatter: white noise −62…−46 dBFS RMS, thr −40 | at most 2 open/close changes in 5.5 s [2, at −52 dBFS RMS; version 5: 12] |
| X4 long | 30 s of white noise whose median reading sits −3 … +3 dB around the threshold, Tight, 42 cases | at most 2 changes per case [2; per offset −3 / −2 / −1 / 0 / +1 / +2 / +3 dB: 2 / 2 / 1 / 0 / 0 / 0 / 0; version 5: 80 in one case] |
| X21 | Swell after a decay: 110 Hz note decaying 20 or 60 dB/s until the deadline closes the gate, then a swell linear in dB from the hiss (−75 dBFS) to −20 dBFS over 0.5, 1, 2, 3, 5 s, Tight and Smooth, thr −40 (20 cases) | gain over −1 dB within 10 ms of the level reading crossing the open level, Tight closed before every swell [4.0 ms at worst, Tight, 20 dB/s, 0.5 s swell] |
| X5 | Pick attack: first 5 ms of each chug note and of an abrupt 1 kHz onset (8 phases) | level lost ≤ 1 dB [chug −0.07; abrupt −0.18] |
| X6 | Mode switches: all 6 transitions, 3 steady inputs, blocks 64 / 512 | added gain step per sample ≤ 1.5 × a full-scale 20 ms linear fade [4.13e-04 against 1.56e-03] |
| X7 | Rates 22.05 and 192 kHz | G2 and G5 cells, bounds 36 ms and 51 ms [31.6 to 31.9 ms] |
| X9 | Low-note decay: 41.2 / 46.25 / 82.41 Hz with harmonics, tau 0.7 / 1.5 / 3 s, thr −40 / −55 | at most 2 open/close changes (one open, one close) [2; deepest dip that reopens 0.00 dB] |
| X10 | Chord decay: 14 verifier chords × thr −30…−65 step 5; the 4 hardest chords also at 22.05 / 44.1 / 96 / 192 kHz (`--quick`: 44.1 and 96 kHz) | every dip of the gain that comes back: none at the range within 60 ms of its start, no reopening click (D2's measure); dips are counted and printed [0 of 112 dip under −1 dB; 0 of 32 at each of the four rates] |
| X11 | Beating: AM notes (82.41 and 110 Hz, depth 0.2 / 0.33 / 0.5 / 0.7 = 3.5 to 15 dB, rate 1 / 3 / 7 / 15 / 30 Hz, thr −40); two strings a few hertz apart, thr −30…−60 | the same bar [AM: 5 of 40 dip under −1 dB, 5 under −6 dB, longest 1160 ms, at most 3 in one case; strings: 6 of 32, 5 under −6 dB, longest 907 ms, at most 13 in one case; both deepest −240 dB, i.e. the range; none inside 60 ms, no click] |
| X12 | Single decays: 41.2…330 Hz × tau 0.15…2.5 s × thr −20…−70 | the same bar [0 of 144 dip under −1 dB] |
| X13 | Palm-mute gaps by gain energy over the last 10 ms of each gap, Tight thr −40 | 50 ms gaps: median ≤ −36 dB, worst ≤ −30 dB [−43.4, worst −43.1]; 40 ms gaps: median ≤ −20 dB [−23.4, worst −23.1]. INFO, the 30 / 40 / 50 / 75 / 100 ms table: −4.2 / −23.4 / −43.4 / −73.2 / −97.3 dB (worst −4.1 / −23.1 / −43.1 / −73.1 / −97.0); gaps over 50 Hz hum −44 dBFS: 40 ms −23.3 (worst −23.0), 50 ms −43.4 (worst −43.0) |
| X14 | Smooth floor: noise, hum and buzz under thr −40, 44.1 / 48 / 96 kHz | largest gain move in 10 ms ≤ 0.5 dB, largest one-sample step ≤ 0.1 dB [0.33 dB; 0.001 dB] |
| X15 | Pick attack: picked notes 12 / 20 / 34 dB over thr −40, 82.41 / 196 / 1000 Hz | energy lost in the first millisecond ≤ 3 dB [82.41 Hz −1.75, peak of the first 5 ms −3.48; 196 Hz −1.68; 1000 Hz −0.82] |
| X16 | Opening click: Tight, splatter at 82.41 / 200 / 1000 Hz | no more than the old 0.5 ms / 60 dB dB-ramp's [−30.8 (old −29.8) / −32.4 (−31.0) / −42.3 (−36.4) dB] |
| X17 | Non-finite input: NaN / ±Inf bursts in every mode and across switches, blocks 1 / 37 / 512, mono and stereo | gain equals the zero-substituted render bit for bit; meters finite [90 pairs] |
| X18 | Oversized blocks: block 512 on a 256-sample prepare | audio unchanged; meters those of the same input in normal blocks (Classic: open, 0 dB) |
| X19 | Long curve delay: deferred, D = 8193 / 30000 / 65535 | bit-exact against the undelayed render, D samples later |
| X20 | Low-note threshold (INFO): sine needed to open at thr −40, HPF 75, a tone that rises over 0.3 s (steady) and one that starts at a crest and must hold the gate open 0.4 s later (abrupt) | 41.2 / 73.42 / 82.41 / 110 Hz: +20.8 / +3.4 / +1.7 / +0.2 dB steady, +20.9 / +3.4 / +1.7 / +0.2 dB abrupt (version 3 read the abrupt tone +15.3 / +0.2 / +0.2 / +0.0 dB; the steady 20.8 dB at 41.2 Hz is the 4th-order 75 Hz filter's price; a low E needs 1.7 dB, was 3.9 with the 2nd-order 90 Hz filter). The table of rows F in §4.1.2 gives the same figures at HPF 75, 40 Hz and off |
| A1 | Stop close: hand stops of notes (41.2 / 82.41 / 196 / 1000 Hz), floor 8, 12, 20, 40, 60, 90 dB under the note, falls of 5 / 15 / 30 ms, Tight; and at 44.1 / 96 / 192 kHz, blocks 1 / 37 / 512, mono and stereo | closed (−20 dB) at most 12 + hold + release/3 + 3 = 35 ms after the level leaves the close level [worst 32.0 (82.41 Hz, floor −8, fall 30 ms); after the end of the fall 31.4; the rates and blocks 31.0] |
| A2 | Hold one for one: hold 0 / 6 / 20 / 50 / 100 / 250 ms, floors 8 to 90 dB under | \|wait(hold) − wait(0) − hold\| ≤ the larger of 10 % of hold and 3 ms [largest 1.3 ms] |
| A3 | Palm mutes over a floor 12, 18, 22, 26, 30, 40 dB under the notes, gaps 50, 75, 100, 150, 250 ms, threshold floor + 8 dB | every gap closed [240 of 240; version 2: 140]; the first 10 ms of the notes 12 dB or more over the threshold keep −1 dB or better [−0.07 over 154 notes] |
| A4 | No hang: soft notes 8 to 40 dB over a hiss floor, stopped by hand | closed within 60 ms of the stop [31.4 to 45.9; version 2: 418 to 430] |
| A5 | Palm mutes near the hiss: chug over white noise, hiss peak −52.9 dBFS, threshold 3 to 5 dB over it | median gap gain over the last 10 ms ≤ −25 dB (75 ms gaps) and ≤ −69 dB (100 ms gaps) [75 ms: −73.6 / −73.6 / −73.7 at thr −50 / −49.89 / −48.89; 100 ms: −120.0 each; version 5: 100 ms −24.8, worst 0.0] |
| B1 | Verifier 2's chords: 16 chords × thr −30…−65 × decay times, rates 44.1 / 96 / 192 kHz, Smooth, holds 0 / 10 / 250 ms (382 cases) | the X10 bar: none at the range within 60 ms, no reopening click [0 of each; 4 of 382 dip under −1 dB, 3 under −6 dB, deepest −50.3 dB, longest 30 ms; the three under −6 dB are hold-0 cases]. Also 256 cases at the default hold 10: no dip at all [0 of 256] |
| info B1 hold 6 | the same 382 cases with hold 6 in place of 10 | INFO [9 of 382 dip under −1 dB, 5 under −6 dB, deepest −50.3 dB; 0 at the range inside 60 ms, 0 clicks] |
| B2 | A quiet decaying chord behind a louder note, hold 10: 104 cases (g3: loud −3 / −12 dBFS 0.25 s before; Tight, Smooth, holds 10 / 20 / 50) and 72 cases of the second sweep (g3b: +12…28 dB, 0.1 to 0.6 s) | the X10 bar, and no case with more dips under −6 dB than the same chord alone, except in g3b at most 4 cells, all at gap 0.1 s (§8 D-62). g3 [0 worse; 4 of 104 dip under −1 dB, deepest −71.7 dB, longest 51 ms; 0 clicks]. g3b [4 worse (E2+G#2 thr −36 +28 dB; C2+Eb2 thr −36 +12 / +20 / +28 dB, gap 0.1 s); deepest −14.5 dB, longest 12 ms, at most 2 in one case; 0 at the range inside 60 ms; 0 clicks]. Version 2: 22 of 104 worse |
| info B2 hold 6 | the same g3 and g3b cases with hold 6 | INFO [g3: 0 worse, 15 of 104 dip under −1 dB, 4 cases with a reopening click, worst +8.2 dB; g3b: 4 worse, 30 of 72 dip under −1 dB, 9 clicks, worst +7.1 dB]: hold 6 is where chords click (§4.1.2) |
| C | Two tones 110 / 196 Hz beating 0.5 to 15 Hz, troughs 6 / 12 / 20 dB, thr −40 / −55, decaying through the threshold | the X10 bar [22 of 72 dip under −1 dB, deepest −240 dB (the range), longest 2555 ms, at most 5 in one case; 0 at the range inside 60 ms; 0 clicks]. The trade-off table of §4.1.5 is printed |
| C decay tail | INFO: how long the floor is heard (over thr − 20 dB) after a note decaying at 5 … 200 dB/s, Tight and Smooth, floor 8 and 3.5 dB under the threshold | INFO [Tight, floor 3.5 dB under: 331 ms at 5 dB/s, 324 at 10, 224 at 20, 225 at 40, 209 at 60, 214 at 80 and 120, 8 at 200; floor 8 dB under: 0 ms at 5, 10, 20 and 200 dB/s, 61 / 95 / 128 / 151 ms at 40 / 60 / 80 / 120. Smooth 1239 to 4092 ms: its gain follows its expander law, not the range] |
| D1 | Pick front: picks 20 and 34 dB over the threshold at 41.2 / 61.74 / 82.41 / 196 / 1000 Hz, and 82.41 Hz at 44.1 / 96 / 192 kHz | the first millisecond keeps −1.5 dB or better [−1.33] |
| D2 | Opening click: swells of 150 and 600 ms, 41.2 / 82.41 / 110 / 220 Hz, thr −30 / −40 / −50, sine and plucked string | above 3 kHz at the opening, re the note: sines ≤ −50 dB [−72.6]; strings ≤ −50 dB or the dry string's own level +1 dB [−38.4, where the dry string reads up to −38.4] |
| E1 | Smooth floor slew: hiss steps +5 / +8 / −8 dB, wobbles ±4 / ±7 dB at 4 Hz, crackle under thr −40, 44.1 / 48 / 96 kHz, block 64 | largest gain move in 10 ms ≤ 1.01 dB (the slew) [1.00; largest one-sample step 0.0023 dB; version 2: 3.6 / 12.4 dB]; a note after the floor reaches −1 dB within 1 ms of the onset [0.19] |
| F | Sidechain filter on bass and hum (INFO): level to open at HPF 75 / 40 / off for 30.87, 41.2, 82.41 Hz, steady and abrupt; the true peak when a decaying dull note shuts the gate; the hum reading | the tables of §4.1.2 |
| R1 | Pulsing floors: 42 cases, 7 floors (spikes, hum AM, hiss AM, crackle, buzz AM, hiss) × tone or chord × 20 / 60 dB/s, both modes | Tight: the floor is over the range at most hold + 250 + 15 = 275 ms after the note's own level is under the close level [189 ms at worst: hum AM, tone, 60 dB/s; version 3: never in 30 s]. Smooth: within 1 dB of its floor-alone setting, last 250 ms ≤ +1 dB re it [2120 ms at worst; last 250 ms −1.0 dB] |
| R2 | Hum under a chord: 37 cases, 50 / 60 Hz hum 10 to 30 dB under, threshold 6 to 10 dB over it | the same bar [Tight 183 ms at worst (60 Hz, 10 dB under, thr +6, t60 2.5); version 3: 1.3 to 4.2 s. Smooth 833 ms; last 250 ms −0.6 dB] |
| R3 | Glides and volume down: 24 cases, an octave glide or the knob to zero over 0.3 to 3 s | the same bar [Tight 193 ms at worst (glide 1 s, floor 36 dB under); version 3: 0.28 to 1.2 s. Smooth 1035 ms; last 250 ms −1.1 dB] |
| R4 | Staccato 180 bpm: 38 ms gaps, floor 15 dB under, thr +8 | INFO [median gap gain: Tight hold 0 −41.1 dB (16 of 16 gaps under −20), hold 6 −29.1 (16 of 16), hold 10 −21.1 (11 of 16), hold 20 −3.1 (0 of 16); Smooth hold 10 −7.2 (0 of 16)] |
| R5 | Tremolo picking: 18 cases, strokes 12 / 16 Hz falling 3 to 10 dB, thr −20…−5 re peak | the X10 bar, and the hand stop after it closes sooner than version 3's 880 to 993 ms [2 of 18 dip under −1 dB, 1 under −6 dB, deepest −26.7 dB, longest 18 ms; 0 at the range inside 60 ms; 0 clicks; the stop closes in 102 ms at most] |
| R6 | Latch bounds: 21 cases, 7 floors × tone 20 / 60 dB/s and chord 20 dB/s, Tight, 10 s | the gain is over −6 dB at most hold + 191 + 1 = 202 ms after the level was last over the open level [200.6 (spikes −20, tone 20 dB/s); counted from the note's own close crossing 0.13 s]; the floor is heard over −20 dB at most 300 ms after the note is under the floor [116 ms] |
| TA1 | The g1 chord grid (16 chords × 8 thresholds × 2 decays, the B1 signals), hold 10: Tight 256 cases, Smooth 277 (21 extra at t60 4) | cases with chatter (a gain excursion under −6 dB that comes back within 250 ms): Tight ≤ 8 [0; model 0], Smooth ≤ 4 [1: A1+C2 thr −35 t60 2.5, one 2.9 ms dip to −8.0 dB; model 1]; no legitimate close (≥ 250 ms) [0] |
| TA2 | 120 seeded chords and beats, thr −70…−25, floors 8 to 40 dB under | no chatter [0] and no dip under −12 dB inside a trough under the open level shorter than 100 ms [0; deepest gain there 0.0 dB] |
| TB1 | 86 seeded Tight decays into wobbling floors 3 to 14 dB under the threshold | the longest stretch under the open level with the gain over −60 dB, minus the hold, ≤ 236 ms [235.3; model 235] |
| TC1 | 120 seeded palm-muted chugs: 41 to 110 Hz, gaps 50 to 250 ms, floors 12 to 40 dB under, thr +6…+12 | every gap under −20 dB over its last 10 ms [120 of 120; worst gap end −36.4 dB; model −36.4] |
| TD1 | 120 seeded chords held 0.6 to 2.5 s, stopped by hand (fall 5 to 30 ms) onto hiss 20 to 60 dB under | closed (−20 dB) within 12 + hold + 150 + release/3 + 3 ms of the end of the fall (one remembered trough or beat period, at most 150 ms, on top of I4's bound) [0 over; worst 161.9 against 185 (E2+F2); worst close 162 ms; model 162] |

Rows X10 to X12, B, C and R5 are the restated form of the verifiers' rows: any reopening dip of the
gain is bounded rather than forbidden (a wait that never lets the gate dip would hold the floor
open; version 3 allowed none after a slow fall and latched). The bar is the one `dip_summary` in
the script applies: none at the range within 60 ms of the dip's start, no reopening click (the
output's peak above 3 kHz from 2 ms before the rise to 10 ms after it, over the larger of the dry
signal's own there + 1 dB and the note's peak − 50 dB). The table lists every row id
`check_gate.py` has (including the common checks of §2.11 run per mode), with the bar the script
enforces. `check_gate.py --rows=I,A,B,...` runs only the named rows (`I` the invariants, `A` to
`G` and `R` the closing rule's other rows, `T` its targets on seeded draws; `X` adds X10 to X20 and
the other verifier rows) and `--verbose` lists every dip that rows B and C classify; the full run
is the acceptance.
Measured, the close times of G2 to G6, G17 and X7 are 31.6 to 32.0 ms in every cell
(12 + hold + release / 3 = 32), well inside their bars. Full run on the merged build: **130
checks, 0 FAIL** (coordinator's run; 102 at the version 3 commit, 79 at `81fd404`). G16 in the
script is a proxy (`lag = delay`: the output must be the undelayed render, D samples later, bit
for bit, at D = 1 / 37 / 512 / 3000, blocks 64 / 512); the processor test is the integrator's.
Not in the table: the verifiers' own scripts on this build read 0 of 256 chord cases with a
dropout at hold 10 and 2 of 256 at hold 6 (chords, g1); 72 slow swells from the hiss (1.5, 4 and
8 s; dB-linear, amplitude-linear, stalled and exponential growth; 44.1, 48 and 96 kHz; Tight and
Smooth) all open with 0 dips under −6 dB after the opening; the hold knob moves the close one for
one over 0 to 250 ms (12 / 18 / 32 / 62 / 112 / 262 ms for holds 0 / 6 / 20 / 50 / 100 / 250, at
floors 18 to 90 dB under the note); 30 s of listening after a decaying note over a pulsing floor
leaves Tight at unity for 0.0 to 0.1 s and Smooth for 0.1 to 0.4 s, and Tight at the range for the
rest.

fxbench keys: `mode threshold release hold range sc_hpf delay deferred on lag prepare_block`
(`delay` = curve delay, applied with `apply` after `measure` as the processor does; `lag`
delays the deferred audio as the amp would; `prepare_block=N` re-prepares for N-sample
blocks, for X18). `--trace` writes the applied gain per sample (linear); `--meters` writes per
block the state, the gain reduction in dB and the detector level in dBFS.
`check_gate.py` takes `--rows=A,B,...` and `--verbose` (§4.1.10).

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
                                                  // [study: R3, one-pole; Shadow Hills 90 Hz evidence];
                                                  // its coefficient is set the moment the filter switches
                                                  // on (reset() keeps the old one), not at the next
                                                  // 16-sample control tick (C31)
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
the amount M through a 20 ms one-pole (no step). **M is a fixed gain, calibrated so that a steady
signal peaking at −10 dBFS comes out at the level it went in (C20); it does not follow the
playing.** A steady −20 dBFS signal comes out 4 to 10 dB louder, a −3 dBFS one 3.5 to 7 dB quieter,
and a plucked chord peaking at −10 dBFS 2 to 12 dB louder in rms (its decay is reduced less than
its peak) (`Compressor.h`, `CompParams::autoMakeup`; §8 D-38).

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
  **Squash keeps this feedback recovery above the threshold**, and it is what separates it from
  Clean (Clean's recovery is the knob in every cell). The release is the knob (t63) when the signal
  falls below the threshold, and while the output stays above it; a drop that leaves the input above
  the threshold but takes the output below it recovers sooner. Measured t63 in ms at knob 150 ms
  (Clean: 149 ms in every cell), T −24, knee 6, from −4 dBFS (copied from `Compressor.h`, held within
  10 % by C26b):

  | ratio | to −8 dBFS | −10 | −14 | −18 | −22 |
  |---|---|---|---|---|---|
  | 2 | 149 | 149 | 142 | 111 | 127 |
  | 4 | 149 | 121 | 75 | 93 | 127 |
  | 8 | 83 | 53 | 65 | 92 | 127 |
  | 20 | 38 | 44 | 64 | 92 | 127 |

  (A recovery down to a third of the knob at R 8, a quarter at R 20.)
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
applying; an incoming Classic runs unheard until its envelope has risen (four attack time
constants, knob / 2π, plus 2 ms). **The detector and the gain computer run from the first
sample after a seed from Classic.** While the detector window (12.5 ms at most) refills, its
maximum can only be lower than the full window's, so only **attacks act**: an attack it calls for
is real, a release is not yet known (the release rates are zero until the window has filled). The
earlier rule froze the seeded gain for the window and an attack inside it passed unattacked. C29
(20 ordered pairs × 3 settings × gaps of 0 to 30 ms between the switch in silence and an attack):
+11.03 dB → +0.94 dB over the louder of the one-type renders (bar 1.0 dB). A request during a
fade hands over within the incoming side, or reverses the fade from where it is. While bypassed
(`skip`), a switch completes without a fade. `reset()` also clears the sidechain filter's
on flag, so a filter that was on is switched on again with its coefficient set (C31).

#### 4.2.7 Meter, latency, CPU

GR = max over the block of −g (dB, makeup excluded, ≥ 0); Classic from the shadow filter, whose
input is limited to ±1e6 like the other types' (a 3e38 sample overflowed its `env / threshold`
and gave an infinite reduction; the audio is untouched, C32). **The meter shows what the
compressor applies to its wet path**, as a hardware meter does: with mix under 100 % the net
change heard is smaller (decision, enforced by C30: 0.000 dB against the wet-path reduction).
**During a crossfade to or from Classic the meter and the trace blend the two engines'
reductions linearly with the fade gains**, as the output does (the old meter read the larger
engine, up to 9.4 dB over what was applied; C30: 4.52 → 0.000 dB). Latency 0. CPU per instance: Classic as today (≤ 0.3 %); new types ≤ 0.35 % (one `log10`
and one `exp2` per sample, deque amortised O(1)).

#### 4.2.8 fxbench keys

`type threshold ratio attack release makeup knee mix sc_hpf auto_makeup on` (`on=0` calls
`skip()`); `--trace=gain` writes the per-sample gain reduction in dB (−g, before makeup, a
positive number) (`CompFx::setGainTrace`); `--meters` writes the per-block gain reduction.
`--rt-check` counts allocations on the render thread only (§5.3, `tools/fxbench/README.md`).

#### 4.2.9 Acceptance (`check_comp.py`)

Time metrics on the gain trace: t63 = time for the gain in dB to cover 63 % of its change.
Burst = 1 kHz, 0.5 s at −10 dBFS (and −15 dBFS) after and before silence; release measured
from the burst end. "Residual" = everything except the fundamental (least-squares fit of
the fundamental and DC over the last 1 s of a 2.5 s steady tone, −10 dBFS, at 48 kHz) —
**not** harmonic THD, which ignores the inharmonic sidebands the study's hold produced
(V:77). Acceptance frequencies are deliberately not divisors of the sample rate:
41.2, 82.4, 98 Hz (and 100 Hz for comparison with the study). Settings T −20, R 4 unless
stated. Model values in brackets `[sim]`. Rows below C20 (C3b onward, C21 to C33 and K) are
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
| C26b | Squash recovery above the threshold: ratios 2 / 4 / 8 / 20 × drops from −4 dBFS to −8 / −10 / −14 / −18 / −22 dBFS, knob 150 ms, T −24, knee 6, attack 2.5; Squash and Clean | every cell within 10 % of the `Compressor.h` table (§4.2.5); none longer than the knob (≤ 153 ms); Clean within 2 % of the knob in every cell (measured: the difference between the two is the feedback, not an assumption) |
| C29 | A switch in silence, then an attack 0 / 5 / 10 / 20 / 30 ms later: a strummed pluck chord, every ordered pair of the five types, three settings (T −30 R 4; T −50 R 20 0.1/10; T −40 R 8, makeup 12, auto), 600 cases, with and without an earlier hand-stopped note | the output level in 2 ms frames over the 0.6 s after the attack is at most **+1.0 dB** over the louder of the render that stayed on the old type, the render that was always on the new type, and the new type's own render without the earlier note's history (its slow stage and memory start empty after a switch from Classic, which has neither to hand over: Opto is up to 3.7 dB louder at the onset 0.5 s after a note) (measured +0.94 dB; before the fix +11.03 dB) |
| C30 | The meter in fades and at mix 50 %: (a) DC input, −10 dBFS stepped to 0 dBFS at the switch, T −30, attack 30 / release 300, every switch with Classic (8), blocks 64 / 37 / 512; (b) mix 50 %, every new type, same signal | (a) every block's meter equals the largest reduction applied in it within 0.05 dB; (b) the meter equals −20·log10 (2·out/in − 1), the wet path's reduction, within 0.05 dB (measured 0.000 dB; was 4.52 dB in the fades) |
| C31 | Reset determinism with the detector filter moving: two automation sets of `sc_hpf` (120, 20, 250, 20, 80 Hz and 120, 300, 20, 99 Hz, steps between control ticks), every type, blocks 1 / 16 / 37 / 48 / 512, mono and stereo, `--reset-repeat`, 2 s of `gen:music` | 0 failures (was 8 of 100) |
| C32 | 3e38 input: 8 samples of +3e38 on every channel, then 8 of −3e38 on the left; every type, `sc_hpf` off and 100, blocks 512 / 37 / 1, mono and stereo, `auto_makeup` | output, gain trace and meter finite everywhere; 4 s later the output within 1 dB of the render of the same note without the spikes |
| C33 | `--rt-check` with a WAV input (every type switched through, `sc_hpf`, mix, auto makeup and makeup moving), block 37, 20 identical runs | 20 of 20 pass (the hook counted frees made by reader threads: 19 of 20 failed) |
| K | Per-control rows, 45: for each type, each control the type reads is moved while the block runs. Classic: threshold, ratio, attack, release, makeup; knee, mix, sc_hpf and auto_makeup (not read) must leave its output bit for bit unchanged. Each new type: threshold, ratio, attack, release, makeup, knee, mix, sc_hpf, auto_makeup | output level change in the right direction and within max (0.25 dB, 20 %) of what the type's curve predicts (the predicted change is at least 1 dB); for makeup, mix and auto makeup the GR change must also match (0 dB for makeup, within max (0.1 dB, 20 %)); attack and release: t63 ratio within 25 % of the knob ratio (1 → 20 ms, 50 → 400 ms). The row that exposed makeup doing nothing in four types |

Also printed, not pass/fail: the low-frequency residual split into harmonic distortion and
inharmonic sidebands for every type at its starting point and at the fastest settings
(41.2 / 82.4 / 98 / 110 Hz at 44.1 and 48 kHz), attack and release per type, and the
Classic crossfade timing at 44.1 kHz. Full run: **152 checks, 0 FAIL** (145 at the commit
that added the K rows, `a27b550`, all still passing; 7 new rows: C26b, C29, C30 twice, C31, C32, C33). C1 in the script is two nulls: ramps and `skip`
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
before any type (or the type switch's copy of the input) sees it; finite samples are untouched, so the
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
y  = HP(y, f_out)                        // the type's output coupling capacitor: Screamer 1.58 Hz,
                                         // Distortion 1.59 Hz, Fuzz 1.9 Hz (§4.3.5), base rate
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
  and Distortion's doubled factor.) `getLatencySamples()` is `latencyFor` of the **requested**
  type, so the host sees the new latency as soon as the type is asked for; during a type switch
  the audio is delayed as §4.3.6 says, and after it by exactly `latencyFor (type)`.
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
  **The glide is cheap.** A moving Screamer drive used to re-solve the clipper's stored points from
  scratch every base sample (2.33 % of real time against the 2.0 % budget). The re-evaluation is
  now one Halley step from the old root, and the diodes' curve follows R2 in steps of at most
  5e-4 of it (`kClipStep`), landing exactly with the glide (a step of 5e-4 moves v by at most
  23 µV, 90 dB under the clipped note). With all three knobs ramped the Screamer costs 1.80 to
  1.87 % (static 1.75 %). Boost and Treble glide their tone as the warped integrator gain, with no
  `tan` per sample: 0.037 % of real time, were 0.047 and 0.049 % (commit 0b891cb, idle machine).
- **Pot taper** for the Screamer's Drive pot and the Distortion's Filter pot:
  `A(p) = (81^p − 1)/80` (10 % of the resistance at mid travel) `[circuit: both are
  audio-taper ("A") pots; 81 is the common 10 %-at-half A-taper convention]`. The Distortion
  gain and the Fuzz sustain are not pot tapers: they are laws in dB (§4.3.5).
- **Non-finite input:** `DriveFx::process` zeroes NaN and infinity before the type switch sees them; the
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
- **Output coupling** `[circuit: cushychicken.github.io/ltspice-tube-screamer, its
  tube_screamer.asc]`: the TS-808 leaves through C7 1 µF and R14 1 kΩ into the 100 kΩ Level
  pot, whose far end sits on the bias rail: a high-pass at 1/(2π · 1 µF · 101 kΩ) = **1.58 Hz**
  (`kTsOutputHz`), a first-order section at the base rate after the down-sampler. Without it
  an asymmetric waveform leaves DC (−38 dBFS from a guitar-like stack).

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
- **Output coupling** `[circuit: the Runt2020 BOM above, C9 1 µF and VOL 100kA, placed as in the RAT's
  output buffer]`: 1 µF into the 100 kΩ Volume pot, a high-pass at **1.59 Hz** (`kRatOutputHz`).
  The buffer's own gate coupling (22 nF into 1 MΩ, C12 and R8: 7.2 Hz) is left out `[choice]`: it
  would take 0.14 dB off 40 Hz, and the rest of the output stage is folded into `drive_level`.
  Both couplings together: DC below −194 dBFS (X4), 40 to 100 Hz moved by at most 0.007 dB
  (X5, bar 0.1 dB), no thump at a note's start or at a switch (X6).

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

`fx::EngineSwitcher` is not used (§2.4): it faded the effect out to the dry input and back, so
with the level knob low the unprocessed guitar came through (+19.0 dB over the louder type for
40 ms at level −20, Fuzz to Treble; all 30 ordered pairs over +7 dB; a −9.9 dB dip at the
starting points). Now **both types run and their outputs are crossfaded**; the input itself is
never heard.

- The incoming type starts from a reset and runs **unheard for 10 ms** (`kSwitchSettleSeconds`,
  `BypassFader`'s settle time, §2.6), then a **20 ms fade** (`kSwitchFadeSeconds`, §2.4's figure).
  The fade is a smoothstep ramp `r` through equal-power gains `cos (πr/2)`, `sin (πr/2)`, so it
  has no corner at either end.
- The gains are corrected by the **measured powers and correlation** of the two outputs (two
  cascaded 4 ms one-poles, so a low note's ripple does not modulate them) so the short-term
  power moves on a straight line **in dB** between the two types' own: equal power alone bumps
  3 dB where the two outputs are alike and holds where they are not, and drive types are anywhere
  between. It is in dB because two types can sit 30 dB apart (Fuzz to Boost at gain 24); a target
  linear in power rose faster than the incoming gain and doubled the outgoing Fuzz's edges. A
  slack of 0.1 keeps the correction finite where two outputs nearly cancel.
- **Latency during a switch.** The two types are lined up at the **larger of their latencies**:
  a rise is slid in on the outgoing type during the settle (10 ms), a drop is slid out of the
  incoming type in the 10 ms after the fade (4-point Lagrange, exact at whole samples), after
  which the delay is exactly `latencyFor (type)`. The reported latency is that of the requested
  type from the moment it is asked for (§4.3.4).
- An outgoing type keeps its knobs; the knobs that arrive with a new type (its starting points)
  reach the incoming one only. A type asked for during a switch waits for it to end. After
  `reset()` the next block starts on the requested type with nothing in flight.
- Classic is unchanged (0 samples differ, nine null runs).
- Measured over every ordered pair, at level −20 / the starting points / +6, a guitar-like
  stack and a low note: in the commit's sweep over 22.05 to 192 kHz and blocks 1 to 4096 the worst
  dip is −0.76 dB and the worst bump +0.75 dB; check S1 (48 kHz) reads −0.7 / +0.7 dB against
  its bar of 1.5 dB. Boost ↔ Treble leaves nothing above 3 kHz (S2).

#### 4.3.7 Latency, CPU

Latency as the type table, exposed twice: `int getLatencySamples() const noexcept` (current
type, prepared rate) and `static int latencyFor (int type, double sampleRate) noexcept`, a pure
function the processor's latency rule can call from the message thread (§6.3). The type table
gives 48 kHz; the figures at every rate are in §4.3.4 (Classic 3 at every rate; Screamer 5 / 4 / 1,
Distortion 5 / 5 / 4, Fuzz 5 / 4 / 2 at 44.1–48 / 88.2–96 / 176.4–192 kHz; Boost, Treble 0).
CPU per instance (the factor adapts to the rate, so budgets hold at every rate), as `BUDGETS` in
`check_drive.py`, in percent of real time at 48 kHz, block 64, pink noise, the minimum of three runs.
The same bar holds for knobs at rest (the common `cpu` row) and with gain, tone and level ramped all
the time for 10 s (row C2, `gain@0..10=0..36 tone@0..10=500..12000 level@0..10=-24..12`):

| type | bar | static, measured | automated, measured |
|---|---|---|---|
| Classic | ≤ 0.35 | 0.214 | 0.219 |
| Screamer | ≤ 2.0 | 1.72 | 1.84 |
| Distortion | ≤ 3.5 | 1.89 | 1.79 |
| Fuzz | ≤ 4.0 | 2.97 | 3.18 |
| Boost | ≤ 0.05 | 0.020 | 0.036 |
| Treble | ≤ 0.05 | 0.020 | 0.036 |

(Measured on the idle-ish machine of this document, by `tubamp_fxbench --bench`; the commit that
added the cheaper glide read the Screamer at 1.75 % static and 1.80 to 1.87 % automated, and the old
glide 2.33 %.) The Distortion and Fuzz budgets were raised from 2.0 / 3.0 % by the coordinator for the
anti-aliasing scheme, per the script's comment. A check run while other work loads the machine reads
higher (a run of this document with six other check scripts going read the Screamer at 2.45 % static
and 2.05 % automated, over its bar); the bars are idle-machine bars. After a knob move the CPU must
return to the settled figure (row C).

#### 4.3.8 fxbench keys

`type gain tone level on`; `--latency` prints the reported latency (the requested type's).

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
| D8 | Screamer small-signal vs analog, d ∈ {0, 0.5, 1}, T ∈ {0, 0.5, 1}, 30 Hz–10 kHz (the reference includes the output coupling of §4.3.5) | within 0.5 dB at 48 kHz (bilinear warping above) |
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
| S1 | Level through a type switch: every ordered pair of the six types (30 pairs), at level −20, at the starting points and at level +6, a guitar-like stack and a low note (6 rows), switched at 1 s; 5 ms rms | the dip and the bump both within 1.5 dB of the two types' own steady levels (measured dip −0.7 dB, bump +0.7 dB at worst) |
| S2 | Switch click between the clean types: Boost ↔ Treble under an 82 Hz −12 dBFS note | content above 3 kHz at most −60 dBFS over the steady content (measured −240) |
| S3 | After a switch: 30 pairs, from 1 s after it | max \|out − the new type alone\| ≤ 1e-3 re peak, at its reported latency (measured 1.6e-6) |
| S4 | Switch real-time safety: a switch every 50 ms and every 10 ms, 22.05–192 kHz, blocks 1 / 37 / 4096, mono and stereo, starting off Classic (its frozen start-up ramp is not reset-deterministic, §2.11) | no allocation, identical after reset, finite |
| X4 | DC on a guitar-like asymmetric stack: 110 Hz stack −12 dBFS through Screamer (gain 12 and 36), Distortion (gain 18 and 36, level −8.2 and 0), Fuzz (gain 18 and 36, levels 5 and 0) | DC ≤ −90 dBFS (measured −194 to −238; the clippers made −38.2 and up to −26.6 dBFS before the output coupling) |
| X5 | The output coupling leaves everything above 40 Hz alone: Screamer and Distortion at gain 0, 40 / 60 / 100 Hz re 1 kHz, against the circuit without it | the change at most 0.1 dB (measured 0.007 at most) |
| X6 | No thump at a note's start or at a switch: Screamer and Distortion at gain 36 | content below 20 Hz at most −20 dB re the output peak at the start (the DC the clipped note makes, −21.6 dB without the coupling) and −60 dB 0.6 s later (measured −25.1 and −103.6) |
| Z | Knob zipper, every knob (gain, tone, level) of every new type: steps and 50 ms ramps both ways at block 64, under a 98 Hz note and 40–800 Hz noise at a level that keeps the circuit linear (Screamer −65, Distortion −70, Fuzz −110, Boost and Treble −12 dBFS) | worst frame's power above 3 kHz during the move ≤ −80 dBc re the settled output (worst now −84.2 dBc); the rest floor is reported |
| C | CPU one second after a move of all three knobs (30 s pink noise, `--bench`, min of 3) | ≤ 1.05 × the CPU of the settled values (the Treble shelf used to cost 23 % more after a gain move) |
| C2 | CPU while all three knobs move all the time: gain 0 → 36, tone 500 → 12000, level −24 → 12 over 10 s, pink noise, `--bench`, min of 3 | ≤ the type's budget (the table of §4.3.7) |
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
curves. **Known limits, not fixed, unchanged by the second round** (§8 D-41): aliasing for input
tones above 12 kHz (tones of 13 to 19.5 kHz at 44.1 kHz: Fuzz −43.1 dBc worst, Screamer −47.2,
Distortion −69.0; from 1 to 12 kHz the loudest inharmonic line is −68.3 dBc in the second
verification's 7728 runs; an 8x factor would double the CPU) and the Screamer's 0.84 dB droop at
10 kHz. The D4 to D7 bars hold at the tones they name (up to 7919 Hz). Commit `0af54cf` stated the
alias limit as "above 5 kHz"; the second verification's figures (`.audit/fx-verify/r2-tone.md`, D6)
put the edge at 12 kHz. Full run: **164 checks, 0 FAIL** on an idle machine (143 before this round; 21 new rows: S1 ×6, S2 to S4, X4 ×3,
X5 ×2, X6, C2 ×6). Run beside six other check scripts, the two Screamer CPU rows read over their bar.

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

**Type switch** (§2.4; every ordered pair of the four types, and every pair of Stack voices; no
`fx::EngineSwitcher`). A crossfade of both engines' outputs. The incoming type starts from reset on
the grid and runs **unheard for 50 ms** (`kSwitchSettleSeconds`), then a **40 ms raised cosine**
(`kSwitchFadeSeconds`) crosses the two outputs, `out = G · ((1 − w) A + w B)` with `w` the raised
cosine. The gain keeps the level between the two: `G = a_t / a_mix`, where `a_t = (1 − w) √P_A + w √P_B` is
the straight line between the two outputs' amplitudes and `a_mix` is the amplitude the plain mix is
predicted to have from the powers and the cross term (running estimates, two cascaded 10 ms one-poles,
`kSwitchPowerSeconds`, which average over a low E's period). `G` is held between 1 and 2
(`kSwitchMaxGain`, +6 dB, which is where two outputs are
in antiphase). Two Stacks run for a voice change. Under strong settings (E27's "low boost", "high cut" and "extremes" sets) the click above 1 kHz is −83.8 dBFS at worst (was −39.4) and the level dip
−0.55 dB (was −14.0), E27; E18 reads a fade of 39.46 to 39.94 ms and an envelope at most +0.63 dB
over the louder of old, new and dry.

**A switch asked back** (E30). A reversal during the unheard settle still cancels. A reversal
during the fade sends the weight back on a cubic Hermite curve from where it is, with the speed it
has, and it arrives at rest one fade time later, in either direction and again and again.
Reversing the raised cosine in place had left a corner in the weight, −45.6 dBFS above 1 kHz.
A switch asked back every 4 ms from 0 to 120 ms after the request now leaves a click of −78.2 dBFS
(was −34.1) and a level of −0.55 / +0.09 dB outside the two types' own ranges (engineer's run).

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
**A moving Classic band glides in a double-precision state-variable filter.** The float direct form
cannot follow a moving design (treble 10 to 0 left −68 dBFS above 1 kHz under a low E at
44.1 kHz, mid across 5 −51.7 at 192 kHz, bass −79.6 at 192 kHz, from float designs jittering 1 %
at 16-sample knots), and a knob change under 1e-4 units was applied at once, so slow automation at
small blocks never counted as a move. Now each Classic knob goes through **three cascaded 5 ms
one-poles** (the new types use two), and while a band is moving it is run in a **double TPT
state-variable filter designed in closed form every 4 samples** (`kGlideKnot`), linear between,
from the smoothed value. The SVF takes over the float filter's state when the move starts
and **fades back to the float filter, designed at the target, over 10 ms**
(`kHandoverSeconds`) once the knob rests. The float filter is JUCE's TDF2 loop verbatim with its
state in reach, so a static band is untouched. **The mid blends Q 1.5 → 0.7 across ±3 dB**
(mid knob 4 to 6, a smoothstep, `kMidBlendDb`) during a glide: the frozen design switches Q at
exactly 0 dB, the bell is flat there whatever its Q but its slope against the knob is not, and a
mid step across 5 under a low E left up to −72 dBFS above 1 kHz depending on the note's phase;
outside the blend, and at rest anywhere, the frozen design holds. Static Classic is bit-identical (E1,
27 nulls). E26: 57 moves × 4 rates × blocks 1 / 37 / 64 / 512 under a low E: worst −86.4 dBFS above
1 kHz (was −51.1), bar −80 (bass −86.7, mid −86.4, treble −102.1); 192 kHz is in the proof set.

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
with 0 before any type or the type switch sees it (finite samples are untouched, so Classic stays
bit-identical). A finite sample near `FLT_MAX` can still overflow Classic's float biquads: if
the block's last output sample is non-finite, the three filters are `reset()` and the non-finite
samples become 0 (`guardOverflow`). **Absurd input:** a sample above +60 dBFS (magnitude above
1000, `kAbsurdInput`) is silenced for every type, so a 1e30 sample cannot hold Classic above
+12 dBFS for 173 ms (E28; a sample of 999 is processed as before, `--legacy`). The new types read their input through
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
**Each shelf and cut feeds its core `k·x` and mixes `m1/k` and `m2/k`**: the same filter, whose
states hold what the note leaves whatever `k` is. The SVF fed `x` holds `x/k` in its band-pass at
the resonance, so a fast Q or bell-gain move rescaled the band-pass term at once while its state
lagged: Q 8 → 0.5 at +15 dB on a 110 Hz note overshot +11.6 dB (a guitar note +10.0, a −15 dB cut
+11.3).

**A bell is fed `x/Q` instead** (every Graphic bell `1/Q`, a constant). Feeding `k·x` keeps the
state right at the resonance only. Off it the state scales with `k`, and a bell's gain moves `k`
fivefold: Q 8 at 100 Hz, −15 to +15 dB under a 90 Hz note overshot +5.39 dB, and a band moving far
above a low note dragged the `k·x` its low-pass held (Graphic g3k2 −57.6 dBFS above 1 kHz). No
single state normalisation is right for gain, Q and off-centre notes at once (`k·x`, `x`, `x/Q`,
`G^b/Q` and diagonal mixes were simulated). With `x/Q` a Q move still finds the resonance right,
and gain moves and far bands leave the state alone. The input scale follows its 16-sample knots
through two 8-sample one-poles and the mix divides by it per sample, because a linear ramp fed a
comb of kinks into the core (−66 dBFS on a 3 kHz bell at 44.1 kHz). Mid frequency, shelves, cuts,
tilt, level and the Stack are unchanged. The worst gain, Q, shelf, cut, tilt, level or Graphic move
overshoots +0.91 dB with notes an octave either side of the band's centre included (row E25a, Q 0.5
→ 8 cut under a note an octave below; +4.07 dB before this change; gain moves alone +0.45;
engineer's run).

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
  **A bell's gain and Q and the Graphic gains go through a third 5 ms one-pole**, so there is no
  jump in curvature when a move starts. **A bell's gain moves no faster than a third of the bell's
  own decay time**, 2/(k·ω0) at the narrower end, so each of the three poles takes at most 20 ms
  (100 Hz, Q 8, +15 dB); bells at 1 kHz and bells of Q 2 and below keep 5 ms. Through 5 ms poles a
  cut-to-boost sweep still rang +2.0 dB at 0.92 × the centre, and +1.5 dB for a 50 ms ramp, in every
  normalisation tried (engineer's run). A narrow low bell therefore answers a gain move more slowly
  (§8 D-58).
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
0.000000 dB, `design_stack.tsv`): **transcribe them from that file**. **The Stack is the circuit
itself in the trapezoidal rule**: each of the three capacitors is discretised with the trapezoidal
rule (a conductance `G = 2·C·fs` in parallel with a current source `G·s`), the network is solved by
nodal analysis into a 4 × 4 state-space map (`design`), and **the state is the capacitor
voltages**, in double. It has the same bilinear response as `design.py`'s digitisation (c = 2·fs; E12
0.00043 dB). The earlier transposed direct form II section overshot +15.9 dB when mid went 10 → 0 at
41 Hz, because its states do not hold what a moving circuit's capacitors do; now the worst move
overshoots +1.68 dB, what the circuit does (the same at 192 kHz; row E25c, bar 2.0 dB). Knobs:
t = treble/10 (linear), m = taper(mid/10), l = taper(bass/10),
taper(x) = (b^x − 1)/(b − 1), b = (1/0.3 − 1)² `[study: design.py:242-246, 261, 264; the
report prose says linear mid, the script and the verifier use the taper on mid too, ver: V:30]`.

| voice | R1 (treble) | R2 (bass) | R3 (mid) | R4 (slope) | C1 | C2 | C3 | makeup | source |
|---|---|---|---|---|---|---|---|---|---|
| 0 Tweed (5F6-A) | 250 k | 1 M | 25 k | 56 k | 250 pF | 20 nF | 20 nF | +8.9 dB | Yeh & Smith |
| 1 Blackface (AB763) | 250 k | 250 k | 10 k | 100 k | 250 pF | 100 nF | 47 nF | +15.1 dB | robrobinette.com (topology approximate, report:428) |
| 2 British (1959/2203) | 220 k | 1 M | 22 k | 33 k | 470 pF | 22 nF | 22 nF | +6.4 dB | caps ToneStack.cc |
| 3 Top Boost (AC30) | 1 M | 1 M | 10 k | 100 k | 50 pF | 22 nF | 22 nF | +13.6 dB | caps ToneStack.cc |

Makeup = minus the mean noon level over 80 Hz – 5 kHz on a log axis `[study: design_stack.tsv §3b]`,
then `eq_level`. The 4 × 4 map is designed every 16 samples from the R1-smoothed knobs and
interpolated linearly per sample over the next 16 (the study's fallback form, a three-state
trapezoidal state-space, `[study: report:426]`, is what is built). A voice change runs two Stacks
and crossfades them as a type switch does (§4.4.1).

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
`reset()` when `on` returns to 1 on a non-Classic type, as the processor does (§2.6, §6.6);
`reenable=0` replays the frozen state the old processor left (row E23).

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
| E18 | Type and voice switch on a sustained chord (9 type / voice switches × blocks 37 / 64 / 512; the reset point is found from the settle time, there is no dry run any more) | fade 40 ± 2 ms (measured 39.46 to 39.94); the envelope at most 3 dB over the louder of old, new and dry (measured +0.63 dB); a pair whose two outputs are identical shows no fade to time |
| E19 | Rates | E2, E4, E6, E7 at 44.1, 96, 192 kHz |
| E20 | Parametric bell vs the analog prototype over Q ∈ {0.5, 0.7, 1, 1.4, 2, 3, 4, 8}, f0 100 Hz – 8 kHz (13 values), gains +15 (mid knob 10), −15 (knob 0), +6 (knob 7), −3 (knob 4) dB, 22.05, 44.1, 48, 96, 192 kHz, 20 Hz to min(20 kHz, 0.45 fs) | at each rate: worst error for Q 0.5 to 4 < 1 dB; for Q 8 no worse than the old design (0.415 / 0.067 / 0.055 / 0.013 / 0.003 dB at 22.05 / 44.1 / 48 / 96 / 192 kHz). Verification had found 1.91 dB at Q 0.5, 8 kHz, 44.1 kHz, and 13.5 dB at 22.05 kHz |
| E21 | Non-finite input: a 10-sample run of NaN, +Inf or −Inf in a 220 Hz sine, and one finite 3e38 sample; Classic 10/5/7 and flat, Parametric all sections and defaults, Graphic and Graphic flat, Stack in four voices; blocks 1, 37, 512; 1 and 2 channels | never a non-finite output sample; NaN and Inf renders equal, bit for bit, the render with zeros in their place; after 3e38, within 1e-4 of the clean render 100 ms later. (One NaN had latched Classic, Parametric, Stack and non-flat Graphic for good) |
| E22a | Neutral controls plus host float noise (tilt 8.94e-8, level ±1.79e-7, bass 5.00000048, mid 4.99999952, treble 5.00000048, lowcut 20.0000019 Hz, highcut 19999.998 Hz, every graphic band 1.79e-7; fresh, and after moving away and back), at 44.1, 96, 192 kHz, 1 and 2 channels; Stack level at noise against level 0 | output equals input bit for bit; Stack level 1.79e-7 equals level 0 bit for bit |
| E22b | Smallest step: 0.1 dB on level, tilt, each graphic band; 0.1 knob on bass; 48 kHz | each within 0.005 dB of the step (level ±0.1, band ±0.1 at its centre, tilt ∓0.1 at 20 Hz, bass 5.1 gives +0.4 dB at 20 Hz) |
| E23 | Re-enable after bypass: process, 1 s not called, `reset()`, process (fxbench `on=0` then `on=1`), +12 dB low boosts under a 98 Hz −12 dBFS note; blocks 1, 37, 512 | silent input at the return gives silence (the frozen state used to replay); held note: first sample within 0.1 of the dry sample (re peak), no step larger than the settled output's own, no overshoot above +0.5 dB in the first 100 ms |
| E24 | Classic retarget: a knob moved again in a ramp's last segment (treble and mid, blocks 1, 37, 77, 100) | click at the retarget ≤ the click of the same move from rest + 3 dB |
| E25a | Move overshoot (a control moved under a held note at the band's centre, and under notes an octave either side of it in quarter octaves and at 0.9 / 0.95 / 1.05 / 1.1 × the centre, a sine at −12 dBFS and a guitar-like note of the same peak; steps and ramps of 10 ms, 50 ms and 0.3 s; block 64; 48 kHz, and 96 kHz for the centre notes' Q and bell-gain moves): Parametric gain, Q, shelves, cuts, tilt and level, Graphic bands and level (528 moves at the centre, more with the off-centre notes) | the output's peak during the move is at most **1.0 dB** over the largest steady peak of the ends and nine settings between (spaced in the control's smoothing domain) (measured +0.91 dB with the off-centre notes, +4.07 on them before the bell was fed `x/Q`; was +11.62 at the centre; engineer's run) |
| E25b | The same for Parametric mid-frequency moves of a narrow cut (110 ↔ 220 Hz, 110 ↔ 8 kHz at Q 8 and 0.7, knob 10 and 0), and half-octave moves under off-centre notes (+1.30 dB there, equal to the analog reference within 0.001 dB; engineer's run) (128 moves at the centre) | at most **3.0 dB** (measured +2.46 dB: a Q 8 cut swept from 110 Hz to 8 kHz in 10 ms off a held note rings as the resonator empties; the overshoot is the filter's own response, not a state mismatch, and E25a's bar does not apply) |
| E25c | The same for the Stack: every knob, 0 ↔ 10, other knobs at 0 and 10, every voice, at 41.2 and 196 Hz (1024 moves) | at most **2.0 dB** (measured +1.68 dB: bass 0 → 10 in 10 ms at 41.2 Hz; the passive circuit emptying its capacitors, the same at 192 kHz) |
| E26 | Classic (and the amp tone stack) knob moves under a low E (82.41 Hz, −12 dBFS): bass, mid, treble, 11 steps and 8 ramps (10 ms, 50 ms, 0.3 s, 1 s, both ways) per knob across 5 and to and from it (57 moves), at 44.1 / 48 / 96 / 192 kHz and blocks 1 / 37 / 64 / 512 | the output's peak above 1 kHz (an eighth-order high-pass, 0.45 s to 2.0 s) ≤ **−80 dBFS** (measured −86.4: mid 10 → 0 step at 44.1 kHz block 512; bass −86.7, mid −86.4, treble −102.1; it was −51.1). 192 kHz is in the proof set |
| E27 | Switch click and level: every ordered pair of the four types and of the four Stack voices, three strong settings ("low boost", "high cut", "extremes"), switched at 1 s under a 98 Hz −12 dBFS note; 44.1 / 48 / 96 / 192 kHz at block 64, and blocks 37 and 512 at 48 kHz (288 switches); level from a guitar-like 110 Hz note, 5 ms rms | click above 1 kHz (eighth-order high-pass, 10 ms before to 250 ms after) ≤ −75 dBFS (measured −83.8; was −39.4); the level never more than 1 dB below the lower or above the higher of the two types' own steady ranges (measured dip −0.55 dB, bump +0.07 dB; was −14.0 dB) |
| E28 | Absurd input: one finite sample of 1e4, 1e10, 1e30 or 3e38 in a 220 Hz −12 dBFS sine; Classic 10/5/7 and 0/0/0, Parametric, Stack, Graphic; blocks 1 / 37 / 512 (60 renders); and one sample of 999 | each render equals, bit for bit, the render with the sample at 0 (and so never passes +12 dBFS); the sample of 999 is processed as the old code (`--legacy`, Classic 10/5/7) |
| E29 | Far-band click: a band four octaves or more above a held low note moves, as steps at 44.1 / 48 / 96 kHz, block 64. Graphic g1k6, g3k2 and g6k4, 12 → −12 and back, alone and with the other bands at +12, under a 98 Hz −12 dBFS note; Parametric Q 0.5 ↔ 8 at 1 and 3 kHz under a 41.2 Hz −3 dBFS note (bell knob 10 and 0); mid 10 ↔ 0 at Q 8 and 0.7; treble 10 ↔ 0 | peak above 1 kHz (eighth-order high-pass, 10 ms before to 300 ms after the step) ≤ −80 dBFS (measured −84.5, was −53.6; engineer's run). The 800 Hz Graphic band, three octaves above the 98 Hz note, is left out: it reads −75.8 dBFS against the −80 bar when its gain and so its damping move (round-four verifier; a plain bell with the same three poles reads −107.6), and the cause is open (§8 D-58) |
| E30 | Type switch asked back: every ordered pair of the four types under three strong settings ("low boost", "extremes", "high cut"), the first type asked back every 4 ms from 0 to 120 ms after the switch; 48 kHz, block 64 (37 for "high cut") | click (as E27, under a 98 Hz −12 dBFS note, 10 ms before to 400 ms after) ≤ −75 dBFS; the 5 ms rms of a guitar-like 110 Hz note never more than 1 dB outside the span of the two types' own steady levels (measured click −78.2 dBFS, was −34.1; level −0.55 / +0.09 dB; engineer's run) |

Full run: **86 checks, 0 FAIL** (engineer's run; 84 before E29 and E30). The table lists every row id `check_eq.py` has, with the bar the
script enforces. **E25a, E25b and E25c have three different bars on purpose.** The commit that
fixed the overshoot (44e54af) left E25b (+2.46 dB) and E25c (+1.68 dB) failing against E25a's
1.0 dB; a second commit (0cc04e2) stated bars of 3.0 and 2.0 dB for them with this reason: E25b is a Q 8
cut swept off a held note, which rings as the resonator empties, and E25c is the passive circuit emptying
its capacitors, not the state mismatch E25a guards; E25a stays at 1.0 dB (§8 D-36).

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
                               // Given every block while the song plays: each is compared with where the
                               // last one, moved on at rateHz, predicts; a small difference is pulled in
                               // slowly, a large one is a jump.
};
```
When `syncPhase ≥ 0` the engine follows the host's song position (voices keep their offsets,
§4.5.4 "Sync"): a small difference from where the last block predicts is pulled in by a slow
rate nudge, a large one is a jump made by a **tap crossfade**, never a slide and no longer a
dip. Classic ignores rate sync and `syncPhase` (the dispatcher passes `mod_rate` to Classic,
§6.5). `process()` treats a non-finite input sample, or one beyond ±1e6, as 0 and writes the 0
back (all modes).
`reset()` clears every line, starts every smoother at its target, and in modes 1–4 passes
the dry at once and fades the wet in as the lines refill; the processor calls it when a
bypassed Wide/CE/Dimension/Tri block is switched back on (§2.6).

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
- **Transitions keep the pitch** (`Chorus.cpp`: `kPitchAllowance`, `kQuietSeconds`,
  `kGlideSeconds`, `kCatchUpSeconds`, `kFadeSpacingSeconds`, `kTapMeasureSeconds`, `kTapFadeSeconds`,
  `kLockSlackCycles`, `kJumpTrainSeconds`).
  A voice's pitch is the slope of its delay, so while settings move every voice delay is
  **slope-limited to the largest steady peak slope of the settings involved plus 4 cents**
  (`2^(4/1200) − 1`). **Pitch is judged per tap** (CH15, CH25): the delay of every audible tap,
  from the engine's trace, stays inside that bound; the instantaneous frequency of the
  summed output is not the measure while two taps sound.
- **Large changes are made by a tap crossfade, not by a dip.** (The earlier rule faded the wet out
  over 10 ms, moved every delay in the silence and faded back in: a hole at mix 1 that also fired
  on ordinary Wide knob moves, up to 41 dB deep on tempo steps.) A change is a crossfade when the
  glide cannot finish soon: the knob stopped 30 ms ago (`kQuietSeconds`) and the glide would need
  more than 80 ms (`kGlideSeconds`, was 50 ms); or a moving knob's glide fell 100 ms behind
  (`kCatchUpSeconds`). While a knob moves, crossfades start at least 250 ms apart
  (`kFadeSpacingSeconds`, was 100 ms after the last one ended), and one more starts when it stops.
  Then every voice starts a second
  read tap that lands on the new law **silent**; its power and its correlation with the old tap are
  measured for 10 ms (`kTapMeasureSeconds`, two cascaded 4 ms one-poles; a retarget restarts the
  measurement only inside a 20 ms pre-roll); then a **cornerless 30 ms fade** (`kTapFadeSeconds`) runs,
  **directly or through the taps half way between** the old and the new, whichever the measured
  correlation says is more alike (the path choice keeps a tone's two faded taps no worse than −0.5
  correlated; the power correction has headroom of at most +8.8 dB, `kTapSlack`). **Every voice
  chooses its path and is corrected on its own power.** The choice used to be made on the sum of
  both channels. CE plays one voice per channel, so on a loop wrap one channel's taps could meet in
  antiphase while the other's added, and the shared choice left a hole of −32.1 dB on the right
  channel at 750 Hz. Now CE has 0 of 36 notes with a hole, the deepest −0.5 dB (engineer's run). The
  gains are scaled so each voice's short-term power moves on the straight line between the two taps'
  own. The
  **outgoing tap** blends its slope over 5 ms into the new tap's and then runs a fixed distance from
  it, so two taps of one signal carry no pitch of their own, only a comb while both sound. Wide's
  pairs glide in lockstep. Ordinary knob moves at the starting points start no audible event: a
  depth step, a rate step, eight hand moves each, the output within 0.26 dB of the steady and note
  dips at most 0.50 dB (CH26); Wide, Dimension and Tri start one crossfade for a step, CE none.
- **Glide or crossfade** (`Chorus.h`, §8 D-56). A glide bends every note by at most 4 cents beyond
  the settings' own detune, but it can follow a delay change only as fast as that allowance lets it. A crossfade slides each
  voice's phase by up to half a cycle in 30 ms, a pitch blip of up to 25 Hz (about +460 cents at
  82 Hz, +43 cents at 1 kHz) that no fade shape removes, because gliding the same delay change as
  fast would bend every note as much. The glide is the smaller bend wherever it keeps up, so the
  crossfade is for lag only: a stopped knob glides up to 80 ms, a moving one 100 ms, and crossfades
  are at least 250 ms apart while a knob moves. Steps every 40 ms start 8 crossfades in 2 s (was
  49), and 10 s rate ramps start 4 in Wide (was 6) and 3 in Tri (was 4) (engineer's run).
- **Lag.** After the knob stops, the heard detune is back inside its own steady band within
  123 ms, which includes the 10 ms measuring window (CH27; the earlier worst was 189 ms in CH15,
  now 73 ms there); a ramp's lag behind the knob is 135 ms (was 80 ms before the crossfades were
  spaced 250 ms apart; CH27 bar 150 ms; engineer's run).
- **Sync** (`ChorusParams::syncPhase`, given every block while the song plays). The host's phase
  is compared with where its previous one predicts, `previous + rate × block`. **Within
  `max (0.01 cycle, 5 ms of LFO time)`** (`kLockSlackCycles`, `kLockSlackSeconds`) the host is
  continuous: the LFO runs free at the tempo's rate and its own error is pulled in with a
  **150 ms time constant** (`kLockSeconds`) by a rate nudge bounded to **0.9 × the 4-cent
  allowance of slope and to 25 % of the rate** (`kLockMaxRatio`). A tempo step, a ramp or
  ±2 to ±16 samples of position jitter all fall inside this. **Beyond it** is a **jump**
  (locate, a loop that is not a whole number of cycles, a note-value change, sync switched on), and
  so is an own error above 0.25 cycle (`kLockLostCycles`); a jump is made by one tap crossfade.
  **A discontinuity within 250 ms of the last one is noise, not a jump** (`kJumpTrainSeconds`), and
  the LFO runs free at the tempo's rate. A song position noisier than the jump limit had made every
  block a jump and froze the LFO on the outgoing taps (±3.0 cents instead of ±10.9); it now keeps
  ±10.9 cents and starts no crossfade (CH31, engineer's run). Each jump holds every channel, since
  every voice chooses its own path (CH30, §8 D-54). In
  104 cases (1 BPM steps, 100 to 140 BPM ramps at blocks 64 / 512 / 4096, ±2 / 4 / 16 samples of
  jitter at 1/32 to 1/1, every mode) there are 0 jumps, the wet is within 0.14 dB of the free
  render, pitch at most the steady value + 1.0 cent and the LFO within 0.0058 cycle (CH24).
- **Huge input.** An input sample beyond ±1e6, like a non-finite one, is processed as 0 and
  written back as 0 (`ChorusFx::process`). Classic stays bit-identical below it: ±9e5 samples give 0
  differing samples (CH28).
- Stereo/mono: Wide and CE run one voice set per input channel (the right voices read the
  right input's line); Dimension and Tri feed their lines from the mono sum (L + R)/2. A
  1-channel block computes both outputs and writes their mean, except **Dimension: a mono
  instance plays line A alone** (the stereo left channel). The mean of its two antiphase
  lines would have no detune; line A alone keeps the 3.5 cents of the stereo instance.
- LFO start after reset: master phase p = 0 (u = 0, shortest delay), offsets as listed.
  `reset()` clears the lines (§4.5.2) and Classic completely (line, oscillator, mixer: its output
  afterwards is exactly the frozen chorus started fresh, CH29); the wet fades in as they refill, the dry is never
  interrupted.
- **Mode switches do not use `fx::EngineSwitcher` (§2.4).** The outgoing mode keeps sounding
  while the incoming mode's line fills (until it has been written for its longest delay,
  `reachSamples`, plus 4 samples). The two **whole outputs** are then crossfaded over 20 ms
  (`kSwitchSeconds`), per channel, in two bands split at 1 kHz (`kSwitchSplitHz`), each band's
  gain scaled by the measured short-term powers and correlation of the two (two cascaded one-poles
  of 4 ms, run for 10 ms before the fade starts: `kSwitchPowerSeconds`, `kSwitchMeasureSeconds`),
  with headroom of +18 dB (`kSwitchSlack`), as `PowerCrossfade` does in the dispatcher (§6.5). The
  fade starts where every channel's outputs correlate no worse than −0.5 (`kSwitchMinCorrelation`)
  and are not heading below it; any such moment after 250 ms (`kSwitchMaxWaitSeconds`), any moment
  after 400 ms (`kSwitchLongestWaitSeconds`). The earlier switch faded the two wets by their power
  summed over both channels, from a fixed moment. On a held note two modes can sit in antiphase, and
  holding the wet's power does not hold the output's (processor probe, engineer's run, Wide → Tri at
  220 Hz −5.5 → −0.8 dB, Dimension → Tri at 440 Hz −8.75 → 0.0, Tri → Wide at 1 kHz −10.4 → −3.2).
  The wet-tap routing of the earlier switch went with it. The switch to or from Classic uses the
  same crossfade. CH33 bounds it on single notes (20 ordered pairs, 3 mixes, each channel): 5 of 60
  switches stay outside the per-switch bounds, 24 before (§8 D-55). CH18 (0.69 dB level bump, click
  −102 dBFS) and CH27 (switching every 30 ms through all five modes stays within 1.70 dB of the
  modes' own levels) were measured with the earlier switch; their bars still hold (engineer's run).

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
Tri ≤ 0.4 %; during a mode switch (two modes running) the two modes' budgets added; while a tap crossfade runs most of the time (a song-position jump every 50 ms, each fade about 40 ms) the mode's own budget, and two alternating modes with jumps on top the two added (CH23: Wide 0.198 %, Wide+Tri 0.334 % crossfading, commit baaaf1f, idle machine).
fxbench keys: `mode rate depth mix width tone sync_phase on`; test hooks `reset` (any change
of its value calls `reset()` at that block, as the processor does when the block comes back, on the
engine and on the legacy reference, whose own `reset()` is what `ChorusFx::reset()` does for
Classic)
and `solo=<voice>` routes one voice to both outputs at mix 1 (implemented as
`ChorusFx::setDebugSolo (int)`, a no-op when −1; allowed because it costs one branch per
block). A simulated play head, `host_bpm host_beats host_jitter host_play host_loop_len
host_loop_end` (`tools/fxbench/README.md`), resolves `rate` and `sync_phase` from a tempo and
a note value as the dispatcher does. `--trace` (while a voice is soloed) writes three channels
per sample: the soloed voice's tap delay in samples, its outgoing tap's delay (equal to the first
outside a tap crossfade) and the crossfade stage (0 none, 1 outgoing tap alone, 2 fading
directly, 3 fading through the midpoint tap) (`ChorusFx::setDebugTrace`); `--meters` writes the
song-position jumps and the tap crossfades started for knob changes since `prepare()`
(`ChorusFx::getDebugCounts`).

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
| CH15 | Transients: rate, depth, width, tone steps and ramps, 16 random steps 25 / 60 ms apart; 997.3 and 3301.7 Hz carriers, every soloed voice, blocks of 64; pitch judged **per tap** from the engine trace (the current tap outside its silent pre-roll, the outgoing tap through the whole crossfade, the midpoint tap on the midpoint path), the instantaneous frequency where no crossfade runs | the loudest-bending tap's \|deviation\| stays within 5 cents of the largest steady peak of the settings involved; back inside the new settings' peak + 5 cents 250 ms after the change; crossfades per case reported (worst +4.6 cents over, settle 73 ms) |
| CH15-rates | CH15's hardest four cases at 22.05 and 192 kHz, blocks 1 / 512 / 4096, mono and stereo | same bar as CH15 |
| CH16 | Sync jumps: transport start, locate, loop, note-value change | pitch bound and settle as CH15; LFO within 2 % of a cycle of the requested phase afterwards; 220 Hz −12 dBFS at mix 0.5 and 1: 2 kHz high-passed peak ≤ −60 dBFS |
| CH17 | Bypass then `reset()` (block off 1 s, back on) | silent input: output ≤ −120 dBFS (no replay of the old lines); 220 Hz −12 dBFS at mix 0.35 and 0.5, mono and stereo: restart peak after 2 kHz high-pass ≤ −60 dBFS |
| CH18 | Mode switch between Wide, CE, Dimension, Tri (every pair, mix 0.35 / 0.5 / 1) | under a chord, the 20 ms level averaged over six switches each way stays within 1.5 dB of the span between the levels before and after; a 220 Hz sine shows no click (2 kHz high-pass ≤ −60 dBFS) |
| CH19 | Mono instance is still a chorus | pitch deviation (\|IF\| p95, mix 1; 220.5, 997.3, 3301.7 Hz) at least half the stereo left channel's, and the comb swing on pink noise at least half as deep |
| CH20 | Non-finite input (NaN, +Inf, −Inf sample, and a run of 100 NaNs), every mode, mono and stereo, blocks 37 and 512 | output bit-identical to the same input with zero samples; one 1e30 sample gives finite output back on the clean render 0.2 s later within −100 dB |
| CH21 | Onset after `reset()` (sustained 220 Hz −12 dBFS) | 2 kHz high-passed peak ≤ −60 dBFS at mix 1, and at mix 0.5 under the dispatcher's 20 ms linear fade |
| CH22 | `--rt-check` and `--reset-repeat` through switches, dips, sync jumps, bypass and reset, 22.05 – 192 kHz, blocks 1 / 37 / 512 / 4096, mono and stereo | exit 0, finite output |
| CH23 | CPU while two modes alternate every 60 ms (Wide+Tri, CE+Dimension, Wide+CE, Dimension+Tri, Wide+Classic); then every mode while a tap crossfade runs most of the time (a song-position jump every 50 ms, each fade about 40 ms); then Wide+Tri and CE+Dimension alternating with jumps on top | the two modes' §4.5.6 budgets added (Classic: 1.2 × the legacy figure); crossfading, the mode's own budget (Wide 0.5, CE 0.25, Dimension 0.25, Tri 0.4 %) |
| CH24 | Sync under an imperfect host (`host_*` keys): tempo in 1 BPM steps at 1/32 and 1/4, 100 to 140 BPM ramps at blocks 64 / 512 / 4096, song position jittered by ±2 / 4 / 16 samples at 1/32 to 1/1; Wide, CE, Dimension, Tri at depth 0.6, mix 1 (26 timelines × 4 modes = 104 cases) | 0 song-position jumps (measured: 0 jumps and 0 crossfades); the wet within 0.5 dB of the same tempo course run free (power of four pink-noise takes, 2 ms frames over 10 ms; measured ≤ 0.14 dB); voice 0's pitch inside the steady bound + 5 cents (measured ≤ steady + 1.0 cent); the LFO within 0.01 cycle of the song after 1 s (measured ≤ 0.0058) |
| CH25 | Large changes are tap crossfades: steps and 0.1 s ramps of rate, depth, width, tone and note-value steps under a play head, every mode, mix 0.35 / 0.5 / 1 (36 timelines) | the wet (out − dry gain × in) on pink noise and a guitar-like take never more than 1.5 dB under the lowest steady wet along the change (measured −1.10 dB); single held notes 82 / 196 / 331.7 / 880 Hz through every soloed voice: the envelope's worst dip against the lower of the levels before and after at most 6 dB (measured −0.55); a 220 Hz −12 dBFS sine: 2 kHz high-passed peak ≤ −70 dBFS (measured −74.0); every audible tap's delay slope inside the steady peak + 5 cents (measured +4.1) |
| CH26 | Ordinary knob moves at each mode's starting point: depth +0.2, rate × 2, eight hand moves of each knob | no audible event: the output within 1 dB of the envelope of the steady outputs along the change (measured ≤ 0.26 dB), soloed notes dip at most 1 dB (measured −0.50), 2 kHz high-passed peak of a 220 Hz sine ≤ −70 dBFS, taps inside the steady peak + 5 cents; crossfades started are reported (Wide 1 / 1 / 4 / 12 for depth step / rate step / 8 rate moves / 8 depth moves, CE 0, Dimension 1 / 0 / 1 / 0, Tri 1 / 1 / 1 / 14) |
| CH27 | Lag and rapid switching: the heard detune of voice 0, per tap, for steps and 0.5 / 2 s ramps; mode switches every 30 ms in random order through all five modes at mix 0.35 / 0.5 / 1 | back inside its own steady band (widened by 12 % or 0.5 cent) with no crossfade running at most 150 ms after the knob stops (measured 123 ms); during a ramp never further behind the knob's course than 150 ms (measured 135 ms, was 80); the level of eight pink-noise takes and eight white-noise takes within 2 dB of the envelope of the five modes' steady levels (measured 1.70 dB); 220 Hz −12 dBFS: 2 kHz high-passed peak ≤ −60 dBFS |
| CH28 | Huge finite input: a +3e38 / −3e38 pair, a 1e7 sample and a run of 1e20, every mode, mono and stereo, blocks 37 and 512; Classic with samples of ±9e5 | the output equals, bit for bit, the render with those samples at 0; Classic 0 samples differ from the legacy chorus |
| CH29 | Classic re-enable: a Classic engine that processed a loud signal, was not called for a second and then `reset()`, silent input, mono and stereo, blocks 64 and 512, mix 0.35 and 1 | ≤ −120 dBFS (measured −240) |
| CH30 | Song-position jumps hold every channel: Wide, CE and Dimension at their sync cases, held notes 82.41 / 110 / 196 / 331.7 / 440 / 880 / 1000 Hz at −12 dBFS, mix 0.5 and 1, locates of 0.1 / 0.25 / 0.5 cycle and a host loop of 2.63 beats (a fractional number of cycles); each channel's level (analytic amplitude, 2 ms frames, 10 ms before to 150 ms after each jump) | at most 3 dB under that channel's lowest steady level (a long render without the jump); click above a high-pass two octaves over the note (at least 2 kHz) ≤ −70 dBFS (measured Wide −0.6, CE −1.5, Dimension −2.8 dB; was −10.0, −20.4, −2.8; engineer's run) |
| CH30t | Sync jumps, Tri, the known limit of CH30 (the same 56 jumps) | at most 8 of 56 jumps under −3 dB and none under −14 dB; click ≤ −70 dBFS. These are regression guards set from the measured worst (6 of 56, −12.11 dB; was −21.9; engineer's run), not design targets (§8 D-54) |
| CH31 | Classic after `reset()` and a noisy song position: `--classic-null` with resets mid-stream, blocks 1 / 37 / 512; every block alternately 0.6 of the jump limit early and late, every mode at its sync case, block 64 | Classic differs in 0 samples (the verifier's 612 runs: 0 differing, was 18 because the bench's legacy reference ignored the reset key; engineer's run); after the first jump the LFO runs free, at most 1 further crossfade, and voice 0's detune (\|IF\| p99, 997 Hz) at least 90 % of the exact position's (measured ±10.9 cents; engineer's run) |
| CH32 | Start-up after `reset()` (cold start and a reset mid-stream): a held 1 kHz and 331.7 Hz note and pink noise, each mode's starting point, mix at its starting point and 0.5, from 50 to 300 ms after | within 1 dB of the range a steady render spans (one-period RMS per channel; Wide's steady render 20 s because its two pairs never repeat), pink noise within 1 dB of the envelope of steady renders at eight LFO phases (measured +0.41 dB at worst). A 1.8 s reference misses comb positions that steady state reaches, which is why Wide read −14.4 dB against it; not a defect (engineer's run) |
| CH33 | Mode switches on single notes: every ordered pair of the five modes at the outgoing mode's rate and depth, mix 0.35 / 0.5 / 1, notes 82.41 / 220 / 440 / 1000 Hz at −12 dBFS switched at 1.5 s, and a guitar-like take switched at 1.5 s and 2.7 s; the references are static renders at every LFO position (60 switches, each channel) | per switch, each channel's one-period RMS over the 0.7 s after the switch at most 3 dB under the lower and 1.5 dB over the higher of the two modes' steady ranges, click ≤ −70 dBFS or the two static renders' own, the take within −3 / +1.5 dB (20 ms frames). **As a distribution bound: at most 6 of 60 switches outside, and none under −22 dB, over +2.0 dB or above −60 dBFS.** Measured 5 of 60 outside, worst −20.29 dB, +1.76 dB, click −69.7 dBFS (§8 D-55; engineer's run, 24 of 60 before). The guards are regression guards set from the measured worst, not design targets |
| CH34 | Automation glides between crossfades: rate ramps 0.05 → 10 Hz over 0.5 / 2 / 10 s (Wide, Tri), depth ramps 0.3 → 0.7 and 0.2 → 1 over 0.5 / 2 s (Wide), depth steps 0.4 ↔ 0.8 every 40 ms for 2 s (Wide, Tri), block 64 | tap crossfades start at least 250 ms apart, except one that finishes the change once the knob has stopped (counted from the engine's trace) |

Full run: 83 checks, 82 PASS and 1 FAIL (82 checks in `0fde1ee`, 77 before the third round, 71 at commit `08da770`). The one FAIL is CH23, a CPU row, on a busy machine (CE crossfading 0.262 % against its 0.25 % budget; the other rows of CH23 pass); CH23's figures are from an idle machine and the row was not re-run idle. CH30 and CH33 pass after the split described in §8 D-54 and D-55. The table lists every row id
`check_chorus.py` has, with the bar the script enforces. The sync rows' measured values are
those of the run of this document.

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
Default `mod_color = on` `[choice: the block logo is the Phase 90 voicing with a measured
reference (0.41–0.70 % rms notch-trajectory error, P4; pitch and waveform null against Guitar
Rig 6, P13 and P14); the script Phase 90 has no measured reference, report.md:10]`.

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
  (output −1). Free-running phase accumulates `rate/fs` per sample in double. With
  `syncPhase ≥ 0` (given every block while the song plays) the engine predicts this block's host
  phase as `previous + rate × samples since / fs`; the host's phase minus the prediction, wrapped to
  ±0.5 cycle, is the host's step.
  - A step of **at least 0.01 cycle and at least 2 ms of LFO time** (`kJumpCycles`,
    `kJumpLfoSeconds`) is a **jump** (transport start, locate, a loop that is not a whole
    number of cycles, a note-value change, sync switched on), unless a rate error of 15 % could
    explain the step since the last position (`kJumpDriftRate`). That is drift, handled by the lock
    below. The phase is set to the host's, and the
    sweep position glides from where it is (a glide still running included, so a second jump
    retargets from the current position) to the new phase's position: the difference (above 1e-6,
    `kGlideThreshold`) is faded out with a raised cosine over a **fixed length counted in samples**,
    so it does not depend on the host block: **10 ms** (Phase 90 script and block, Small Stone
    Color off) and **40 ms for Small Stone Color on** (`kGlideSeconds`). Color on sweeps its
    resonance through up to five octaves on a half-cycle jump, and at 10 ms that zipped at
    −31 dBFS once the host block was long enough for the glide really to last 10 ms. **Phase 90 then
    reaches the new phase only through its gate smoothing**, the circuit's own RC between the LFO
    and the JFET gates (3.54 Hz, a 45 ms time constant). It takes 63 to 201 ms to reach within −40 dB of the
    voicing at the new phase all along, depending on the jump. That is the voicing's sweep, as the
    pedal's own would lag a jump of its LFO, not the glide, and shortening it would mean leaving the
    circuit (`Phaser.h`; P15).
  - Any smaller step is **no jump**: the LFO runs at the tempo's rate and its error against the host
    is pulled in by bending the rate, never by a glide. **The lock is second order.** A phase term,
    `nudge = (|e| − d)·sign(e) / τ`, τ = 0.15 s (`kLockSeconds`), at most ±3 % of the rate
    (`kLockMaxNudge`), dead band d = min (0.002 cycle, 0.5 ms of LFO time) (`kLockDeadCycles`,
    `kLockDeadLfoSeconds`), set once per block, closes a phase error. A rate integral (integral time
    0.3 s, `kLockIntegralSeconds`, damping 0.71, bounded to ±10 % of the rate, `kLockMaxDrift`) learns
    the host's rate error and is kept across jumps. The phase term alone, capped at 3 %, never
    closed a 5 % host rate error (0.5 cycle of error and 215 glides over the test). Host jitter of ±2
    and ±8 samples, 1 BPM tempo steps and a 100 to 140 BPM ramp cause no glide (P16). A host whose
    song position runs 1, 5 or 10 % fast or 10 % slow against its reported tempo causes 0 glides and
    leaves 0.0024 cycle after 2 s (the jitter dead band), with ticks at or under −85.4 dBFS (P20,
    engineer's run). **The bounds, as the round-four verifier measured them** (`.audit/fx-verify/r4-mod.md`
    finding 4): inside ±10 % the lock holds (host 0.5 to 10 % off its reported tempo, 1/4, 1/16 and
    4/1, blocks 16 to 4096, 44.1 to 192 kHz: 0 glides, within 0.005 cycle after at most 1.07 s,
    0.002 cycle standing). **Outside them it does not.** A host 12 % off leaves 0.007 cycle (1/4) and
    0.026 cycle (1/16) standing; at 15 % a 1/4 note slips through every phase with no glide while
    the LFO rate swings 24 to 33 %, and a 1/16 note glides on nearly every block (1287 glides in
    16 s at block 512); at 25 % at 1/16, 1499 glides. A synced rate above the 20 Hz clamp (300 BPM
    at 1/32) glides 116 times in 10 s at block 4096 and slips with a 48 % rate swing at block 64
    (160 BPM at 1/32, 21.3 Hz, locks). Position jitter of ±64 samples at 1/16, block 256, gives
    138 glides in 12 s (±16 samples: 0). A loop-wrap tick, Small Stone Color on, 587.3 Hz, 4 Hz,
    block 64, reads −60.4 dBFS above 2 kHz (uninterrupted −80.7), 0.4 dB inside P15's −60 bar,
    while P15 reports −81.8 as its worst; every other case −67 dBFS or lower. P16 and P20 stay inside
    these bounds (jitter of ±2 and ±8 samples, drift up to 10 %).
  - The first block after `prepare()` or `reset()` sets its phase with no glide.
- **Phase 90** (`P90_*`): C = 47 nF, R_fix = 22 kΩ `[circuit]`; leg R(u) = (RS^m + R_ch^m)^{1/m},
  R_ch = R1·e^{−γu}, RS = 916.8 Ω, R1 = 1777.7 Ω, γ = 0.8952, m = 3.876 `[fit]`;
  fc = (1/R_fix + 1/R(u))/(2πC); κ = −0.007; gate one-pole 3.542 Hz on the LFO before the
  law, k = 1 − exp(−2π·3.542/fs), state starting at the first LFO value. Script: wet = A⁴.
  Block: β = 0.3957, lag one-pole 14746 Hz (analog) in the feedback path, the per-sample loop of
  voicings.md. Mix: out = M·(0.5·x + 0.5·wet), M the fixed make-up below.
- **Small Stone** (`SS_*`): κ = 0.702; the sweep is exponential in the LFO position v in
  [−1, 1], `ln fc = (ln f_lo + ln f_hi)/2 + ln (f_hi/f_lo)/2 · v + curve·(v² − 1)`.
  **Color off** is **fitted on the complex response** (magnitude and phase of 277 snapshots,
  `evidence/fit/snapshots.tsv`, not on magnitudes alone: `dry + wet·P⁴` and `wet + dry·P⁴` have the
  same magnitude and opposite motion, and the first fit had picked the wrong one): f_lo 326.4 Hz,
  f_hi 9086.3 Hz, curvature term `curve` = 0.0128 (rms of the break frequency 0.43 % → 0.18 %;
  the pitch deviation at 1.78 Hz from +16 % to +13.6 % of Guitar Rig 6's), no feedback,
  **dry 0.5003, wet 0.7500** (the wet path dominant; it was 0.75 / 0.50), and a **first-order
  high-pass at 44.2 Hz on the wet input** before the stages (residual of the snapshot fit −46.7 dB
  re |H| with it, −14.8 dB without). It moves notch 1 up near the sweep bottom (148.7 Hz at f_lo,
  0.456·fc instead of tan(π/8) = 0.414) and takes the wet out at DC. **Color on:** f_lo 98.5 Hz,
  f_hi 3105.4 Hz (refitted on the snapshots; curve 0, the fitted curvature 0.003 does nothing),
  feedback c = 0.7548 around all four stages through F(s), wet input through Ha(s), poles
  f_p1 49.8, f_p2 267.9 Hz, zeros f_zf 12.0, f_za 12.7, f_zb 862.1 Hz, dry 0.1318, wet 0.4395
  (the snapshots give dry/wet 0.2980 against the prototype's 0.2999); TPT discretisation of
  voicings.md (Ha and F as two first-order sections each; the chain's p⁴ is the product of the
  four per-stage factors `p[0]·p[1]·p[2]·p[3]`, and `y4 = (p⁴(xs + c·bF) + K)/(1 − c·aF·p⁴)`,
  divisor ≥ 0.24). Phase 90 block: weights and sign confirmed on the snapshots (dry/wet 0.997).
- **Make-up (fixed per voicing, `[choice]` in voicings.md, "Level"): unity loudness against
  bypass on guitar.** Each make-up is the negated mean, over three guitar-like takes (a mixed
  take, a low riff 82 to 147 Hz, a high take 330 to 784 Hz, 12 s each), of the ungated ITU-R
  BS.1770 K-weighted loudness of the output against the input at the voicing's starting rate,
  rounded to 0.01 dB (P18, bar ±0.25 dB): Phase 90 script ×1.3152 (+2.38 dB), block ×1.3137
  (+2.37 dB), Small Stone off ×1.0186 (+0.16 dB), Small Stone on ×0.9237 (−0.69 dB);
  `kP90ScriptMakeup`, `kP90BlockMakeup` and `StoneVoicing::makeup` in `Phaser.cpp`. They were
  +2.70 / +1.50 / +0.50 / 0 dB, whose register means were +0.32 / −0.87 / +0.32 / +0.68 dB. The
  Small Stone's dry and wet constants above are multiplied by it. What one fixed gain cannot
  remove is the register dependence, which is the voicing (Color on lifts a low riff by +2.16 dB and
  leaves a high take −3.01 dB). Classic is unchanged.
- **Headroom.** Nothing saturates and there is no limiter: a full-scale input can leave the
  engine above 0 dBFS. Worst peak for a full-scale input (squares of 41.2, 82.4 and 110 Hz, sines
  of 97 Hz to 3.3 kHz, a guitar-like take, at the starting rate and at 10 Hz; P18): Phase 90 script
  +2.4 dB, block +5.9 dB, Small Stone off +5.7 dB, Small Stone on **+13.6 dB** (82.4 Hz square at
  10 Hz). **The chain after the phaser needs up to 14 dB of headroom** (6 dB for every
  voicing but Small Stone Color on). Guitar Rig 6 peaks the same way within 0.3 dB on a −6 dBFS
  low square (voicings.md "Peaks on a low square wave"), so the peak is the voicing.
- No oversampling, no saturation, no output filter `[study: voicings.md]`. One LFO for both
  channels; per-channel stage states. A sample that is not finite, or whose magnitude is 1e6 or
  more, is replaced by 0 before the engine sees it (Classic included; everything below is
  untouched, so the Classic null holds; a ±3e38 pair used to latch Classic's
  `juce::dsp::Phaser` to NaN, P12).
- **Model and Color switches** (§2.4; all five voicings, Classic included; no
  `fx::EngineSwitcher`): the outgoing voicing keeps running on one copy of the input; the incoming
  one is reset and runs on another copy, **unheard for 30 ms** (`kSwitchWarmSeconds`: its states and
  the power estimates settle). **The fade then waits for a moment the two will not cancel
  through.** On one held note two phasers can sit near antiphase, and a crossfade there passes a null
  whose constant-power correction (up to +18 dB in the earlier switch) was itself heard. As
  `PowerCrossfade` does (§6.5), the fade starts where the outputs' short-term correlation (two
  cascaded one-poles of 2.5 ms each, `kSwitchStatSeconds`) is at least −0.5 (`kSwitchMinCorrelation`)
  and not falling. Any such moment counts from 250 ms (`kSwitchWaitSeconds`); at 280 ms
  (`kSwitchLongestWaitSeconds`) the fade starts regardless. From the request to a fully new voicing
  takes 69 to 390 ms. The fade is a smoother-step ramp of **40 ms** (`kSwitchFadeSeconds`), **80 ms
  when it starts in antiphase** (`kSwitchTurnSeconds`), with no corner at either end. **Above an LFO
  rate of 1.5 Hz** the two sweeps turn the outputs' phase difference on a held note too fast for that
  (about 1 degree per ms per Hz, `kSwitchSweepTurn`): any fade then lasts as long as that turn takes to
  reach 60 degrees (30 ms at 2 Hz, 15 at 4, 7 at 8, never under 5 ms), and on a held note (the two
  outputs one complex gain of each other, `kSwitchHeldCoherence`) the fade starts where their phase
  difference, extrapolated from its measured turn past the estimates' delay, stays within 60 degrees
  through the middle half of the fade; request to fully new then takes 37 to 330 ms. At 1.5 Hz and
  below the switch is unchanged (bit-identical renders). The level
  correction runs in two bands split at 1 kHz (`kSwitchSplitHz`). In each band the power of the sum,
  predicted from the short-term powers and the cross term, follows the straight line in dB between
  the two outputs', and the gain is bounded at **+1.4 dB** (`kSwitchMaxGain` 1.18 in amplitude). A
  band still in antiphase turns through a 90-degree rotated copy of the input (an all-pass
  quadrature pair, `kQuadratureA`, `kQuadratureB`) instead of through the null, and the rotated copy
  fades out as the two come into phase (`kSwitchBridgeFrom`, `kSwitchBridgeFull`). A request during a
  switch is taken when it ends. Classic's own output is untouched (the same code path whenever no
  switch is in flight). The engine holds two Phase 90 / Small Stone instances that share one set of
  coefficient tables. Measured in P17 (engineer's run), every ordered pair of the five voicings, held
  notes at 82 / 196 / 440 Hz at −12 dBFS and a guitar-like take, the level in 20 ms frames within
  +0.79 dB of the higher and −2.20 dB of the lower of the two voicings' own levels, click −93.8 dBFS
  above 2 kHz (+0.95, −2.88 before the third round; the old switch faded through the dry input, +11.5
  dB above and 20.3 dB below both levels). P19 follows the whole switch on held notes and a take. It reads a dip
  of 3.00 dB under the lower voicing (was 32.8) and a bump of +1.25 dB (was +1.62) at 0.3 and 1.3 Hz, and
  reports 2, 4 and 8 Hz per rate. See §8 D-57 for what is left.

**Decision — oversampling (Small Stone Color, and any voicing): not done.** Internal 2x
needs up/down-sampling filters, i.e. latency, and D6 allows latency only for drive. The
notch-matched discretisation above removes what the 2x would have bought for the notches
(the fits are to the analog response and the notches land on it at 44.1 to 192 kHz). What it
cannot fix is the magnitude response away from the notches: four first-order sections at
44.1 or 48 kHz cannot follow an analog chain's phase near 10 kHz, so the frozen responses
between 48 and 192 kHz differ on the slopes above 3 kHz (§4.6.6 P9, "frozen response",
failing; §8 D-8). The Color-on resonance at the sweep top also sits +2.6 % high at 48 kHz
(3259 against 3176 Hz analog; peak height within 0.02 dB) for the same reason [voicings.md].
The remaining gaps against Guitar Rig 6 (`compare.txt`: Small Stone Color off, notch-1 ends
within 0.53 %; Phase 90 block notch 2, not a target, +0.28 to +0.80 % at the sweep bottom and
+1.57 to +3.34 % at the top; Color on, notch-1 minimum +2.84 % at 1.78 Hz) are small
against the 0.41 to 0.70 % (block), 0.12 to 0.21 % (Color off) and 0.30 to 0.44 % (Color on) rms
trajectory fits.

#### 4.6.5 Latency, CPU, fxbench

Latency 0 (`getLatencySamples()` returns 0). CPU per instance: Classic as today; Phase 90
≤ 0.4 %; Small Stone ≤ 0.5 % (`BUDGETS` in `check_phaser.py`; per sample one table
interpolation of four or five coefficients plus up to eight one-poles, no `tan`; the four
tables, 257 entries each, are built in `prepare()`, which
takes under 10 ms [voicings.md]). Measured at 48 kHz, block 64 (commit 8f8f197, idle machine):
script 0.066 %, block 0.093 %, Small Stone off 0.064 %, on 0.135 %; switching model or Color
every 50 ms 0.30 % (the switch row's bar is the Small Stone budget, 0.5 %). fxbench keys:
`model rate depth mix color sync_phase on reset_on_enable freeze_u start_reset`, and a simulated host,
`host_bpm host_note host_ppq host_jitter host_drift` (`tools/fxbench/README.md`). `reset_on_enable`,
`freeze_u`, `start_reset` and the `host_*` keys are test hooks. `start_reset` calls `reset()` after
the first block's parameters are set, so the engine starts as a voicing switched in does (without it
Classic's LFO glides from juce's 1 Hz to the set rate). `host_drift D` makes the simulated song
position advance 1 + D times as fast as the reported tempo says, as when the tempo parameter lags
the host's own. `freeze_u` calls `PhaserFx::freezeSweep`, which holds the
sweep position in [−1, 1], −1 the bottom (NaN releases it). With `host_bpm` present the adapter
resolves `rate` and `sync_phase` from the tempo, the note value (`host_note`, LFO cycles per beat)
and the song position (`host_ppq` beats at the first block, the tempo integrated between blocks as
a straight line); `host_jitter N` reports each block's position off by a whole number of samples
drawn uniformly from −N..N. `--trace` writes three channels per block: the LFO phase in cycles at the
block's last sample (`PhaserFx::getLfoPhase`), the song-position jumps glided so far
(`getSyncJumpCount`), and the simulated host's exact (unjittered) phase; use `--block=1` for a
per-sample trace.

`reset()` clears every state: filters, feedback, the LFO (phase 0 = sweep bottom), the gate
smoothing, the sync glide and lock, any model switch in flight and the parameter smoothers, so the next block
is a fresh engine's. The processor calls it when a Phase 90 / Small Stone block is switched
back on (§2.6, §6.5). Non-finite samples, and samples of 1e6 magnitude or more, are replaced
by 0 at the top of `process()`, for every model.

#### 4.6.6 Acceptance (`check_phaser.py`)

| # | Check | Pass |
|---|---|---|
| P1 | Classic null (§2.11) | 0 differing samples |
| P2 | Parity with `phaser_ref.render()` (imported from the prototype): 4 s of pink noise −18 dBFS plus an impulse train, each voicing × color, rates 0.143, 1.071, 3.72, 9.76 Hz (Phase 90) and 0.1585, 0.316, 1.778 Hz (Small Stone), at 44.1, 48, 88.2, 96, 192 kHz (`--quick`: 48 kHz only) | rms(C++ − ref)/rms(ref) ≤ −80 dB; max abs difference ≤ 1e-4 (reported per voicing: Phase 90 script, Phase 90 block, Small Stone off, Small Stone on) |
| P3 | Frozen-fc table: the engine at a fixed sweep position (adapter key `freeze_u` = −1 and +1, `PhaserFx::freezeSweep`) reproduces `evidence/fit/spec_check.txt` (notch 1/2 frequencies and levels, peak, DC at 44.1, 48, 88.2, 96, 192 kHz); the impulse response's last eighth holds under 1e-12 of its energy | frequencies ± 0.1 %, levels ± 0.05 dB (e.g. block 48 kHz: 419.4 / 1934 Hz, −29.18 / −15.27 dB at the bottom; 1589.9 / 7481 Hz, −25.27 / −18.46 dB at the top; the same notch frequencies at every rate) |
| P4 | Trajectory against GR6 (`evidence/fit/trajectories.tsv`, `itrack.py`, 48 kHz), regression guard only (`--quick`: the noon rates) | Phase 90 block ≤ 0.8 % rms, ≤ 1.4 % max over the six rates; Small Stone off ≤ 2.0 % rms; on ≤ 1.2 % rms (current values `compare.txt`: block 0.41 to 0.70 % rms, max 1.18 %; off 0.12 to 0.21 % rms, max 0.53 % (was 1.24 to 1.25 % rms); on 0.30 to 0.44 % rms, max 1.21 % (was 0.80 to 0.85 % rms); the Small Stone rows have no max bar) |
| P5 | Rate: three rows. (a) "rate (audio)": the notch-1 period tracked from the P4 renders; (b) "rate (LFO)": the phase trace of Phase 90 and Small Stone at 0.01, 0.143, 1.071, 3.72, 9.76, 20 Hz, block 1, 48 kHz; (c) "sync (CH13)": 120 BPM, 1/4 note, song starting at beat 0.3, blocks 64 and 512, both models, Color on | (a), (b) period error < 0.01 %; (c) period 0.5 s ± 0.1 %, phase-0 timing error ≤ 1 ms |
| P6 | Stability: Phase 90 block and Small Stone Color on, 30 s at 10 Hz of full-scale uniform noise, at 22.05, 48, 192 kHz | finite, \|out\| < 4 |
| P7 | Model / color switch on a chord (five switches, at 48 kHz blocks 64 and 512, 44.1 kHz blocks 512 and 37, 96 kHz block 384), and Classic with a color toggle. The fade is measured without a dry point: 30 ms unheard, the wait for an acceptable moment, then the crossfade | no step > 3 dB within 1 ms (measured ≤ 0.12 dB); fade 40 ± 2 ms (measured 39.7 to 39.9); the Classic color toggle bit-identical to no toggle |
| P8 | depth/mix independence | Phase 90 and Small Stone output bit-identical for any `depth`/`mix` values (0/0, 1/1, 0.13/0.91, and ramps) |
| P9 | Rate independence, against each voicing's own 48 kHz render at 44.1, 88.2, 96, 192 kHz (`--quick`: 44.1, 48, 96). Three rows per voicing: **trajectory** (notch 1 at the noon rate, tracked like P4), **frozen notches** (notches ≤ 10 kHz at sweep positions −1, −0.5, 0, 0.5, 1; same notch count) and **frozen response** (\|H\| from 20 Hz to 10 kHz where the 48 kHz response is within 20 dB of its peak) | trajectory ≤ 0.3 % rms; notches ≤ 0.2 %; response ≤ 1.8 dB. **Trajectory and notch rows are met** (0.049 % Phase 90 script, 0.048 % block, 0.182 / 0.154 % Small Stone off / on rms; notches within 0.166 %). **The frozen-response bar is the structure's limit, not a target:** measured script 1.69 dB, block 1.40, Small Stone off 1.02, on 1.66 (1.02 to 1.69 dB); the first bar of 0.1 dB cannot be met by four first-order stages at 44.1 / 48 kHz (`evidence/fit/rate_limit.txt`; D-30). At 44.1 kHz against 192 kHz the script (2.05 dB) and Color on (2.00 dB) are over the 1.8 dB bar; the row compares every rate against 48 kHz only |
| P10 | Sync jumps: a 220 Hz tone at −12 dBFS, rate 1 Hz, block 64, 48 kHz; a song-position jump at sweep phase 0, 0.25, 0.5, 0.75 of +0.05, +0.25, +0.5, −0.3 cycle, per voicing (16 jumps) | the largest tick above 2 kHz ≤ −60 dBFS (Kaiser FIR highpass residual) |
| P11 | Re-enable, driven as the processor will drive it: a loud signal, no `process()` for 1 s (`on` 0), then `reset()` (adapter key `reset_on_enable=1`) and processing again; silent input and continuing signal, at 48 kHz block 64 and 44.1 kHz block 512 | silent input peaks ≤ −60 dBFS; a continuing signal differs from a fresh engine's output by ≤ −60 dBFS |
| P12 | Non-finite input: NaN, +Inf, −Inf samples in either channel and a 64-sample NaN burst; Classic and the four voicings, blocks 64 and 512. And huge input: a +3e38 / −3e38 pair, 2e6 and −1.5e6 samples, the same cases | output finite and bit-identical to the render with zeros in place of the bad samples (nothing latched) |
| P13 | Pitch of a held note against Guitar Rig 6: 196 Hz sine at −12 dBFS, instantaneous frequency of the output, p99 of \|cents\|, three rates per voicing (block 0.247 / 1.071 / 3.721 Hz; Small Stone 0.316 / 1.778 / 5.012 Hz), `evidence/fit/motion.tsv` | ours within 15 % of Guitar Rig 6's p99 at the worst rate. Measured: Phase 90 block 13.8 % (0.6 / 2.5 / 5.8 cents against 0.7 / 2.6 / 5.7), Small Stone off 13.6 % (12.7 / 65.9 / 165.7 against 12.4 / 57.9 / 158.5; it was −79 %), Small Stone on 1.9 % (25.0 / 136.8 / 372.6 against 24.9 / 134.9 / 379.9) |
| P14 | Waveform null against Guitar Rig 6: the same 8 s guitar-like take through both (`evidence/fit/gr6_takes/`), our LFO started at the phase that nulls best, one least-squares gain; residual re Guitar Rig 6's output at the two rates | Phase 90 block ≤ −25.9 dB (measured −26.7 / −27.1 at 1.071 / 3.721 Hz), Small Stone off ≤ −24.0 dB (−40.8 / −37.3 at 0.316 / 1.778 Hz; was −11.6 / −11.0), Small Stone on ≤ −29.2 dB (−36.0 / −31.7). The dry signal alone nulls at −3 to −5 dB. Needs the six Guitar Rig 6 renders in the repository (§8 D-40) |
| P15 | Sync jump: tones of 82 / 147 / 220 / 330 Hz at −12 dBFS, jumps of 0.1 / 0.25 / 0.5 cycle from sweep phases 0 / 0.25 / 0.5 / 0.75, blocks 16 to 4096 at 48 kHz and phases 0 and 0.5 at 44.1 / 96 / 192 kHz (252 renders per voicing); a second jump of +0.25 about 15 ms after a +0.5 one; settle time to within −40 dB of the voicing running at the new phase all along, for jumps from sweep phases 0 and 0.25, blocks 16 to 4096 at 48 and 192 kHz | the largest tick above 2 kHz ≤ −60 dBFS (measured worst: −140.5 script, −138.0 block, −83.8 Color off, −81.8 Color on; two jumps −75.8 at worst); the settle times over the block sizes spread by at most 20 % (measured 169.4 to 170.6 ms script, 145.6 to 147.6 block, 7.7 to 8.5 Color off, 40.6 to 41.2 Color on, for a half-cycle jump from phase 0: the block-size spread is 1.2 ms or less) |
| P16 | Sync lock: song position jittered by ±2 and ±8 samples per block, tempo stepped 120 to 135 BPM in 1 BPM steps, ramped 100 to 140 BPM over 4 s, at 1/4 and 1/16 notes, 44.1 kHz block 64 and 4096, 96 kHz block 512, 192 kHz block 16 | 0 glides; the high-passed difference from an exact host (or from the free-running LFO at the same rates) ≤ −80 dBFS (measured worst −86.6); phase error after 1 s < 0.01 cycle (measured 0.0058 at worst) |
| P17 | Switch level: every ordered pair of the five voicings (20), held notes at 82 / 196 / 440 Hz at −12 dBFS and a guitar-like take, switched at two times; 20 ms frames over the 90 ms from the switch | at most +1.5 dB above the higher and 3 dB below the lower of the two voicings' own levels (measured +0.79 and −2.20 dB, was +0.95 and −2.88; engineer's run); click above 2 kHz ≤ −60 dBFS (measured −93.8) |
| P18 | Level against bypass: ungated K-weighted loudness over three guitar-like takes (mixed, a low riff, a high take), each voicing at its starting rate; and the worst peak for a full-scale input | the register mean within ±0.25 dB of unity (measured 0.00, −0.01, 0.00, 0.00 dB); the peaks are reported: Phase 90 script +2.4 dB, block +5.9, Small Stone off +5.7, on +13.6 (headroom, §4.6.4) |
| P19 | Switch transit: every ordered pair of the five voicings (20), held notes 110 / 256 / 368 / 659 Hz at −12 dBFS and a guitar-like take, LFO rates 0.3 and 1.3 Hz, switched at four LFO phases of the outgoing voicing, followed from the request until the output is the incoming voicing's (5 ms windows of the note's band for held notes, 20 ms frames for the take; a dip counted where the lower voicing is within 40 dB of the input) | dip at most 3 dB under the lower voicing; rise at most 1.5 dB over the higher; off the note (outside ±40 % of it) at most 0.5 dB above the larger of −60 dBFS, the voicings' own steady content there and an ideal equal-power fade between the two at the same moment and length; request to fully new at most 400 ms. Measured 3.00 dB dip (was 32.8; no margin against the bar), +1.25 dB bump (was +1.62), 69 to 390 ms (engineer's run) |
| P20 | Sync drift: a host whose song position runs 1, 5 or 10 % faster, or 10 % slower, than its reported 120 BPM (adapter key `host_drift`), 1/4 and 1/16 notes, the four Phase 90 and Small Stone voicings; 44.1 kHz block 64, 48 kHz block 512, 44.1 kHz block 4096, 192 kHz block 16 | 0 glides; the high-passed difference from the same voicing free-running at the host's true rate ≤ −80 dBFS (1/4 notes only, as P16); phase error after 2 s < 0.01 cycle. Measured 0.0024 cycle, ticks ≤ −85.4 dBFS (engineer's run) |
| switch rt safety | Model and Color switched every 50 ms, no allocation, reset-repeat identical at 48 kHz / 64, 44.1 kHz / 37, 96 kHz / 512, 192 kHz / 4096; CPU while switching | `--rt-check` clean, output identical after `reset()`, CPU ≤ 0.5 % (the Small Stone budget; measured 0.22 % in the run of this document) |
| finite (bad keys) | `model` 7 and −3, `rate` 1e9, −4, NaN, +Inf, `sync_phase` NaN and 1e12, Classic with NaN depth/rate and −Inf mix | output finite, \|out\| < 1e3 |

P9 to P12 are new with the sample-rate, sync-jump, re-enable and non-finite fixes; P13 to P18 and the switch row came with the second round (Small Stone refit on phase, sync lock, crossfaded switches, unity loudness); P19 and P20 came with the third (switches that wait for and bridge antiphase, the second-order sync lock). Full run: 138 checks, 0 FAIL (engineer's run; 133 before P19 and P20, 96 before the second round; P7 now measures the fade without a dry point, its bars unchanged; P4, P13 and P14 read the same as before the third round). The table lists every row id `check_phaser.py` has, with its bar. P11
depends on the processor calling `reset()` on re-enable, which it now does (§2.6); the adapter
key stands in for it in fxbench.

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
- **Non-finite and huge input:** a NaN or Inf sample, and a finite sample beyond 1e6 in
  magnitude, are silenced on the way in (voices 1-4 and Classic alike), so they cannot enter
  Bias's envelope or Harmonic's filter state and spoil at most their own output sample (N1,
  Y7). Everything below 1e6 passes untouched, so Classic stays bit-identical (Y7: 9.9e5 gives
  0 samples differing from the legacy code).
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
- **Sync:** rate from the dispatcher; with `syncPhase ≥ 0` the LFO follows the host's song
  position so the dip lands on the grid `[study: R4]`. Rate multiplier: none (§8 D-13). The
  lock tells a host jump from the engine's own error. Each block the host phase is compared
  with where the previous host phase, advanced by the previous block's rate, predicts.
  - A difference above `max (0.01 cycle, 2 ms of LFO time)` (`kJumpCycles`, `kJumpSeconds`)
    is a **jump** (transport start, locate, a loop that is not a whole number of cycles, a
    note-value change, sync switched on). The LFO moves to the new phase once, and the
    gain target crossfades from the old phase's LFO to the new one's over **10 ms of
    samples** with a raised cosine (`kJumpFadeSeconds`), whatever the host block size. A second
    jump inside the fade starts from the blend heard at that moment. The 120 Hz smoother alone
    left a slope step that clicked at −44 dBFS on a low note (S1).
  - Anything smaller is host jitter or the residue of a tempo change. The LFO runs free at the
    tempo's rate and its own error against the host is pulled in by a **phase-locked rate
    nudge**, error / 150 ms (`kLockSeconds`), at most 3 % of the rate (`kLockMaxNudge`).
    Released (sync off), the LFO keeps its phase and its rate glides from the tempo's to
    the knob's.
  - Measured (Y1, Y2): click above 2 kHz for a 110 Hz note at −12 dBFS over host blocks 16 to
    4096 at 44.1 to 192 kHz, before the rule → now: Smooth −46.1 → −114.4 dBFS, Harmonic
    −48.9 → −100.7, Bias −59.0 → −129.2; Optical −58.9 against −58.8 at its own lamp edges.
    Host jitter of ±2 and ±8 samples per block: 6301 / 21286 jumps before, 0 now, phase error
    under 0.007 cycle; 1 BPM steps and a 100 to 140 BPM ramp: 0 jumps.
- **Voice switch** (no `fx::EngineSwitcher`, §2.4). Both voices run on **one LFO** (the phase
  is carried across, Classic included: Classic's dip is at phase 0.75, the others' at 0)
  and their outputs crossfade over **40 ms** (`kVoiceFadeSeconds`). The incoming voice joins
  its own steady trajectory: its gains are snapped to their targets, Optical's photocell is
  set to its periodic state, Bias's detector is primed. Level outside both voices' own
  trajectories: 10.06 dB before, 0.03 dB now over 160 switches (Y3).
- **LFO start after `reset()`, sync off:** the LFO starts where the voice's gain is at its
  **crest**, so the first note is heard at full level: Smooth at phase s (its skew), Bias at
  0.5, Optical and Harmonic at 0. (The first version started at the dip: Smooth read −25.3 dB
  re crest 20 ms after a re-enable, now −0.4 / −0.5 dB at depth 0.6 / 1, Y4.) With sync the
  host's phase rules.
- **`reset()`** clears every state; voices 1–4 start with their knobs at the last values sent
  and **every gain at unity**, so the first block glides from the input into the tremolo
  through the gain smoother instead of stepping to the dip (the reference starts at y0 = 1).
  Every `setParameters()` until the next `process()` snaps the knobs. The processor calls it
  on a non-Classic voice when the block is switched back on (§2.6;
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
  **Pulse edges are raised cosines** of 1 ms (`kPulseEdgeSeconds`), centred on the hard
  edge, ahead of the 120 Hz smoother (the prototype's pulse is a hard step). A one-pole alone
  starts every edge with a corner, which splattered above 2 kHz. The 10 to 90 % time is 2.96 ms
  (a one-pole alone: 2.92); the energy above 2 kHz is 21.9 to 29.4 dB below a one-pole edge of
  the same time over rates 1 to 20 Hz and duties 0.1 to 0.9 (Y5; the run of this document
  prints a least improvement of 21.9 dB). An edge must fit inside the narrowest pulse
  (`static_assert`, 20 Hz × 1 ms < 0.1).
- **Optical (2)** `[study: R2, proto :193-203]`: lit = (ph ≥ 0.75) (lamp on a quarter of the
  cycle); target = 1 − 0.9·depth·lit (floor −20 dB at depth 1); gain follows target with a
  one-pole whose corner is 31.83 Hz (τ 5 ms) while falling and 7.96 Hz (τ 20 ms) while
  rising, chosen by the direction of change; start 1.0. Ignores wave and shape; no make-up.
  Spread shifts the right channel's lamp phase.
- **Bias (3)** `[study: R5, proto :267-288]`: u = 0.5 + 0.5·cos(2π ph) (1 = dip);
  env = peak follower of the block's mono sum (attack 5 ms, release 150 ms);
  q = env/knee_lin, knee_lin = 10^((−24 + 24·shape)/20) (default −12 dBFS);
  `gain = sqrt((1 − depth·u + q)/(1 + q))`, then the 120 Hz smoother. The detector input is
  **AC-coupled at 10 Hz** (`kBiasDetectorHz`): a DC offset is a fault no AC-coupled amp lets
  reach the bias, and DC 0.01 under a −40 dBFS tone used to lower the depth from 14.4 dB to
  11.7 dB; now it stays 14.41 dB at DC +0.01 and ±0.1 (Y8). **The onset dip is the voice:** a note
  that lands on the dip is cut by up to 4.7 dB for about 6.5 ms (depth 0.75, 196 Hz at
  −6 dBFS) while the 5 ms follower catches up, as the study's envelope does (Y8 reports
  −4.7 dB). Clean gain, no waveshaper (the amp supplies the distortion). The name rests on an inferred reading of
  Guitar Rig's AC30 (V:67).
- **Harmonic (4)** `[study: R6, proto :315-331]`: L = TPT one-pole low-pass at f_x,
  H = x − L, f_x = 300·2^(2(tone − 0.5)) Hz; u = 0.5 − 0.5·cos(2π ph);
  `out = sqrt(2/(2 − depth))·(sqrt(1 − depth·u)·L + sqrt(1 − depth·(1 − u))·H)`;
  the two band gains pass the 120 Hz smoother. Literature-only voice (no installed
  reference); judge by ear before shipping (report.md:386).

#### 4.7.6 Latency, CPU, fxbench

Latency 0. CPU per instance: Classic as today; Smooth, Optical, Bias ≤ 0.10 %; Harmonic
≤ 0.15 % (`BUDGETS`), at every Width (Y6: worst over widths 0 to 1, in the run of this
document, Smooth 0.085, Optical 0.038, Bias 0.062, Harmonic 0.071 %; Smooth with a jump fade
always on, 0.151 %, is printed and has no bar). The per-sample `std::pow` and `std::cos` are
gone: the loudness law comes from a table once Width rests, the cosine is a series, settled
knobs are held and not stepped, and one LFO evaluation serves a channel pair at spread 0.
Smooth's worst over widths went from 0.134 % to 0.066 % on an idle machine (commit 26148a8).

fxbench keys: `voice rate depth mix width tone shape wave sync_phase on`, and a scripted play
head, `host_sync host_bpm host_beats host_play host_ppq host_jitter` (`tools/fxbench/README.md`).
With `host_sync=1` the adapter stands in for the processor's dispatcher: `rate = host_bpm /
(60 · host_beats)` and, while `host_play` is 1, `sync_phase = frac (ppq / host_beats)` at each
block start, the song position advancing by the block's tempo. On `on` going from 0 to 1 with a
voice other than Classic the adapter calls `reset()` before the next block, as the processor
will. `--trace=gain` writes the left (and right) gain; `--meters` writes per block the engine's
song-position jump count and its LFO phase at block start minus the true (unjittered) host
phase.

#### 4.7.7 Acceptance (`check_tremolo.py`)

Steady 1 kHz sine at −18 dBFS, 4 Hz unless stated; envelope from `tremenv.py`
(study evidence/tools) or the gain trace. Verifier tolerances: rate 0.1 %, gain 0.005,
edge 0.3 ms, depth 0.15 dB.

| # | Check | Pass |
|---|---|---|
| T1 | Classic null (§2.11) | 0 differing samples |
| T2 | Parity with `tremolo_proto.py` per voice (Smooth at wave 0/0.5/1 and shape 0.2/0.5/0.8; Optical; Bias; Harmonic), spread 0 and 180. The Smooth and Bias references start at the voice's crest (§4.7.4), Smooth's pulse has the raised-cosine edges, Bias's detector the 10 Hz AC coupling | rms difference ≤ −80 dB re output |
| T3 | Smooth depth 1, wave 0 | crest +3.0 ± 0.2 dB; floor ≤ −60 dB (spread 0); loud half 0.50 ± 0.01 |
| T4 | Smooth depth 0.5 | floor −12.0 ± 0.5 dB |
| T5 | Smooth wave 1 (pulse) | 10–90 % edges 2.9 ± 0.5 ms (measured 2.96); loud fraction = s ± 0.01 (shape 0.5 / 0.25 / 0.75); splatter 3–6 kHz around the carrier (the band is carrier + 2 … 5 kHz) ≤ **−88.8 dBc at worst-case carrier phase** (8 carrier phases incl. cosine, carriers 997 / 1001.5 Hz, and a dense sweep of 64 phases at 1 kHz plus off-grid carriers; measured worst −90.1 dBc at 1 kHz). The raised-cosine edges of §4.7.5 moved the bar from −58 dBc (the study's −64.8 dBc held only with the carrier at a zero crossing on every edge) `[ver: V:32, V:44; r2-mod N9]`. **Splatter by carrier frequency** (worst of 16 carrier phases; for a low carrier the band also catches the edge's lower sideband, which adds coherently): carriers 40 / 48 / 64 / 84 / 112 / 148 / 196 / 248 / 332 / 440 / 660 / 1000 / 1320 / 2000 Hz must stay at or below −84.8 dBc from 40 Hz, −86.0 from 82 Hz, −87.6 from 200 Hz, −89.1 from 500 Hz (the bars are the measured value plus 1 dB; measured −85.8 at 40 Hz to −90.6 at 2 kHz) |
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
| Y1 | Sync transitions: sync engaged and released, note value changed, locate, loop and transport start while a 110 Hz note plays, voices 1 to 4 at depth 1, host blocks 16 / 64 / 128 / 256 / 512 / 1024 / 4096 at 48 kHz and 64 / 512 / 4096 at 44.1, 96, 192 kHz, two event instants each; plus 0 dBFS takes (48 kHz, blocks 64 and 256) | peak above 2 kHz from 5 ms before the event to 250 ms after ≤ −70 dBFS at a −12 dBFS input (−58 at 0 dBFS), or what the voice puts there on its own in the same take outside the event, whichever is higher; for Optical also its steady lamp edges at the worst carrier phase |
| Y2 | Sync under host jitter and tempo changes (`host_*` keys): song position jittered by ±2 and ±8 samples per block, tempo in 1 BPM steps, a 100 to 140 BPM ramp over 4 s, at 1/32 and 1/4, host blocks 16 to 4096, 22.05 to 192 kHz (104 takes) | 0 song-position jumps; from 1 s on the LFO within 0.01 cycle of the true (unjittered) host phase (measured worst 0.0068) |
| Y3 | Voice switch against both voices' trajectories: every ordered pair of the five voices at depth 0.6 and 1, four switch instants (160 switches), synced at 240 BPM, 48 kHz, block 64; level from a quadrature probe in 5 ms frames floored at −40 dB, from 5 ms before the switch to 100 ms after | level within 1.0 dB of the span of the two voices' own levels (measured 0.03 dB outside it); click above 2 kHz of a 110 Hz note over the same window ≤ −62 dBFS or the two voices' own figure there, whichever is higher; Optical on either side adds its lamp edges (Optical to Classic −61.1 against its own −58.8) |
| Y4 | Switch-on: gain 20 ms after a re-enable, voices 1 to 4 at depth 0.6 and 1 | ≥ −1.0 dB re crest (measured −0.5 to +0.0) |
| Y5 | Pulse edges against a one-pole edge of the same 10–90 % time, rates 1 / 2 / 4 / 10 / 20 Hz, duty 0.1 to 0.9 | energy above 2 kHz at least 8 dB lower (measured 21.9 dB at the least) |
| Y6 | CPU at widths 0 to 1, every voice (worst over the widths) | ≤ the voice's budget (0.10 / 0.10 / 0.10 / 0.15 %) |
| Y7 | Huge finite samples in every voice: eight bursts of 3e38, of 3e38 and −3e38, and of 1e7 in pink noise; Classic with one sample of 9.9e5 | output finite and 0.5 s after the last burst equal to the clean render (≤ −100 dB rms; measured bit-identical); Classic: 0 samples differ from the legacy code |
| Y8 | Bias depth with DC on the input (DC 0, +0.01, +0.1, −0.1 under a −40 dBFS tone) | each depth within 0.5 dB of the DC 0 depth (measured 14.41 dB in all four); the onset dip on the dip (−4.7 dB) is printed, not a bar |
| Z1 | `--rt-check`, `--reset-repeat` and finite output during voice switches (32 runs) | 0 failing |

Full run: 95 checks, 0 FAIL (86 before this round, all still passing). The table lists every
row id `check_tremolo.py` has, with the bar the script enforces; the common checks of §2.11 run
for every voice as well.

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
  `gen:music`; hash with `juce::SHA256` over the interleaved float bytes; run the message
  loop (60 ms), because the processor reports latency from a timer (§2.9); record
  `getLatencySamples()` after the last block.
- File row: `case  fs  block  sha256  latency`.
- `--verify ... --restore host|preset` renders each case the way an old session reaches the
  plugin instead of setting it up directly. A processor writes the case's order and
  settings; the state is saved; every parameter the base commit did not have (version hint
  4) is taken out; the result is restored into a fresh processor through
  `setStateInformation` (`host`) or `applyStateVar` (`preset`). The type ids are not set by
  hand, so the legacy pin alone has to bring every block back to Classic for the captured
  hashes to hold. A restored row must (a) have every type id of a block in the chain come
  back Classic, and (b) have every value the base commit had come back as the directly set
  case has it, so the base capture and the expected-change rules apply. A state round trip
  can move a value in its last bit (a parameter left at its default holds the unsnapped
  default until it is set; the base commit does the same). Such a row cannot be compared
  with the base hash, so it is compared with the same state saved with explicit Classic
  types (the pinned ids written as 0 instead of left out) and restored the same way: audio
  hash and latency must be equal, and the pin must be exactly that. These rows print as
  `val` and are counted apart from the identical and changed rows; any row that differs
  prints `FAIL`.
- `--both-orders host|preset` renders every row twice and requires equal hashes and latency. The
  case's old session (every newer id left out) is restored before `prepareToPlay`, as a session load
  in most hosts does, and onto an instance already prepared, as a host that prepares first or a
  preset recall does, with 100 ms of silence ahead of the music. `host` restores through
  `setStateInformation`, `preset` through `applyStateVar`. 0 of 339 rows differ (parent's run). It
  compares the two orders with each other, not with the old build (§8 D-52).

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

After the processor's second round the legacy side also mirrors the bypass fader (the legacy
class runs through the processor's own `BypassFader`, reset where the processor resets) and the
`mod_type` crossfade (hold, wait and the 40 ms `PowerCrossfade`). So `--legacy-check` proves
static Classic renders against the old code, not transitions. Transitions against the real old
build are covered by the verifiers' harness (`build-verify/compat`, `build-verify/processor-r2`).
The smoke test runs at 44.1, 48, 96 and 192 kHz.

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
| `src/library/PresetManager.cpp:196-205` | `writeFactoryPreset`: pass every new type id as 0 to `makeStateVar()`, which builds the state without touching the live parameters (§8 D-2) |
| `src/WebEditor.cpp:39-118` | append relays: sliders (+64), toggles (+6: `comp_auto_makeup` ×3, `mod_color` ×3), combos (+25), in Parameters.cpp append order |
| `src/WebEditor.cpp:120-122` | `static_assert`s → 183 / 36 / 42 with updated reasons |
| `src/WebEditor.cpp:413-414`, `:1018-1024` | reset the new meter atomics on editor open; emit `dynMeters` (§2.8) in `timerCallback` |
| `src/WebEditor.h:43` | relay count comment |
| `docs/REACT-UI.md:59-107, 284` | relay lists and counts; `dynMeters` event contract |
| `src/dsp/FxBlocks.h/.cpp`, `PluginProcessor.cpp` (processor round) | `BypassFader` (§2.6): per-kind faders (`gateFader`, `compFader[3]`, `driveFader[3]`, `eqFader[3]`, `modFader[3]`), `engineStarts`, `applyGate`; `processBlock` zeroes non-finite and absurd (1e6 or more) samples at the input and again before the DC blocker (§2.2); `DelayGlide`, `PowerCrossfade` (round two, §6.5) |

### 6.1 Gate

| File:line | Change |
|---|---|
| `PluginProcessor.cpp:805-806` | `gate.prepare (spec)` (prepare resets) |
| `PluginProcessor.cpp:919` | `gate.reset()` in `releaseResources`, and in `resetBlockInstance` on re-entry: the gate is now drained through `pendingReset` like every block and runs as bypassed while pending (`gateRuns`). Classic's reset only drops its curve, so Classic is unaffected (legacy case `gate_absent`) |
| `PluginProcessor.cpp:1064-1065` → | after `deferGateToAmp` (now computed after the reset drain, from `gateEngine`: the gate runs, or a Tight/Smooth gate is still fading out): D = Σ `blockLatency (order[i])` for i from `gateIndex + 1` through `ampIndex`; `blockLatency` is the per-entry term `computeChainLatency` sums (amp, amp2 with a live model, FX slots), so the amp's own latency and anything between share one rule; `gate.setCurveDelay (deferGateToAmp ? D : 0)`, with D clamped in the processor to `GateFx::kMaxCurveDelaySamples` (65535). Drive latency is in `blockLatency` (§6.3), so the host report, the lane ring and D all count it |
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
| `blockLatency` (the per-entry term `computeChainLatency` sums and the gate's curve delay uses) | for drive instances: `latency = DriveFx::latencyFor (type, preparedSampleRate)`. A non-Classic type returns it whenever the drive is in the chain, bypassed or waiting for its reset included (the FX-slot rule): the block holds its input back by the same amount through `BypassFader`, so toggling it never changes the plugin's latency. Classic returns it only while `drive_on` and not waiting for its reset (`waiting` is `pendingReset`, passed by the audio thread), the amp rule, so a bypassed Classic drive is not delayed and old sessions keep their audio. `latencyFor` is a pure static function of (type, rate) so the message thread (`updateLatency`) can call it without touching the audio-thread object. `preparedSampleRate` is an atomic the processor stores in `prepareToPlay`: `getSampleRate()` is only what the host last announced (0 when `prepareToPlay` is called directly, as the console tools do) |
| `PluginProcessor.cpp:2150`… | the lane ring (`compensateLaneLatency`) aligns drive lanes automatically. It follows the lanes' per-sample delay difference (`computeLaneDelta`: blocks by `blockLatency`, a drive by the delay it applied per sample), reading taps with 4-point Lagrange interpolation. A difference that moves smoothly (a drive's own slide) is followed exactly; a jump of up to `kLaneSlideSamples` (16) restarts a slide from where the taps are now (a second jump within the 10 ms retargets it); a larger one (a model swap, a hosted plugin) crossfades from what is heard to the new taps. History the ring already holds is read, so the ring is never cleared (step 0.25 at first, 0.0033 after the first round) |
| `PluginProcessor.h` `DriveTimeline` | per drive instance, mirrors `DriveFx`'s own type-switch timing (`Drive.cpp`: settle 10 ms, fade 20 ms, slide 10 ms, each at least 8 samples): the outgoing type's latency, a rise slid in over the settle, both at the larger latency through the fade, a drop slid out over the 10 ms after it. Lane alignment and the reported latency follow what the drive applies, not what its parameters ask for; the reported latency holds the larger figure for up to 40 ms during a drive type switch, and lanes stay within 0.41 samples (§8 D-43) |
| tests | reported latency with drive on/off and per type; D18 split-lane flatness; `splitA`/`splitAB` expectations |

`DriveFx` therefore has, besides §2.1: `static int latencyFor (int type, double sampleRate) noexcept;`
(`getLatencySamples()` returns `latencyFor (currentType, preparedRate)`).

Invariant: latency is reported from the message thread only (§2.9): the audio thread stores
what it applies, the timer, the parameter listeners and `updateLatency()` hand it to the host,
and with no audio running the message thread computes it; both paths use
`computeChainLatency`. `DriveFx` cannot report its applied delay during a switch, so
`DriveTimeline` has to follow any change to `Drive.cpp`'s switch timing (§8 D-50).

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
    static int crossfadeSamples (double sampleRate) noexcept;   // 40 ms
    static int crossfadeWaitSamples (double sampleRate) noexcept;          // 250 ms
    static int crossfadeLongestWaitSamples (double sampleRate) noexcept;   // 400 ms
    static int crossfadeHoldSamples (const ModParams&, double sampleRate) noexcept;
    void prepare (const juce::dsp::ProcessSpec&);
    void reset();
    void setParameters (const ModParams&);
    void process (juce::dsp::AudioBlock<float>);
};
```
- Rate resolution (`resolveTiming`): Classic voicings always get `mod_rate` and
  `syncPhase = −1` (bit-identity). Otherwise sync Off is `mod_rate`, free-running; a note
  value is `rate = bpm / (60·beats)` at the host tempo, with the delay's fallback (no tempo,
  or ≤ 1 BPM → 120 for the rate), and `syncPhase = frac(ppqPosition / beats)` while the host
  plays, gives a position and reports a tempo above 1 BPM, else −1 (free-running at the
  synced rate). A tempo at or under 1 BPM runs the LFO free: a song position read at a tempo
  the song does not run at would hold the LFO still. The processor hands the dispatcher the
  tempo as reported (`hostBpm`: 0 when the host gives none, a tempo that is not positive or
  is not finite), so a tempo at or under 0 runs the LFO free too. The engines get a sync
  phase on every block while the transport plays and none when it is stopped. A synced rate outside the
  engines' [0.01, 20] Hz is clamped **and runs free**: a clamped LFO cannot follow the song
  position (1/32 above 150 BPM, 1/16T above 200, 4/1 below 9.6).
- `mod_type` switch — **a crossfade, not `fx::EngineSwitcher`.** On a change the incoming
  engine is reset in that block and runs from there, exactly where the old block reset
  everything and jumped; the outgoing engine keeps running on a copy of the input with the
  last parameters it had (still following the song if it was synced), and is heard alone
  while the incoming one settles unheard. That hold (`crossfadeHoldSamples`) is 30 ms for
  a phaser or tremolo, 50 ms for chorus modes 1-4, and for Classic chorus
  ceil ((7 + 10 × depth) ms × fs) + 1 samples (the 7 ms centre delay plus the swing
  `juce::dsp::Chorus` adds, depth clamped to [0, 1]) plus 10 ms for the power estimates to
  catch the step its wet makes. Then `PowerCrossfade` starts a 40 ms power-matched crossfade
  (`crossfadeSamples()`), equal-power gains on a cornerless ramp scaled by the two outputs'
  measured power and correlation. It starts only when the correlation is acceptable (at least
  -0.5) and not heading toward antiphase (by its trend, it would not fall below that before the
  fade is over). After 250 ms it takes any acceptable moment, and at 400 ms it gives up and
  starts anyway. The level correction is bounded at +18 dB and applied in **three bands split at
  150 Hz and 1 kHz, with a gain for each band of each channel** (complementary one-pole filters),
  because two effects' lows can cancel while their highs (a pick's attack) add and one broadband
  gain would lift the attack, and a channel's outputs can oppose each other while the other
  channel's add. **A fade whose power lies mostly below 150 Hz takes three times as long, 120 ms
  instead of 40** (`kBassShare` 0.5, `kBassLengthFactor` 3), which keeps a 40 ms fade's spread of a
  55 to 110 Hz note out of the sub-bass. When the fade ends the output is the incoming engine alone, the old
  block's output from then on. The switcher's fade through dry was not used: it resets the
  incoming engine 20 ms after the request, so that type would run late for good. A second
  request inside a fade waits for it to end, then fades from the type reached to the newest
  request; requests in between are dropped (§8 D-53, D-65). A block larger than `prepare()` promised completes the switch outright (the old
  jump). Snap on the first block after prepare/reset. Only the running engines get
  `setParameters`, as the old block only set the active type. The price is a wait of up to
  400 ms before a type switch is heard, then the fade (§8 D-32, D-42). Two effects in antiphase on a
  held pure note cannot be crossfaded without a dip, and the check bounds that as a distribution
  (§8 D-53).
- Voicing switches inside one type (and colour switches) go straight to the engines,
  Classic included, which fade them themselves and are never reset for them. Song-position
  jumps reach the engines unsmoothed: `syncPhase` is passed on as computed, and each engine
  glides or dips itself (tremolo and phaser §4.7.4 and §4.6.4, chorus §4.5).
- `processBlock` reads the play head once (`HostTransport { bpm, ppqPosition, playing }`) and
  hands it to `processChainBlock`; the delays take `bpm` from it, the mod blocks all three.

| File:line | Change |
|---|---|
| `FxBlocks.h:114-134`, `FxBlocks.cpp:349-449` | replace the old `Modulation` with the dispatcher above; delete `Drive`, `ToneStackEQ`, `NoiseGate` (`FxBlocks.h:15-112`, `FxBlocks.cpp:15-30, 45-193, 222-344`) and the AudioDSPTools include; keep `DcBlocker`, `CabSim` byte-identical |
| `PluginProcessor.cpp:818, 926, 1845` | prepare/reset unchanged in form |
| `PluginProcessor.cpp:1428-1442` | `modParams (k, transport)` fills `ModParams` from the 4 existing + 9 new ids, raw, and the transport |
| `PluginProcessor.cpp:965-973` | capture tempo, `ppqPosition` and `isPlaying` once per callback (`HostTransport`) |
| tests | smoke `runModEngineTests`: every new voicing (knob steps, voicing and colour switches) through the processor equals the engine alone; sync 1/8 at 120 BPM for CE, Phase 90 block and Smooth, host playing and stopped: equal to the engine alone at 4 Hz with `frac(ppq/0.5)` or free-running, output repeats every 250 ms (residual ≤ −60 dB; a 0.1 % tempo error exceeds it), Smooth's dips within 2 ms of each eighth; 120 → 150 BPM followed; no play head = 120 BPM; clamped 1/32 at 200 BPM free at 20 Hz; Classic ignores sync; instances 2/3 drive their own ids; voicing and `mod_type` switches on P7's chord ≤ 3 dB per 1 ms; the `mod_type` level check covers 55 to 440 Hz at four mixes (2640 switches; hold, wait, `PowerCrossfade`): its per-switch bounds must hold for 98 % of them, with outer guards no switch may pass (§8 D-53). `runModRtTests`: three mod blocks cycling types, voicings, colours, sync, transport, bypass and re-entry allocate nothing. `--legacy-check` mod cases: see §5.4.3. The voicing-id pin is `runLegacyPinTests` |

### 6.6 Invariants that must not regress (whole integration)

- APVTS ids, ranges, defaults, order of the first 166 parameters (golden `--params` diff).
- The audio thread never allocates or locks; every instance is prepared eagerly in
  `prepareToPlay` (`:808-821`); blocks larger than promised still run (lanes run flat,
  gate passes ungated).
- Re-entry reset through `pendingReset`, at most two per callback; a pending block
  processes as bypassed (`:1067-1092`). **A restored state that lands on silence is the exception** (§2.6, §8 D-52). It is
  marked for its duration and once more when it ends (`RestoreScope`), and if the last callback's
  output was silence the next callback resets every entering or retyped block at once, with no drain,
  and snaps every fader to the restored on or off; a restore that lands on sound changes the blocks
  as the same change by hand does. Such a silent render is the same whether the host restores before or after `prepareToPlay`. An engine is `reset()` when its block starts again,
  every type including Classic (`engineStarts`), and its input and the engine output
  crossfade over 10 ms (`BypassFader`; the engine runs unheard for another 10 ms after a
  reset before it fades in) for all five block kinds (gate, compressor, drive, EQ,
  modulation); the fader's delay slides (`DelayGlide`) when a bypassed drive's type changes,
  and a Classic drive keeps its 3-sample rule with its input path sliding 0 to 3 samples
  while the engine is unheard (§2.6). `processBlock` zeroes every non-finite or absurd
  (1e6 or more) sample at the input and again before the DC blocker (§2.2, §8 D-31, D-42).
- `applyStateVar` publishes the chain order before any synchronous model/IR loading;
  `setStateInformation` publishes immediately and defers `adoptChainOrder` (`:2127-2170`).
  The legacy pin must not reorder these.
- Latency counts only blocks that run; FX slots and a non-Classic drive count while bypassed
  (the drive holds its input back by it, so toggling it never moves the latency); a Classic
  drive counts only while running and not waiting for its reset. Latency is computed after
  the reset drain, and reported only by the 20 ms timer on the message thread (§2.9). The lane ring aligns the
  shorter lane: `compensateLaneLatency` follows a per-sample delay difference and restarts a
  slide from where the tap is, and `DriveTimeline` mirrors the drive's own switch timing so
  alignment and reported latency follow what the drive applies (§6.3).
- Chain order is state, never a parameter; `chainLock` is a leaf lock.
- Reverb/delay untouched (F1); cab untouched (F2).
- `tubamp_smoke` passes at 44.1, 48, 96 and 192 kHz (the 22.05/32 kHz split-path failure is
  pre-existing on `main`; the 192 kHz row of §8 D-33 is closed).
- Tests of the processor round: the golden tool reads latency after running the message loop
  (the processor reports from a timer, §2.9), and its legacy side mirrors the bypass fader
  and the new crossfade, so `--legacy-check` proves static Classic renders against the old
  code, not transitions. Transitions against the real old build are covered by the
  verifiers' harness (`build-verify/compat`, `build-verify/processor-r2`). `tubamp_golden
  --both-orders host|preset` requires the same render whether the state is restored before or
  after `prepareToPlay` (§5.4).

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
/** Classic, Tight, Smooth. */           export const GATE_STARTING_POINTS  = { release: [30, 30, 80], hold: [6, 10, 10], sc_hpf: [75, 75, 75] };
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
| D-2 | Factory presets are captured by the running build, so a fresh install would write them with the recommended types while existing installs keep old files (pinned to Classic). | Write factory presets with every new type id = 0 so they sound the same everywhere; a later preset refresh can adopt the new types deliberately. Update (678c9c8): factory presets are built by `makeStateVar()` from the parameter defaults and each preset's own values, without touching the live state, so a fresh instance is identical with or without the presets folder (before, the first instance on a machine without it came up with 23 parameters changed). |
| D-3 | Gate deferred-curve alignment (study R4) would change Classic renders whenever the NAM model resamples. | Alignment for Tight/Smooth only; Classic stays unaligned (strict D2). |
| D-4 | D7 asks for knob smoothing in every EQ type incl. Classic and the amp EQ. | Done; static Classic renders stay bit-identical, automated ones change (the click fix). Not listed as a D2 exception because static bit-identity holds. |
| D-5 | `mod_type` switches today reset and jump instantly. | The dispatcher crossfades over 20 ms from the outgoing type to the incoming one, which is reset where the old block reset it (§6.5, not through the switcher); static renders, and everything after a fade, are unchanged. Update (678c9c8): a switch into Classic chorus holds for 7 + 10 × depth ms before the fade starts (`crossfadeHoldSamples`, §6.5). A second request inside a fade now waits for it to end (430e35f); an old session with two changes inside one fade is a declared difference (§8 D-65). |
| D-6 | Drive latency: JUCE's integer-latency mode would add a fractional delay and change Classic audio. | Report `lround` (Classic 3; Boost/Treble 0; Screamer and Fuzz 5 at 48 kHz, 4 at 96, then 1 and 2 at 192; Distortion 5, 5 and 4; `latencyFor` in `Drive.cpp`) and leave the audio alone; residual ≤ 0.44 sample (measured for the first design, not re-measured). The first version of this row said 3 at 96 kHz and 1 at 192 for every oversampled type; that was the first-order, single-clipper design. |
| D-7 | Drive latency counted only while `drive_on`, like the amp; automating `drive_on` or `drive_type` moves the reported latency mid-playback. | Follow the amp precedent (D2 "like the amp"). Alternative: the FX-slot rule (count while bypassed and delay the bypassed signal) — not chosen because it would delay a bypassed Classic drive. Update (processor round, 678c9c8): the FX-slot rule is now used for every non-Classic drive type: it counts its latency while bypassed and holds its input back by it, so toggling it never changes the plugin's latency. A Classic drive keeps the amp rule (only while running and not waiting for its reset), for the reason given here. Split-lane delay changes slide over 10 ms; latency is computed after the reset drain; reported equals measured for every drive type, slot and lane from 44.1 to 192 kHz. |
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
| D-22 | The NAM resampler's reported latency is 1.9 (44.1 kHz) / 3.0 (96 kHz) / 5.0 (192 kHz: 70 reported against 75.0 measured) samples short of its true delay (§4.1.7), so a deferred Tight/Smooth curve leads the audio by that much, and the host's delay compensation is off by the same amount. | Left as is: D uses the reported latency as specified, and the host figure is frozen by the golden renders. A measured group delay from `NamEngine` (probe at model load), used for D only, would close it for the gate. |
| D-23 | `eq_tilt`, `eq_level` and the seven `eq_g*` read their default 0 back from the parameter as 8.9e-8 / 1.8e-7 dB on arm64 (exactly 0.1f × 120 − 12 with the product unrounded, as a fused multiply-add in JUCE's `snapToLegalValue` gives; visible in `tests/golden/params-v4.tsv`), so a fresh Parametric or Graphic block in the plugin runs its level/tilt/band stages instead of the exact 0 dB paths E4/E17 test on the engine. Measured through the processor: within −144 dBFS of the input at 44.1 kHz (−150 at 48, −162 at 96). | Left as is: the processor passes values raw and the layout is frozen. Closing it means the engine treating \|dB\| below 1e-5 as 0 (engine owner), never a processor-side rounding. Classic and the amp EQ are unaffected (their knobs read back exactly). Update: fixed in the engine (fe2fee6): a control within 1e-3 dB of neutral counts as neutral (host noise measured at 1.8e-7 dB; rows E22a, E22b), not 1e-5 as written above. |
| D-24 | **Verification round, 2026-11-11 (seven `dsp:` commits after the integration).** The engines were fixed against the verification reports; this table keeps the earlier decisions above and adds what changed. | Gate (81fd404): the close rule depends on the fall from the recent peak; hold is the wait after a note stops (§4.1.5). Open: a pure 41.2 Hz tone now needs 15.4 dB over threshold to open (was 13.6), the price of the 4th-order 75 Hz sidechain filter. Update (2df395d, second round): that rule is replaced by version 3 (§4.1.5, closes on how fast the level fell); the 15.4 dB was the figure for a tone that starts at a crest, a tone that rises needs 20.8 dB (row X20, rows F), and the costs are in D-35. Update (version 5.1): the rule is replaced again, D-59. |
| D-25 | Compressor type switches (a27b550). | `fx::EngineSwitcher` dropped (§2.4, §4.2.6): switches among Clean, Squash, FET and Opto keep the gain state with no fade; to or from Classic both outputs crossfade over 40 ms (+0.01 dB bump). Total makeup is capped at 48 dB. |
| D-26 | Drive limits left open (0af54cf). | Aliasing for input tones above 5 kHz (Fuzz worst −43.1 dBc, per the commit; 8x oversampling would double the CPU) and the Screamer's 0.84 dB droop at 10 kHz are known and not fixed. The Fuzz mid-knob row Y2 is a sustain row (output moves at most 3 dB for 18 dB of input) because D14 requires the Muff saturated at half sustain. |
| D-27 | EQ block switched on under signal (fe2fee6). | Level, shelves and cuts still step. Open: needs a crossfade in the processor, not in the engine. Update (678c9c8): closed. `BypassFader` crossfades the block's input and the engine output over 10 ms in the processor for every non-Classic type (§2.6). |
| D-28 | Chorus mode switches and mono Dimension (08da770). | A crossfade by measured power replaces `fx::EngineSwitcher`; a mono Dimension instance plays line A alone, so CH-mono compares with the stereo left channel, not the mean (the mean of two antiphase lines has no detune). Update (0fde1ee): the two whole outputs are crossfaded per channel in two bands, from a moment where they will not cancel (§4.5.4; D-55). |
| D-29 | Tremolo (2b847b9). | A mono instance ignores Width in every voice; `reset()` starts gains at unity; a song-position jump crossfades over 10 ms; the stereo loudness law is Smooth only (the earlier text said it applied to Optical, Bias and Harmonic as well, which the code never did). |
| D-30 | **Known limit: the phaser's level between notches depends on the sample rate above 3 kHz.** After the notch-matched discretisation (§4.6.4) the notches and trajectories are rate independent, but the frozen magnitude response at 48 kHz differs from 192 kHz by 1.02 to 1.69 dB (script 1.69, block 1.40, Small Stone off 1.02, on 1.66; at 44.1 kHz 1.24 to 2.05 dB: script 2.05, block 1.70, off 1.24, on 2.00; `evidence/fit/d30.txt`), on the upper slopes of the response rather than inside its top 3 dB (the band where the difference is more than half its largest value runs from 1.8 or 6.0 kHz up to 10 kHz for the script, 1.8 or 4.4 kHz for the block, 1.0 to 10 kHz and 5.4 to 8.7 kHz for Color off, 2.7 to 4.6 kHz at the top for Color on; inside the top 3 dB it is 0.67 to 1.54 dB at 48 kHz and 0.83 to 1.93 dB at 44.1 kHz). The 48 kHz render is the one off the analog response (up to 1.80 dB; 192 kHz is within 0.11 dB): four first-order stages lag the analog cascade by 4.5 to 7.3° near 10 kHz (`docs/research/fx/phaser/evidence/fit/rate_limit.txt`). Row P9 compares every rate with 48 kHz, so it reads the 48 against 192 kHz figure; at 44.1 kHz the script (2.05 dB) and Color on (2.00 dB) exceed the 1.8 dB bar that P9 enforces at 48 kHz. | Left as is; row P9 "frozen response" enforces 1.8 dB as the structure's limit, not 0.1 dB. Tried and measured: four free stage coefficients per position reach only 0.57 to 1.45 dB and give up exact notches. A minimum-phase correction on the wet output closes it (0.06 to 0.28 dB with a 3 dB rise above 10 kHz, 0.03 to 0.12 dB with 6 dB) but lifts the wet above 10 kHz at every rate, which neither the pedals nor the Guitar Rig models do. Owner's call to revisit: that correction, or 2x internal oversampling at 44.1 / 48 kHz (D-8). |
| D-31 | The processor side of the contract (closed, 678c9c8). | Delivered: (1) `processBlock` zeroes every non-finite sample of the plugin input on every channel before anything reads it; (2) an engine is reset when its block starts again and its type is or was non-Classic, and its input and output crossfade over 10 ms for gate, compressor, drive, EQ and modulation (worst switch-on −119.9 dBFS, was −7.2); Classic switches as before (§2.2, §2.6, §6.6). Not covered: the sanitiser guards the plugin INPUT only. A non-finite value produced inside the chain (a hosted plugin, the neural model) is not caught. |
| D-32 | **Known open item: level moves at voicing switches inside one type.** The engines handle these switches (§6.5), and the level can leave both steady levels for a moment: a phaser model switch up to 9 dB, a switch into tremolo Smooth about 5 dB, a Classic chorus to CE or Dimension about 6 dB. | Left as is: no fix in the processor or the dispatcher. Closing it means a loudness-matched crossfade in each engine (engine owner). Update (second round): **resolved inside the engines for the phaser, the tremolo and the chorus**, by crossfades with gains from measured power (§2.4). Worst cases measured: phaser P17, +0.95 dB above the higher and −2.88 dB below the lower of the two voicings' own levels over 20 ordered pairs (+0.79 and −2.20 dB after the third round) (the old switch: +11.5 and −20.3 dB); tremolo Y3, 0.03 dB outside both voices' trajectories over 160 switches (was 10.06 dB); chorus CH18 +0.69 dB from the span of the levels before and after, CH27 1.70 dB from the envelope of the modes' own levels when switching every 30 ms through all five modes (the old Classic switch was about 6 dB out). **Still open and not part of this round:** the `mod_type` switch between effects (chorus, phaser, tremolo) belongs to the dispatcher (§6.5) and is being fixed there. Update (processor rounds, 579b6dd and 2a280b9): **resolved in the dispatcher** (hold, wait, then a 40 ms power-matched crossfade corrected in two bands, §6.5; three bands per channel and 120 ms below 150 Hz after the third round, D-53). Over 440 `mod_type` switches the smoke test reads at most 1.18 dB above the higher steady level, 2.28 dB below the lower, and a click of -86.0 dBFS at 48 kHz (was up to -17.1 dB). The price is a wait of up to 400 ms before a type switch is heard. |
| D-33 | At 192 kHz the processor smoke test reads −79.6 dBFS for a Classic EQ bass 5 to 10 step against its −80 dBFS bound; the processor equals the engine alone. 192 kHz was not in the proof set before (the proofs are 44.1, 48 and 96 kHz). | Open. Whether it predates this branch is being checked. Update (44e54af, second round): **resolved in the engine.** The Classic knob moves now glide in a double-precision filter (§4.4.3); row E26 covers 44.1, 48, 96 and 192 kHz at blocks 1 / 37 / 64 / 512 with a bar of −80 dBFS and reads −86.4 at worst (bass −86.7 at 192 kHz included). The processor smoke test at 192 kHz was not re-run for this document. Update (2a280b9): the smoke test passes at 192 kHz (0 FAIL at 44.1, 48, 96 and 192 kHz). |
| D-34 | Not fixed in 678c9c8 (from its commit message). | `setLatencySamples` is called from two threads (audio and message); `setStateInformation` can run off the message thread; the CabSim flag race; the NAM latency 2 to 3 samples short (D-22); a NaN produced inside the chain (D-31). Update (579b6dd): `setLatencySamples` is now called from the message thread only (§2.9), and a value of 1e6 or more or a non-finite one made inside the chain is zeroed before the DC blocker (§2.2). Still not fixed: the `setStateInformation` race (D-48), the CabSim flag (D-47), the NAM figure (D-22, D-45). |
| D-35 | **Gate, version 3 closing rule: stated costs** (2df395d). **Superseded by version 5.1 (D-59 to D-64) for the closing-rule costs; the last sentence, the sidechain default, still holds.** | Accepted, no parameter, default or API change. A natural decay (not a stop) closes up to 1 s after it passes the close level (version 2: 0.4 s). A stop on a chord whose partials beat fast waits up to 20 ms more. Fast, deep beat troughs dip partway, never to mute, and the crest reopens the gate (12 + 5 verifier cases, deepest about −36 dB; the trade-off table of §4.1.5 from row C: troughs of 12 dB at 4 and 8 Hz dip to −34 and −33 dB for 82 and 45 ms, troughs of 20 dB at 1 to 8 Hz to −30 to −38 dB for 46 to 119 ms). The 75 Hz 4th-order sidechain default reads a pure 41.2 Hz tone 20.8 dB under its level; for bass use the knob's minimum or about 40 Hz (rows F, §4.1.2). |
| D-36 | **Two wider EQ overshoot bars** (0cc04e2). E25a (gain, Q, shelves, cuts, tilt, level, Graphic) is 1.0 dB; E25b (Parametric mid-frequency moves) is 3.0 dB and E25c (Stack) is 2.0 dB. | Accepted. The reason the script gives: E25b is a narrow cut swept off a held note, which rings as the resonator empties (measured +2.46 dB), and E25c is the passive circuit emptying its capacitors (+1.68 dB, the same at 192 kHz); both are the filters' own response to a 10 ms knob move, not the state mismatch E25a guards (worst +0.58 dB, was +11.62). Tightening them would need a different filter, not a fix. |
| D-37 | **Squash keeps its feedback recovery above the threshold** (b4afe4b). | Kept: the release is the knob when the signal falls below the threshold and while the output stays above it, and a drop that leaves the input above the threshold but takes the output below it recovers sooner (down to a third of the knob at R 8, a quarter at R 20; the table of §4.2.5). That is what separates Squash from Clean. The second verification had read 41 to 93 ms at knob 150 as a defect (r2-dynamics C5); C26b holds the stated table within 10 % and Clean at the knob within 2 %. |
| D-38 | **Auto makeup is a fixed gain calibrated for a steady −10 dBFS peak** (b4afe4b). | Law unchanged. It does not follow the playing: a steady −20 dBFS signal comes out 4 to 10 dB louder, a −3 dBFS one 3.5 to 7 dB quieter, a plucked chord peaking at −10 dBFS 2 to 12 dB louder in rms. The reference level is the study's guess (`kAutoMakeupReferenceDb`); a level-following makeup is not built. Total makeup stays capped at 48 dB. |
| D-39 | **Phaser headroom**: nothing saturates and there is no limiter (8f8f197). | Small Stone Color on peaks +13.6 dB above a full-scale input (an 82.4 Hz square at 10 Hz); the other voicings +2.4 to +5.9 dB. Guitar Rig 6 peaks the same within 0.3 dB on a −6 dBFS low square, so it is the voicing. The chain after the phaser needs up to 14 dB of headroom (6 dB for every voicing but Color on); the processor's documentation should say so. Not limited in the engine. |
| D-40 | **Six Guitar Rig 6 reference renders are committed**: `docs/research/fx/phaser/evidence/fit/gr6_takes/` (`p9_block_p050.wav`, `p9_block_p090.wav`, `ss_off_r050.wav`, `ss_off_r075.wav`, `ss_on_r050.wav`, `ss_on_r075.wav`; 4.4 MB in all) (8f8f197). | Rows P13 and P14 of `check_phaser.py` read them, so the acceptance run needs them in the repository. **Whether to keep vendor-plugin renders in the repository is the owner's decision**; without them P13 and P14 cannot run. |
| D-41 | **Drive limits unchanged by the second round**: aliasing for input tones above 12 kHz and the Screamer's droop of 0.84 dB at 10 kHz. | Known and not fixed. Tones of 13 to 19.5 kHz at 44.1 kHz alias to −43.1 dBc (Fuzz), −47.2 (Screamer) and −69.0 (Distortion); an 8x factor would double the CPU. D-26 stated the aliasing limit as "above 5 kHz" from commit 0af54cf; the second verification measured it from 12 kHz (1 to 12 kHz: −68.3 dBc at worst). |
| D-42 | **Declared differences from the old build** (579b6dd, 2a280b9): (1) Classic toggles no longer burst or pop (old: up to +6.0 dBFS on a hot drive, -8.1 dBFS EQ, -14 to -28 dBFS chorus, phaser, tremolo; now at most -73 dBFS under the fade); (2) the `mod_type` fade is hold, wait, then 40 ms (120 ms when the power sits below 150 Hz) where it was 20 ms linear; (3) reported latency holds the larger figure for up to 40 ms during a drive type switch; (4) samples of magnitude 1e6 or more become 0; (5) a split session that automates `amp_on` in a lane holding a resampled model crossfades the other lane over 10 ms (-20.6 dBFS against the old render at 96 kHz) where the old build blanked it for 27 to 42 samples. | Accepted; static Classic renders stay bit-identical (golden and `--legacy-check`, §5.4.3). |
| D-43 | `DriveTimeline` keeps split lanes within 0.41 samples of each other through a drive type switch, because it follows the drive's applied delay rather than the delay its parameters ask for (§6.3). | Kept; see D-50 for what it costs to maintain. |
| D-44 | A chain reorder while playing still peaks about 4.4 dB over any steady order, at blocks of 256 and 2048: a removed or moved block switches hard. | Not changed. Fixing it needs two chains running side by side. |
| D-45 | The NAM latency is reported 2 to 5 samples short of the measured delay (27 against 28.9 at 44.1 kHz, 70 against 75.0 at 192 kHz; D-22). | Not changed. |
| D-46 | `NamEngine::collectGarbage` and `applyStaging`/`process` write the same four slots unsynchronised during model swaps; the worst case is a leaked model. | Not changed; `NamEngine` is untouched by this branch. |
| D-47 | `CabSim`'s `irLoaded` is an unsynchronised one-byte flag, also written on every restore without an IR. | Not changed. |
| D-48 | Off-thread `setStateInformation` races the parameter flush. | Not fixed, because blocking on or deferring to the message thread is not provably safe in every host. |
| D-49 | Automating `amp_slim` still calls `triggerAsyncUpdate` from the audio thread. | Not changed; older than this branch. |
| D-50 | **Known coupling:** `DriveFx` cannot report its applied delay during a type switch, so `DriveTimeline` (`PluginProcessor.h`) mirrors the timing in `Drive.cpp` (`kSwitchSettleSeconds`, `kSwitchFadeSeconds`, `kSwitchSlideSeconds`). | `DriveTimeline` must follow any change to `Drive.cpp`'s switch timing, or lane alignment and reported latency drift from the audio. |
| D-51 | The plugin has not been run in a real host and `auval` has not been run against it. | The second verifier recommends a pass in Logic to see how it handles latency reports that change during playback (§2.9). |
| D-52 | **State restore: a snap after a silent callback, the same change as by hand under sound** (2bbda8c, 430e35f). A state restored onto a prepared or running instance went through the user-toggle reset and fade, behind the two-per-callback drain, so each block passed dry for 10 ms and then faded in. | Done. A restore is marked, and the next callback resets every entering or retyped block at once and snaps the faders (§2.6, §6.6) only if the last callback's output was silence. Under sound a restore equals the same change by hand: retyped blocks crossfade in their engines, toggles and entering blocks fade, a fade in flight carries on (430e35f). Before it, the snap cut a live restore at −19.0 dBFS above 5 kHz at 48 kHz / 256 and −14.4 at 96 kHz / 64 when comp, EQ and mod retyped together (round-four verifier's run); now −72.3 and −71.7, steady −71.8 and −70.6 (coordinator's run of the verifier's scripts on a harness rebuilt from the merged sources). User toggles and chain edits still fade. A silent render is the same whether the host restores before or after `prepareToPlay`: `tubamp_golden --both-orders host\|preset` finds 0 of 339 rows differing in both modes, and the smoke test reads 694 ok and 0 FAIL at 44.1, 48, 96 and 192 kHz (coordinator's run at 430e35f; golden 270 identical, 69 expected, 0 unexpected; legacy-check 0 of 168; reverb and delay guard PASS 144). The same commit as the first snap moves latency reports to the 20 ms timer alone (§2.9). **Not the same in both orders for seven gain parameters and the cab IR:** `input_trim`, `output_level`, `amp_input`, `amp_output`, `mix_alevel`, `mix_blevel` and `mix_level` ramp from their default for 21 ms when restored after prepare, and a cab IR arrives late. 8 to 12 of the 239 old states differ between the two orders (8 with the first 50 ms silent, 12 with audio from sample 0). The old build behaves identically on the same states, so it is not a regression (round-four verifier, finding 4, measured). **Declared difference from the old build:** restore-before-prepare matches the old build exactly. In prepare-first and live restores some renders still differ from the old build (18 of 239, 64 of 478 and 22 of 84 in the compatibility harness; round-four verifier's denominators, finding 5), all on rows where the old build's own two orders differ because of its drain. Which order Logic uses is untested. A restore onto sound now fades, which is its own limit (§8 D-66). |
| D-53 | **A `mod_type` switch on a held pure note with the two effects in antiphase cannot be crossfaded without a dip** (2bbda8c). | Known limit, bounded as a distribution. The smoke test's level check covers 55 to 440 Hz at four mixes (2640 switches). 34 to 38 of the 2640 are outside the per-switch bounds (1.5 dB above the higher steady level, 3 dB below the lower, click −60 dBFS, −50 dBFS under 60 Hz), with a worst dip of 10.24 dB, a worst bump of 3.20 dB, and −45.5 dBFS under 60 Hz at 96 kHz (parent's run). The check requires 98 % inside and has outer guards no switch may pass (3.5 dB, 12 dB, −60 dBFS, −45 dBFS). The guards are regression guards set from the measured worst, not design targets. Spread outside the check's set (round-four verifier, measured): 3.2 to 3.9 % of held-note switches are outside the bounds over eight switch instants and both channels, against 1.3 to 1.4 % at the check's single instant and left channel; the worst dip is 11.89 dB and the worst bump 3.99 dB, which is past the check's 3.5 dB guard; nothing is outside below 55 Hz or on chords. A second request during a fade waits for it, so nothing cuts (coordinator's run: worst −67 dBFS on the chord against steady −66.1; storms every 1 to 150 ms for 1 s no higher than −66.5 dBFS; before the fix 27 of 28 storms clicked at −22.1 to −28.2 dBFS, round-four verifier's run). The last request lands within 1.14 s at most by the fixer's bound, measured at most 317 ms (fixer's run). |
| D-54 | **Chorus Tri at mix 0.5 dips on some song-position jumps** (0fde1ee). | Known limit, **understated by the author's figure; corrected to the round-four verifier's (measured).** Author's run: on a pure tone 6 of 56 jumps drop up to 12.1 dB under the steady minimum. Three voices and the dry share a channel, and no path combination avoids it: a phasor model says the best combination still dips 4 to 6 dB at 1 kHz, and waiting for a good moment and a dry-aware choice reached 4 of 56 at best and were not kept (engineer's run). **Round-four verifier, measured** (`.audit/fx-verify/r4-mod.md` finding 2: 48 kHz, depth 0.8, 120 BPM, 26 notes, loops of 2.63 / 1.37 / 0.81 / 3.5 beats, each channel against the mode's own minimum over 60 s; a hole is more than 3 dB under it for at least 5 ms; the same detector finds 0 on the steady render): at mix 0.35, 161 of 2288 channel-jumps have a hole (7.0 %, 12 of 26 notes), 53 deeper than 10 dB, deepest 19.7 dB for 20 ms (1500 Hz), longest 48 ms (4.7 dB, 952 Hz); at mix 0.5, 122 of 2288 (5.3 %, 13 notes), 43 deeper than 10 dB, deepest 33.5 dB for 5 ms (758.7 Hz), longest 17 ms (7.2 dB, 306 Hz); at mix 1, 27 of 2288 (1.2 %, 3 notes), 12 deeper than 10 dB, and a 53 ms hole with the right channel at −44 to −47 dBFS where its steady minimum over 90 s is −26.0 dBFS (481.7 Hz). So the count is as stated and the depth and length are worse (12.1 dB against 33.5 dB, 5 ms against 53 ms). **The round-three fix did not improve Tri:** with the 0.5th-percentile reference on both builds the holes went from 109 to 171 at mix 0.35, 128 to 136 at mix 0.5 and 28 to 29 at mix 1 (the 53 ms hole is in both); 44.1 kHz decorrelated stereo and 96 kHz mono show the same picture (mix 0.5: 221 and 45 holes, deepest 39 and 50 dB). **The holes are a single-note effect; chords show none:** an E chord, an A power chord, E1 and a high triad, 20 ms frames, 0 of 2976 channel-jumps more than 3 dB under the steady minimum, worst 0.6 dB, every mode. Wide, CE and Dimension: the author's run reads worst −2.82 dB, 0 of 56 under −3 dB; the verifier confirms it (Wide and CE 0 of 13 728 against the 60 s minimum, Dimension 0 of 6864 against the 0.5th-percentile reference). CH30 keeps its bound for those three. CH30t holds Tri to at most 8 of 56 jumps under −3 dB and none under −14 dB, regression guards set from the author's measured worst (6 of 56, −12.11 dB), **not design targets and not the verifier's figures**. |
| D-55 | **Chorus mode switches on single notes: 5 of 60 outside the bound** (0fde1ee). | Known limit, **understated by the author's figure; corrected to the round-four verifier's (measured).** Author's run: worst −20.3 dB, Classic to CE at mix 1: Classic's phase turns about 25 Hz at 1 kHz, faster than the 20 ms fade. The rest are CE to Dimension at 440 Hz (antiphase through the whole 400 ms wait, −4.4 dB; engineer's run) and Tri to Classic on the guitar-like take (−3.04 dB). CH33 is a distribution bound, at most 6 of 60 outside and none under −22 dB, over +2.0 dB or above −60 dBFS; measured worst −20.29 dB, +1.76 dB, −69.7 dBFS (engineer's run). **Round-four verifier, measured** (`.audit/fx-verify/r4-mod.md`, verdict table, finding 3 and "The declared limits"; 20 ordered pairs, 9 notes, 4 times, each channel against the 60 s steady minima, 2880 channel-switches per mix, two settings: 0.8 Hz depth 0.5 at 48 kHz, and 3 Hz depth 0.8 at 44.1 kHz with decorrelated stereo): **mix 0.35** 2 of 2880 dip, at most 5.3 dB for 9 ms (the row holds); **mix 0.5** 13 of 2880, deepest 15.2 dB for 15 ms (Dimension to Tri, 1318.5 Hz), deeper than the row's −4.4 dB; **mix 1** 91 of 2880 (3.2 %), 41 deeper than 10 dB, deepest 40.1 dB for 6 ms (Classic to CE, 440.3 Hz, against the row's 20.3 dB) and 34.6 dB for 8 ms **without Classic** (CE to Dimension, 82.4 Hz), so the dips are not specific to Classic. At 0.8 Hz, depth 0.5, 48 kHz: with Classic 26 of 576 channel-switches (4.5 %), 9 deeper than 10 dB, deepest 27.0 dB for 7 ms (Dimension to Classic, 1318.5 Hz); modes 1 to 4 only 17 of 864 (2.0 %), 10 deeper than 10 dB, deepest 34.6 dB. At 3 Hz, depth 0.8, 44.1 kHz: with Classic 32 of 576 (5.6 %), 17 deeper than 10 dB; modes 1 to 4 at mix 1 16 of 864, deepest 23.7 dB for 5 ms (Wide to CE, 82.4 Hz). **The rise passes the +2.0 dB bound:** +3.49 dB over both modes' maxima (Classic to CE, 440.3 Hz, at 3 Hz) and +2.35 dB without Classic (CE to Dimension, 1318.5 Hz); +1.85 dB with Classic at 0.8 Hz. The click bound holds (loudest −75.5 dBFS at 0.8 Hz; at 3 Hz −57.1 dBFS where the modes' own read −57.7). Chords and a bass note, 20 ms frames: 11 of 1920 dip more than 3 dB, worst 5.7 dB, all at mix 1. The guards of CH33 are the author's regression guards, not design targets, and the verifier's grid is outside them at mix 1. |
| D-56 | **Chorus tap crossfade: glide where it keeps up, crossfade for lag only** (0fde1ee, `Chorus.h`). | A crossfade slides each voice's phase by up to half a cycle in 30 ms, a pitch blip of up to 25 Hz (about +460 cents at 82 Hz, +43 at 1 kHz) that no fade shape removes, because gliding the same delay change as fast would bend every note as much. So a change glides, at most 4 cents beyond the settings' own detune, wherever that lags a stopped knob by under 80 ms or a moving one by under 100 ms, and is crossfaded otherwise, at most once per 250 ms while a knob keeps moving. Cost (engineer's run): rate ramps of 0.05 to 10 Hz in 0.5 / 2 / 10 s read +140 / +118 / +181 cents at 82 Hz (were +139 / +174 / +97: fewer catch-ups, each larger), and a ramp's lag behind the knob is 135 ms (was 80; bar 150). Single steps are unchanged (Wide depth 0.4 → 0.8 +315 / +43 / +6 cents over steady at 82 / 220 / 997 Hz). Longer fades (45 and 60 ms) halve the blip but deepen the comb under them (CH25 wet −2.08 / −1.83 dB against its −1.5 dB bar), so they were dropped. |
| D-57 | **Phaser voicing switch: 4 of 1200 switches still dip more than 6 dB** (006f743). | Known limit. Worst 17.3 dB for 3.5 ms, all into the script Phase 90 at 2479 Hz (was 19 of 1200). Off the note the energy is at worst −52.1 dBFS, 9 of 720 above −60 dBFS (was 101; parent's run). The engineer attributes the dips to that voicing's own notch on a fresh start, where an ideal fade dips 15.6 dB; not independently verified. Small Stone Color on's own sweep reads about −50 dBFS at 200 to 256 Hz and an ideal 40 ms fade −55.7 dBFS at 110 Hz, so −60 dBFS everywhere is not reachable (engineer's run). **Row P19 passes at 3.00 dB against its 3 dB bar, with no margin.** Update (fast LFO rates): a verifier found the switch worse than before 006f743 from 2 Hz up on held notes. Measured on the outputs of the two voicings alone: from a moment within 60 degrees the two stay within 90 degrees for a median 132 ms at 1.3 Hz but 53 ms at 4 Hz and 24 ms at 8 Hz (6 ms at the 10th percentile), while an ideal fade with exact level correction capped at +1.4 dB stays under 3 dB at every rate when it starts at a good moment, even 40 ms long. So the limit was the choice of moment and the fade's length against the turn, not the cap. Above 1.5 Hz the fade now lasts as long as a 60-degree turn at that rate and, on a held note, starts where the predicted phase difference stays within 60 degrees through its middle half; at 1.5 Hz and below the switch is unchanged. Same 384 switches (8 notes, 12 pairs without Classic, 4 times), dips over 6 dB under both voicings' steady span, before 006f743 / 006f743 / now: 1.3 Hz 14 / 1 / 1, 2 Hz 14 / 9 / 0, 4 Hz 14 / 38 / 5 (longest 4.8 / 11.5 / 3.1 ms), 8 Hz 13 / 60 / 10 (longest 3.4 / 13.2 / 2.1 ms). Over the 3 dB bar on the verifier's other notes (560 switches per rate), 006f743 / now: 0.7 Hz 0.4 / 0.4 %, 2 Hz 5.4 / 1.2 %, 4 Hz 24.8 / 1.2 %, 8 Hz 31.2 / 0.2 %, worst 13.4 / 5.5, 18.4 / 5.0, 18.2 / 3.7 dB. What is left: the remaining dips over 6 dB at 4 and 8 Hz are mostly into the script Phase 90 at 3 kHz with that voicing 20 to 58 dB under the input; off the note a fade at 8 Hz reaches up to +4.7 dB over an ideal fade of the same length (−55.3 dBFS) where the old switch read at most +0.5 dB; above 2 kHz the switch stays under −75.6 dBFS (−67.2 before). Row P19 now runs 0.3 to 8 Hz; its bars at 2 to 8 Hz (at most 2 % over 3 dB, worst at most 8 dB) were set after measuring this engine. Coordinator's check at rates the fixer did not tune on (the verifier's `ph_switch.py`, 4800 switches at 1.4 / 1.6 / 3 / 6 / 11 Hz, new binary against the previous one): dips over 3 dB 16.2 % before and 3.0 % after; by rate 28 / 37 / 147 / 250 / 317 before and 28 / 16 / 17 / 24 / 60 after; the longest stretch more than 6 dB under 21.2 ms before and 5.7 ms after. **Declared trade:** cases where the off-note peak is above −40 dBFS and more than 3 dB over both references went from 0 / 0 / 1 / 0 to 2 / 64 / 98 / 54 at 1.6 / 3 / 6 / 11 Hz (of 960 per rate), worst −36.6 dBFS (P90 block to Stone off, 82.4 Hz, 3 Hz), where a plain crossfade of the same window reads −50.6. The short fades put more energy near the note; accepted for the dips they remove. |
| D-58 | **EQ: narrow low bells answer a gain move more slowly; E29 leaves out the 800 Hz Graphic band** (d2234ab). | A bell's gain moves no faster than a third of its own decay time, up to 20 ms per pole at 100 Hz, Q 8, +15 dB, where other bells keep 5 ms. Through 5 ms poles a cut-to-boost sweep still rang +2.0 dB at 0.92 × the centre. Off-centre frequency moves overshoot +1.30 dB, equal to the analog reference within 0.001 dB (engineer's run). The far-band row E29 leaves out the 800 Hz Graphic band, three octaves above the 98 Hz note: it rings at −75 dBFS when its gain and damping move, as it did in the second round (−69 dBFS). The round-four verifier measured it at −75.8 dBFS against the row's −80 dBFS bar, 79 dB under a note that leaves the EQ near +3 dBFS, and showed that the band's own response does not explain it: a plain bell with the same three poles reads −107.6 dBFS. The cause is open, not traced. |
| D-59 | **Gate: closing rule version 5.1 replaces version 3; Tight's hold default is 10 ms** (bd1f69a, 8a1081a, 5b4377f). | Declared difference from the version 3 text of D-24 and D-35. The wait after a stop is the hold plus remembered troughs; a slow fall is ridden; an in-band stop closes a stop onto a floor just under the threshold; a single deadline puts the gain at the range `hold` + 250 ms after the level last was over the open level; reopening after a deadline close needs 6 dB over the lowest floor estimate since (§4.1.5). `gate_hold` defaults to 10 (`kGateHoldDefault {6, 10, 10}`; the UI starting points the same, §7.3); it was 6 for Tight. Versions 1 to 4 were rejected by independent verification (chatter, palm mutes, a 30 s latch, chord chatter); version 5 was specified from an offline model validated against the engine and implemented to match a Python reference within 0.1 dB (measured worst 0.0138 dB on 153 signals, implementer's run). Full `check_gate.py` run on the merged build: 130 checks, 0 FAIL (coordinator's run). The limits that remain are D-60 to D-64 and the sidechain default of D-35. |
| D-60 | **Gate known limit: a slowly decaying note is faded out `hold` + 250 ms after it falls under the open level, even if it is still inside the 6 dB band.** | Known limit, by design: the deadline is the bound that ends the 30 s latch of version 3. The cap on the gain is 0 dB until `hold` + 170 ms and falls over the last 80 ms, so a chord's troughs up to about `hold` + 190 ms keep the gain over −6 dB. Measured (coordinator's run): R6 gain over −6 dB at most 200.6 ms after the level was last over the open level (limit 202); TB1 the longest stretch under the open level with the gain over −60 dB, beyond the hold, 235.3 ms (limit 236); Tight at the range for the rest of 30 s over a pulsing floor, at unity for 0.0 to 0.1 s (verifier's `r3_gate_latch`, merged build). A lower threshold moves the deadline later in the note's decay. Smooth takes its expander law instead of the range. |
| D-61 | **Gate known limit: palm-mute gaps of 40 to 52 ms at hold 10 end at −18 to −23 dB, not fully closed.** | Known limit, arithmetic of the rule: a gap closes after the 12 ms window, the fall to 6 dB under the note, the hold (10 ms) and a release to −20 dB (10 ms at the default 30 ms). Measured: X13 40 ms gaps median −23.4 dB, worst −23.1; 30 ms gaps −4.2; 50 ms gaps −43.4 (coordinator's run). One seeded near-hiss chug (82 Hz, 52 ms gaps, threshold 5.4 dB over the hiss) ends one gap at −18.4 dB instead of −20 (model, SPEC_v51 §6.3 and §7). Hold 6 closes them further (X13 40 ms gap −31 dB, model; R4 staccato at 180 bpm: median −29.1 dB at hold 6, −21.1 at hold 10, 11 of 16 gaps under −20 dB at hold 10) and costs chords: the `info` rows show B2 at hold 6 with reopening clicks (g3: 4 cases, worst +8.2 dB; g3b: 9 cases, worst +7.1 dB) and B1 at hold 6 with 9 of 382 cases dipping under −1 dB (5 under −6 dB). Hold is the knob: 6 closes 38 ms staccato gaps (16 of 16), 10 rides chords (0 of 256 chord cases dip, against 2 of 256 at hold 6; verifier's g1 script). |
| D-62 | **Gate known limit: a loud note stopped 0.1 s after a quiet chord that sits about 6 dB over the threshold keeps one or two more dips than the chord alone (4 cases of 72).** | Known limit (row B2 g3b, bar: at most 4 cells, all at gap 0.1 s). The cells are C2+Eb2 at threshold −36 with the loud note +12, +20 and +28 dB over the chord, and E2+G#2 at −36, +28 dB. The loud note stops while the quiet chord, only 6 dB over the threshold, is in a beat trough under the open level: that is the in-band stop of I4 (a fall from a note onto a level inside the band), and the chord's crest returns 4 to 7 ms later. Measured: deepest dip −14.5 dB, longest 12 ms, at most 2 in one case, 0 at the range inside 60 ms, 0 clicks. Removing them breaks hand stops: an extra in-band wait of 4 ms leaves 2 cells and an I4 margin of +0.2 ms, 8 ms leaves 0 cells and 24 of 120 stops over the I4 bound (worst margin −3.8 ms) (model, SPEC_v51 §6.1); a 0.2 ms margin would not survive float differences between the engine and the model. Not changed. |
| D-63 | **Gate known limit: after a deadline close, a note less than 6 dB over a floor that sits inside the band does not reopen the gate.** | Known limit, the price of the reopening test (§4.1.5 point 9): a level over the open level counts only if it is also 6 dB over the lowest floor estimate since the close, so when the floor's estimate is inside the band (under the open level by less than 6 dB) a note between the open level and the estimate + 6 dB stays shut out. The threshold is too close to the floor. A threshold at least 6 dB over the floor's own reading is never held to more than the open level. Only a deadline close sets the test; a stop close does not. Noise peaking at the threshold, which the test was added for, held the gate to 2 changes at most over 30 s (X4 long, 42 cases) where version 5 had 80. |
| D-64 | **Gate known limit: a decay with the threshold 3 dB over the hiss keeps the floor audible for 346 ms in one of 76 cases.** | Known limit (coordinator's figure from the model run, SPEC_v51 §7; unchanged by version 5.1). It counts the time the note plus the hiss still cross the open level, measured from the script's point (the note alone under the hiss's peak reading); from the last crossing it is `hold` + 235.3 ms at every rate, which is the bound of D-60 (TB1: 235.3 ms). The verifier's decay grid on the merged build (`r3_gate_decay`, hold 6) reads at most 162.5 ms of floor-alone tail. |
| D-65 | **Declared difference: an old session with two `mod_type` changes inside one fade now finishes the first fade before the second starts** (430e35f). | The second type is reset up to 64 ms later than the old build reset it in the golden case `mod3_typesteps` (61.7 ms at 48 kHz / 37; bounded by 570 ms), and its LFO runs that much behind from then on. Everything else is unchanged: legacy-check 0 of 168, and the check prints the waited request for those rows. See §8 D-5. |
| D-66 | **Known limits of a restore under sound** (430e35f). | (1) A live restore that changes the chain structure (split, lanes, amp) still clicks, at −24.0 dBFS over steady in the fixer's run, exactly as the same chain edit by hand does. (2) A restore onto sound now fades, so a state recalled while the input is sounding differs from the old build inside those fades; restores onto silence, and every restore-before-prepare, still equal the old build (D-52). |
| D-67 | **Known limit: audio that stops inside the latency hold leaves the held latency figure reported until audio resumes** (round-four verifier, finding 7). | Fuzz to Boost with the audio stopping 10 ms later: the report stays at 5 (4 at 96 kHz) at +30, +80, +280 and +680 ms while the chain's latency is 0; it corrects itself once audio runs again (verifier's `p_latency.py`, measured). A type changed after the audio stopped is reported right at +80 ms (48 kHz / 256, 96 kHz / 32) and +280 ms (44.1 kHz / 4096). Not fixed: reporting a computed figure while stopped would flap on resume (fixer's inference, not measured). |
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
