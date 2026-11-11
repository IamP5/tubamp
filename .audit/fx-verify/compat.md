# Compatibility verification: old sessions in the new plugin

Independent end-to-end check, 2026-11-11. OLD = `221a3cb` (extracted with `git archive`),
NEW = working tree at `157ab02` (HEAD moved to `a84ca22` during the run; that commit touches
only `.audit/fx-implementation.tsv` and `.gitignore`, no plugin code). arm64, Release, Apple
clang, default Makefiles generator. Nothing under `src/ ui/ tests/ tools/ docs/` was changed.

## 1. Verdicts

| # | Claim / check | Verdict | Short reason |
|---|---|---|---|
| 1 | An OLD session loads into NEW and sounds exactly the same apart from exceptions a, b, c | **NOT VERIFIED as worded**; VERIFIED for static sessions | Static: 239 states, 2868 renders, every difference is exception a or b. Contradiction: moving an EQ or amp-EQ knob mid-stream (old parameter, Classic type) changes the audio by up to -6 dBFS for ~25 ms and never returns to bit-identity. Documented by the team as D-4 (`docs/FX-ENGINES.md` section 8), but not one of the three stated exceptions. |
| 2 | Reverb, delay and cab untouched | VERIFIED | 61 states, 732 renders bit-identical; text fences empty |
| 3 | No existing parameter changed | VERIFIED | OLD list (166) is a byte-identical prefix of NEW (261) over 23 columns |
| 4a | Flat EQ = exact pass-through, difference = old residual | VERIFIED, size understated | NEW == OLD-with-those-EQs-off bit for bit in all 348 such renders; residual is up to -78 dB relative / -65 dBFS peak at 96 kHz, not "near -99 dB" |
| 4b | Drive reports latency, audio unchanged | VERIFIED | 0 -> 3 samples per active Classic drive at 44.1/48/96 kHz, audio bit-identical; split: the other lane is delayed by exactly 3 samples |
| 4c | `mod_type` change differs only within 20 ms | VERIFIED (ahead of the DC blocker) | After +20.0 ms the old/new difference is exactly the free decay of the 5 Hz DC blocker; at the plugin output it is therefore still -67 dBFS at +100 ms |
| 5 | Type read-back and round trip | VERIFIED | 239 blobs + 239 presets |
| 6 | Forward: NEW state in OLD | VERIFIED | no crash, 166/166 old ids restored |
| 7 | Parameter prefix | VERIFIED | see 3 |
| 8 | Text fences | VERIFIED | |
| 9 | Team's proof tools | Golden file provenance VERIFIED; coverage gaps listed in section 9 | |

## 2. Method

- Harness: `build-verify/compat/harness/compat_harness.cpp`, one source file, compiled against
  each tree by `build-verify/compat/harness/CMakeLists.txt` (`-DTUBAMP_TREE=`), which mirrors
  the `tubamp_smoke` target (same sources minus WebEditor, same definitions, same link set, no
  LTO). Uses only API both trees have: `apvts`, `setChainOrder`, `loadModel/loadModelB/loadIr`,
  `get/setStateInformation`, `captureStateVar`, `presets.loadPreset`, `prepareToPlay`,
  `processBlock`, `getLatencySamples`, `setPlayHead`, `setBusesLayout`.
- The processor constructor writes factory presets into the user library when it is empty, so
  every run used `CFFIXED_USER_HOME=build-verify/compat/home-{old,new}`. The user's real
  `~/Library/Application Support/tubamp` was not written to (checked before and after).
- Input (`run/lib.py`, numpy, written once per rate as raw float32 and read by both builds):
  6.5 s stereo, R = 0.8 x L delayed 7 samples. Three plucked low notes (82/110/73 Hz, peak 0.6),
  a six-note chord, a 440 Hz sine stepping -12/-6/-34/-46 dBFS, noise at -30 then -65 dBFS,
  0.5 s of digital silence, one pluck at 0.9, tail.
- Comparison: uint32 view of the float samples (bit for bit, catches -0.0 and NaN), plus max
  abs difference, first/last differing sample, relative RMS.
- Model `A2.nam` (engine A), `wavenet.nam` (engine B); a synthetic 60 ms cab IR.
- Not vacuous: in OLD, every block case changes the input by more than -60 dB relative to the
  empty chain, except the deliberately neutral ones (flat EQ, mod at depth 0 / mix 0, cab
  without IR). 21 of 21 one-step parameter changes applied to NEW only were detected
  (`run/sens.py`, smallest: `cab_highcut` 8000 -> 8001 Hz, -107 dBFS).

## 3. Static matrix: OLD saves, OLD renders, NEW loads the OLD blob and renders

239 states saved by the OLD binary (`run/cases.py`): default state with and without model,
empty chain, gate at -80/-60/-40/-20 alone and ahead of / behind the amp with model, each
compressor instance default/fast/slow/edge/off, each drive instance default/hot/dark/mid/off,
each EQ instance shaped/cut/boost/mild/flat/5-5-5.1/4.9-5-5/5-5.1-5/off, amp tone stack
on-flat/on-shaped/near-flat/off with and without model, output modes, calibration, amp + amp2,
each mod instance x chorus/phaser/tremolo x default/extreme/slow/mid/zero, every reverb type on
every instance plus shaped, every delay type x mode plus instances 2/3, sync and shaped, cab
without and with IR, classic chain five ways, shaped rigs, all-instances chains, 20 split states.

| Run | Conditions | Renders |
|---|---|---|
| base | 44.1 kHz / 127, 48 kHz / 512, 96 kHz / 64; state before prepare | 717 |
| varbs | 44.1 kHz and 96 kHz, random block 1..512 / 1..333 | 478 |
| prepfirst | prepare before `setStateInformation` | 239 |
| mono | mono bus layout, block 256 | 239 |
| playhead | host playing at 97 BPM, ppq running | 478 |
| preset | the OLD `captureStateVar` JSON through `PresetManager::loadPreset` | 478 |
| presetph | preset path, 141.5 BPM playing, random blocks, 44.1 kHz | 239 |
| total | | **2868** |

State-level result (a state counts as identical only if all 12 of its renders are):

| Class | States | What |
|---|---|---|
| bit-identical audio, same latency | 176 | |
| bit-identical audio, latency +3 per active drive | 23 | exception b (22 at +3, 1 at +6) |
| differs, NEW == OLD with the flat EQs off, bit for bit | 29 | exception a (9 of them also b latency: +3, +6, +9) |
| differs, split path with a drive in one lane | 11 | exception b (split) |
| unexplained | **0** | |

By block tag (renders): reverb 252/252 identical, delay 420/420, cab 60/60, comp 180/180,
mod 588/588; every differing render of gate, drive, eq, amp, chain is class a or b above.

Also bit-identical or explained the same way: 4 oldest-format blobs (no `chainOrder`, only the
37 v1 ids, made by rewriting OLD blobs; `run/legacy.py`), the 4 factory presets OLD writes,
the user's 4 real v1 preset files (read-only copies; their `eq_*` are not flat, so only the amp
EQ residual applies), and 84 "host restores state while audio is already running" renders
(`run/live.py`: 80 identical, 4 = exception a, confirmed against the EQ-off variant).

Full table: `build-verify/compat/work/matrix-all.tsv` (one row per render).

## 4. Exception checks

### a. Flat EQ / flat amp tone stack

For every state with an EQ or the amp tone stack on at exactly 5/5/5, OLD also rendered the
same state with those `*_on` ids off. NEW equals that render bit for bit in all 348 differing
non-split renders. So the old/new difference is exactly the old code's residual.

Size of that residual (old flat vs new pass-through), relative RMS / worst peak:

| Rate | One flat amp EQ, amp alone | Worst case in the matrix |
|---|---|---|
| 44.1 kHz | -98.7 dB | -88.1 dB, -74.0 dBFS |
| 48 kHz | -97.8 dB | -87.1 dB, -74.9 dBFS |
| 96 kHz | -88.7 dB | -78.3 dB, **-65.4 dBFS** |

"Near -99 dB" holds for one flat EQ at 44.1/48 kHz only. 5/5/5.1, 4.9/5/5 and 5/5.1/5 are
bit-identical to OLD (the filters still run).

### b. Drive latency

- Serial: audio bit-identical in every drive case; reported latency 0 -> 3 per drive that is
  on, at all three rates (two drives 6, three 9). A drive that is off reports 0.
- Gate ahead of the amp with a drive between (`gate,drive,amp`, Classic): audio bit-identical
  once the flat amp EQ is accounted for; only the latency moves. The Classic gate does not wait
  (consistent with D-3).
- Split, lanes panned apart (`run/split.py`), 44.1/48/96 kHz: with the drive in lane A and the
  dry lane B hard right, NEW's right channel is OLD's right channel delayed by exactly 3
  samples, bit for bit; the drive lane's channel is unchanged (left differs by 1.2e-7 only
  because a hard-right pan leaks -4e-8 of lane B into it). Drive in both lanes: bit-identical,
  latency 3. `split_on` off with a drive in lane A: bit-identical, latency 3.
- Summed response of the two lanes, centred, impulse 0.001, drive gain 0 dB, tone 12 kHz, 48 kHz:

  | Hz | 100 | 1k | 2k | 4k | 6k | 8k | 10k | 12k | 16k | 20k |
  |---|---|---|---|---|---|---|---|---|---|---|
  | OLD dB | 3.01 | 2.73 | 1.85 | -2.69 | **-39.72** | -2.34 | 1.89 | 1.03 | -4.61 | -3.29 |
  | NEW dB | 3.01 | 3.00 | 2.95 | 2.75 | 2.26 | 1.19 | -1.11 | -5.73 | -4.61 | -2.48 |

  OLD had a comb notch at 6 kHz; NEW removes it.
- Two notes, neither a compat break: (1) the Classic drive's real mid-band delay is 3.9-4.0
  samples at 44.1/48 kHz and 5.0 at 96 kHz with tone at 12 kHz (the tone filter adds to the
  oversampler), so the reported 3 under-counts and the lanes are not fully aligned (hence the
  -5.7 dB at 12 kHz above). (2) The reported latency now follows `drive_on` automation: a
  session that automates `drive_on` reports 3 -> 0 -> 3 mid-playback where OLD reported a
  constant 0 (D-7). Audio is still bit-identical in a serial chain.

### c. `mod_type` change mid-stream

Driven through the harness (`run/auto.py`, `run/modtype.py`): mod alone, type 0 -> 1 at 1.0 s,
1 -> 2 at 2.5 s, 2 -> 0 at 4.0 s, at 44.1/48/96 kHz. No sample differs before the first
change. Max |old-new| 0.095 / 0.026 / 0.0015. The last sample at which the difference is not a
free decay of the 5 Hz second-order DC blocker is +19.95 to +20.02 ms after the change in all
nine events (residual of the decay law after 21 ms <= 2.4e-7, float rounding). So ahead of the
DC blocker the two builds differ for 20 ms only. At the plugin output the DC blocker's decay
keeps the difference alive: 4.5e-4 (-67 dBFS) at +100 ms, and a delay/reverb behind the mod
repeats it (0.064 beyond 100 ms in the full chain). Static mod renders: 588/588 identical.

## 5. Mid-stream changes of old parameters (`run/auto.py`, 57 tests x 2 configs)

Each base state is bit-identical when static. 46 tests bit-identical in both configs:
gate threshold steps (alone, in a chain, ahead of the amp with model), all five compressor
knobs, drive gain/tone/level, amp input/output/output mode, mod rate/depth/mix for all three
types, delay and reverb knobs and type/mode switches, input trim, output level, bypass off/on
of gate, comp, drive, eq, mod, delay, reverb, amp, amp EQ, cab, remove-and-re-add of each
block, chain reorder.

Differing:

| Test | Size | Explained by |
|---|---|---|
| `eq_bass/mid/treble` steps, EQ alone | 0.42 (-7.5 dBFS) in the first 25 ms, 0.06 at 25-100 ms, <= 1.1e-3 later; never bit-identical again while signal is present | **none of a/b/c** (team's D-4: Classic knob smoothing) |
| one 0.1 step of `eq_bass` | 0.012 (-38.7 dBFS), still 5.6e-5 after 100 ms | same |
| EQ to exactly 5/5/5 and back | 0.20 (-13.8 dBFS) | same, plus the flat reset |
| `amp_eq_*` steps, amp with model | 0.50 (-6.0 dBFS) | same |
| EQ steps inside the full FX chain | 0.40-0.46, carried by delay/reverb to the end | same |
| `mod_type` changes | see 4c | c |
| split with drive: static, `drive_on` toggle, mixer knobs | 0.13-0.15 | b |
| adding comp2, drive2, eq2, mod2 to the chain after load | 0.86 | not an old-session difference: blocks added in NEW start on the new default types by design |

## 6. Type read-back and round trip (`run/readback.py`, `run/ontop.py`, `run/classicignores.py`)

- 239 OLD blobs and 239 OLD presets loaded into NEW: all 166 old ids read the same as OLD
  reads them; chain, model and IR paths equal; every type/mode/voicing id of a block in the
  saved chain reads 0 (19 pinnable ids); every other new id reads its new default
  (`gate_mode` 1, `*_type` 1, mod voicings 1, the rest per `params-new.tsv`). 0 problems.
- NEW saves, NEW loads: values round-trip in 478/478 (blob) and 478/478 (preset); audio
  round-trips bit for bit along the same path (blob -> blob, preset -> preset). Crossing paths
  (blob render vs preset render of the same state) differs by <= 1.5e-4 in 63 states; OLD has
  the same 63-state path dependence, so it is not from this work.
- OLD state loaded on top of a live NEW instance that had every new parameter away from its
  default and all types non-Classic (blob then blob; blob then preset), host playing: 956/956
  bit-identical to a fresh load, pins all Classic. `replaceState` resets absent ids to default.
- With an OLD blob loaded, pushing all 76 new non-type parameters to a range end with the host
  playing changes nothing: 239/239 bit-identical. Classic reads none of them.

## 7. Forward check (`run/forward.py`)

Six states saved by NEW with new types selected (all 17 blocks with every new id at a range
end; all at mid; classic chain on the recommended types; fresh default; split with Fuzz and
Distortion; Smooth gate), as blob and as preset, loaded into OLD at 48 and 44.1 kHz: no crash,
166/166 old ids restored, chain/model/IR equal, output finite, and bit-identical to OLD's
render of the same old-id values saved by OLD. OLD ignores the new ids and plays its own
engines (so it sounds different from NEW, e.g. +17 dB relative for the all-mid state). OLD's
host-state re-save keeps the unknown `PARAM` children: NEW -> OLD -> saved by OLD -> NEW
changes 0 parameter values. (A preset re-saved by OLD drops them; not rendered.)

## 8. Parameters and text fences

- `compat_harness params` on both builds: index, id, name, version hint, class, range start /
  end / interval / skew / symmetric, default plain and normalised, steps, discrete, boolean,
  automatable, meta, category, label, choices, text at 0 / 0.5 / 1. OLD's 166 rows are
  byte-identical to NEW's first 166. NEW adds 95, all hint 4, no duplicate ids. Old hints:
  44 x 1, 83 x 2, 39 x 3. Files: `work/params-old.tsv`, `work/params-new.tsv`.
- `git diff --stat 221a3cb -- src/dsp/Reverb.cpp src/dsp/Reverb.h src/dsp/Delay.cpp
  src/dsp/Delay.h src/dsp/FxPrimitives.h docs/REVERB-DELAY.md ReverbBody.tsx DelayBody.tsx
  CabBody.tsx` is empty; each file also `cmp`-identical to the extracted OLD tree.
- `class CabSim` (883 characters with its comment) and all six `CabSim::` definitions are
  textually identical; no `CabSim::` occurs outside them.

## 9. The team's proof tools

Provenance, checked: `tests/golden/fx-golden.tsv` was added in `bfb6e30`, whose processor,
FxBlocks, Parameters, NamEngine, Reverb, Delay and PresetManager sources are byte-identical to
`221a3cb` (only unused engine stubs added). I built `tubamp_golden` from that commit and
re-captured with `A2.nam`: 339 rows, byte-identical to the committed file. The hashes are the
old plugin's. `reverb_delay_guard.py --check` passes against a `tubamp_fxrender` built from
the old engines (144 renders).

Could they pass while the plugin is wrong? Yes, in these places:

1. `--verify` does render through the real processor, but never through a state restore. It
   sets all 19 type ids to 0 by hand, so the legacy pin, `setStateInformation` and
   `applyStateVar` are outside it. The pin is covered only by smoke `runLegacyPinTests`, which
   checks parameter values on states made by deleting ids from a NEW state, not audio and not
   a blob written by the old binary.
2. The `off:` rule compares this build with itself. For `classic` (7 configs), `gateamp`,
   `ampM` and `gateampM` the captured old hash is not used at all, so the full chain, the
   deferred gate and every case with a model are not compared with the old plugin by
   `--verify`. The doc says the off renders were once captured on the pre-integration
   processor; that is not in the automated check.
3. `splitA differs` passes on any change whatsoever.
4. Fixed block 512 (1 and 37 for starred cases), no play head, no mono layout, no parameter
   movement, state always set before prepare.
5. `reverb_delay_guard.py` compiles only `Reverb*.cpp` / `Delay*.cpp` with `FxRender.cpp`, so
   it re-proves the file fence and cannot see how the processor drives the two blocks (the
   processor's `dp.bpm` line did change). It does not cover the cab. `range(6)` asks for a
   sixth type that does not exist, and Dual equals Stereo at the default ratio: only 90 of the
   144 hashes are distinct.

## 10. Not tested / inconclusive

- The shipped AU binary (LTO, AU wrapper), `auval`, and a real host: INCONCLUSIVE. The harness
  has the smoke target's flags.
- The WebView editor. By reading only: `applyStartingPoints` is called from `onPick` handlers
  and nowhere on load. Not run.
- Hosted AU slots (`fx1-3`): the harness has no AU hosting.
- 22.05 and 192 kHz; blocks larger than the prepared size; a sample-rate change between two
  prepares.
- A preset re-saved by OLD from a NEW state.

## 11. Files

`build-verify/compat/harness/` (harness and CMake), `build-verify/compat/run/*.py` (matrix,
summaries and each check named above), `build-verify/compat/work/` (`matrix-all.tsv`,
`matrix-*.json`, `auto.json`, `readback.log`, `state-classes.json`, OLD blobs and presets in
`states-old/`, parameter dumps, re-captured golden file). All scratch WAVs were deleted after
comparison. `build-verify/` is untracked; nothing was added to git.
