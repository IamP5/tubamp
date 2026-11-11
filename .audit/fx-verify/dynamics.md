# Dynamics verification: gate (`GateFx`) and compressor (`CompFx`)

Independent, adversarial re-measurement. Tree: `claude/tubamp-effects-reverse-engineer-90c7f7` at `a84ca22`
(gate/comp integration commit `6c2c294`). Nothing under `src/ ui/ tests/ tools/ docs/` was changed.
Everything below was measured by me unless a line says "team" or "study".

## How it was measured

- **Harness** `build-verify/dynamics/harness/dynproc.cpp`: drives the whole `TubampAudioProcessor` headless on a float
  WAV (chain order, parameters, per-block parameter steps and ramps, any block size including random and
  larger-than-prepared, mono or stereo bus, real NAM model `A2.nam`, per-block dynamics meters, allocation counter
  around `processBlock`, wall-clock bench). Three builds of it: Release (`hrel`), ASan+UBSan (`hasan`), and the
  **base commit `bf81728`** (`hbase`, sources from `git archive`, read-only) for bit comparisons against the old code.
- Team tools were built in `build-verify/dynamics/rel` and run once to see what they cover (`check_gate.log`,
  `check_comp.log`, `golden_verify.log`, `golden_legacy.log`); they are not used as proof below.
- Scripts and raw outputs: `build-verify/dynamics/{ga,gb,gb2,gb3,gb3b,gc,gd,gd2,ge,gf,ca,ca2,cb,cc,cd,cf,xa,xb,xc,misc}.py`
  and the `.txt` next to each. Scratch WAVs were deleted.
- **Pitfalls hit and corrected (each one first produced a wrong "defect")**:
  1. The processor's always-on DC blocker leaves a sub-sonic tail after any abrupt level change (-31 dBFS after a
     -6 dBFS chug note). Any "is the gate closed / what is the gain" reading taken from output level, from a
     one-period demodulation or from crest values is contaminated by it. All gain readings below use a first or
     second difference (Nyquist probe, or 2nd-difference ratios) or a wet/dry pair through the same processor.
  2. Second-difference gain ratios are invalid while the gain itself moves inside a cycle (fast compressor settings).
  3. A `#if` in my own harness removed a loop increment in the base build (hang); fixed before any base comparison.
- Controls that show each measurement can fail: Classic compressor reads 2.396 % at 82.4 Hz and -24 dBc automation
  clicks; Classic compressor is unlinked (-9.98 dB L/R difference); the C21 comparison fails at -13 dB when the
  steps are off the block grid.

## Verdicts

| Claim | Verdict | My number |
|---|---|---|
| G-a close times | **VERIFIED** (Tight), **marginal** (Smooth) | Tight worst case over 24 random note-off phases: 27.7 to 48.6 ms (41.2 / 82.41 / 110 / 330 / 1000 Hz, sine and 5-harmonic, -6 and -20 dBFS, thr -30..-70, at 44.1 / 48 / 96 kHz, through the processor). Lower bound is 27.7 ms, not 32 (hold is now 6 ms). Smooth: 48.3 to 69.3 ms; 69.3 at 41.2 Hz / thr -70 (2 ms over the stated 67). No reopening in any of 2 880 note-offs. |
| G-b palm mutes, no chatter | **PARTLY: single notes VERIFIED, chords NOT** | 50 ms gaps: -34.1 dB median / -31.8 worst by RMS over the last 10 ms (deepest point -43.4 / -40.8), consistent with the team's -37.8 / -35.1 median-of-trace. Decaying single notes (41.2 to 330 Hz, tau 0.15 to 2.5 s, thr -20..-70, 144 cases): exactly one open and one close, always. **Chords chatter**: see defect 1. |
| G-c pick attack | **NOT VERIFIED as a general statement** | Their two signals reproduce (-0.81 dB on an abrupt 1 kHz onset). On a pluck with a pick click: first 5 ms -1.6 to -1.9 dB, first 1 ms -8.7 to -11 dB, peak of the transient -2.9 dB (1 kHz) / -4.4 dB (196 Hz) / -6.8 dB (82 Hz). See defect 4. |
| G-d no steps / zipper | **VERIFIED for Tight and for block-rate zipper; NOT for Smooth on a noise floor** | Gate then amp (A2.nam, deferred): output is bit-identical for block 16 / 64 / 512 / 37 / 509 / random at 44.1, 48, 96 kHz (and block 1 at 48 kHz), 16 gate transitions in the file. Applied curve equals the engine's own curve delayed by D to -50 dB (the rest is the DC blocker). Smooth: defect 2. |
| G-e deferred alignment | **VERIFIED** | Curve delay applied = reported latency exactly (best-fit D = 27 at 44.1 kHz, 42 at 96 kHz; D+-1 is 33 dB worse). True extra delay of the amp path against the native-rate render (cross-spectrum, flat from 90 Hz to 5 kHz): 28.92 samples at 44.1 kHz, 45.00 at 96 kHz, so the curve is 1.92 / 3.00 samples early. Also 2.84 at 88.2 kHz, 1.46 at 22.05 kHz, 5.00 at 192 kHz (0.026 to 0.066 ms). |
| G-f Classic gate bit for bit | **VERIFIED** | Team tools pass (`--classic-null` rows, `--legacy-check` 168 renders). Not covered by them: the golden `gateamp*` rows are compared against this build's own render with `amp_eq_on` off, so the deferred Classic gate on a real model was never compared with the old code. I did, against a build of the base commit: gate deferred onto A2.nam, gate after amp, threshold steps/ramps, `gate_on` and `amp_on` toggles, 22.05 to 192 kHz, blocks 512 / 37 / 16 / random / 1, mono bus, and with every new-only parameter off default: 125 renders, 0 differing samples. |
| C-a no distortion on low notes | **VERIFIED for one sustained note >= 40 Hz with release >= 50 ms** | 27 to 200 Hz in 1.37 Hz steps plus 39.9 / 40.1 / 41.2 Hz: worst total residual (harmonic + inharmonic, least squares) 0.00004 % for all four types at their starting settings; my floor (compressor bypassed) is 0.00001 %. Levels -22 to 0 dBFS, 44.1 and 48 kHz: same. Vibrato (+-30 and +-50 cents): 0.000 % equivalent AM. Decaying notes: <= 0.06 %. Below 40 Hz: 30.87 Hz 0.018 to 0.051 %, 27.5 Hz 0.05 to 0.14 %. Limits: release 30 ms gives 0.25 % at 41.2 Hz, release 20 ms 1.08 % at 41.2 Hz and 0.02 % at 82.4 Hz. Two notes at once: 1.0 to 1.3 % (fifth E2+B2), 0.19 % (octave), 3.3 to 4.9 % (minor third E2+G2), 2.7 to 4.5 % (E1+B1); Classic gives 3.7 to 16.8 % on the same. Fastest settings (0.1 / 10): 0.35 to 7.77 % (Squash 41.2 Hz 7.77, Clean 5.86 / 1.66 / 0.97 / 0.61 at 41.2 / 82.4 / 98 / 110 Hz), all harmonic; Classic 11.9 to 16.8 %. |
| C-b knobs mean what they say | **VERIFIED, with one exception (Squash)** | Clean attack t63 0.10 / 0.50 / 1.00 / 4.98 / 19.9 / 98.3 ms for knobs 0.1 / 0.5 / 1 / 5 / 20 / 100; release 9.8 / 29.7 / 119.2 / 397.6 / 990 for 10 / 30 / 120 / 400 / 1000; identical for 3, 7.5 and 15 dB of reduction, all types. FET (T -24, R 6, knee 4, 0.3 / 55): t50 / t63 / t90 = 64.1 / 93.4 / 393.1 ms (study 1176 Rev A: t63 95, t90 399). Opto (T -24, R 4, knee 12, 2 / 60): 85.0 / 136.6 / 693.7 (LA-2A Gray: 132, 720). Exception: Squash release when the level drops but stays above threshold is 52 ms t63 with the knob at 150 (R 8); and neither FET nor Opto has any programme dependence (identical for 50 ms, 0.5 s and 5 s bursts; the hardware models move 83 to 123 and 132 to 171 ms). |
| C-c Squash stable, no undershoot | **VERIFIED** | 1 869 renders (22.05 / 48 / 192 kHz, R 1 to 20, attack 0.1 to 100, release 10 to 1000, knee 0 to 24, thr -60 to -10, 0 and +24 dBFS after silence), per-sample gain: worst undershoot 0.009 dB, never above 0 dB, always finite. 1 kHz and 5 kHz sine bursts: <= 0.14 dB; 0.6 dB with the sidechain HPF at 300 Hz (the filter's own onset transient). |
| C-d static curves | **VERIFIED** | Ratio 1.50 / 2.00 / 4.00 / 8.00 / 20.00 measured; knee within 0.04 dB of the textbook curve (Clean, FET, Opto). Auto makeup: worst +0.001 dB at -10 dBFS over 4 types x R 1 to 20 x T -60 to 0 x knee 0 / 6 / 24. Mix: 50 % equals the mean of the 0 % and 100 % renders to -132 dB, 0 % is bit-identical to bypass. SC HPF 100 Hz: 1.21 dB of reduction at 40 Hz against 7.46 dB at 1 kHz. Note: Squash's knee is on the output level, so its curve is not the knob's (R 20, knee 24: 7.6 dB of reduction at threshold against 2.85). |
| C-e stereo link, meter | **VERIFIED** | Hard-panned programme: L and R gains differ by 0.0000 dB in all four new types (Classic: 9.98 dB, as before). Meter against the reduction really applied, per block, during level steps: within 0.04 dB at blocks 16 / 64 / 512. With mix below 100 % the meter shows the wet path (15.0 dB shown, 7.8 dB net at 50 %). |
| C-f automation clean; C21 | **VERIFIED; the team's C21 argument is RIGHT** | 10 Hz square-wave automation between the extremes of threshold, ratio, attack, release, knee, makeup: energy more than 3 kHz above the carrier stays at -101 to -117 dBc under 1 kHz and -75 dBc or lower under 82.4 Hz, same at blocks 16 / 64 / 512 / 37; mix -81 to -92, auto makeup -77 to -88, sc_hpf -59 (FET on bass) to -112 dBc. Classic: -24 to -45 dBc. C21: with every step on the 512-sample grid, the block-512 render is bit-identical to blocks 64, 16 and 1, and the lines at 1 kHz +- k x 93.75 Hz (-59.8 / -76.1 / -84.4 dBc Clean) are identical when the same modulation is rendered at block 8 off the grid: they are the modulation's own sidebands. But `check_comp.py` still exits 1 on that row, so "every acceptance check passes" is not literally true. |
| C-g Classic compressor bit for bit | **VERIFIED** | Against the base-commit build through the processor: defaults, fastest, makeup ramps, `comp_on` toggles (the `skip` path), three instances in series, a compressor in each lane of a split, 22.05 to 192 kHz, blocks 512 / 37 / 16 / random / 1, mono bus, new-only parameters off default: 149 renders, 148 identical. The one that differed (gate, comp, amp at 96 kHz block 16, 90 samples, 3.9e-23) is a run-to-run nondeterminism of the resampled NAM path that the base build shows against itself (2 of 5 repeats, 18 482 samples); identical pairs exist. |
| X-a RT safety, robustness | **VERIFIED except NaN / Inf** | 244 ASan+UBSan renders: 0 sanitizer reports, 0 non-finite outputs on finite input (full-scale noise, 20 Hz square, +24 dBFS, 1e-30, 1e-42 denormals; every gate/comp parameter at both extremes alone, all-min, all-max, 40 random corners; 22.05 to 192 kHz; blocks 1 to 4096 and blocks larger than prepared; mono and stereo; mode, type and bypass switched every few ms). Allocations inside `processBlock`: 0 in 8 scenarios (libmalloc logger). The smoke test's `AllocationGuard` does arm around the call and has a positive control. NaN / Inf: defect 5. |
| X-b CPU | **VERIFIED within budget** | see CPU |
| X-c professional standard | see the last section | |

## Defects, most serious first

1. **Tight chatters on decaying chords at its default hold of 6 ms.** The "opens and closes once" result holds for
   single notes only. While the chord decays through the threshold (tau 1.5 s, 12 harmonics per string):
   C2+G2+C3 (drop-C power chord) at thr -45: 28 state changes = 13 dropouts in 0.40 s, median 6 ms long, 11.6 dB
   deep (max 22 dB); A1+E2+A2 at thr -50: 32 changes, 15 dropouts in 0.54 s, to -22 dB; E2+G2 (minor third) at
   every threshold from -30 to -55: 10 to 14 changes, dropouts 23 ms long and 46 dB deep (max 71 dB);
   E2+G#2: up to 8; E1+B1 at thr -65: 8. Beating also does it: 9.5 dB of amplitude swing at 3 / 7 / 15 Hz gives
   6 / 12 / 20 changes, two strings 2 Hz apart 5, 6 Hz apart 13 to 17. Standard E2+B2(+E3), drop-D dyads and a
   six-string E chord are clean. Hold >= 10 ms cures three of the chord cases and >= 15 ms the others; the study's
   references use 20 to 100 ms. Smooth (hold 10) still gives 10 and 8 changes on two of them.
   Reproduce: `python3 build-verify/dynamics/gb2.py` (and `gb.py` sections 3 and 4, `gb3.py` for depths).
2. **Smooth mode steps its gain on a stationary noise floor.** White noise 22 dB under the threshold (thr -40,
   release 80, hold 10): 72 upward gain steps per second, median 1.1 dB, 95th percentile 4.0 dB, largest 7.7 dB,
   each within one sample at 2.5 dB per sample; the gain wanders over 5.4 dB (5th to 95th percentile). On 50 Hz hum
   with harmonics: 200 steps per second, up to 7.3 dB. Cause: the closed-state expander target follows the 12 ms
   window maximum with the 0.5 ms attack slew and no smoothing of its own. Tight is silent there (muted).
   Reproduce: `python3 build-verify/dynamics/gd2.py`.
3. **A compressor type switch lets the uncompressed signal through.** With 15 dB of reduction on a sustained note
   (T -30, R 4, -10 dBFS, no makeup) every switch between any two types raises the output by +15.0 dB for the
   40 ms fade (it fades to dry and back). No click (-78 to -85 dBc), which is all the team's C22 looks at.
   `comp_on` is a hard switch (-44 dBc), unchanged from before. Reproduce: `python3 build-verify/dynamics/cf.py`.
4. **Tight takes the front off a picked note.** It opens 0.62 to 0.75 ms after the sidechain crosses the
   threshold and has no lookahead: on a pluck with a pick click the first millisecond is down 8.7 to 11 dB and the
   click's peak 2.9 to 6.8 dB, at any level from 6 to 34 dB over the threshold. The sidechain filter also makes low
   notes read low: an 82.4 Hz sine needs to be 3.9 dB over the threshold to open at all, and a soft (8 ms) onset
   loses everything until the level crosses. Reproduce: `python3 build-verify/dynamics/gc.py`.
5. **NaN handling.** (a) New compressor types with the sidechain high-pass on (`comp_sc_hpf` > 20.5): eight NaN
   input samples make the output NaN forever; the detector filter state is the one recursive state not
   sanitised (`tubamp_fxbench comp nan.wav out.wav type=1 sc_hpf=100`: 35 409 of 36 000 samples bad; without the
   filter it recovers in 12.5 ms, the gate in 0). (b) At processor level one NaN or Inf input sample makes the
   output non-finite until the next reset in every chain, including an empty one and including the base commit:
   that is the always-on DC blocker, older than this work, but it means "finite output for NaN and infinity" is
   false for the plugin. Reproduce: `python3 build-verify/dynamics/xa.py` section 1 and the `xa2.txt` table.
6. **Squash release is level dependent** (52 ms against a 150 ms knob while the signal stays above threshold at
   R 8), and its knee knob does not give the textbook knee. FET and Opto have no programme dependence at all
   (declared by the team as not delivered). `cb.py`, `cd.py`.
7. **Palm-mute gaps shorter than 50 ms are not muted**: 40 ms gap -14 dB, 30 ms gap -1 dB (thr -40). `gb3b.py`.
8. **`check_comp.py` exits 1** (C21 at block 512). The explanation is correct, the row is a bad test and should be
   replaced by the block-aligned comparison plus a broadband click metric.
9. **Golden coverage hole**: `gateamp`, `gateampM` and `ampM` rows are checked against this build with the amp EQ
   off, not against the base capture. (I closed it for the gate with the base build; identical.)
10. Outside these two effects, seen while testing: the amp at a non-native rate with 16-sample blocks is not
    deterministic from run to run (amp alone, 44.1 kHz: 2 of 7 runs differ by up to 1.9e-5 from sample 23), in the
    base commit too.

## CPU

Method: `dynproc --bench 9` twice (minimum of 18 passes), wall clock around `processBlock` only, 48 kHz, 64-sample
blocks, stereo, 10 s; figure = chain with the block minus the empty chain (0.070 % on pink noise). The machine was
shared with other builds, so read these as upper bounds.

| per instance, % of real time | pink -18 dBFS | guitar-like | silence | 1 s playing + 9 s zeros | + 9 s of 1e-30 |
|---|---|---|---|---|---|
| gate Classic | 0.070 | 0.073 | 0.067 | 0.069 | 0.063 |
| gate Tight | 0.064 | 0.068 | 0.065 | 0.067 | 0.059 |
| gate Smooth | 0.065 | 0.067 | 0.065 | 0.065 | 0.060 |
| comp Classic | 0.074 | 0.065 | 0.042 | 0.044 | 0.046 |
| comp Clean | 0.077 | 0.066 | 0.034 | 0.038 | 0.040 |
| comp Squash | 0.081 | 0.068 | 0.035 | 0.042 | 0.039 |
| comp FET | 0.078 | 0.069 | 0.037 | 0.050 | 0.042 |
| comp Opto | 0.079 | 0.069 | 0.038 | 0.049 | 0.041 |
| comp Clean + sc HPF + mix + auto | 0.088 | 0.077 | 0.038 | 0.042 | 0.042 |

Silence never costs more than signal. The processor always runs under `ScopedNoDenormals`, so denormal safety
without FTZ cannot be exercised through it; the team's engine-level figure (`--no-ftz`) is the only evidence for that.
All three gate detectors run in every mode, which is why Tight costs what Classic costs.

## Against the study's commercial units

Gate (study table, thr -40, 1 kHz -20 dBFS note then -70 dBFS; mine through the processor, worst of 16 phases):

| | -1 dB after note-off | -20 dB after note-off | opens to -1 dB | hysteresis | palm-mute gap gain |
|---|---|---|---|---|---|
| Solid Dynamics gate (study) | 66 ms | 79 ms | < 0.3 ms | 4.9 dB | not closed |
| bx_console gate (study) | 31 ms | 147 ms | < 0.3 ms | 9.5 dB | -5.3 dB |
| Gateway (study) | 66 ms | 100 ms | 1.2 ms | 0 | -1.6 dB |
| tubamp Classic | 64.0 ms | 89.9 ms | 5.0 ms (abrupt onset after silence) | 0.1 dB | (study) -2.0 dB |
| tubamp Tight | 20.2 ms | 29.7 ms | 0.75 ms | 6.0 dB | -34 dB at 50 ms gaps, -84 dB at 75 ms |
| tubamp Smooth | 24.9 ms | 50.3 ms | 0.75 ms | soft: -3 dB at -47.5 dBFS both ways | (team) -19 dB |

Compressor: on a sustained 100 Hz-region note the study measured 0.49 to 3.75 % THD on the commercial units
(Solid Bus 2.11, 1176 Rev A 0.51 mid / 3.53 fastest, LA-2A Gray 1.02, Shadow Hills optical 3.75, Focusrite SC 0.49);
the new types read 0.0000 % at their starting settings and 0.35 to 7.8 % at 0.1 / 10 ms, where the 1176 at its
fastest reads 3.53 %. Attack and release equal the knobs to 2 % and do not depend on depth, which no measured
hardware model does. FET and Opto land inside the 1176 Rev A and LA-2A Gray release bands for one burst length.

What a professional would still call missing or wrong, bluntly:
- **Gate**: the default hold (6 ms) is far below what every reference uses and chatters on dyads and drop-tuned
  power chords; no lookahead, so the pick transient is clipped where Solid Dynamics and bx open in under 0.3 ms;
  Smooth's floor is audibly unstable on hiss and hum; the threshold means different things in Classic
  (mean square) and Tight/Smooth (filtered peak), and the fixed 90 Hz filter moves it by 3.9 dB for low E and
  13.6 dB for a 41 Hz fundamental; no threshold learn (study R5).
- **Compressor**: FET and Opto are the Clean compressor with a two-time-constant release. They have no
  programme-dependent release (1176: 83 / 95 / 123 ms by burst; LA-2A Gray: 132 / 171), no harmonic colour
  (1176 0.1 to 0.4 % at 1 kHz, LA-2A 0.19 %), no ratio-dependent knee or all-buttons mode, no level-dependent opto
  attack. No lookahead, no RMS detector option, no external or listenable sidechain, no auto release (Solid Bus).
  A type switch bumps the level by the amount of gain reduction. Squash's release and knee do not mean what the
  knobs say.

Judgment: the compressor's core (detector, gain computer, ballistics, link, mix, meter, automation, CPU, Classic
identity) is at a professional standard and measurably cleaner on low notes than every unit in the study; its FET
and Opto types are well-behaved approximations of two release curves, not models, and should not be sold as more.
The gate's Tight mode is faster and tighter than every measured commercial gate on single notes and palm mutes,
but at its shipped default it sputters on ordinary two- and three-note chords as they decay, eats the first
millisecond of a pick attack, and its Smooth mode crackles on a noise floor; those three are what a professional
would hear first, and the first two are a default and a design choice (hold, no lookahead) rather than bugs.
Classic modes of both are the old code bit for bit, checked against a build of the base commit.

## Not tested

- No listening and no real DI guitar recordings: every signal is synthetic (additive plucks with differential
  decay, pick click as a noise burst, chords as sums of strings).
- The commercial units were not re-measured; their numbers are the study's.
- The installed Audio Unit in a host, the UI meters and their ballistics, host automation timing.
- Denormal behaviour without FTZ through the processor (not reachable; engine-level only, team's tool).
- Chain edits while audio runs (the re-entry reset path), hosted FX slots between gate and amp (their latency
  in the curve delay), a split with the gate and amp in different lanes, parameter changes from a second thread.
- A gate re-enabled with a stale closed state while a note is already sounding.
- 20 s torture inputs at every rate (mine were 1.6 to 2.4 s per render).
