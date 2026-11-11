# Code review: seven effect engines and their integration (221a3cb..HEAD)

Reviewer role: independent, adversarial. Nothing under `src/`, `ui/`, `tests/`, `tools/` or
`docs/` was changed. Everything written is under `build-verify/code/` and this file.

## 1. What ran

Builds (all from `build-verify/code/wrap/CMakeLists.txt`, which adds the repo as a
subdirectory plus two harness targets; the repo's `CMakeLists.txt` is untouched):

| Tree | Flags | Targets |
|---|---|---|
| `build-verify/code/asan` | Release, `-fsanitize=address,undefined,float-divide-by-zero` | `tubamp_smoke`, `tubamp_fxbench`, `tubamp_stress`, `fuzz_engines` |
| `build-verify/code/tsan` | Release, `-fsanitize=thread` | `tubamp_smoke`, `tubamp_stress` |
| `build-verify/code/rel` | Release, `-g` | `tubamp_stress`, `fuzz_engines`, `tubamp_fxbench` (audio-thread allocation counter) |
| `build-verify/code/dbg` | Debug | `tubamp_stress`, `fuzz_engines` (assertions) |

Harnesses: `build-verify/code/harness/stress.cpp` (processor: one thread in `processBlock`
with a play head, the message thread doing random parameter sets, type switches, chain
orders with and without a split, `setStateInformation` of current and legacy-stripped
state, A/B capture and recall, factory preset loads, model loads, release and re-prepare
at six rates and five block sizes; a third "host" thread doing `getStateInformation` /
`setStateInformation`, meter exchanges and parameter automation), `fuzz_engines.cpp` (each
engine alone), `fxbench_fuzz.py` (random timelines through `tubamp_fxbench`),
`symbolize_tsan.py` (offline symbolizer: `atos`, `sample` and `lldb` cannot attach here).
Logs are in `build-verify/code/logs/`.

| Run | Length | Result |
|---|---|---|
| `tubamp_smoke` ASan+UBSan, 44.1 / 48 / 96 kHz | full suite x3 | 29 PASS each, 0 sanitizer reports |
| `tubamp_smoke` TSan, 44.1 / 48 kHz | full suite x2 | 29 PASS each, 0 reports |
| stress, TSan, all operations incl. model loads, 48 kHz | 75 s, 36 ops, 449 k blocks | 0 reports |
| stress, TSan, no model loads, 44.1 kHz | 75 s, 21 580 ops, 541 k blocks | 62 reports, 14 distinct, 3 families, all in code this change did not touch (section 3) |
| stress, ASan+UBSan, all operations, stereo 48 kHz | 75 s, 3.8 M blocks | 0 reports, 0 non-finite samples |
| stress, ASan+UBSan, no model loads, stereo 96 kHz | 75 s, 28 167 ops, 4.3 M blocks | 0 reports |
| stress, ASan+UBSan, no model loads, mono 48 kHz | 60 s, 26 107 ops, 6.2 M blocks | 0 reports |
| stress, release with allocation counter (self-tested: it sees a deliberate `malloc`) | 60 s + 45 s (mono) + 30 s + 25 s, 32 M blocks | 0 allocations inside `processBlock`, 0 non-finite, 0 legacy-pin violations |
| stress, Debug | 40 s, 1 592 ops | 0 assertion failures |
| engine fuzz, ASan+UBSan (`all 1200 2 oversize nan-params`) | 1.43 M blocks, 7 engines | 0 reports, 0 failures |
| engine fuzz, release (`all 4000 3 oversize nan-params`) | 4.75 M blocks | 0 non-finite, 0 meter faults, reset deterministic, 0 allocations |
| engine fuzz, Debug (`all 250 21 oversize nan-params`) | 299 k blocks | only JUCE's own Classic chorus/phaser asserts, for inputs the plugin cannot produce (rate >= 100 Hz, fewer channels than prepared) |
| `tubamp_fxbench` random timelines, ASan+UBSan | 434 cases, 7 effects, 8 rates, blocks 1..4096 | 0 sanitizer reports; 1 case over the contract's 1e3 bound (defect 5) |

The engine fuzz covers: prepare twice with a different shape, re-prepare mid-run, blocks
larger than `maximumBlockSize`, mono blocks on a stereo-prepared engine, out-of-range,
infinite and NaN parameters, out-of-range type ints, gate `measure`/`apply` with different
lengths and without `measure`, curve delays from -1000 to 199 000, eight input kinds
(full-scale noise, 20 Hz square, 1e-30 tail, zeros, sine, impulse, DC, low noise).

## 2. Defects, most serious first

### 1. CONFIRMED. Compressor makeup knob does nothing in Clean, Squash, FET and Opto
`src/dsp/Compressor.cpp:580` reads `makeup.getNextValue()` every sample, but the
smoother's target is only written in `setParameters` once after `prepare` (`:301-305`), in
`skip` (`:379`) and in `processClassic` (`:421`). While a non-Classic compressor is on,
`comp_makeup` never reaches the audio; it only takes effect after the block has been
bypassed once. Clean is the default type of every new comp block.

Reproduction: `build-verify/code/rel/fuzz_engines_artefacts/Release/fuzz_engines makeup`
(engine alone) and `.../tubamp_stress --makeup` (through the processor, chain `[comp]`):

```
comp_type 0: comp_makeup 0 -> 12 dB changes the output by +12.00 dB (expected +12)
comp_type 1..4: comp_makeup 0 -> 12 dB changes the output by +0.00 dB (expected +12)
```

### 2. CONFIRMED. `drive_on` / `drive_type` now blank the other lane of a split chain
`src/PluginProcessor.cpp:1956-1962` makes lane latency depend on two automatable
parameters. When the pair of lane delays changes, `compensateLaneLatency` clears both
rings (`:1843-1849`), so the lane that has to wait outputs `delay` samples of silence (3 to
5) and then resumes: a full-amplitude hole in the lane that holds no drive. Before this
change only a model swap, a hosted plugin or `amp_on` with a resampling model could do it.
Each such change also memsets 1 MB of ring on the audio thread.

Reproduction: `tubamp_stress --laneclick` (chain `split,drive,lane2,eq,mix`, lane A at
-60 dB, 100 Hz sine at 0.25, so the output is lane B alone):

```
steady, drive on                    largest sample-to-sample step 0.0023
drive_on 0 -> 1 (Screamer, lane A)  largest sample-to-sample step 0.1531
drive_type Boost -> Distortion      largest sample-to-sample step 0.0367
```

### 3. CONFIRMED. First instance on a fresh install is left on Classic for every engine type
`src/library/PresetManager.cpp:205-207` sets all 21 type ids to 0 before each factory
capture. The restore after the second preset (`:221`) makes `apvts.state` share
`originalState`'s object, so the restores at `:227` and `:236` are no-ops
(`juce_ValueTree.cpp:614`: assignment of the same object does nothing). The instance that
creates the presets ends its constructor with every type at Classic, against section 2.7.3
(a fresh instance starts on the recommended type). The same no-op also leaves
`drive_on`, `drive_gain`, `delay_mix`, `reverb_mix` at the last preset's values, which
predates this change.

Reproduction: `CFFIXED_USER_HOME=<empty dir> tubamp_stress --fresh`:

```
preset files before construction: 0, after: 4
gate_mode 0 (default 1)  comp_type 0 (1)  drive_type 0 (1)  eq_type 0 (1)
mod_chorus_mode 0 (1)  mod_phaser_model 0 (1)  mod_trem_voice 0 (1)
drive_on 1 (default 0)  drive_gain 18 (12)  delay_mix 0.35 (0.25)  reverb_mix 0.40 (0.25)
second instance (presets now exist): gate_mode 1 comp_type 1 drive_on 0
```

### 4. CONFIRMED. One NaN or Inf input sample is never flushed out of three new paths
Section 2.2 has recursive states sanitised once per block so that a bad sample cannot
live in a loop. `fuzz_engines latch` and
`fuzz_engines gatenan` (one bad sample in clean input, then 2 s of clean input):

- EQ Parametric, Stack and Graphic: output non-finite for good. `flushed()`
  (`src/dsp/Eq.cpp:78`) tests `abs(x) < 1e-15`, which is false for NaN, and nothing else
  sanitises the double states. NaN and Inf both latch.
- Compressor Clean, Squash, FET, Opto with the detector high-pass on (`comp_sc_hpf` >
  20.5): `jlimit` at `src/dsp/Compressor.cpp:582-583` passes NaN into `scFilter`
  (`:588-591`), whose state is never sanitised; output is NaN from then on. With the
  filter off they recover within a block.
- Gate: the Classic trigger now runs in every mode (`src/dsp/Gate.cpp:581`) on an
  unsanitised mono sum (`:138`). A NaN seen while in Tight leaves the Classic trigger
  poisoned: after a later switch to Classic the gate never closes (0 dB on a -100 dBFS
  tone with a -40 dB threshold, state 2). `GateFx::reset()` does not clear it; only
  `prepare` does.

Classic comp, drive, EQ, chorus and phaser latch too; that is the old code's behaviour.

### 5. CONFIRMED. Compressor output exceeds the contract's 1e3 bound
Manual makeup (24 dB) plus auto makeup (up to about 47 dB) is applied before the gain
computer has attacked. Found by the fxbench fuzz: peak 1324 on a 0 dBFS 41 Hz sine,
`threshold=-60 ratio=17.4 attack=36 makeup=24 auto_makeup=1`, when the block is re-enabled
(`on@0.83=0 on@1.58=1`); full command in `build-verify/code/logs/fxbench-fuzz.log`.
Section 2.11 "Finite" requires abs(out) < 1e3.

### 6. CONFIRMED, cost only. Treble drive never returns to its cheap path after a gain move
`src/dsp/Drive.cpp:922` re-designs the shelf while `abs(g - target) > 1e-6 * g`. The float
one-pole (`FxPrimitives.h:402-406`) stalls about 6e-5 away from its target, so the test
stays true for ever. `tubamp_fxbench drive ... type=5 gain=20 --bench`: 5.15 ns/sample;
with `gain=12 gain@0.05=20`: 6.36 ns/sample (Boost: unchanged).

### 7. PLAUSIBLE. Deferred gate curve is misaligned behind a high-latency FX slot
The curve delay is clamped to 8192 samples (`src/dsp/Gate.cpp:554`,
`Gate.h kMaxCurveDelaySamples`), while the lane rings accept 65 535
(`PluginProcessor.h:249`). `gate, fx1 (lookahead plugin > 8192), amp` applies the Tight or
Smooth curve early by the excess. Not reproduced: the headless build cannot host an AU.

### 8. PLAUSIBLE. Classic EQ: a retarget in the last sub-block of a ramp jumps
Reported by a sub-reviewer from reading `src/dsp/Eq.cpp:206-215, 334, 384-385`: a segment
that straddles a block boundary snaps to the new target while its coefficients are those
of the old one, and the next block re-designs in one step. Not reproduced.

### 9. PLAUSIBLE, small. Drive latency is counted while the drive waits for its re-entry reset
`blockLatency` (`src/PluginProcessor.cpp:1959`) tests `drive_on` only; the DSP path
(`:1506`) also requires `! pendingReset`. For the callbacks a re-entering drive waits to be
drained (two resets per callback, in id order) the lane ring and the gate's curve delay
are 3 to 5 samples off. Not reproduced.

### 10. PLAUSIBLE, small. Gate meters go stale on an oversized block
`GateFx::measure` returns at `src/dsp/Gate.cpp:585-586` before the meters are written, and
the processor publishes them anyway (`src/PluginProcessor.cpp:1473-1475`): the lamp and GR
show the previous block while the audio passes ungated.

### 11. Contract and comments
- `docs/FX-ENGINES.md:2340` (D-6) says the oversampled drive types report 3 at 96 kHz;
  `DriveFx::latencyFor` (`src/dsp/Drive.cpp:509-513`) returns 4 for Screamer at 96 kHz
  (`tubamp_fxbench drive gen:silence /dev/null type=1 --sr=96000 --latency` prints 4).
  `getLatencySamples()` equals `latencyFor` for every type at eight rates.
- `CMakeLists.txt:44-46` still says "the gate wrapper in FxBlocks"; it is in `Gate.cpp`.

## 3. Checked and found sound

- Audio thread: no allocation in 32 M processor blocks and 4.75 M engine blocks; no
  container growth, `std::function`, `juce::String` or `setSize` outside `prepare` and
  constructors; no engine has mutable static state.
- Threads: the only new shared state is `preparedSampleRate`, the four meter atomics and
  the parameter atomics; TSan reports nothing on them. `publishMax` against the editor's
  `exchange` cannot tear or lose a larger value.
- Legacy pin: in both restore paths the pinned ids are written before the order is
  published, so a pinned block never runs a non-Classic type; 0 violations under stress.
  The table covers all seven type ids. Absent non-pinned ids take their defaults in both
  paths (`tubamp_stress --absent`). In host state `comp_type=17` loads as 4 and
  `drive_type=-4` as 0; an unknown chain token is dropped (`gate,futureblock,comp,amp`
  loads as `gate,comp,amp`).
- Latency: `blockLatency` is atomics only; the lane ring clamps its delays (read); the
  gate's curve index stays inside its ring for delays from -1000 to 199 000 (ASan).
- Test hooks (`setGainTrace`, `setTraceBuffers`, `freezeSweep`, `getLfoPhase`,
  `setDebugSolo`) have no caller under `src/` outside their engines.
- Warnings with `-Wall -Wextra -Wshadow -Wconversion -Wfloat-equal -Wdouble-promotion`:
  22 new, all `-Wdouble-promotion`: `Eq.cpp` 19, `Chorus.cpp:274`, `Drive.cpp:648`,
  `PluginProcessor.cpp:2303`. Gate, Compressor, Phaser, Tremolo, FxBlocks, Parameters and
  PresetManager: 0. The two `-Wshadow` hits in `PluginProcessor.cpp` (`:1455`, `:1855`) are
  in the base commit too. (`build-verify/code/logs/warnings.log`)

## 4. Pre-existing, in code this change did not touch

- `juce::AudioProcessor::setLatencySamples` is called from `processBlock` and from
  `updateLatency` on the message thread: a data race on JUCE's plain `int` (TSan, 7
  distinct stacks).
- `CabSim::clearImpulseResponse` writes a plain flag that `processChainBlock` reads (TSan,
  2 stacks).
- `setStateInformation` off the message thread: `apvts.replaceState` adopts a child of the
  local `root`, whose destructor then runs on the host thread and calls the APVTS tree
  listeners while the message thread's flush timer writes the same tree (TSan, 5 stacks).
  The harness also restores state on the message thread at the same time, which a host
  does not do; the flush timer race does not need that.
- `createFactoryPresetsIfMissing` leaves `drive_on`, `drive_gain`, `delay_mix` and
  `reverb_mix` changed on the first instance of a fresh install (same cause as defect 3).
- Toggling `amp_on` in a lane with a resampling model blanks the other lane the same way
  as defect 2.

## 5. Not reviewed

- Hosted AU slots (headless builds do not host): FX-slot latency into the gate curve and
  the lane ring was reasoned, not run.
- The real AU wrapper and a real host's threads; `WebEditor` at run time (read only).
- Sound quality, Classic bit-identity and golden renders (other verifiers).
- The engines were read by four sub-reviewers and by me in the parts cited above, not
  line by line by one reader; their unreproduced low-priority notes are items 8 and the
  Classic paths' NaN behaviour.
- `tubamp_smoke` at 22.05 and 32 kHz (known failures on `main`).
