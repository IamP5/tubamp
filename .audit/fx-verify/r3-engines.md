# Effect engines, round 3: regression pass on gate, compressor, drive, EQ, chorus, phaser, tremolo

Independent re-measurement at HEAD `2a280b9` of every numbered finding of the three round-2 reports
(`r2-dynamics.md`, `r2-tone.md`, `r2-mod.md`): their "partly" and "not fixed" round-1 rows and all "New findings".
The round-2 verifiers' own scripts were re-run; the engineers' check rows (`tools/fxbench/check_*.py`) were not run.
No tracked file was changed and no git state was changed.

Labels: **[M]** measured (script named; scripts and their outputs are under `build-verify/r3-engines/`),
**[L]** measured again by the lead with separate code (`build-verify/r3-engines/lead/`), **[I]** inferred, **[G]** guess.
Nothing was listened to: every statement about audibility is [I] or [G].

## Method

- **Engine**: `build/tubamp_fxbench_artefacts/Release/tubamp_fxbench` (built 19:13, newer than every engine source and
  adapter; the two later commits touch only `FxBlocks` and the processor, which the bench does not compile).
- **Processor**: the three verifier harnesses (`dynproc`, `procrun`, `modh`) rebuilt from HEAD sources in one CMake tree,
  `build-verify/r3-engines/hbuild` (sources copied to `r3-engines/harness/`, logs `hbuild-*.log`). They compiled unchanged.
- **Sanitizers**: `tubamp_fxbench` rebuilt at HEAD with ASan + UBSan in `r3-engines/asan`.
- **Scripts**: the round-2 directories were copied to `r3-engines/dyn`, `/tone`, `/mod` with only paths changed
  (bench, harness, scratch). Adaptations beyond that are listed in the last section. Round-2 directories untouched.
- **Who measured**: seven sub-verifiers under the lead, one per engine (the tremolo one also took the mod block through
  the processor), each re-running the round-2 scripts for its engine and spending at most a quarter on new attacks.
  The lead then re-measured the worst findings with code that shares nothing with theirs: gate tail and latch
  (`lead_gate.py`, `lead_gate2.py`), chorus crossfade pitch (`lead_chorus.py`), CE channel hole, EQ off-centre overshoot,
  phaser switch blip (`lead_misc.py`), tremolo sync click (`lead_trem.py`), the chorus Classic null with a reset, and CPU.
  All agree with the sub-verifiers' numbers.
- **Reference**: Guitar Rig 6 rendered live as a black box through `auhost` with the round-2 states, Rack gate off.
- **Controls that the measures can still fail**: `t_dswitch.py` on the pre-fix engine still reads +19.0 / +18.8 / -9.9 dB;
  the round-2 `modh` with the same tremolo script reads -44.9 dBFS; Clean / Tight against `--legacy` differ in
  374 390 / 159 934 samples; a re-prepare is caught by `--rt-check` (8 mallocs); the phaser re-enable measure reads
  -4.4 dBFS without the reset.

## Counts (round-2 open findings only)

| Engine | Fixed | Partly | Not | Regressed since round 2 / new |
|---|---|---|---|---|
| Gate | 6 | 2 | 2 | 3 new defects of the new closing rule (latch, decay tail, hum under a chord); G1 worse than round 2 |
| Compressor | 3 | 1 | 2 (both declared) | 1 small (dip with sc_hpf after a Classic switch) |
| Drive | 4 | 1 | 4 (2 declared limits, 2 unclaimed) | none audible; 2 small |
| EQ | 5 | 1 | 1 | 3 (off-centre overshoot, band-move clicks, switch-back corner) |
| Chorus | 4 | 1 | 0 | 2 round-1 defects back on the round-2 measure (C1, C2), 1 new (CE channel hole) |
| Phaser | 6 | 4 | 1 (declared) | 1 new (switch blip in antiphase) |
| Tremolo | 9 | 1 | 0 | none |

## Gate (`2df395d`)

| Finding | Round 2 | Now (worst case) | Status |
|---|---|---|---|
| N1 palm-mute gaps, floor 12 to 40 dB under the notes | 0 of 8 gaps closed (floor 18 dB under); processor 0 of 8 | 8 of 8 in all 39 Tight cells (floors 14 to 40 dB, gaps 75 to 250 ms, hold 0 / 6), worst gap -80 dB; same at 44.1 / 96 kHz; processor 8 of 8. Smooth -10.0 dB in the gap (was 0) [M `g9_tight_floor.py`, `p1_processor.py`, `r3_gate_vary.py`] | **FIXED** |
| N2 hang after a soft note is stopped | 418 to 430 ms; processor 426 | Note at or over the threshold when stopped: 28.9 to 47.5 ms (processor 32 to 39). **Note already 2 to 6 dB under the threshold (inside the hysteresis band) when stopped: 173 to 1024 ms at unity**; round 2's own `g10` row 417 -> 901 ms; processor HEAD 295 to 1023 ms, pre-fix 25 to 33 ms [M `g2_hang.py`, `g10_claims.py`, `r3_gate_decay.py band`, `r3_gate_band_proc.py`] | **PARTLY** (worse inside the band) |
| N3 hold knob one for one | hold 100 read 210 ms; floor 18 dB under: 357 to 412 ms whatever the knob | 12.0 ms + knob exactly at four floor depths, hiss floors 16 to 60 dB under, 44.1 / 48 / 96 kHz; release 4.98 / 29.90 / 99.65 / 498.4 ms. Inside the band hold 50 gives 394 to 927 ms (N2) [M `g7_knobs.py`, `r3_gate_more.py 1`] | **FIXED** for stops from above the open level |
| N4 chatter behind a louder note | 22 of 104; +24 dB / 0.1 s: 12 of 16 | 0 of 104; 0 of 400 in the sweep; 0 of 16 at 44.1 and 96 kHz [M `g3_chatter2.py`, `g3b_sweep.py`] | **FIXED** |
| G1 chatter on chords and beating notes | 1 of 382 (one 50.8 ms mute); round-1 set through the processor 0 of 112 dirty | **5 of 382** (E2+F2, one dip each, -32.8 dB, 57 ms). **Round-1 set: 12 of 112 dirty** (AM depth 0.5 to 0.7 at 3 / 7 Hz, unison +6 Hz, E2+F2), up to 11 dips per decay, -27 to -36 dB, 34 to 143 ms, none to mute. Hold 20 leaves up to 8 dips, hold 50 leaves 2 to 4 [M `g1_chatter.py`, `r1/gb.py`, `r3_gate_gbdepth.py`] | **PARTLY**, worse than round 2 (partial dips, not mutes; round 1 had 22 dirty) |
| N5 Smooth on a moving floor | 3.6 dB in one sample, 12.4 dB per 10 ms (+5 dB step); 19.0 dB (AM +-7); crackle 11.3 dB per sample | at most 0.002 dB per sample; per 10 ms 0.59 (+5), 0.83 (+8), 0.90 (AM +-7), 1.00 (crackle); same at 44.1 / 96 kHz [M `g6_smooth.py`] | **FIXED** |
| N7 opening click on swells | -35.0 dB re note (82 Hz swell), -40.4 (41 Hz) | sine swells -72.9 to -76.4 dB; strings -38.7 to -74.9, never worse than the old ramp [M `g5_attack.py`] | **FIXED** |
| G3 pick attack | first ms -0.01 to -1.19 dB; processor -1.21 / -1.72 / -0.83 | identical; notes 6 to 12 dB over the threshold open 0.4 to 6.4 ms late (was 0.3 to 5.3) [M `g5_attack.py`, `r1/gc.py`] | holds |
| N12 range automation | 0.0121 per sample at block 512 | under 0.00005 at blocks 64 and 512 [M `g7_knobs.py`] | **FIXED** |
| G7 palm-mute gap depth, hand stops | 30 / 40 / 50 ms: -0.9 / -13.4 / -33.5 dB; palm (tau 6 ms) 50 ms -12.5, 60 ms -30.9 | -0.8 / -12.8 / -32.4 (worst -0.3 / -10.1 / -30.3); palm 50 ms -9.2 (worst -5.3), 60 ms -27.6 [M `g10_claims.py`, `g4_close.py`, `r1/gb3b.py`] | **NOT** (palm 3 dB worse) |
| N6 low notes against the 75 Hz sidechain filter | pure 41.2 Hz needs 20.7 dB over; dull E1 10.1 dB; decaying dull E1 shut 3.1 to 4.4 dB over the threshold | static figures unchanged. The decaying dull E1 now shuts 22 to 33 dB under the threshold, 2.8 s late; pure sines still shut early (E1 +2.9, B0 +12.9 dB) [M `g8_low.py`, `g11_more.py`, `r3_gate_more.py 2`] | **NOT** |
| G2, G4, G5, G6, close times, knob moves | fixed in round 2 | unchanged: 0.002 dB per sample; non-finite diff 0; delay clamp 0 samples; meters 0 blocks; abrupt stops 27.8 to 28.0 ms, 0 reopenings in 1197 stops [M `g6_smooth.py`, `x1_robust_gate.py`, `g4_close.py`] | hold |

### The new closing rule under material neither round used [M `r3_gate_new.py`, `r3_gate_decay.py`, `r3_gate_latch.py`, `r3_gate_more.py`; L `lead_gate2.py`]

**Where a natural decay now holds the gate open on the floor.** Tight, defaults, threshold 8 dB over a steady hiss floor,
196 Hz tone; time from the moment the note's own level is under the floor until the gain is -20 dB:

| Decay (dB/s) | 1.5 | 2 | 3 | 5 | 10 | 20 | 40 | 80 | 110 to 160 |
|---|---|---|---|---|---|---|---|---|---|
| Floor heard (s) | 4.14 | 2.93 | 2.22 | 1.65 to 1.98 | 1.30 to 1.61 | 1.11 to 1.29 | 0.97 to 1.15 | 0.40 to 0.59 | 0.21 to 0.52 |

Counted from the close level: 5.47 s at 1.5 dB/s, 1.50 to 1.81 s at 10, 1.21 to 1.39 s at 20. There is no maximum: it grows
as the decay slows. Floor depth (16 to 50 dB under the note) changes it by about 20 %; strings and 44.1 / 96 kHz fall in the
same ranges. Threshold 12 dB over the floor: 0.81 to 0.95 s. Through the processor on hiss: HEAD 0.85 to 1.58 s, pre-fix
build 0.14 to 0.45 s. Lead, own signal (two partials, floor peak 10 dB under the threshold): 1.01 / 1.02 / 0.82 / 0.48 s at
10 / 20 / 40 / 80 dB/s, Smooth 0.95 / 1.01 / 0.77 / 0.37 s.

| Material | Result |
|---|---|
| Note bent or slid into silence | dying over 0.15 to 0.3 s closes like a stop (12 to 31 ms); over 0.6 s: 0.28 to 0.41 s |
| Tremolo-picked E5 chord, then stopped | no dips while picking. The stop closes in 48 ms with the threshold 20 dB under the stroke peak, 236 to 342 ms at 12 dB, **880 to 993 ms or not within 1 s at 3 to 8 dB** |
| Volume knob turned down | over 3 s: floor heard 1.09 to 1.22 s; over 1 s: 0.55 to 0.75 s |
| Floor that pulses (one mains spike per cycle over hiss; hum, buzz or hiss wobbling +-3.5 to 4 dB at 1.5 to 4 Hz), highest reading 8 to 12 dB under the threshold | **after any natural decay (20 or 60 dB/s, tone or chord) the gate stays at unity for all 30 s rendered, Tight and Smooth, 44.1 / 48 / 96 kHz.** The same floor alone never opens the gate; a hand stop closes in 16 ms. Processor: HEAD never closes in 8 s, pre-fix build 67 to 194 ms. Hiss with crackle: 11.5 s. Lead: hum wobbling +-4 dB at 1.5 Hz, floor peak 10 dB under: open to the end of the file (12.4 to 13.6 s) at 10 / 20 / 40 dB/s, closed after 0.28 s at 80 dB/s; hiss wobbling at 2 Hz: 10.9 s (Tight), to the end (Smooth) at 40 dB/s |
| Single-coil hum (50 / 60 Hz + harmonics) 10 to 30 dB under a decaying E5 chord, threshold 6 to 10 dB over the hum | open 1.3 to 4.2 s after the chord's own level is under the hum (worst: 50 Hz, 30 dB under, t60 6 s). Processor pre-fix: 0 s with 1 to 2 dips |
| Staccato, 180 BPM sixteenths (38 ms gaps), floor 15 dB under | Tight hold 0: 16 of 16 gaps closed, median -35 dB; hold 6: 14 to 16 of 16, -23 dB; hold 20: 0 of 16, -1 dB; Smooth: -8 dB |
| Threshold 3 to 4 dB over the hiss (round-1 chug, thr -50) | 75 / 100 ms gaps went from -25 / -69 dB median to **0 dB**; worst first-5-ms loss -7.4 dB (was -0.7) [M `r1/gb3b.py`]. With the threshold 1.6 dB over the hiss peak a stopped chord leaves the gate open to the end of the file (4.3 s; round-2 build 0.1 to 0.77 s) [M `r3_comp_gate_aside`] |

Cause of the latch [I, read in `Gate.cpp` 497 to 513 by the sub-verifier]: a 6 dB climb from the lowest level seen restarts
both the wait and the 1 s cap, so a floor that moves 6 dB keeps restarting it.

Huge samples: 48 of 48 cases finite, nothing latches; a 3e38 sample or the +3e38 / -3e38 pair gives output equal to the clean
render from the next sample [M `r3_gate_huge.py`]. ASan + UBSan: 216 runs, 0 reports.

## Compressor (`b4afe4b`)

| Finding | Round 2 | Now (worst case) | Status |
|---|---|---|---|
| N8 attack after a Classic -> new switch | +8.7 dB over both one-type renders (switch in silence, chord 5 ms later); +9.6 with an earlier note | no earlier note: +0.15 dB (gap 0, attack 100), 0.00 from 3 ms on, 0.2 ms steps around 12.5 ms, 44.1 / 96 kHz. **With a note 0.5 s earlier: +6.57 dB (frames), peak +6.99 dB, at any gap 0 to 300 ms**: the seed is Classic's reduction (0 dB) while the new type's own render still holds 9.3 dB; same in the round-2 build at gaps >= 15 ms [M `c2b_switch_silence.py`, `r3_comp_c2b_detail.py`, `r3_comp_refill.py`] | **PARTLY** (the frozen window is gone, the history is not) |
| N9 meter in Classic crossfades | up to +9.4 dB over the reduction applied | trace against out / in per sample 0.000 dB; block meter = block maximum of the trace, 44.1 / 48 / 96 kHz, blocks 16 to 4096 [M `c6_meter.py`, `r3_comp_meter.py`] | **FIXED** |
| N9 at mix < 100 % | meter 15.0 dB, heard 4.6 (mix 50) | same 15.00 / 4.60; up to 12.5 dB over what is heard at mix 30. Now stated in `Compressor.h` as the convention | declared |
| N11 sc_hpf switched on off the grid | 14 of 240 `--reset-repeat` runs fail | 0 of 240; 0 of 1360 under toggling (blocks 3 to 4096, three rates, reset while on); switch-on at 16 grid phases 0.0001 dB spread (round-2 build 0.41 dB) [M `x4_rt.py`, `r3_comp_schpf.py`] | **FIXED** |
| N13 Classic meter for a 3e38 sample | non-finite, 1 case of 30 | finite in all 30, and for 3e38 / 1e30 / 1e7 / the pair in every type [M `x2_robust_comp.py`, `r3_comp_huge.py`] | **FIXED** |
| C5 Squash release while the input stays over the threshold | 53.4 ms at knob 150 (round-1 case); 41.3 ms (R 8, -4 -> -10 dBFS, knee 0) | 53.4 and 41.3 ms: unchanged, by decision. The table now in `Compressor.h` (knob 150: R 2 111 to 149 ms, R 4 75 to 149, R 8 53 to 127, R 20 38 to 127) is within 1.1 % at its own conditions (knee 6), 3.6 % at 96 kHz / 110 Hz. It is not a law of the knob: knee 0 -22 %, knee 12 +29 %, knob 30 +53 %, knob 300 -14 %, knob 1000 -23 % [M `c3_ballistics.py`, `r1/cb.py`, `r3_comp_c5.py`] | **NOT** changed; documented, table true only as stated |
| N10 auto makeup is a fixed gain | +0.00 at -10 dBFS; +3.1 to +9.5 at -20; -2.3 to -6.7 at -3 | identical; documented [M `c7_misc.py`] | **NOT** (declared) |
| C1, C2, C4, C6, LF distortion, controls, stereo link | fixed in round 2 | no number moved (C2 +0.84 dB, +2.65 / -3.06 at attack 0.1; 48.00 dB cap) [M `c1*.py`, `c2_switch.py`, `c3b`, `c4_lf.py`, `c5_controls.py`] | hold |

New, compressor:
1. **Dip after Classic -> new with the sidechain filter on** (regression of `b4afe4b`, small). Steady G3 tone, T -30, R 4,
   sc_hpf 150: gain 1.32 to 1.36 dB under both steady renders, more than 0.5 dB under for 55 to 81 ms, in 10 to 12 of 32
   switch phases; round-2 build 0.65 dB for 4 to 6 ms. E2: 1.52 to 1.61 dB for 26 to 34 ms. sc_hpf off: 0.00 [M `r3_comp_dip.py`].
   The restarted detector filter reads as an attack and release is blocked for the window [I].
2. Re-enable through the processor with attack 100, makeup 12 + auto on a held chord: new types peak at +18.1 to +20.0 dBFS,
   15 to 17 dB over the always-on render, more than 3 dB over for 172 to 236 ms (the reset state attacks from 0 dB of
   reduction). Present in the round-2 build too. With defaults the level never exceeds the dry level [M `r3_comp_proc2.py B`].
3. One loud finite sample at the engine (3e38): output finite, one sample passes at up to 4.0e6; within 0.09 dB of the clean
   render after 0.73 s (Squash) to more than 5 s (Opto). In the plugin the processor zeroes samples >= 1e6 first. A single
   +29.5 dBFS sample through the processor ducks FET / Opto by 28 dB and takes 3.3 / 4.4 s to recover (their slow stage)
   [M `r3_comp_huge.py`, `r3_comp_proc2.py D`].

## Drive (`0b891cb`)

| Finding | Round 2 | Now (worst case) | Status |
|---|---|---|---|
| N1 type switch level and dry leak, 30 ordered pairs, three level settings | +19.0 dB rms / +18.8 dB peak for 40 ms (Fuzz -> Treble, level -20); all 30 pairs over +7 dB; dip -9.9 dB at the starting points | round-2 conditions: bump +0.22 dB, peak +0.72, dip -0.01 (level -20); +0.04 / +1.27 / -0.03 (starting points); level +6 equals level -20 within 0.01 dB. 110 Hz stack over 540 switches (three levels, 44.1 / 48 / 96 kHz, blocks 37 / 64 / 512): dip -0.03, bump +0.24, peak +1.75 dB (Fuzz -> Distortion, 96 kHz / 512). Low E1 stack: -4.00 dB for about 2 ms in a 5 ms window (Boost -> Fuzz); with a one-period window -0.62 / +0.65 dB [M `t_dswitch.py`, `r3_drive_dswitch.py`, `r3_drive_dswitch_notes.py`] | **FIXED** |
| N5 DC out | Screamer -38.2 dBFS; Distortion -42.7 / -34.8 / -26.6 | settled: Screamer -234.5 (gain 36: -194.4), Distortion -203.5 / -210.2 / -194.7 dBFS; worst over notes and rates -177.3 (Distortion gain 36, level +12). Cost at 40 Hz -0.007 dB, B0 fundamental -0.012 dB [M `r3_drive_misc.py`, `r3_drive_dc.py`] | **FIXED** |
| N8 Screamer CPU with the gain automated | 2.40 to 2.45 % (budget 2.0), under load | 1.726 % (static 1.621); every type inside its budget while moving. Load average 2.6 [L `cpu2/t_cpu.txt`] | **FIXED** |
| E4 (drive part) Classic drive re-enable through the processor | 0.0 dBFS into silence, step +27.1 dB | -225.6 dBFS, step 0.0 dB, overshoot +0.2 dB; varied (96 kHz, 61 Hz, -6 dBFS, gain 33) -216.6 dBFS [M `r3_drive_reenable.py`, `r3_drive_reenable_var.py`] | **FIXED** |
| D5 Fuzz asymmetry | H2 -50.0 dB re H1 | -50.0 (-49.4 to -60.2) [M `t_dresp.py`] | **PARTLY**, not moved |
| D6 aliasing off the grid (limit) | Screamer -47.2 dBc (-82.9 dBFS), loudest -65.2 dBFS; Fuzz -43.1 dBc; Distortion -69.0 dBc | identical to 0.1 dB [M `t_alias_hi.py`] | **NOT** (declared) |
| D7 Screamer at 10 kHz (limit) | -0.83 dB (48 / 96 kHz), -0.91 (44.1 kHz); -0.32 dB at 5 kHz | -0.83 / -0.83 / -0.91; -0.32 [M `t_dresp.py`] | **NOT** (declared) |
| N6 loudness at the starting points | Screamer +6.8 / +11.7 dB(A), Distortion +4.0 / +11.1, Fuzz +3.9 / +3.1; Fuzz +21.1 at -30 dBFS | same to 0.1 dB [M `t_dyn.py`] | **NOT** (not claimed) |
| N7 Fuzz sustain law | 15.8 / 9.0 / 4.4 / 2.6 / 0.7 / 0.2 dB | same [M `t_dyn.py`] | **NOT** (not claimed) |
| D1 to D4, latency | fixed in round 2 | zipper -125.0 and -100.7 dBc as before (Screamer gain moves rose from -134.7 to -125.4 dBc with the cheaper glide); 0 samples differ from the static render 4 s after a move in 150 runs; latency error 0.39 sample worst (was 0.36); 234 robustness runs clean [M `r3_drive_dzip*.py`, `r3_drive_glide.py`, `t_dlat.py`, `r3_drive_robust.py`] | hold |

New, drive (none rated audible [G]):
1. A pick that lands inside the 20 ms fade loses its leading edge: Treble -> Fuzz, -6 dBFS pluck 15 ms after the switch on a
   -32 dBFS note: 1 ms rms -31.6 dBFS where Treble alone gives -21.7 and Fuzz alone -16.2, for about 1.5 ms; 5 ms measure
   -8.8 dB (48 kHz), -17.4 dB for the extreme pair (Boost level -24 -> Fuzz gain 36 level +12). The 4 ms power estimate lags
   the attack [I] [M `r3_drive_swtrace2.py`].
2. Pitch blip from the latency slide on switches between types of different latency: 1.55 to 1.72 % for 10 ms at 44.1 / 48 kHz
   (3.4 to 3.7 % at 22.05 kHz, 0.1 to 0.3 % at 192 kHz), 1 kHz and 5 kHz sines; no comb [M `r3_drive_sw2.py D`].
3. The output coupling lengthens recovery from one hot sample: +12 dBFS sample, Distortion 13 -> 154 ms, Screamer 19 -> 97 ms
   to within 1e-3; 3e38: 215 to 441 ms at the engine (was 65 to 75), 0 ms through the processor [M `r3_drive_huge.py`].
4. A switch into a pedal starts it from reset: -18 dB (Fuzz) to -34 dB difference from an always-that-type render at 45 ms,
   -117 dB at 1 s, 0 samples after 4 s.

Held under attack: two and three requests inside 30 ms, a random type every 25 ms for 2 s, 22.05 to 192 kHz, blocks 1 to 4096
(40 runs): finite, dip at most -1.03 dB, final state bit-identical to the always-final render; lag after a switch 0.0000
samples; click above 3 kHz at most -101.6 dBFS; ASan 64 switch-storm runs, 0 reports.

## EQ (`44e54af`, `0cc04e2`)

| Finding | Round 2 | Now (worst case) | Status |
|---|---|---|---|
| N2 overshoot on fast Q / gain moves, note on the bell's centre | +11.6 dB (step), +8.5 (50 ms), +1.4 (0.3 s); guitar stack +10.0; cut +11.3 | +0.0 dB on every round-2 case at 44.1 / 48 / 96 kHz; Graphic +0.01 [M `t_eqover.py`, `r3_eq_over2.py`] | fixed on the centre |
| N2, note near but off the centre | not tested in round 2 (157ab02 build: +2.0) | **+5.39 dB**: bell 100 Hz Q 8, 90 Hz note, mid 0 -> 10 step; +5.22 over 10 ms, +3.81 over 50 ms, above +0.5 dB for about 120 ms; guitar stack at 82.41 Hz +3.43 (was +0.55); through the processor +5.38 [M `r3_eq_offcentre.py`]. Lead: +5.39 / +5.12 / +4.35 dB at 90 / 110 / 82.41 Hz against +2.01 / +1.91 / +0.80 on the 157ab02 build; 50 ms move +2.16 against +1.27; 0.00 on the centre; same at -24 dBFS [L `lead_misc.py`] | regressed |
| N2 mid-frequency jump with a +15 dB Q 8 bell on the note | -26.0 dBFS line | -26.0 [M `t_eqchirp.py`] | not fixed |
| **N2 overall** | | | **PARTLY** |
| N3 Classic mid across 5, four rates | 10 -> 0: -59.9 / -54.1 / -51.7 dBFS (48 / 96 / 192 kHz); 8 -> 2 -64.1; 7 -> 3 -67.5; 6 -> 4 -73.4; 0 -> 10 -68.6 | 10 -> 0: -86.4 (44.1 kHz, block 512), -88.4 at 48 / 96 / 192 kHz; 8 -> 2 -94.2; 7 -> 3 -102.0; 6 -> 4 at the floor; 0 -> 10 -90.6; processor EQ block and amp tone stack -88.4; varied (110 Hz at -6 dBFS, 10 ms ramp) -83.1 [M `r3_eq_mid4.py`, `t_eq192.py`, `t_proc_mid.py`] | **FIXED** |
| N4 type switch under a strong low boost | -54.8 dBFS (96 kHz), -60.9 (48 kHz); dip -9.1 dB | -109.5 / -105.6; 72 switches: -101.4, dip -0.0 dB; a note starting 0 to 120 ms after the switch +0.42 / -0.73 dB [M `t_eqswitch.py`, `r3_eq_small.py`, `r3_eq_p2b.py`] | **FIXED** |
| N9 Classic knob step at 192 kHz | -79.6 (block 512), -79.0 (64) | -88.4 / -89.3 [M `t_eq192.py`] | **FIXED** |
| N11 finite absurd input | 1e30: Classic above +12 dBFS for 173 ms | 1e6 to 3e38 and the pair: 0 ms, every type and the processor; 1000.0 passes, 1000.0001 is zeroed [M `r3_eq_huge.py`] | **FIXED** |
| E4 Classic EQ re-enable through the processor | +0.9 dBFS into silence, step +34.9 dB, click -2.5 dBFS | -226.5 dBFS, +0.0 dB, -68.8 dBFS; other types as before (-62.3 dBFS, guitar +2.9 dB) [M `r3_eq_reenable.py`] | **FIXED** |
| E13 Stack against the analog closed form | 0.36 dB to 5 kHz, 1.66 dB to 10 kHz (44.1 kHz); 13.0 dB above 10 kHz | identical after the rewrite [M `r3_eq_gs.py`] | **NOT** |
| E3 retarget, E5, E6, E2, E1 | fixed in round 2 | -86.8 dBFS (was -68.6); 0.27 dB; +10.000; 0 samples; 390 runs clean [M `r3_eq_retarget.py`, `r3_eq_resp.py`, `t_neutral.py`, `r3_eq_robust.py`] | hold |

### Are the 3.0 and 2.0 dB overshoot bars the filters' own response? Yes. [M `r3_eq_ref.py`, model `r3_eq_reflib.py`]

Own reference: the analog filter as a time-varying state-space model with physical states, integrated with the trapezoidal
rule at 32x (cross-checked at 64x and against DOP853 / Radau to -141 / -155 dB), driven by the parameter trajectory the
engine applies. Bell: two-integrator loop, capacitor voltages as states, the frequency control scaling both integrator
rates (a ganged-pot sweepable mid; for a pure frequency move every such realisation gives the same output). Stack: the
passive circuit with its three capacitor voltages (matches the Yeh-Smith closed form to 4e-6 dB).
Overshoot in dB over the larger steady peak, engine / reference, at 44.1 | 48 | 96 kHz:

| Move | Note | Step | 10 ms | 50 ms |
|---|---|---|---|---|
| Bell Q 8, -15 dB, swept from the note to 8 kHz | 110 Hz | 2.90/2.90, 1.16/1.17, 1.20/1.21 | 2.43/2.43, 2.46/2.46, 2.74/2.74 | 1.44/1.45, 1.38/1.39, 0.68/0.68 |
| same | 196 Hz | 2.42/2.43, 1.68/1.68, 1.65/1.65 | 1.67/1.67, 1.72/1.72, 2.19/2.19 | 1.79/1.79, 1.82/1.82, 1.94/1.94 |
| same, bell starts at 100 Hz | 82.4 Hz | 0.91/0.91, 1.78/1.78, 1.78/1.78 | 0.51/0.52, 0.50/0.51, 0.57/0.57 | 0.79/0.79, 0.76/0.76, 0.35/0.36 |
| same, bell starts at 100 Hz | 41.2 Hz | 0.00/0.00 | 0.00/0.00 | 0.00/0.00 |
| Stack bass 0 -> 10, worst voice | 41.2 Hz | 0.62/0.63, 1.36/1.36, 1.36/1.36 | 1.59/1.59, 1.58/1.58, 1.50/1.50 | 0.17/0.17 |
| same | 110 Hz | 0.49/0.49, 0.93/0.93, 0.92/0.92 | 0.47/0.47, 0.47/0.47, 0.46/0.46 | 0.14/0.14 |
| same, 48 kHz only | 82.4 / 196 Hz | 1.14/1.13; 2.02/2.02 | 0.77/0.77; 2.00/2.00 | 0.05/0.05; 1.73/1.73 |
| Stack mid 10 -> 0, 192 moves | all | at most 0.02/0.02 | | |

Engine equals reference within 0.011 dB over 162 bell moves and 0.004 dB over 576 Stack moves; 192 kHz spot checks agree to
0.001 dB; waveform null -40.3 dB (bell step, 44.1 kHz) and -46.9 dB (Stack) re the steady level, -72 to -95 dB on 50 ms
moves. The claim holds. Two footnotes: the engineers' +2.46 dB is one note phase (over phase it is +2.92, still under the
3.0 bar), and the Stack reads +2.02 dB at 196 Hz Blackface, at the 2.0 bar; of that, 1.60 dB is the static response on the
way and only 0.42 dB is transient. No reference was built for gain and Q moves (there is no single analog realisation), so
the off-centre finding rests on the before / after comparison.

New, EQ:
1. **Bell gain moved fast with the note near the centre: +5.39 dB** (table row above). The new feed (k x into the core) keeps
   the states right at resonance only; off it they scale with k [I]. The engineers' row E25a tests band-centre notes only.
2. **Clicks when a band above the note moves** (same cause [I]): Graphic g3k2 12 -> -12 with the other bands at +12 under a
   98 Hz note: -57.6 dBFS above 1 kHz (round 2: -93.7); Parametric Q 0.5 -> 8 on a +15 dB bell at 1 kHz under a 41.2 Hz note
   at -3 dBFS: -59.3 dBFS at 96 kHz (157ab02: -99.6). In the 2575-case zipper set 132 Graphic and 39 Parametric cases are more
   than 6 dB worse than in round 2; the worst lines are unchanged (Parametric -15.2, Stack -72.4, Graphic -70.2 dBFS; Classic
   -75.1 -> -90.7) [M `r3_eq_zip.py`, `r3_eq_follow.py`].
3. **Type switch asked back while the fade runs** (52 to 88 ms after the first): -45.6 dBFS above 1 kHz (44.1 kHz, Classic <->
   Parametric, 82.41 Hz note) against -100.6 for a single switch; the fade reverses with a corner [I] [M `r3_eq_turn.py`].
4. Switch gain: with the low note out of phase between the two types an in-phase 3 kHz partial rises +6.0 dB mid-fade;
   Parametric low cut 400 -> Classic bass 10 under a 98 Hz note dips 10.2 dB under the quieter type for 5 ms (the gain clamps
   at +6 dB) [M `r3_eq_p2b.py gain`].
5. Classic knob steps below about 0.003 (engine API only; the plugin's knobs step by 0.1 and read -90.3 dBFS): bass 10 ->
   9.9999 under a low E: -46.2 dBFS at 192 kHz, -69.4 at 48 kHz; toggled every block -32.6 dBFS at 192 kHz [M `r3_eq_tiny.py`].
6. Classic bass 0 -> 10 with treble at 10: -73.3 dBFS (192 kHz), -75.5 (48 kHz); the claimed -86.4 holds only with the other
   knobs at 5 [M `r3_eq_follow.py`].
7. At the +60 dBFS limit samples are zeroed one by one: a +60.01 dBFS sine comes out with holes. +24, +50 and +60.0 dBFS
   pass untouched.

## Chorus (`baaaf1f`)

| Finding | Round 2 | Now (worst case) | Status |
|---|---|---|---|
| N1 sync under tempo steps, ramps, position jitter | 1-BPM steps, block 512: 39 dips in 40 (1/32), 32 (1/4); ramp at block 4096: 47 / 34; jitter +-2 samples: 200 dips in 9.5 s, 41 dB deep, -1.3 dB mean; +-4: 462, -4.1 dB | 0 dips in every case including +-16 samples, varying blocks, loops, 60 s runs. Noise at mix 1, 72 cases: lowest 2 ms frame within 0.5 dB of the steady control, mean within 0.03 dB, 0 jumps, 0 crossfades [M `ch2_jitter.py`, `ch2_jitter2.py`, `ch2_loop.py`, `r3_ch_n1.py`] | **FIXED** |
| N7 the dip at mix 1 | -26 dB (tone) to -38 dB (noise), 6 to 7 ms under -10 dB; mix 0.5 -2.5 dB; click -84.6 dBFS; eight hand moves: Wide 4 to 5 dips, Tri 1 to 2, CE / Dimension 0 | Broadband (24 noise seeds, 2 ms frames): Wide -1.17 dB (steady spread -1.35); CE rate 1 -> 10 at mix 1 -2.72 dB; mix 0.5 at most -1.40. Soloed held notes 82 to 1500 Hz: -1.16 dB. Click -86.9 dBFS. **Unsoloed CE on a song-position jump: one channel -32 dB** (below). Eight hand moves now start 10 to 11 crossfades in Wide, 4 to 6 in Tri, 0 in CE / Dimension [M `r3_ch_dip_aud.py`, `r3_ch_xf_tone.py`, `r3_ch_dips_auto.py`] | **PARTLY** |
| N12 slope-limit lag | settle 220 ms; ramp lag 0.45 s; rapid mode switching sags 5.4 dB | settle at most 140 ms; ramp lag 210 ms; rapid switching -1.2 / +1.7 dB at mix 1 [M `ch2_lag.py`, `ch2_switch2.py`] | **FIXED** |
| C3 / N13 re-enable, Classic included | Classic bursts -5.0 / -7.3 dBFS | all five modes -346 dBFS (12 runs each) [M `ch2_misc.py`, `r3_ch_reen.py`] | **FIXED** |
| N14 huge finite sample | the +3e38 / -3e38 pair latched Classic for good | 0 failures in 600; 3e38 / 1e37 / 1e30 / 1e7 in every mode through the processor: finite, level back within 1.5 s [M `ch2_nonfinite.py`, `r3_ch_x_huge.py`] | **FIXED** |
| C1 pitch per tap on rate and depth changes (the round-1 defect, fixed in round 2) | +7.5 cents over the steady peak worst; steps and sweeps at most +5.0 | **per soloed voice at 48 kHz: +76.9 cents at 997 Hz (Wide rate toggle), one step +76.3 (Wide depth 0 -> 1, -107 cents for 28 ms); +237 at 331.7 Hz, +538 at 110 Hz, +601 at 82.4 Hz.** The tap delays themselves stay inside the bound (34.6 cents against steady 30.5): the excursion is the 30 ms crossfade between two taps, a phase slide of up to half a cycle, about 17 to 40 Hz whatever the note. Whole output, one knob step at mix 0.35: CE +80 cents for 16 ms (220 Hz), Dimension +177 for 16 ms, Tri +186 for 19 ms (331.7 Hz) [M `r3_ch_pitch.py`, `r3_ch_tapslope.py`, `r3_ch_c1_full.py`]. Lead: Wide soloed, depth 0.4 -> 0.8: -338 cents for 31 ms at 82.4 Hz, +63 at 220 Hz, -94 at 340 Hz, +25 at 997 Hz (steady 19 to 22); whole output at mix 0.5, Tri: -48 cents for 29 ms at 110 Hz, -80 for 29 ms at 220 Hz (steady 5.5 / 13.1); CE -12 cents at 110 Hz [L `lead_chorus.py`] | **REGRESSED** on the round-2 measure (short: about 30 ms; round 1 was -1223 cents for 1.9 s) |
| C2 pitch on sync start, locate, loop | +4.5 cents over steady | soloed: +75.6 cents (Wide, locate every 15 ms, 997 Hz); single events Tri start +61, Wide locate +54, about 25 ms each. Processor CE: -353 cents on a locate at 1/4 (the channel hole) [M `ch2_sync.py`, `r3_ch_sync_look.py`] | **REGRESSED** (same cause) |
| C4 mode switches | within +-0.6 dB; pairs with Classic dip 2.6 dB for 10 ms; click -98.5 dBFS | +0.73 / -0.29 dB (Classic -> Tri, mix 1); Classic pairs -0.5 dB; click modes 1 to 4 -103.6, with Classic -86.4 dBFS [M `ch2_switch2.py`] | **FIXED** |
| C5 mono, C6, C7 | fixed in round 2 | Dimension 3.47 to 3.56 cents in mono, -148.1 dB against stereo left; levels and image as round 2; 0 non-finite in 950; `mod_type` tick -114.8 dBFS (Classic -85.6) [M `ch2_misc.py`, `r3_ch_x_spot.py`] | hold |

New, chorus:
1. **CE: a song-position jump (loop wrap, locate) blanks one channel on a held note.** Mix 1, depth 0.8, 750 Hz: right
   channel -32.1 dB while the left stays at 0 to +3 dB; 14 ms under -3 dB, 5.6 ms under -10 dB per event. 17 of 46 notes
   between 150 and 1500 Hz go deeper than -3 dB, 7 deeper than -10 dB; the same at 44.1 and 96 kHz, at -30 dBFS, and through
   the processor (-19 / -28 dB). Soloed, each voice is clean; pink noise shows nothing [M `r3_ch_xf_chan.py`].
   Lead: 750 Hz right channel -32.1 dB (48 kHz) and -28.1 dB (44.1 kHz) under the steady minimum, left +0.0; 440 Hz left
   -2.7 dB; 220 and 1000 Hz at most -1.4 dB [L `lead_misc.py`].
   Reproduce: `tubamp_fxbench chorus gen:sine:750:-12 o.wav mode=2 mix=1 depth=0.8 host_bpm=120 host_beats=4 host_loop_end=4 host_loop_len=2.63 --seconds=3`, right channel at 2.025 s.
2. **Crossfades fire during automation**, where round 2 had no dip during a ramp: Wide depth 0.3 -> 0.7 over 2 s starts 7,
   rate 0.05 -> 10 over 10 s starts 17 (Tri 15). A move every 40 ms gives 24 per second: soloed voice +214 cents, noise
   -0.6 / +2.1 dB beyond the steady spread, L/R +-2.5 dB. Guitar pluck, 20 ms frames: at most -2.55 dB under both steady
   renders (Wide, mix 0.5) [M `r3_ch_dips_auto.py`, `r3_ch_xf_attack.py`].
3. Song position noisier than the jump limit (alternating +-132 samples per block) makes every block a jump (7499 in 10 s)
   and freezes the modulation on the outgoing tap: Wide at 2 Hz swings +-3.0 cents instead of +-10.9 [M `r3_ch_overlimit.py`].
4. Classic after a `reset()` no longer equals the legacy code: `--classic-null` with `reset@2.5=1` fails in 18 of 612 runs
   (round 2: 0); lead: 192 056 of 576 000 samples differ from the reset on, largest difference 0.104. Static Classic and
   Classic through on/off are bit-identical. This is the cost of the re-enable fix (C3) [I].

Sync lock held under attack: host 0.1 / 1 / 3 % off the engine's rate, tempo ramps 60 -> 200 BPM in 2 s and 20 s at blocks
16 / 64 / 4096, jitter up to 0.45 of the limit, 1/1 at 40 BPM and 1/32 at 150 BPM, 44.1 to 192 kHz: 0 jumps, detune at most
+3.9 cents (block 4096), error at most 0.028 cycle during a ramp and 0.0002 after [M `r3_ch_lock.py`].

## Phaser (`8f8f197`)

| Finding | Round 2 | Now (worst case) | Status |
|---|---|---|---|
| N2 Small Stone Color off against Guitar Rig 6 | null -10.8 to -12.1 dB; pitch p99 at 196 Hz ours 3 / 18 / 71 cents, GR6 14 / 58 / 144 | null -40.3 dB (0.32 Hz) to -32.2 dB (5 Hz, a rate the fit never saw); 196 Hz ours 13.8 / 64.8 / 142.9 against 13.7 / 58.2 / 143.9; worst note +11.3 % [M `p2_null.py`, `r3_ph_pitch.py`] | **FIXED** |
| N3 / P2 sync jump glide against block size | -31.1 / -24.7 / -23.9 dBFS at blocks >= 512; settle 9 ms to 850 ms depending on the block | Stone on -85.2 / -81.1 / -80.0 dBFS on the same probes; Stone off worst -78.7; identical at blocks 16 to 4096 and at 96 kHz; settle Stone off 9 to 11 ms, Stone on 37 to 47 ms; 1/1 -> 1/4 -82.2 (was -31.7); transport start -82.5 (was -47.6) [M `p2_syncjump2.py`, `p2_syncjump3.py`, `p2_syncevents.py`] | **FIXED** |
| N6 model and Color switches | +11.5 dB above / 20.3 dB below both steady levels for 48 to 65 ms; click -62.2 dBFS | above the span of both voicings at most 0.01 dB; broadband +1.9 / -1.5 dB; click above 2 kHz -101.4 dBFS. **A blip remains mid-fade when the two voicings are near antiphase**: envelope up to 26.4 dB under anything either voicing reaches (Stone off -> on, 368 Hz) for at most 5.2 ms; 19 of 1200 switches dip more than 6 dB; off the note -25.2 dBFS for a -12 dBFS note (Classic -> Stone on, 256 Hz), 101 of 720 switches above -60 dBFS [M `p2_switch.py`, `p2_switch2.py`, `r3_ph_swdip.py`, `r3_ph_swtick.py`]. Lead, 12 switch times per pair: -14.0 dB (Classic -> Stone on, 368 Hz, 3 of 12 deeper than 6 dB), -10.8 dB (Stone off -> on, 368 Hz), 0.0 dB at 196 Hz [L `lead_misc.py`] | **PARTLY** |
| P6 / N8 level against bypass per register | pink +1.21 / -0.15 / +1.08 / +0.29 dB (script / block / Stone off / on); Stone on guitar +1.6 / +2.9 / -2.2 | pink +0.89 / +0.72 / -0.56 / -0.39; Stone on guitar +0.9 / +2.2 / -2.9 (5.0 dB spread, was 5.1); the three-register mean is within +-0.17 dB [M `p2_level2.py`] | **PARTLY** (re-centred, spread unchanged) |
| P5 peak headroom | 82.4 Hz full-scale square, Color on: 5.15 (+14.2 dB) | 4.77 (+13.6 dB); 110 Hz 4.45; guitar take 1.24. Other voicings on full-scale squares: block +9.9 dB (660 Hz), Stone off +9.1, script +7.6. Takes peaking at -1 dBFS: Stone on +2.37 dBFS (258 samples over), Stone off +0.68, script +0.66 [M `p2_level2.py`, `r3_ph_peak.py`] | **NOT** (declared) |
| P4 second notch | block +4.6 %; Stone off 4.1 % rms, +5.0 / +6.9 %; Stone on 0.41 %, +-1.0 % | block unchanged (1.63 to 2.17 % rms, +4.63 %); Stone off 1.44 to 1.54 % rms, worst still +4.5 %; Stone on 0.53 to 0.57 % rms, +2.3 % (worse) [M `p2_gr6snap.py`, `p2_gr6.py`] | **PARTLY** |
| N16 / D-30 | wording wrong for three voicings; 44.1 kHz not stated | 48 minus 192 kHz: script 1.70 dB at 10 kHz, block 1.42 at 8.0 kHz, Stone off 1.02 at 7.4 kHz, Stone on 1.66 at 3.49 kHz; 44.1 kHz: 2.06 / 1.70 / 1.24 / 2.00. The documents now say this. Null 48 against 192 kHz: -43.7 / -39.9 / -41.6 / -39.5 dB [M `p2_frozen.py`, `p2_d30.py`] | **FIXED** (documents; the difference itself stays) |
| P3 / N13 re-enable, Classic included | Classic burst -11.3 dBFS, click -18.7 | burst -364.8 dBFS in all five models; click at most -89.9 (Classic -103.7) [M `p2_reenable.py`] | **FIXED** |
| N14 huge finite sample | the pair latched Classic | 3e38, 1e30, 1e7, the pair: 0 non-finite, engine equals the clean render within 10 to 30 ms, no latch; 9e5 passes linearly [M `r3_ph_huge.py`] | **FIXED** |
| P1, P7 | fixed in round 2 | notches across rates at most 0.166 %; non-finite clean [M `p2_frozen.py`, `r3_ph_robust.py`] | hold |

### The three voicings against Guitar Rig 6, 48 kHz (round 2 in brackets) [M `p2_null.py`, `r3_ph_pitch.py`, `p2_gr6.py`, `p2_gr6snap.py`]

| | Phase 90 block vs Phaser Nine | Small Stone Color off vs Stoned Phaser | Small Stone Color on vs Stoned Phaser |
|---|---|---|---|
| Waveform null on a guitar take | -25.9 to -27.6 dB (same) | -40.3 to -32.2 dB (-10.8 to -12.1) | -35.8 to -31.7 dB; -24.4 at 5 Hz (-29.2 to -31.2; -23.9) |
| Pitch deviation p99, ours against GR6, worst of four notes, per rate | 0.25 Hz about 1 cent both; 1.30 Hz +3.9 %; 3.72 Hz +2.6 % | 0.32 Hz -3.9 %; 1.78 Hz +11.3 % (196 Hz); 5.0 Hz -3.7 % | 0.32 Hz +0.7 %; 1.78 Hz -2.6 %; 5.0 Hz +43.5 % at 82.4 Hz (597 against 416 cents, measure-limited), other notes within -3.9 % |
| Notch 1 trajectory, rms / worst | 0.41 to 1.70 % / -3.03 % (same) | 0.12 to 0.17 % / +0.83 % (1.19 / -3.6) | 0.31 to 0.32 % / -0.53 % (0.82 / -1.4) |
| Notch 2 trajectory, rms / worst | 1.63 to 2.17 % / +4.63 % (same) | 1.44 to 1.54 % / +4.5 % (4.1 / +5.0) | 0.53 to 0.57 % / +2.3 % (0.41 / +1.0) |
| Resonance peak, ours against GR6 | +5.86 against +8.93 dB (was +4.99) | +2.10 against +1.94 (was +2.44) | +6.15 against +6.84 (was +6.85) |
| Response between notches, rms / worst | 1.01 dB / +2.67 at 5.5 kHz (same) | 0.48 / +2.85 at 10 kHz (0.62 / +3.0) | 0.43 / +2.29 at 3.4 kHz, sweep top (0.37 / +1.48) |

Phase 90 block was not refitted: every trajectory number equals round 2 and only its make-up changed (the commit says
"confirmed"). Its long-term level is still +1.2 / +2.2 / +3.7 dB over GR6 at 10 / 12.7 / 16 kHz. Stone-on notch rows are for
rates up to 0.63 Hz (tracker-limited above, as in round 2). At 96 kHz the between-notch error is 0.25 dB rms (Stone off) and
0.12 dB (Stone on). The new 44.2 Hz high-pass on Small Stone's wet path matches GR6 on B0 / E1 / E2 within 0.04 dB and 0.4
degrees (-1.11 / -0.11 / +1.17 dB against bypass) [M `r3_ph_low.py`].

New, phaser:
1. The switch blip in antiphase (N6 row).
2. Small Stone Color on at 48 kHz is slightly further from GR6 between the notches (+2.29 dB at the resonance, notch 2 +2.3 %,
   peak 0.7 dB low) although its null improved by 4 dB.
3. A model or Color switch restarts the free-running sweep at phase 0 (0.65 instead of 0.95 half a second later; inline probe).
4. The sync lock never corrects an error that builds slowly: host position 5 % off the reported rate gives 0.32 cycle after
   8 s with 0 jumps (up to 3 % it holds, 0.009 cycle); synced rates above the 20 Hz clamp run up to half a cycle off, and at
   block 4096 glide every block (70 jumps, tick +4 dB over the floor) [M `r3_ph_sync.py`].
5. Phase 90 takes 63 to 201 ms to come within -40 dB of a new sync phase (no tick; the "one glide of 10 ms" holds for Small Stone).
6. A second jump 2 to 5 ms into a glide: Stone off -70.7 to -71.9 dBFS; a 94-jump scrub -69.5 dBFS [M `r3_ph_jump2.py`].

## Tremolo (`26148a8`)

| Finding | Round 2 | Now (worst case) | Status |
|---|---|---|---|
| N4 engaging sync / changing the note value at blocks 64 to 256 | Smooth -44.9 dBFS (110 Hz at -12 dBFS), -37.4 at 0 dBFS; Harmonic -47.5 / -38.9; Bias -57.0; 1/4 -> 1/16 -47.8 | blocks 16 to 1024 at 48 kHz: Smooth -117.4, Harmonic -103.2 (its steady -107.6), Bias -122.8; 1/4 -> 1/16 -108.8. Over 44.1 to 192 kHz, blocks 64 to 512, 1400 renders: Smooth -107.7 (-12 dBFS) / -99.0 (0 dBFS), Harmonic -98.1 / -87.9 [M `x_recheck_tr.py`, `tr2_b1b_engage.py`, `tr2_b1_sync.py`]. Lead, bench play head, 110 / 196 / 82.4 Hz at -12 / -6 / 0 dBFS, 48 / 44.1 / 96 kHz, blocks 64 / 128 / 256: at most -98.6 dBFS (Smooth, 0 dBFS, steady -100.0) [L `lead_trem.py`] | **FIXED** |
| T3 locate / loop / transport start | at most -90.6 dBFS at 0 dBFS | at most -90.6 over 2106 jumps [M `tr2_a3_sync.py`] | **FIXED** (holds) |
| N5 voice switch | Optical -> Smooth +6.7 dB (depth 0.6) / +20.0 (depth 1); Smooth -> any +14; then a restart in the dip | against both trajectories +0.02 / -0.03 dB at depth 0.6, +0.05 / -0.77 at depth 1 (Optical -> Harmonic); 0 ms beyond 1 dB; no restart (within 0.22 dB of the new voice 45 ms later); click -106.0 dBFS, into Optical -58.8 (its lamp edge). On a tone above Harmonic's crossover (2917 Hz) Optical <-> Harmonic falls 0.8 to 2.2 dB under both [M `r3_tr_b5_switch.py`, `r3_tr_vswitch.py`] | **FIXED** |
| N9 / T4 pulse splatter, rate 1 to 20 Hz, duty 0.1 to 0.9 | 4 Hz -55.8 dBc (40 Hz carrier), -59.1 (1 kHz); 10 Hz -51.8; 20 Hz synced -48.8; duty 0.1 -42.0; tick -43.4 dBFS | -85.8, -90.1; -81.8; -78.8; -71.9. Whole grid, three carriers: worst **-65.4 dBc** (40 Hz carrier, 20 Hz, duty 0.1); tick -81.5 dBFS; same within 0.1 dB at 44.1 / 96 kHz. The 10 to 90 % time went 2.94 -> 2.98 ms; a 5 ms pulse (20 Hz, duty 0.1) reaches -0.28 dB of the crest; a 5 ms gap bottoms at -26.9 dB (was -29.7). Optical lamp-on -58.8 dBFS (unchanged, not claimed) [M `tr2_a4_splatter.py`, `r3_tr_pulse.py`] | **FIXED** |
| N10 switch-on starts in the dip | -11.9 dB (depth 0.6), -22.3 (depth 1) 20 ms after `mod_on` | 0.4 / 0.5 dB under the crest; with sync on it equals the beat-locked take within 1 dB after 21 ms. Harmonic's "crest" puts the band above its crossover in the dip: -9.0 dB at 2917 Hz [M `tr2_b2_reset.py`, `r3_tr_proc_p2.py crest`] | **FIXED** |
| N11 Bias onset dip and DC | -4.7 dB for 6.5 ms; DC 0.01 lowers the depth 14.4 -> 11.7 dB | onset -4.7 dB, 6.2 ms (kept, declared); DC 0 to +-0.5: 14.41 dB [M `tr2_b6_bias.py`] | **PARTLY** (DC fixed, onset by design) |
| N15 Smooth CPU at width 0.5 | 0.119 % (0.191 % with a fade always running), budget 0.10 | 0.064 % (0.116 % with a fade always running), load average 2.3 [L `cpu2/tr2_d3_cpu.txt`] | **FIXED** at rest |
| N13 Classic on/off | step 0.975, -17.3 dBFS | step 0.0031, at most -90.5 dBFS [M `tr2_a2_reenable.py`] | **FIXED** |
| N14 huge finite sample | non-finite output for the rest of the take (Smooth depth 1, Classic, Small Stone on); empty chain above +288 dBFS for over 3 s | 0 non-finite over 11 chains x 15 inputs through the processor and 5 voices x 15 at the engine; at or above 1e6 clean within 11.2 ms (tremolo 0.8 ms, empty chain 0 ms), no toggle needed. A 0.99e6 sample passes as audio: empty chain 602 ms, Bias 1640 ms to recover, no latch [M `tr2_a6b_huge.py`, `r3_tr_xhuge.py`] | **FIXED** |
| T1, T2, T5, T6, zipper, loudness | fixed in round 2 | mono 0 differing; re-enable burst -289 dBFS; depth to zero 800 of 800 exact; zipper -94.8 dBc; loudness within 0.2 dB of round 2 [M `tr2_a1_mono.py`, `tr2_a2_reenable.py`, `tr2_a5_depth0.py`, `tr2_b4_zipper.py`, `tr2_a7_loud.py`] | hold |

Sync lock and voice crossfade held under attack: jitter under the limit 0 jumps, error at most 2.05 ms; jitter at 1.1 to 5
times the limit up to 15 499 jumps, Smooth at most -96.5 dBFS, Harmonic -72.7; a second or third jump inside the fade at most
-77.9 dBFS at 0 dBFS; tempo ramp 60 -> 200 BPM in 2 s at block 4096: 11 jumps, -95.7 dBFS, lag 5.7 ms; switching voices every
17 / 30 / 50 ms within +0.00 / -0.13 dB of the voices' envelope [M `r3_tr_lock*.py`, `r3_tr_vswitch.py`]. Unchanged and never
listed: a loop that wraps on a pulse edge plays a +1.0 dB blip for 10 ms at every wrap [M `tr2_b1d_pulsewrap.py`].

## Through the processor (harnesses rebuilt at HEAD)

| Check | Round 2 | Now | Status |
|---|---|---|---|
| Re-enable, Classic gate / comp | Classic comp chord 5 ms after re-enable 7.7 dB under the always-on render; `comp_on` click -44 dBc | 0.41 dB; -79 dBc; no burst into silence (-104 dBFS) in any kind; Classic gate passes hiss for 18 ms after re-enable, never louder than dry [M `r3_comp_proc2.py`, `r1/cf.py`] | fixed |
| Re-enable, Classic drive / EQ | drive 0.0 dBFS into silence, step +27.1 dB; EQ +0.9 dBFS, step +34.9 dB, click -2.5 dBFS | drive -225.6 dBFS, step 0.0; EQ -226.5 dBFS, +0.0 dB, -68.8 dBFS | fixed |
| Re-enable, mod block, 15 kinds | Classic chorus -5.0 / -7.3 dBFS burst; phaser -11.3 burst, -18.7 click; tremolo -17.3 | into silence identical to the take left off; held-note click Classic chorus -83.3, phaser -89.7, tremolo -97.1 dBFS; others -81.6; Optical -46.9 (its steady -50.0) [M `r3_tr_proc_reenable.py`] | fixed |
| `mod_type` switch, tick above 2 kHz, mix 0.5 | -66.0 dBFS in / -63.6 out | default voicings -106.7 dBFS; into a Classic voicing -73.2 (tremolo -> Classic chorus, 440 Hz) [M `r3_tr_proc_type.py`] | fixed |
| `mod_type` switch, level in 5 ms frames | not measured | mix 0.5: +0.72 / -2.20 dB on held notes (Classic phaser -> tremolo, 82.4 Hz), guitar take +1.40 / -1.77. **Mix 1: up to 14.8 dB under both steady renders (Wide -> Phase 90, 110 Hz), envelope 20 to 37 dB deep for 5 to 7 ms, thump below half the note's frequency at -27.4 dBFS (steady -53.1); mix 0.8: 6.3 dB; Classic at mix 1: tick -62.3 dBFS.** The round-2 build read 72.9 dB for 16 to 31 ms [M `r3_tr_proc_mix1.py`] | partly |
| `mod_type` switch, delay before it is heard | 0 ms | median 32 ms, at most 92 ms (default voicings), 239 ms (Dimension / Stone / Harmonic); on a tone in antiphase 421 to 453 ms, then a 4.1 dB dip; never stuck in about 1700 switches | new behaviour, acceptable [G] |
| Non-finite input | 0 non-finite out | gate / comp: 0 non-finite, equal to the zeroed-input render from the first sample; drive NaN run 16.4 ms (Fuzz); EQ and mod kinds clean within 10.8 ms (single) and 167.7 ms (runs; empty chain 159.1) | holds |

## All engines

- **Classic bit-identity** (`--classic-null`, 44.1 / 48 / 96 kHz, blocks 1 / 37 / 512, mono and stereo, several knob settings
  and timelines): gate + compressor 414 runs, drive 108 + 60 random, EQ 126 + 60 (static settings), phaser 126, tremolo
  234 + 90, chorus 90 (lead settings) + 594 of 612: **0 differing samples**, except the 18 chorus runs with a `reset()` in the
  timeline (above) and, by design, Classic EQ during a knob move (up to -4.0 dBFS against the legacy during the move; mid and
  treble are back to bit-identity with an always-final render after 70 to 730 ms, bass not within 2.5 s in 12 of 18 runs at
  no more than -94 dBFS, which the legacy code does too between two renders with different history) [M `x3_null.py`,
  `r3_drive_null.py`, `r3_eq_null.py`, `r3_eq_classic_after.py`, `r3_ph_robust.py`, `tr2_c_classic.py`, `ch2_classic.py`].
- **`--rt-check`** on generated input under switches and automation: gate + compressor 1184 runs, drive 840, EQ 690, chorus
  540, phaser 170, tremolo 184: 0 allocations. With a WAV input (the tool defect of round 2): 480 dynamics runs, 0 failures.
  `modh --rt` through the processor reports 0 to 1 allocation in 12 of 24 runs, also with an empty chain and no events
  (round-2 build 2 of 24): process-wide hook, off the audio thread [I]; not attributed to an engine.
- **`--reset-repeat`**: every non-Classic run passes (dynamics 240 + 1360, drive 252, EQ 72, chorus 378, phaser, tremolo 184).
  Runs that start on a Classic model fail as the legacy reference fails (gate 60, drive 18 + 18, chorus 72, phaser 28).
- **Huge finite samples**: nothing latches and nothing goes non-finite in any engine or through the processor (rows above).
  The engines differ in where they cut: EQ at +60 dBFS (1000), chorus / phaser / tremolo and the processor at 1e6, the
  compressor passes one sample at up to 4.0e6 when driven without the processor.
- **ASan + UBSan at HEAD**: gate 216 runs, compressor 296, drive 64, EQ 164, chorus 40, phaser 30 + 90, tremolo 96: 0 reports.
- **CPU**, `--bench`, 48 kHz, block 64, stereo, pink noise at -18 dBFS, minimum of three calls of the tool's minimum of five,
  load average 2.1 to 3.7 on 12 cores (Logic and a browser idle; the first pass at load 8 to 58 was discarded) [L `cpu2/`]:

| % of real time | Now | Round 2 (under load) | Budget |
|---|---|---|---|
| Gate Classic / Tight / Smooth | 0.084 / 0.085 / 0.087 | 0.089 / 0.093 / 0.101 | 0.10 |
| Comp Classic / Clean / Squash / FET / Opto | 0.098 / 0.079 / 0.089 / 0.088 / 0.087 | 0.101 / 0.082 / 0.089 / 0.091 / 0.091 | 0.35 |
| Drive Classic / Screamer / Distortion / Fuzz / Boost / Treble | 0.192 / 1.589 / 1.572 / 2.767 / 0.021 / 0.021 | 0.216 / 1.734 / 1.740 / 3.003 / 0.027 / 0.024 | 0.35 / 2.0 / 3.5 / 4.0 / 0.05 |
| Drive, gain ramped for the whole run: Screamer / Distortion / Fuzz / Boost | 1.726 / 1.690 / 2.976 / 0.032 | 2.40 to 2.45 / 1.85 / 3.36 / n.a. | as above |
| EQ Classic 8/3/7 / Parametric all sections / Stack / Graphic all bands | 0.066 / 0.140 / 0.051 / 0.180 | 0.082 / 0.165 / 0.049 / 0.215 | 0.10 / 0.30 / 0.15 / 0.30 |
| EQ, ten seconds of ramps on every knob: Classic / Parametric / Stack / Graphic | 0.125 / 0.170 / 0.084 / 0.243 | 0.166 / 0.201 / 0.059 / 0.286 | 0.15 / 0.30 / 0.15 / 0.30 |
| EQ at 96 kHz: Parametric / Graphic | 0.285 / 0.360 | 0.336 / 0.442 | 0.30 (Graphic over) |
| Phaser Classic / script / block / Stone off / Stone on | 0.190 / 0.063 / 0.086 / 0.060 / 0.120 | 0.203 / 0.066 / 0.089 / 0.063 / 0.128 | |
| Phaser, model switched every 60 ms | 0.210 | 0.116 | |
| Chorus Classic / Wide / CE / Dimension / Tri | 0.078 / 0.097 / 0.072 / 0.061 / 0.099 | 0.084 / 0.084 / 0.068 / 0.058 / 0.092 | |
| Chorus, Wide depth moved every 0.1 s / mode switched every 60 ms | 0.145 / 0.157 | n.a. / 0.155 | |
| Tremolo Classic / Smooth w 0.5 / Optical / Bias / Harmonic | 0.014 / 0.064 / 0.027 / 0.045 / 0.053 | 0.018 / 0.119 / 0.028 / 0.053 / 0.066 | 0.10 (Smooth) |
| Tremolo Smooth w 0.5 with a jump fade always running; voice switched every 60 ms | 0.116; 0.078 | 0.191; n.a. | 0.10 |

  Chorus Wide and Tri cost about 15 % and 8 % more than in round 2 although the machine was quieter (the second tap set);
  the phaser costs 1.8 times as much while switching (two voicings run).

## Regressions and new findings, most audible first (ranking is [G]: nothing was listened to)

1. **Gate: stays open on a floor that moves after any natural decay.** Hum, buzz or hiss whose level wobbles 6 dB or more
   (or one mains spike per cycle over hiss) with the threshold 8 to 12 dB over it: after a note that rings out the gate never
   closes (30 s rendered; lead 12 to 14 s to the end of the file), in Tight and Smooth, and through the processor, where the
   pre-fix build closed in 67 to 194 ms. A gate that does not gate, on exactly the floors a single-coil guitar has.
2. **Gate: a note that rings out leaves the floor audible for 1.0 to 1.6 s at 10 to 20 dB/s, 2 to 5.5 s for slow decays**,
   1.3 to 4.2 s of hum under a ringing chord, and up to 1.02 s after a hand stop made from inside the hysteresis band
   (pre-fix build: 0.14 to 0.45 s, 0 s, 25 to 33 ms). Threshold set 3 to 4 dB over the hiss: 75 / 100 ms chug gaps no longer
   close at all (0 dB, were -25 / -69 dB).
3. **Chorus CE: one channel drops 32 dB for 6 to 14 ms on a held note at a loop wrap or locate** (mix 1; 17 of 46 notes
   deeper than 3 dB).
4. **Chorus: every tap crossfade slides one voice's phase**, a pitch blip of about 17 to 40 Hz for about 30 ms: -338 cents at
   82 Hz, +63 to -94 cents at 220 to 340 Hz per voice; -48 to -80 cents for 29 ms on the whole Tri output at mix 0.5. Fired by
   single knob steps, by locates, and now also 7 to 17 times during an automation ramp. On the round-2 measure C1 and C2 are
   back (+77 cents at 997 Hz against +7.5), though short where round 1 lasted seconds.
5. **`mod_type` switch with the mod mix at 0.8 to 1 on a held low note**: 5 to 7 ms null (14.8 dB in 5 ms frames) and a
   sub-bass thump at -27.4 dBFS; fine at mix 0.5.
6. **EQ: bell gain stepped with the note near the centre overshoots +5.39 dB** (before the fix +2.0); 50 ms move +2.2 to +3.8.
   Clicks of -57.6 to -59.3 dBFS when a band above the note moves (were -93.7 / -99.6).
7. **Gate: beating notes and close chords dip again**: 12 of 112 round-1 cells, 5 of 382 round-2 chords, -27 to -36 dB for
   34 to 143 ms, up to 11 dips in one decay; none to full mute.
8. **Phaser: model / Color switch blips for up to 5 ms when the voicings are near antiphase** (envelope -14 to -26 dB, energy
   off the note up to -25.2 dBFS); 19 of 1200 switches deeper than 6 dB.
9. **EQ: a type switch asked back within 52 to 88 ms**: -45.6 dBFS above 1 kHz.
10. **Compressor: Classic -> new with the sidechain filter on dips 1.3 to 1.6 dB for 26 to 81 ms**; Classic -> new after an
    earlier note still passes +6.6 dB (history, unchanged).
11. **Drive: a pick inside the 20 ms switch fade loses 9 to 17 dB of its first 1.5 to 5 ms**; a 1.6 % pitch slide for 10 ms
    when the two types differ in latency.
12. Phaser: slow sync drift (5 % rate error) is never pulled in; Phase 90 takes 63 to 201 ms to reach a new sync phase;
    Small Stone on is over full scale by 2.4 dB on a -1 dBFS take.
13. Chorus crossfade count under automation (7 to 17 per ramp), frozen modulation when the song position is noisier than the
    jump limit; Classic chorus after `reset()` no longer equals the legacy render.
14. Compressor re-enable with attack 100 and 12 dB of makeup: 15 to 17 dB over the always-on render for about 0.2 s (old).

## Statements in the commits that do not hold as written

- `2df395d` (gate): "a natural decay closes up to 1 s after it passes the close level": 1.2 to 5.5 s on hiss, up to 4.2 s on
  hum, never on a pulsing floor; the 1 s cap is restarted by a 6 dB rebound. "A fall within 50 ms is a stop at any floor
  depth" and "soft notes stopped by hand close in 29.5 to 47.5 ms": not for a note already under the open level (up to
  1024 ms). "Hold moves the wait one for one": only from above the open level. "12 + 5 cases dip partway": confirmed, and it
  is 12 of 112 and 5 of 382 where round 2 had 0 and 1. `Gate.h` "with 6 ms every 50 ms gap closes": not with a palm stop
  (-9.2 dB median).
- `b4afe4b` (compressor): "+0.94 dB over the louder one-type render": +6.57 dB with a note 0.5 s earlier. "While the window
  refills only attacks act": the restarted sidechain filter makes an attack neither render has. The C5 table "within 10 %":
  only at knee 6 and knob 150.
- `0b891cb` (drive): "dip -0.76 dB, bump +0.75 dB ... stack and low note": -4.0 to -7.4 dB on E1 / B0 in a 5 ms window (holds
  with a window of one period). "DC below -194 dBFS": -177.3 dBFS at level +12. Immaterial.
- `44e54af` (EQ): "worst gain / Q / ... move now +0.58 dB" and "states hold what the note leaves whatever k is": on the
  centre only (+5.39 dB off it). "E26 worst -86.4 dBFS": with the other knobs at 5; -73.3 with treble at 10. `0cc04e2`: the
  two bars are the filters' own response (confirmed), but the readings are +2.92 and +2.02 dB, not +2.46 and +1.68.
- `baaaf1f` (chorus): "taps <= +4.1 cents": the delay trace only; the audio of one voice reads +77 to +601 cents. "Soloed
  single notes -0.55 dB": -1.16 dB, and unsoloed CE loses 32 dB on one channel. "One crossfade each ... none in CE": a CE
  depth step starts one; eight hand moves start 10 to 11 in Wide. "reset() already clears Classic completely": the reset
  null changed and `--reset-repeat` still fails on Classic starts.
- `8f8f197` (phaser): "full-scale peak ... +5.9 dB others": +7.6 to +9.9 dB on full-scale squares. "P17 +0.95 / -2.88 dB":
  26 dB under both voicings in the envelope for up to 5 ms. "One glide of fixed length": Small Stone only. P14 Stone off
  "-37.3 dB": -32.2 dB at the unseen 5 Hz. "P15 <= -81.8 dBFS": -78.7. Phase 90 block was confirmed, not refitted.
- `26148a8` (tremolo): "outside both trajectories 0.03 dB": 0.77 dB through the processor and 2.17 dB at the engine for
  Optical -> Harmonic above the crossover. Sync click -114.4 / -100.7: worst -107.7 / -98.1 over notes and rates. All inaudible.
- `2a280b9` (processor): "at most 2.28 dB below": at mix 0.5 (2.20); 6.3 dB at mix 0.8, 14.8 dB at mix 1.

## What could not be run, and adaptations

- No listening, no recorded guitar or bass, no host (Logic), no `auval`, no Intel build. Whether a real host's song
  position is exact decides how often the sync findings occur.
- Guitar Rig 6 was rendered at 48 kHz (96 kHz for the between-notch rows); `p2_gr6snap` at 96 kHz and the diagnostic
  scripts of the old phase error (`p2_ssphase*`, `p2_ssimp`, `p2_period`, `p2_explore`) were not re-run. The script-logo
  Phase 90 still has no reference. Small Stone on above 0.63 Hz is covered by the null and the pitch rows only.
- `ch2_pitch.py` ran on a reduced grid (48 kHz: all 35 scenarios, blocks 64 / 512, 1540 renders; 192 kHz: 8 scenarios, 176
  renders; 44.1 / 96 kHz and the processor driver not run). The EQ reference ran its full grid at 48 kHz, two moves and two
  notes at 44.1 and 96 kHz, 11 spot checks at 192 kHz; no reference for gain and Q moves. `r3_tr_lock.py`'s 192 kHz / block 16
  slow-rate section was stopped.
- `p1_processor.py` was run once (by the gate verifier) and read by the compressor verifier. `r1/cd.py` stops at its line 50
  (`No module named 'gd'`) exactly as in round 2. `x4_rt.py`'s own positive control (`prepare_block=256`) no longer allocates;
  the `x4b` control and a WAV control still fail as they should.
- Gate re-enable through the processor was exercised on a hiss floor only (the held chord kept the gate open).
- Adaptations to the round-2 scripts: paths (bench, harness, scratch); worker pools reduced to 2 to 3; `r1/work/` created;
  effect-restricted copies of the shared scripts (`r3_drive_*`, `r3_eq_*`, `r3_ch_x_*`, `r3_ph_*`, `r3_tr_x*`);
  `r3_tr_b5_switch.py` widens the lag search from 60 ms to a full cycle because the new voice no longer restarts (unadapted
  it reads a false -13.5 dB); `r3_ch_dip_aud.py` windows T-10 to T+450 ms because there is no silent dip to centre on;
  `x_recheck_ch.py`'s tone-based dip counter reads 33 "dips" on a steady render too, so N1 and N7 were judged on noise;
  `p2_ssswap.py`'s weight-swap half is obsolete and its pitch half became `r3_ph_pitch.py` (three voicings, six notes);
  `mod/p2_null.txt` in the r3 copy holds the new table (round-2 copy in `mod/out/r2_p2_null.txt`).
- One rule breach by a sub-verifier: `tr2_d_rt.py` contains a `--bench` section that ran; its CPU numbers were discarded.
  All CPU figures in this report are the lead's.
- "Before" builds used read-only as controls: `build-verify/dynamics/hrel` (a84ca22), `build-verify/dynamics-r2` (round-2
  bench and `dynproc`), `build-verify/drive-eq` (157ab02), `build-verify/tone-r2/procrun`, `build-verify/mod-r2/modh`.
- Scratch left in `build-verify/r3-engines/`: 409 MB, of which 401 MB are the two build trees (`hbuild`, `asan`); no WAVs.

## Judgment

Five of the seven would pass as professional in steady playing and in most transitions now: the compressor, drive, tremolo
and phaser are there (the phaser's Small Stone now moves like the reference, -32 to -40 dB null), and the EQ is there except
for fast gain moves next to a narrow bell. The gate is not: the rule that cured the palm-mute and hold defects lets a
ringing note leave one to five seconds of floor and never closes on hum or buzz that wobbles, which a guitarist would call
broken; and the chorus traded its wet dips for a 30 ms pitch blip per knob step or locate and a one-channel dropout in CE,
which a careful player would hear on clean held notes.
