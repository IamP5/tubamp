# Round 3: regression pass on the plugin processor (HEAD 7b7322a, source = 2a280b9)

Independent verifier, 2026-11-11. Method: the round-1 and round-2 verifiers' own harnesses and
scripts, rebuilt against the current tree in `build-verify/r3-processor/` (nothing in
`build-verify/compat/`, `build-verify/processor-r2/`, `build/` or `build-fx/` was changed or
rebuilt). No tracked file changed, no git command that changes state, the installed Audio Unit and
`~/Library/Application Support/tubamp` untouched (24 files before and after, none newer than my
start marker; every run used `CFFIXED_USER_HOME` under `build-verify/r3-processor/`). `git diff
2a280b9 HEAD -- src tests tools` is empty (the two later commits are docs and audit). arm64,
Release, Apple clang. The engineers' smoke test, golden tool and legacy-check were not used.
Labels: **[M]** measured (script named), **[I]** inferred from reading, **[G]** guess.

Adaptations to the earlier harnesses (all in the r3 copies):
- `harness/r2.cpp`: latency is now reported by a 20 ms message-thread timer, so (a) during a
  render the processor's own `timerCallback()` is run on the harness main thread (the JUCE message
  thread) every 20 ms of audio time, between blocks; (b) the real message loop is pumped 45 ms
  before every latency read (`print`, `dump`, the `RESULT` line); (c) `pumpevery` runs the real
  message loop instead of (a). (a) and (c) give the same report sequence [M]. With neither, no
  report arrives until a later message-thread parameter change [M]. The thread stress already
  pumped the loop and is unchanged, plus a new flag for automation delivered on the audio thread.
- `compat-harness/compat_harness.cpp`: the pump before the end-of-render latency read is 50 ms
  (was 10). Old binary: `build-verify/compat/hb-old`, as in rounds 1 and 2.

## 1. Verdict

- Old sessions, state restored before `prepareToPlay` (3 346 static renders, 22.05 to 192 kHz):
  **0 differences that are not a declared exception** [M].
- Old sessions, `prepareToPlay` before the state restore (239 renders): **22 differ and are not a
  declared exception as worded** [M]. Same mechanism as the new declared exception 5: every block of
  the restored chain is heard dry for 10 ms and fades in over 10 ms. Round 2 had 0 here. With
  audio from the first sample it is 250 of 478 renders. Details in 2.3. This is the one regression.
- Round-2 findings (12 rows): **5 fixed, 3 partly, 4 not fixed** (none of the four was claimed
  fixed by the two commits).
- Guard, AddressSanitizer + UBSan: clean. ThreadSanitizer: the `setLatencySamples` race is gone,
  no report in the new code; the three older races remain.

## 2. Old sessions

### 2.1 Static renders: 239 states saved by the old binary [M] (`compat-run/matrix.py`, `summ.py`)

| Run | Conditions | Renders | Bit-identical | +latency only (b) | Flat EQ (a) | Split with drive (b) | Not declared |
|---|---|---|---|---|---|---|---|
| base | 44.1 kHz / 127, 48 kHz / 512, 96 kHz / 64 | 717 | 528 | 69 | 87 | 33 | 0 |
| extra | 192 kHz / 256, 22.05 kHz / 128, 88.2 kHz / 512 | 717 | 528 | 69 | 87 | 33 | 0 |
| varbs | 44.1 and 96 kHz, random blocks | 478 | 352 | 46 | 58 | 22 | 0 |
| playhead | host playing at 97 BPM | 478 | 352 | 46 | 58 | 22 | 0 |
| preset | through `PresetManager::loadPreset` | 478 | 352 | 46 | 58 | 22 | 0 |
| presetph | preset, 141.5 BPM, random blocks | 239 | 176 | 23 | 29 | 11 | 0 |
| mono | mono bus | 239 | 176 | 23 | 29 | 11 | 0 |
| prepfirst | prepare before restore, 48 kHz / 512 | 239 | 175 | 18 | 13 | 11 | **22** |
| total | | **3 585** | 2 639 | 340 | 419 | 165 | **22** |

By state: 176 bit-identical, 23 latency only, 29 flat EQ, 11 split with drive (the round-1 and
round-2 split), and 22 of them also differ in prepfirst. Latency: +3 in 570 renders, +6 in 45, +9
in 30, 0 elsewhere. Parameter dump: old 166 rows are a byte-identical prefix of the new 261, and
the new dump is byte-identical to round 2's [M] (`compat_harness params`).

Also re-run [M]: `legacy.py` (identical), `readback.py` (only the 63-state blob/preset path
dependence the old build has too, largest 1.5e-4), `ontop.py` (956 of 956), `classicignores.py`
(239 of 239), `forward.py` (ok), `pathdep.py` (same 63 states in both builds), `split.py` (as
round 1). `live.py` and `auto.py`: section 2.3 and 2.2.

### 2.2 Toggles, reorders, splits, presets: the 192 extra renders of round 2 [M] (`compat-run/r2_auto.py`, `r3_auto_size.py`)

No render differs before its first event except exception b.

| Class | Renders | Tests | Size (old - new) |
|---|---|---|---|
| bit-identical | 54 | split static, split reorders, mixer moves, model-in-lane static, `amp_on` at 48 kHz | 0 |
| b: Classic drive in a split lane, static | 12 | A5_static | -19.5 dBFS |
| b + toggle fade | 12 | A5_toggles | -0.6 dBFS |
| lane slide (declared now) | 6 | `amp_on` in a lane with a resampled model, 44.1 / 96 kHz | -20.0 dBFS (96 kHz), -26.6 dBFS (44.1 kHz) |
| block switched off / on (declared, new) | 72 | each block off and on, all at once, fast toggle, classic chain with model, deferred gate, lane toggles | up to +13.5 dBFS (fast toggle); deferred gate -86.3 dBFS |
| block removed from the chain and re-added, reorder that re-adds (same mechanism, **not listed as worded**) | 36 | reorders (2 of 5 events), remove_readd_each, A2_reorders | +3.9 dBFS, -1.1 dBFS, -55.5 dBFS |

Size of the Classic-toggle difference, old burst against new, at the events (48 kHz / 512; the
44.1 kHz / 127 run is within 2 dB):

| Test | Static render: peak / largest step / >5 kHz | Old at events | New at events |
|---|---|---|---|
| five Classic blocks toggled every 30 ms | +8.2 / -9.2 / -20.5 dBFS | **+15.4 / +11.1 / +3.9** | +11.7 / -12.7 / -30.5 |
| each block off and on in turn | same | +7.7 / -3.1 / -10.4 | +7.7 / -12.3 / -20.3 |
| split, Classic drive lane toggles | +2.3 / -12.0 / -20.2 | -1.1 / -0.7 / -8.1 | -1.0 / -17.9 / -22.5 |
| chain reorders | +8.2 / -9.2 / -20.5 | +3.8 / **+1.6** / -5.6 | +3.8 / **+1.6** / -5.6 (same click in both) |
| each block removed and re-added | same | +7.7 / -8.8 / -15.8 | +7.7 / -8.5 / **-13.2** (2.6 dB more >5 kHz than old) |
| split lane toggles (no drive) | -3.7 / -10.3 / -13.9 | -2.8 / -10.3 / -14.6 | **-1.7** / -10.3 / -14.6 (peak 1.1 dB over old) |

Round 1's automation set (`auto.py`, 114 renders): 78 identical; the 36 others are the 22 of
rounds 1 and 2 (EQ knob ramps, `mod_type`, split with drive, blocks added after load) plus 8
block toggles and 6 remove-and-re-add renders.

### 2.3 Not declared: a state restore onto a prepared or running instance [M]

`matrix.py prepfirst`, `matrix_r3.py pf0 / std0`, `r3_prepfirst.py`, `live.py`.

| Case | Renders | Differ beyond a / b | Size |
|---|---|---|---|
| prepare, then restore, input silent for the first 50 ms | 239 | 22 (chains with a model and a gate, or 7 to 15 blocks) | model + gate: the new build emits -58.3 dBFS at 10 ms where the old was silent (9e-20). 15-block chain: old - new 0.38 (-8.4 dBFS) at 83 ms, -29 dBFS until 2 s |
| prepare, then restore, audio from the first sample (48 kHz / 512 and 44.1 kHz / 127) | 478 | **250** (125 states) | first 20 ms: up to +11.8 dBFS (comp with make-up); the new build is the louder one in 152, by up to **18.8 dB** (comp fast: -5.8 against -24.6 dBFS). 20 to 100 ms: up to +0.2 dBFS (7-block chain). After 1 s: 26 renders above -60 dBFS, worst -35.8 dBFS. 20 renders are below -60 dBFS after 20 ms |
| restore, then prepare, same input | 239 | **0** | |
| old state restored while audio runs (`live.py`) | 84 | 72 (round 2: 4, all flat EQ) | 0 samples before the restore; 0 to 25 ms up to 2.46 (+7.8 dBFS), 25 to 100 ms 0.71, after 1 s 6.8e-3 |

Cause [I]: a block that enters the chain on a prepared processor is reset and goes through
`BypassFader::reset (false)` + `restart()`: dry for the 10 ms settle, then the 10 ms fade; with
many blocks the two-resets-per-callback drain stretches it (15 blocks at 512 samples: about 95
ms). The old build ran the block from the first sample. After `prepareToPlay` the fader snaps, so
the restore-then-prepare order is exact. The engineers' legacy-check mirrors the fader on the
legacy side (docs/FX-ENGINES.md 6.0), so it cannot see this [I]. Which order Logic uses was not
tested [G: both occur in hosts; a preset or state recall on a live instance always takes this path].

### 2.4 Declared exceptions, measured size [M]

| Exception | Size |
|---|---|
| a. Classic EQ at exactly neutral | New equals the old render with those EQs off, bit for bit, in all 419. One flat amp EQ: -107.6 dB (22.05 kHz), **-98.7 (44.1)**, -97.8 (48), -89.6 (88.2), -88.7 (96), **-79.9 dB (192 kHz)** relative. Worst state: -74.0 dBFS peak / -88.1 dB (44.1), -75.0 / -87.1 (48), -65.4 / -78.3 (96), -57.0 dBFS / -69.3 dB (192) |
| b. Classic drive reports 3 samples | +3 per active Classic drive, 22.05 to 192 kHz; series audio bit-identical (340 renders); split: 165 renders, the dry lane delayed by 3 samples |
| c. `mod_type` change | 9 events (`modtype.py`): max old - new 0.295 (-10.6 dBFS); last sample that is not the DC blocker's free decay +60.6 to +69.0 ms (round 2: +20 ms); 7.4e-4 at +100 ms. Wait before the fade over 2 200 switches: section 7 |
| d. Classic EQ knob automation | larger than in round 2: EQ alone -5.1 dBFS in the first 25 ms (round 2: -7.5), -18.3 dBFS at 25 to 100 ms, <= -46.9 dBFS at 100 to 500 ms, <= -75.9 dBFS after 1 s; amp EQ **-3.2 dBFS** at 96 kHz (round 2: -6.0 at 48 kHz); one 0.1 step -35.6 dBFS |
| e. every block kind reset and crossfaded over 10 ms when switched (new) | section 2.2: the old +11.1 dBFS step and +3.9 dBFS >5 kHz burst are gone (new -12.7 / -30.5 dBFS); old - new up to +13.5 dBFS |
| f. samples of magnitude 1e6 or more become 0 (new) | `py/t_huge.py`: 3e38 with `input_trim` +12 dB, 1e30, 1e25, 1e20 in 10 layouts: 0 non-finite, tail finite; +40 dBFS (100.0) passes. Not reached by any old-session render |
| g. latency holds the larger figure during a drive type switch (new) | rise reported at the first timer tick (1.3 ms after the switch with a 5 ms pump, <= 20 ms); drop reported 40 to 43 ms after the switch plus the tick; bypassed drive 10.7 ms. Two switches 3 ms apart: **77 ms** after the second (`py/t_lane.py`) |
| h. split-lane delay changes slide over 10 ms (new) | `amp_on` in a lane with a resampled model: -20.0 dBFS (96 kHz), -26.6 dBFS (44.1 kHz) against the old build's 27 / 42 blank samples; Classic `drive_on` in a lane: dry lane 1.25e-2 during the slide |

## 3. Round-2 findings

Section 6 items 1 to 10, then the two "partly" rows of section 2.

| # | Finding (round 2) | Status | Number now | Script |
|---|---|---|---|---|
| 1 | Latency changes reported from the audio thread: one lock each, async message in the AU wrapper | **Fixed** [M] | `setLatencySamples` reached from the audio thread 0 times in every run (14.4 M blocks of round 2's stress, 7 composite scenarios, all scenario scripts; every latency log entry is from the message thread). Guard under message-thread `drive_on` / `drive_type` changes at block rate: 0 allocations, 0 locks, 0 system calls. Residual: section 6.4 | `r2 stress`, `py/t_scen_san.py`, `py/t_fader2.py`, `py/t3_drivesplit.py` |
| 2 | `drive_type` change on a bypassed drive clicks (-37.0 dBFS) | **Fixed** [M] | -131.7 dBFS in the >5 kHz band (60 pairs x 3 rates), step <= +0.1 dB over steady, settled in 9.5 ms; type changed while off then enabled: -137.7 dBFS | `py/t_drivetype.py`, `py/t_fader2.py typeoff` |
| 3 | Two lane-latency changes within 10 ms step the other lane (0.17 to 0.24) | **Fixed** [M] | 0.111 against 0.104 steady at fs/10 (44.1 / 48 / 96 kHz); envelope dip <= 0.4 dB (round 2: -4.8 dB and a full notch) | `py/t_lane.py` |
| 4 | Lane slide not declared | **Fixed** (declared) [M] | -20.0 dBFS at 96 kHz, -26.6 dBFS at 44.1 kHz | `r2_auto.py` |
| 5 | Crossfade dips larger than reported (-17.1 dB `mod_type`, -15.4 dB phaser model, -5.5 dB chorus mode) | **Partly** [M] | 220 Hz: `mod_type` worst **-2.0 dB** (tremolo Harmonic to Small Stone, outside +-1 dB for 6.3 ms; Small Stone to CE 0.0); phaser model **-1.0 dB**; chorus Wide to Tri **-5.5 dB, unchanged**. Other probes, 660 switches each, into chorus Wide left out (below): 82 Hz -1.8 dB, 110 Hz -0.2, 440 Hz -2.35, 1 kHz 0.0. Chorus mode switches at 440 Hz: Dimension to Tri **-8.75 dB**; at 1 kHz Tri to Wide -10.4 dB on the right channel for 50 ms. Pops: <= -69.9 dBFS (into tremolo Optical), into any chorus <= -73.7 dBFS; 0 non-finite | `py/t_mod.py`, `py/t3_mod_probe.py`, `py/t3_wide.py` |
| 6 | Reported latency stale while no audio runs | **Partly** [M] | With no audio running: correct at once in 7 of 7 cases. For 500 ms after the last audio callback a type change still reports the old figure (5 where 0 is right at +0, +100, +400 ms; 0 at +650 ms) | `work/stale.txt` |
| 7 | Tremolo Optical after a restart: -68.1 dBFS | **Not fixed** [M] | -68.1 dBFS at re-enable (34.8 dB over steady); -67.0 dBFS after a transport change | `py/t_toggle.py`, `py/t_misc.py tempo` |
| 8 | Chain reorder peak +4.2 to +4.4 dB | **Not fixed** (as reported) [M] | +4.4 dB (48 kHz / 256), **+4.7 dB** (96 kHz / 2048), 0.0 dB at block size 1; the old build has the same click at reorders (step +1.6 dBFS in both) | `py/t_fader2.py reorder` |
| 9 | Mixer pan gains ramp from un-panned for 20 ms after prepare | **Not fixed** (old code) [M] | left 0.494 falling to 0.250 over 20 ms (+5.9 dB) | inline check: split, lanes panned apart, left-only input |
| 10 | NAM garbage slots race | **Not fixed** [I]; not reproduced [M] | `NamEngine.*` unchanged since bf81728; 0 stacks in one 60 s TSan run with model swaps (round 2: 3) | TSan C |
| S2-8 | Host tempo 0 or negative held the LFO (0.075 Hz) | **Fixed** [M] | 2.000 Hz at 0 and -5 BPM, tremolo Smooth and Bias; all 16 transport cases as expected except 1.01 BPM, which the 5 s window cannot measure | `py/t_tempo2.py` |
| S2-9 | Reported latency against measured delay | **Partly**, unchanged [M] | Bypassed drive: pure delay of the reported figure, bit-exact 216 of 216. Running drive: measured exceeds reported by the same 0 to 184 us as round 2 (Fuzz at 192 kHz: reported 2, measured 37.2) | `py/t_latency.py` |

Round 2's other rows, re-run [M]: NaN / Inf input 286 renders, 0 non-finite, all identical to the
zeroed input (`py/t_nan.py`); stale tail after re-enable <= -201.8 dBFS, Classic included
(-210.7 dBFS hot Classic drive, round 2 +6.0 dBFS; Classic EQ pop -103.5 dBFS, round 2 -8.1)
(`py/t_toggle.py`); drive waiting for its reset: latency log 0 at the removal, 3 at the re-entry,
both from the message thread, no 3 -> 0 -> 3 (`py/t_pending.py`); 3e38 with input trim: finite
(`py/t_huge.py`); fader restarted mid-fade 306 renders: peak <= +0.44 dB, >5 kHz <= -46.6 dBFS,
as round 2 (`py/t_fader.py`); state round trip 0 value differences (`py/t_state.py`); NAM latency
short by the same 1.5 to 5.0 samples (`py/t_namlat.py`); meters and deferred-gate alignment as
round 2 (`py/t_misc.py`).

## 4. Latency truth [M]

`py/t_latency.py`, 6 types x 3 slots x series / lane A / lane B x 4 rates = 216, latency read
after pumping. All reports come from the message thread.

| Type | Rate | Reported on | Reported off | Measured on (9 placements) | Off = pure delay of the report | `drive_on` toggle reports |
|---|---|---|---|---|---|---|
| Classic | 44.1 / 48 / 96 / 192k | 3 | 0 | 3.81 / 3.90 / 4.91 / 6.80 | bit-exact 9/9 each | 3 -> 0 -> 3 |
| Screamer | 44.1 / 48 / 96 / 192k | 5 / 5 / 4 / 1 | same | 5.96 / 6.07 / 6.41 / 6.56 | bit-exact | none |
| Distortion | 44.1 / 48 / 96 / 192k | 5 / 5 / 5 / 4 | same | 5.35 / 5.36 / 5.20 / 4.66 | bit-exact | none |
| Fuzz | 44.1 / 48 / 96 / 192k | 5 / 5 / 4 / 2 | same | 13.10 / 13.80 / 21.70 / 37.21 | bit-exact | none |
| Boost | all | 0 | 0 | 0.43 / 0.50 / 1.18 / 2.44 | bit-exact | none |
| Treble | all | 0 | 0 | 0.00 | bit-exact | none |

- Dry lane next to a drive lane: delay equals the reported latency, largest difference 5.6e-9.
- What a host sees during a `drive_type` switch (`py/t_drivetype.py`, `py/t3_align.py`, real
  message loop pumped every 5 ms): to a larger latency: the new figure at the first timer tick
  (+1.3 ms pumped at 5 ms; at most one 20 ms period), while the audio's delay slides over the first
  1 to 9 ms. To a smaller one: the old figure stays through settle and fade, the audio slides at
  +31 to +39 ms, the report follows at +40 to +43 ms plus the tick. Bypassed drive: slide 0.5 to
  9.5 ms, report at the next tick. No report from the audio thread.
- Drive on, `drive_type` switch: no gap, level dip <= 0.7 dB (round 2: -6 to -12.5 dB), pop not
  above the steady renders.
- Split-lane alignment during a switch (`py/t3_align.py`, probe at fs/48, lanes panned apart, 60
  ordered pairs x 4 rates, drive on and bypassed). Bypassed: **0.000 samples** at all four rates.
  Running, pairs of unequal latency: <= 0.005 samples (44.1 / 48 kHz), 0.019 (96 kHz), 0.093
  samples for 1.4 ms (192 kHz) without the Fuzz; Fuzz <-> Screamer at 192 kHz reads 0.51 samples
  for 8 ms, which this method cannot tell from the phase of the blend of two circuits 51 samples
  apart in phase delay (equal-latency pairs read 0.12 at 44.1 kHz the same way) [I]. The 0.41
  sample claim is not contradicted. Dry-lane largest step <= 1.019 x the steady sine.

## 5. Real-time safety and threads

- Guard (`harness/guard.c`, self-test sees 2 allocations, 2 frees, 1 lock, 1 write) [M]:
  round 2's thread stress, 4 runs of 40 s, 14.4 M blocks (toggles, type switches, reorders with
  and without split, state restores, re-prepare at six rates, knobs, transport, model swaps, IR
  loads, AU-like listener): **0 allocations, 0 frees, 0 locks, 0 system calls**, 0 latency
  reports from the audio thread (round 2: one lock per report). Seven composite scenarios (22.05
  to 192 kHz, mono, blocks larger than prepared, NaN bursts): the same. Thirteen blocks flipped
  every block for 10 s, non-Classic and all-Classic: the same.
- Automation delivered on the audio thread (new stress flag, `setValueNotifyingHost` inside the
  audio callback every block): 9.0 M events on `drive_on`, `drive_type`, `drive2_on`,
  `drive2_type`, `drive3_on`, `amp_on` with the full stress around it: 0 system calls, 0 latency
  reports from the audio thread, 4 allocations in total (the same 4 in every run, not per event),
  **2.89 locks per event against 1.99 for `eq_bass`** (section 6.4).
- AddressSanitizer + UBSan [M]: 3 x 70 s thread stress (3.8 M blocks), the seven composite
  scenarios, five scenarios aimed at the new code (drive types, `mod_type`, toggles, reorders,
  re-prepare, 3e38 and -1e6 samples, 22.05 to 192 kHz, blocks 1 to 1024): **0 reports**, 0 non-finite.
- ThreadSanitizer [M]: 4 x 60 s (35 k to 152 k blocks each; the machine was shared). 12 distinct
  reports: 3 are the harness's own play head and flags; CabSim flag and JUCE's convolution queue
  (5 stacks: `clearImpulseResponse` / `loadImpulseResponse` against `processChainBlock`, also on
  every state restore without an IR); off-thread `setStateInformation` against the parameter flush
  (3 stacks, ValueTree listener list and `~SharedObject`). **`setLatencySamples` from two threads:
  0 stacks (round 2: 13). Nothing in the latency timer and its atomics, `BypassFader`,
  `DriveTimeline` or `PowerCrossfade`.** Toggles, type switches, knobs and transport alone: 0
  reports in plugin code. `logs/tsan-symbolized.txt`.
- CPU (`py/t_cpu.py`, other agents' jobs running, indicative only) [M]: 13 blocks toggled every
  block against steady: mean 4.7 against 4.7 % of a core (48 kHz / 64), 6.3 against 5.6 % (96 kHz
  / 32); worst block 17 against 12 %, 41 against 37 %, 50 against 37 % (48 kHz / 16) of its budget.

## 6. Fences [M]

- `git diff --stat bf81728 -- src/dsp/Reverb.* src/dsp/Delay.* src/dsp/FxPrimitives.h
  docs/REVERB-DELAY.md`: empty. `NamEngine.*`: unchanged.
- `compat-run/r2_fence.py`, round 2's settings (43 old reverb, delay, mixed and cab states, 7
  configurations, with and without 15 mid-stream changes): **378 of 378 bit-identical**, latency
  equal. Static matrix: every reverb, delay and cab case bit-identical.

## 7. New findings, worst first

1. **A state restored onto a prepared or running instance fades its blocks in (section 2.3).**
   [M] Not a declared exception as worded. 22 of 239 old states at a silent start (-58.3 dBFS
   where the old build was silent; -8.4 dBFS when the first note falls inside the drain), 250 of
   478 renders with audio from the first sample (new up to 18.8 dB louder than old for 20 ms,
   bounded by the block's bypassed level), 72 of 84 live restores. Regression against round 2 (0).
   Likelihood: every preset or state recall while audio runs; a session load only in hosts that
   prepare before restoring [G]. Consequence: 20 to 95 ms of partly unprocessed signal, then
   identical apart from tails (<= -35.8 dBFS after 1 s).
2. **Chorus mode switches still dip, and more at other notes.** [M] Wide to Tri -5.5 dB (220 Hz,
   unchanged), Dimension to Tri -8.75 dB (440 Hz), Tri to Wide -10.4 dB on one channel for 50 ms
   (1 kHz). Engine, not processor. Separately, chorus Wide after any reset (cold start, re-enable,
   `mod_type` or mode switch into it) passes a comb notch 14.4 dB below its later range at 1 kHz,
   140 to 190 ms after the reset, the same in a static render from prepare: not a crossfade dip.
3. **Remove-and-re-add and lane toggles are slightly worse than the old build.** [M] >5 kHz at
   chain remove / re-add events -13.2 dBFS against -15.8 old (static -20.5); split lane toggles
   peak 1.1 dB over old.
4. **One mutex lock per automated latency parameter on the audio thread.** [M + I] 2.89 locks per
   event against 1.99 for a plain parameter: `parameterChanged` calls
   `juce::MessageManager::existsAndIsCurrentThread()` (`PluginProcessor.cpp:502`), which takes
   `messageThreadIdMutex` (`juce_MessageManager.cpp:193`). Uncontended and short; far less than
   round 2's lock plus async message. The 4 one-time allocations were not traced.
5. **Latency reports are not rate-limited for message-thread changes.** [M] `drive_type` changed
   every block from the message thread at 96 kHz / 32: 10 424 reports in 10 s; Classic `drive_on`
   every block: 7 500 at 48 kHz / 64. Automation on the audio thread goes through the timer (234
   reports in 20 s for 239 k events).
6. **The larger latency is held longer than 40 ms when two switches queue** (77 ms), and the
   report is stale for 500 ms after audio stops (row 6). [M]
7. **Classic EQ knob glide differs more from the old build than in round 2** (2.4 d). [M] Declared.

New attacks that found nothing more [M]:
- `mod_type` wait (`py/t3_modwait.py`, 110 ordered pairs x 10 signals x 2 knob sets at 48 kHz,
  time until the output leaves the render that never switches): held notes min 21.0, median 30.0
  to 39.3, 90th percentile 51.7 to 65.3, **max 156.8 ms** (Small Stone to Dimension, 440 Hz);
  above 100 ms in 37 of 1 100; none reached 250 ms. Switch in silence, then play: a note 5 ms
  after the switch is heard through the old effect for 21 to 84.5 ms (median 30), then the 40 ms
  crossfade; a note 60 ms or 200 ms after it is the new effect from its first sample. 0 non-finite.
- Thirteen Classic blocks toggled every block for 10 s (`py/t3_blockrate_classic.py`, up to
  390 013 events): 0 non-finite, peak not above steady, >5 kHz <= -64.2 dBFS, guard 0 / 0 / 0 / 0.
- `drive_type` at block rate in a split lane for 10 s (`py/t3_drivesplit.py`, five
  configurations): 0 non-finite, peak <= +0.5 dB over the largest steady peak, >5 kHz <= +0.5 dB
  over steady, dry lane step <= 1.016 x steady and <= -125.7 dBFS above 5 kHz, guard 0 / 0 / 0 / 0,
  output equals the static render 1 s after the last change (1.2e-7).

## 8. Not run

- The shipped AU (LTO, wrapper), `auval`, a real host: which order Logic prepares and restores
  in, what it does with latency reports during a switch, the wrapper's own cost.
- Automation through the AU wrapper itself (my stress thread stands in for it).
- Hosted FX slots; the 65 535 curve clamp.
- A longer ThreadSanitizer run with model swaps (item 10 not reproduced in 60 s).
- CPU on an idle machine. 176.4 kHz. Rates below 22.05 kHz.
- `tubamp_golden --params` against `tests/golden/params-v4.tsv` (the harness dump was compared
  with round 2's instead).

## 9. Files

`build-verify/r3-processor/`: `harness/` and `compat-harness/` (the adapted copies), `py/`
(round 2's scripts unchanged, plus `t3_align.py`, `t3_mod_probe.py`, `t3_wide.py`,
`t3_modwait.py`, `t3_blockrate_classic.py`, `t3_drivesplit.py`), `compat-run/` (rounds 1 and 2
unchanged, plus `matrix_r3.py`, `r3_prepfirst.py`, `r3_auto_size.py`), `logs/`, `work/*.json`,
`compat-run-work/matrix-*.json`. `rel/` and `hb-new/` are the release builds; the sanitizer
builds were deleted after use. No WAV was kept.
