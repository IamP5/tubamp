# Dynamics verification, round 2: gate (`GateFx`) and compressor (`CompFx`) after the fix round

Independent, adversarial re-measurement of the fixes in `81fd404` (gate) and `a27b550` (compressor).
Tree: `claude/tubamp-effects-reverse-engineer-90c7f7` at `93b05cf`; `src/dsp/Gate.cpp`, `Compressor.cpp` and the
bench adapters are unchanged since those two commits. No tracked file was changed, no git state was changed.
Every number below is **measured** by me with the script named next to it unless the line says *inferred* or *guess*.

## Method

- **Engine level**: `build/tubamp_fxbench_artefacts/Release/tubamp_fxbench` (built 13:38, after both fix commits).
  Driven by my own library `build-verify/dynamics-r2/r2lib.py`: own WAV I/O (scipy), own signals, own analysis.
  Nothing is imported from `tools/` and none of the authors' check scripts was run.
- **Signals are not the team's**: a plucked-string model (inharmonic partials, pick-position comb, per-partial decay,
  two detuned polarisations, pick-noise burst), chords as strummed sums of strings, notes stopped by a hand
  (exponential damping, tau 2.5 to 6 ms) instead of 1 ms digital fades, hiss, pink noise, mains hum with harmonics,
  rectifier buzz, crackle, floors whose level drifts or jumps.
- **Gain is read from the audio**: gain = out / in per sample. The bench's `--trace` was first checked against it
  (`g0_sanity.py`: gate max difference 5.9e-8 linear; compressor 0.0000 dB in all five types) and only then used.
- **Processor level and A/B against the pre-fix code**: the round-1 harness `dynproc` rebuilt from HEAD in
  `build-verify/dynamics-r2/hrel`, run against the round-1 build of the pre-fix tree
  (`build-verify/dynamics/hrel`, commit `a84ca22`) on the same signals (`p1_processor.py`).
- **Round-1 regression set**: the round-1 verifier's scripts copied to `build-verify/dynamics-r2/r1/` with only the
  binary path changed (and `gate_sc_hpf` 90 -> 75, the new default), run through the HEAD processor
  (`r1/*_r2.txt`).
- **Sanitizers**: `tubamp_fxbench` built with ASan + UBSan in `build-verify/dynamics-r2/asan`.
- Controls that show the measurements can fail: Classic compressor 2.39 % at 82.4 Hz and 16.3 dB L/R gain
  difference; Classic meter 0.012 dB off at block 16; release 30 ms reads 0.25 % at 41.2 Hz where the default reads
  0.0000 %; a mid-render re-prepare is caught by `--rt-check` (8 mallocs); Clean against `--legacy` differs in
  374 390 samples where Classic differs in 0.
- Pitfalls hit and corrected: (1) a "+16.5 dB for a 12 dB makeup move" was my chord decaying, fixed by comparing with
  a fixed-makeup render; (2) "non-finite output on finite input" in the gate ASan run was the bench adapter's own
  mono sum of two 3e38 samples (deferred path), not the engine; (3) "switch in silence is 35 dB louder" compared
  against the new type only, corrected to the band between always-old and always-new renders; (4) `--rt-check`
  reported frees with a WAV input, see the tool note below.

## Round-1 defects: status

| # | Round-1 defect | Status | My number (worst case) | Script |
|---|---|---|---|---|
| G1 | Tight chatters on decaying chords and beating notes | **PARTLY** | Isolated chords: fixed. 16 chords x 8 thresholds x 2 decay rates plus 44.1 / 96 / 192 kHz, hold 0 / 6 / 250 and Smooth: 1 of 382 cases has a dropout (E2+F2, thr -60, one 50.8 ms mute). Round-1 set through the processor: 112 of 112 cells clean (was 22 dirty, up to 32 changes); AM beats and detuned pairs all clean. **Returns after a louder note**: a quiet decaying chord that starts 0.1 to 0.25 s after a note 20 dB or more louder chatters again: 22 of 104 cases, up to 5 dropouts, to full mute, 2 to 88 ms long; +24 dB / 0.1 s apart: 12 of 16 cases; none when the earlier note is 16 dB louder or less, or 0.6 s or more earlier. Hold 20 cures it, as before. | `g1_chatter.py`, `g3_chatter2.py`, `g3b_sweep.py`, `r1/gb2_r2.txt`, `r1/gb_r2.txt` |
| G2 | Smooth steps its gain on a noise floor | **FIXED** (stationary floors) | Hiss, pink, hum with harmonics, 50 / 60 Hz buzz, spike-per-cycle buzz with hiss, 44.1 / 48 / 96 kHz, release 5 to 500: largest one-sample step 0.002 dB, largest move in 10 ms 0.50 dB (was 7.7 dB per sample, 72 to 200 steps per second). A floor that rises is a new finding (N5). | `g6_smooth.py`, `g6b_buzz.py`, `r1/gd2_r2.txt` |
| G3 | Loss on the first millisecond of a pick | **FIXED**, one residual | Notes 20 dB or more over the threshold, 41 Hz to 1 kHz, 44.1 to 192 kHz: first millisecond -0.01 to -1.19 dB (was -8.7 to -11), gate at -1 dB 0.17 to 0.27 ms after the note starts. Round-1 plucks through the processor: first ms -1.21 / -1.72 / -0.83 dB at 82 / 196 / 1000 Hz; the click's own first peak is still -3.5 / -1.8 / -1.6 dB (was -6.8 / -4.4 / -2.9). Notes 6 to 12 dB over the threshold open 0.3 to 5.3 ms after they start (they cross the threshold that late; no lookahead). | `g5_attack.py`, `r1/gc_r2.txt` |
| G4 | Non-finite input (gate) | **FIXED** | NaN, +Inf, -Inf, 0.3 s of NaN, +Inf left with -Inf right, modes 0 / 1 / 2, blocks 1 / 37 / 512: the gain equals the gain for the same input with the bad samples zeroed (largest difference 6e-8, Classic), output identical 0.35 s later, meters finite. Processor: 0 non-finite output samples in four chains (pre-fix build: 72 000 to 144 000). | `x1_robust_gate.py`, `p1_processor.py` |
| G5 | Curve-delay clamp at 8192 | **FIXED** | Deferred render with D = 1 / 8192 / 8193 / 30 000 / 65 535 equals the undelayed render shifted by D, 0 differing samples; 70 000 and 200 000 clamp to 65 535; -5 to 0. | `x1_robust_gate.py` |
| G6 | Stale meters on oversized blocks | **FIXED** | 512-sample blocks on a 256-sample prepare: Tight and Smooth state, GR and level equal the normal-block render in all 282 blocks; Classic reads open / 0 dB as the contract now says. GR meter against the applied gain per block: 0.00 dB, blocks 64 / 512, three modes. | `x1_robust_gate.py`, `c6_meter.py` |
| G7 | Palm-mute gaps under 50 ms are not muted | **PARTLY** | With digitally abrupt note ends (the team's and round 1's signal) the claim holds: 30 / 40 / 50 ms gaps -11.0 / -31.0 / -51.0 dB (was -1 / -14 / -34). With a hand stop (tau 2.5 ms): -0.9 / -13.4 / -33.5 dB, worst gap -11.0 at 40 ms. With a palm (tau 6 ms): 50 ms -12.5 dB, 60 ms -30.9 dB. | `g10_claims.py`, `g4_close.py`, `r1/gb3b_r2.txt` |
| C1 | Makeup dead in Clean, Squash, FET, Opto | **FIXED** | 0 -> 12 dB while running: +12.000 dB in all five types, tone and chord, blocks 37 / 64 / 512; 24 -> 3: -21.00; a 0.5 s ramp to 7.5: +7.50; moved while bypassed: +12.00. | `c1_makeup.py`, `c1b_makeup_chord.py` |
| C2 | Type switch passes the uncompressed signal (+15 dB, 40 ms) | **FIXED** | 20 ordered pairs, steady tone and held chord, 6 settings, blocks 37 / 64 / 512 / 4096, switch off the block grid: gain never more than +0.84 dB over the band between the always-old and always-new renders (Clean -> Classic, attack 100); at attack 0.1 / release 10, where the gain ripples anyway, +2.65 / -3.06 dB. HF frame at the switch -63 dB re level or lower on the steady tone. Round-1 test through the processor: +0.0 dB new to new, +0.5 dB with Classic. One corner is N8. | `c2_switch.py`, `c2b_switch_silence.py`, `r1/cf_r2.txt` |
| C3 | Non-finite input with the sidechain filter | **FIXED** | 8 NaN, 8 +Inf, 8 -Inf, 0.3 s NaN, five types, sc_hpf 20 and 100, blocks 1 / 37 / 512, mono and stereo: output finite at every sample, 0 differing samples against the zeroed-input render 0.8 s later, trace and meter finite. | `x2_robust_comp.py` |
| C4 | Unbounded output | **FIXED** (bounded) | Steady manual + auto makeup measured 48.00 dB at T -60, R 20, makeup 24. Full-scale 41 Hz onset, attack 100, auto on: peak 182 to 189; round-1 fuzz case 120 to 128 (was 1324); 120 random renders with bypass and type flips: worst 251.2 = full scale x 48 dB. A full-scale onset can therefore still leave the block at +48 dBFS. | `c1_makeup.py` |
| C5 | Squash release and knee do not mean the knob | **PARTLY** | Knee: fixed, R 20 knee 24 at the threshold reads 2.850 dB (textbook 2.850, was 7.61); six curves within 0.001 dB. Release to silence: 149.9 ms at 150, within 0.7 % from 10 to 1000 ms. **Release while the input stays above the threshold: not fixed.** Round-1 case (-4 -> -16 dBFS, T -34, R 8): 53.4 ms at knob 150 (was 52.2). Mine (T -24): R 8, -4 -> -10 dBFS 41.3 ms; -14: 64.6; -18: 92.7; R 4, -14: 64.6; R 2, -14: 149.7. The commit says "matches its knob above the threshold"; that is true only while the *output* stays above the threshold, i.e. for input drops smaller than (level - T) / R: 2.5 dB at R 8. The contract now documents it as the feedback character. | `c3_ballistics.py`, `c3b_ballistics.py`, `r1/cb_r2.txt`, `r1/cd_r2.txt` |
| C6 | FET and Opto have no programme dependence | **FIXED** | Release t63 after 0.1 / 0.5 / 5 s bursts: FET 67.1 / 97.9 / 123.7 ms (the study's unit 56 / 95 / 123), Opto 93.3 / 143.2 / 164.5 ms; Clean and Squash stay at 119.9 and 149.9. A -20 dBFS burst releases in 63.7 ms against 97.9 at -10 (FET). | `c3b_ballistics.py`, `r1/cb_r2.txt` |

Count: **10 fixed, 3 partly fixed, 0 not fixed** (G1, G7, C5 partly).

## New findings, most audible first

| # | Finding | Number (worst case) | Evidence |
|---|---|---|---|
| N1 | **Tight no longer closes between palm mutes when the floor is within about 22 dB of the notes.** The wait before closing now depends on the fall from the recent peak (16 dB or less: 0.4 s; 26 dB or more: the hold). After an amp, a drive or a compressor, or with a noisy pickup, a muted chug falls less than that. Regression of the fix. | 75 ms notes, threshold 8 dB over the floor. Floor 18 dB under the note peak: 0 of 8 gaps closed at 75, 100 and 150 ms gaps, 1 of 8 at 250 ms (gain 0.0 dB in the gap). 20 dB: 0 of 8 at 100 ms, 4 of 8 at 150 ms. 22 dB: 1 of 8 at 75 ms. 26 dB and more: all closed. Hold 0 does not cure it (18 dB, 100 ms: 4 of 8). Same signals through the processor: pre-fix build 8 of 8 closed, HEAD 0 of 8 (18 dB) and 2 of 8 (22 dB, 75 ms). Smooth: same state machine, gaps stay at 0 dB. | `g9_tight_floor.py`, `p1_processor.py` |
| N2 | **The gate hangs open on the floor after a soft or decayed note is stopped.** Same cause as N1, in the default position before the amp. | Note stopped by the hand 8 to 12 dB over the floor: gate open for 418 to 430 ms, floor passes at 0.0 dB for the whole window; 16 dB: 226 to 312 ms; 18 dB: 92 to 140 ms; 20 dB: 55 to 84 ms; 22 dB and more: 37 to 64 ms. Hold 0 changes nothing at 12 dB (425 ms). Through the processor: pre-fix 32 ms, HEAD 426 ms (12 dB over); 34 against 246 ms (16 dB). Smooth: 417 ms at unity before the expander starts. | `g2_hang.py`, `g10_claims.py`, `p1_processor.py` |
| N3 | **The hold knob is the wait only after a fall of more than about 31 dB.** | Floor 90 dB under the note: 12 ms + knob exactly (0 to 250). Floor 28 dB under: hold 100 reads 210 ms, hold 250 reads 378 ms (the stop detector lapses while the recent peak decays 20 dB/s, then the long wait takes over). Floor 23 dB under: hold 0 / 6 / 20 / 50 read 20 / 53 / 94 / 159 ms. Floor 18 dB under: 357 to 412 ms whatever the knob. Release is exact (5 / 30 / 100 / 500 ms read 4.98 / 29.90 / 99.65 / 498.4 ms for 60 dB). | `g7_knobs.py` |
| N4 | Chatter returns behind a louder note (see G1). | 22 of 104 cases, up to 5 dropouts to mute; needs 20 dB or more and 0.25 s or less. | `g3_chatter2.py`, `g3b_sweep.py` |
| N5 | Smooth: a floor that rises is first ignored, then snapped to. The floor average restarts when the level is 6 dB over it, and the gain then moves at the 0.2 ms opening rate. | Hiss that steps up 5 dB: 3.6 dB in one sample, 12.4 dB within 10 ms; 8 dB: 4.1 dB / 15.4 dB. Hiss with +-4 dB of 4 Hz level change: 5.2 dB in one sample, 13.3 dB within 10 ms, 4.4 such steps per second; +-7 dB: 7.1 dB / 19.0 dB, 18.8 per second. Crackle under the threshold: 11.3 dB in one sample. A floor that falls 8 dB, or drifts 10 dB in 2 s, is tracked without steps (0.82 and 0.35 dB per 10 ms). One spike per mains cycle (20 ms apart, longer than the 12 ms window) on digital silence moves the gain 5.9 dB at 50 Hz; with hiss at -70 dBFS under it, 0.5 dB. | `g6_smooth.py` |
| N6 | The 75 Hz 4th-order sidechain filter is wrong for bass and dull low notes at its default. Bright guitar strings in drop tunings are fine. | Steady level over the threshold needed to open: pure 41.2 Hz 20.7 dB (theory 20.8; the contract's X20 says 15.4, which is the value for an abrupt onset: 15.3 / 14.9), B0 30.1 dB; dull string (neck pickup, flatwound) E1 10.1 dB, B0 13.1 dB, A1 7.1 dB; bright strings B0 to A2: -1.9 to +2.7 dB. A decaying dull E1 note is shut while its true peak is still 3.1 to 4.4 dB **over** the threshold (B0: 7.9 to 9.5 dB), where the filter off gives -9.8. A dull E1 onset 8 to 12 dB over the threshold loses its first 14 ms (-3.9 dB over 25 ms; filter off: open after 4 ms). | `g8_low.py`, `g10_claims.py`, `g11_more.py` |
| N7 | Opening click, no attack control. The 0.2 ms rise opens in the middle of the waveform on slow onsets and swells. | Energy above 3 kHz at the opening, peak, re the note's level: -35.0 dB (82 Hz sine swell, thr -30), -40.4 dB (41 Hz, thr -40), -40 to -73 dB over 46 cases. Against the old 0.5 ms / 60 dB ramp at the same instant: 5.2 dB quieter on average, 16.6 dB louder in the worst case (41 Hz, thr -50, both under -49 dB). On the sine swells a 1 ms linear rise would be 7 to 15 dB quieter and a 3 ms cosine 10 to 40 dB; on the strings 0 to 12 dB. | `g5_attack.py` |
| N8 | Compressor, Classic -> new type: the handed-over gain is frozen for the detector window (12.5 ms). An attack that lands in it passes unattacked. | Switch in silence, chord 5 ms later: up to +8.7 dB (2 ms frames) over both the always-Classic and the always-new render, with no earlier note in the file; +9.6 dB with one. Peak not higher than the louder render in the clean case (-1.3 dB). | `c2b_switch_silence.py` |
| N9 | Compressor meter during a Classic crossfade reads the larger of the two engines. With mix under 100 % it shows the wet path (known). | Up to +9.4 dB over the reduction applied for the 40 ms fade (FET -> Classic); mix 50 %: meter 15.0 dB, heard 4.6 dB. Outside fades: 0.000 dB, five types, blocks 16 to 4096, with makeup. | `c6_meter.py` |
| N10 | Auto makeup matches level only for a -10 dBFS peak. | Steady tone at -10 dBFS: +0.00 dB over 4 types x 4 thresholds x 5 ratios. At -20 dBFS: +3.1 to +9.5 dB; at -3 dBFS: -2.3 to -6.7 dB. A plucked chord at -10 dBFS peak comes out +2.0 to +11.7 dB louder (rms) with auto on. By design, but it is not what a player expects from the word. | `c7_misc.py` |
| N11 | Compressor: the sidechain filter switched on off the 16-sample control grid runs up to 15 samples on the coefficient it had before (`fx::TptOnePole::reset()` clears the state, not the coefficient; cause *inferred* from the code, grid dependence measured). | `--reset-repeat` fails in 14 of 240 compressor runs, all at block 37 with sc_hpf automation, largest difference 0.012; blocks 48 and 512 pass. Inaudible. | `x4_rt.py` |
| N12 | Gate range automation steps at block rate. | Range ramped -90 -> -10 on a closed gate: gain steps of 0.0121 per sample at block 512 (0.0015 at block 64), each within 0.2 ms. | `g7_knobs.py` |
| N13 | Classic compressor meter or trace goes non-finite for a 3e38 input sample. Audio stays finite. | 1 case of 30. | `x2_robust_comp.py` |

N1 and N2 are one design decision seen from two sides, and they are the price paid for G1. Before the fix a stopped
note closed in 32 to 39 ms whatever its level over the floor.

## Everything else asked for

**Gate close times** (`g4_close.py`): abrupt stops, 41.2 Hz to 1 kHz, -6 and -20 dBFS, thr -30 to -70, 44.1 / 48 / 96 kHz,
5 phases, 870 stops: 27.8 to 28.0 ms to -20 dB. Hand-stopped strings (tau 3 ms), 300 stops: 33.1 to 51.2 ms. Hard-driven
(tanh) input: 36.5 to 39.0 ms. No reopening after any of 1 497 stops. The stop detector does remove the filter
ringing: the 41.2 Hz row is not slower than the 1 kHz row.

**Gate under knob moves** (`g7_knobs.py`): threshold, release, hold, range, sc_hpf, mode (including a new request every
10 ms) at blocks 64 and 512: no gain step larger than the opening ramp (0.1042 per sample at 48 kHz), all output finite.
`on` is a hard switch at engine level (step 1.0); the processor wraps it. Floors at the threshold (hiss, pink, hum
from -66 to -46 dBFS, thr -40): at most 1 state change in 6 s. 16th notes at 200 bpm: never shuts.

**Compressor ballistics** (`c3_ballistics.py`, t63 = time constant of the gain in dB, 44.1 / 48 / 96 / 192 kHz):
attack knob 1 / 5 / 20 / 100 ms reads within 2.6 % in all four new types; knob 0.1 reads 0.125 to 0.136 ms; undershoot
0.000 dB. Release, Clean and Squash, to below the threshold: within 0.7 % from 10 to 1000 ms. FET and Opto after a 1 s
note: 2.15 to 2.18 and 2.70 to 2.72 times the knob (two stages; the knob is the fast one).

**Low-frequency distortion** (`c4_lf.py`, residual besides the fundamental, -10 dBFS, T -20, R 4, 44.1 and 48 kHz):
starting points, 41.2 to 110 Hz in 8 frequencies: 0.0000 % in all four new types (also at 96 kHz and -3 dBFS);
30.87 Hz 0.015 to 0.027 %; Classic 1.81 to 4.50 % (5.71 % at 30.87 Hz). Fastest (0.1 / 10) at 41.2 / 82.4 / 110 Hz: Clean 5.85 / 1.66 / 0.61 %,
Squash 7.11 / 2.35 / 0.69 %, FET 3.41 / 0.93 / 0.34 %, Opto 3.07 / 0.87 / 0.32 %, Classic 16.8 / 13.6 / 11.9 %. Release 30 ms:
0.25 % at 41.2 Hz. Two notes at once (starting points): 1.1 to 5.2 % (Classic 10 to 18 %).

**Controls moved while running** (`c5_controls.py`): threshold, ratio, attack, release, knee, mix, makeup, sc_hpf (two
moves), auto makeup, in Clean / Squash / FET / Opto, on a steady complex tone, a bass complex and a pulsed tone, blocks
37 / 512 / 4096, moves off the block grid (200 cases): every control lands on the render that always had the new setting
(Clean and Squash within 0.02 dB on the steady tone and under 0.5 dB on every material; FET and Opto up to 3.3 dB short
0.7 s after a threshold or ratio move, which is their slow stage),
never leaves the band between the two static renders by more than 0.3 dB, loudest HF frame -77 dB re level.

**Stereo link, mono, block sizes, rates** (`c7_misc.py`, `x1_robust_gate.py`): L and R gains differ by 0.0000 dB in the
four new types, loud side left or right, sc_hpf on or off (Classic 16.3 dB, by design). Blocks 1 / 37 / 4096 against 512
at 44.1 / 96 / 192 kHz: 0 differing samples for both effects, every mode and type; a mono render equals the left
channel of the stereo render exactly.

**Classic bit-identity** (`x3_null.py`): `--classic-null` with my own stereo and mono programme at 44.1 / 48 / 96 kHz,
blocks 1 / 37 / 512, 8 gate settings (threshold values and ramps, deferred, bypass toggles, the new-only knobs off
default) and 7 compressor settings (extremes, makeup ramps, threshold ramps, `skip` sequences, new-only knobs off
default): 270 nulls, 0 differing samples.

**Real-time safety and reset** (`x4b_rt_gen.py`, `x4_rt.py`): `--rt-check`, 1 184 runs (3 generated inputs, 44.1 to
192 kHz, mono and stereo, blocks 1 / 37 / 512 / 4096, every mode and type, with and without my automation of every
key including mode and type flips and curve delay): 0 allocations. `--reset-repeat`, 480 runs: Tight and Smooth 0
failures; Classic gate 60 (the frozen code, the legacy reference fails the same way); compressor 14 (N11).

**Robustness** (`x1_robust_gate.py`, `x2_robust_comp.py`): ASan + UBSan, 216 gate and 296 compressor renders (NaN,
3e38, 1e-39 denormals, DC, full-scale 30 Hz and 1 kHz squares, +24 dBFS noise, impulses, silence; every type; blocks
1 / 37 / 512 / 4096; mono; everything automated): 0 sanitizer reports, 0 non-finite outputs on finite input. A minute of
digital silence then a chord: gate gain 0 during the minute (Classic 9.5e-17), Tight and Smooth open 0.17 ms after the
chord starts (Classic 5.0 ms); compressor reduction 0.0000 dB at the end of the minute, 16.3 to 18.8 dB 20 ms into the
chord. DC 0.5 settles at 0.050 to 0.058. No-FTZ bench within 1.06 x of the FTZ figure.

**CPU** (`x5_bench.py`, `--bench`, 48 kHz, 64-sample blocks, stereo, 10 s, minimum of 3 invocations of the bench's own
min-of-5). The machine was **not quiet**: load average 5.4 to 6.5 on 12 cores from other agents, so these are upper
bounds.

| % of real time | pink -18 dBFS | music | silence | 96 kHz pink | block 512 |
|---|---|---|---|---|---|
| gate Classic | 0.089 | 0.100 | 0.100 | 0.195 | 0.078 |
| gate Tight | 0.093 | 0.099 | 0.114 | 0.195 | 0.085 |
| gate Smooth | 0.101 | 0.105 | 0.110 | 0.197 | 0.082 |
| comp Classic | 0.101 | 0.065 | 0.046 | 0.202 | 0.086 |
| comp Clean | 0.082 | 0.053 | 0.045 | 0.163 | 0.075 |
| comp Squash | 0.089 | 0.066 | 0.048 | 0.177 | 0.080 |
| comp FET | 0.091 | 0.065 | 0.057 | 0.181 | 0.080 |
| comp Opto | 0.091 | 0.063 | 0.053 | 0.181 | 0.081 |
| comp Clean + sc_hpf + mix + auto | 0.097 | 0.063 | 0.053 | 0.194 | 0.086 |

The gate sits at its 0.10 % budget (three detector windows and two filter sections now run in every mode); the
compressor is far inside its 0.35 %.

## Statements in the commits and the contract that do not hold as written

- `a27b550`: "Squash's release matches its knob above the threshold". 41 to 93 ms at knob 150 for ordinary drops at
  R 4 and above (C5).
- `81fd404` and `Gate.h`: "40 / 50 ms gaps reach -31 / -51 dB". Only for note ends of 1 ms; -13 / -34 dB with a hand
  stop (G7).
- `docs/FX-ENGINES.md` X20: "steady sine needed to open ... 41.2 Hz tone needs 15.4 dB". Steady state is 20.7 dB;
  15.4 is the abrupt-onset figure (N6).
- `docs/FX-ENGINES.md` 4.1.2: "hold ... the wait after a note stops". Only when the note was more than about 31 dB
  over the floor (N3).
- Tool: `tubamp_fxbench --rt-check` with a WAV input reports "malloc 0, free N" (N random, 1 to 66) in 11 of 12
  identical runs and passes 12 of 12 with a generated input. The libmalloc hook in `tools/fxbench/main.cpp` is armed
  process-wide and counts frees made by another thread (*inferred* from the code and from 0 mallocs in every case).
  A red `--rt-check` on a file input proves nothing either way.

## Could not test

- No listening, no real DI guitar or bass recordings: every signal is synthetic.
- The commercial units were not re-measured; references to them are the study's numbers.
- The installed Audio Unit in a host, the UI meters, host automation timing, parameter changes from a second thread.
- Processor level beyond the spot checks in `p1_processor.py`: the deferred curve behind the amp and behind hosted FX
  slots, a gate and amp in different split lanes, chain edits while audio runs, the bypass faders around the
  compressor (the engine itself keeps a stale gain through `skip`: 15 dB down for 260 ms after re-enable at engine
  level; the processor resets it on re-entry, read in `PluginProcessor.cpp`, not measured).
- N1 was measured with a synthetic floor at a set distance under the notes, not with the gate behind a real NAM
  model; the 18 to 22 dB figure for "after a high-gain amp" is a *guess* about real rigs.
- CPU on a quiet machine.
- Eight scratch WAVs (12 MB) are left in `build-verify/dynamics-r2/work/`: the shell's safety check refused the
  glob removal and I did not work around it.

## Judgment

The compressor is at a professional standard: every control works in every type and lands where a static render
lands, the knobs mean milliseconds, low notes are clean, stereo is linked, the meter is true, type switches hold
level, and nothing I threw at it broke it; what remains is a feedback release that is faster than its knob and an
auto makeup that is a fixed gain. The gate is not there yet: the chatter fix made the closing time depend on how far
the note stood above the floor, so the same Tight mode that mutes a loud chug in 28 ms lets 0.4 s of hiss through
after a soft note and does not close at all between palm mutes when the floor is within 22 dB, which is exactly the
place a player puts a gate for tightness, and the hold knob no longer says what it does.
