# Phaser voicings: build spec

Two voicings for tubamp's phaser, "Phase 90" and "Small Stone". **The constants below are our
own.** They come from the published circuit topology where an opened source gives a value,
and otherwise from fits to measurements of a reference model's behaviour (Guitar Rig 6's
Phaser Nine and Stoned Phaser, measured black-box). No coefficient table or code sequence
from a vendor binary is used. The executable form of this spec is `prototype/phaser_ref.py`;
every constant here is the value the prototype uses, and where the two disagree the
prototype wins. The numbers this document quotes for its own equations (break frequencies, notch
positions, depths, resonance) are printed by `evidence/fit/spec_check.py`
(`evidence/fit/spec_check.txt`), not typed by hand.

**What the reference is.** Every fit below is to Guitar Rig 6 at settings this study chose to
stand for the two pedals: Phaser Nine with four stages (Notches 4), Color 0.4217, Frequency 0.5,
Depth 0.5, Dry/Wet 0.5, and Stoned Phaser with Color Strength 0.74, Dry/Wet 0.5, Sweep Min/Max
0.5. They were chosen because four stages is the pedal's stage count and 0.4217 and 0.74 are the
values the components' own DSP code sets as defaults [decompiled: Guitar Rig 6
`OrangePhaserAU::setDefaultParameterValues` 0x1391b8, Color 0x3ed7e910; verification-refit.md
section 2]. They are not the components' INIT presets, which a user gets when dropping the
component into a rack: Phaser Nine INIT stores seven stages, Color 0.5027 and Frequency 0.4758,
Stoned Phaser INIT stores Color Strength 0.40 [measured: verification-refit.md section 2, read
from the factory `.ngrc` files]. So "sounds like the pedal" here means "matches a vendor's model
of the pedal at settings we picked"; no hardware Phase 90 or Small Stone was measured, and an A/B
against a freshly loaded Guitar Rig component will not match.

Source tags on constants:
- `[circuit: <source>]`: the value is given by an opened circuit reference.
- `[fit: evidence/fit/targets.tsv]`, `[fit: evidence/fit/trajectories.tsv]`,
  `[fit: evidence/fit/snapshots.tsv]`: fitted to Guitar Rig 6 by `evidence/fit/fit.py`, which
  regenerates it from that file (a rerun lands within 0.2 %, optimizer tolerance; flat directions
  are named where they occur). `snapshots.tsv` holds complex responses (magnitude and phase) along
  the sweep, written by `evidence/fit/snapshots.py`.
- `[guess]`: neither; the reason for the choice is given.
- `[choice]`: a design decision of ours (discretisation, make-up, glide), with its reason and,
  where it rests on one, the measurement.
Structural statements keep the `[literature]`, `[decompiled]` and `[measured]` tags of
`tools/re/README.md`.

## Measured accuracy

### Motion: phase, pitch and waveform

A phaser's magnitude response does not fix its sound. `dry + wet·P⁴` and `wet + dry·P⁴` have the same
magnitude at every instant (|a + b·e^jθ| = |b + a·e^jθ|) and opposite motion: with the wet path
dominant the output phase follows the all-pass chain all the way round, and a held note wobbles in
pitch several times more. Until this round every fit targeted magnitudes only, and Small Stone Color
off had picked the wrong one of the pair (dry 0.75, wet 0.50; round-2 verification, N2). Every voicing
with a reference is now fitted or checked on the complex response and on its motion:

- **Complex snapshots** (`evidence/fit/snapshots.py` → `snapshots.tsv`; fitted by `fit.py`): the
  impulse response of Guitar Rig 6 every 43 to 171 ms along a slow sweep, as magnitude and phase at
  40 frequencies (50 Hz to 6 kHz). Each voicing's structure is fitted with a pure delay (Guitar Rig's
  latency, 16.01 samples) and one break frequency per snapshot. Residual re |H|: Small Stone Color
  off −46.7 dB with the structure below (−14.8 dB without its highpass, the same for either weight
  order as a start), Color on −41.6 dB, Phase 90 block −37.0 dB. Weights from the phase: Color off
  dry 0.5003, wet 0.7500 (was 0.75 / 0.50); Color on dry/wet 0.2980 (prototype 0.2999); Phase 90 block
  dry/wet 0.997 (prototype 1), sign as ours (the swapped and the negated starts land at worse minima).
- **Pitch deviation of a held note** (`check_phaser.py` P13, `evidence/fit/motion.tsv`): a 196 Hz
  sine at −12 dBFS, instantaneous frequency of the output (`refmeasure.cents()`), p99 of |cents|,
  ours against Guitar Rig 6 at three rates; bar 15 %:

  | Voicing | Rates (Hz) | Guitar Rig 6 | Before | Now |
  | --- | --- | --- | --- | --- |
  | Phase 90 block | 0.247 / 1.071 / 3.721 | 0.7 / 2.6 / 5.7 | 0.6 / 2.5 / 5.8 (−13.8 % worst) | the same |
  | Small Stone off | 0.316 / 1.778 / 5.012 | 12.4 / 57.9 / 158.5 | 2.6 / 17.8 / 60.6 (−79 %) | 12.7 / 65.9 / 165.7 (+13.6 %) |
  | Small Stone on | 0.316 / 1.778 / 5.012 | 24.9 / 134.9 / 379.9 | 24.9 / 136.1 / 369.8 (−2.7 %) | 25.0 / 136.8 / 372.6 (−1.9 %) |

- **Waveform null** (P14, `evidence/fit/gr6_takes/`): the same 8 s guitar-like take
  (`refmeasure.guitar`, the round-2 verifier's generator) through Guitar Rig 6 and through ours, our
  LFO started at the phase that nulls best (Guitar Rig starts within 0.001 cycle of 0), one
  least-squares gain; residual re Guitar Rig's output, at noon and a fast rate. The dry signal alone
  nulls at −3 to −5 dB.

  | Voicing | Rates (Hz) | Before | Now | Bar |
  | --- | --- | --- | --- | --- |
  | Phase 90 block | 1.071 / 3.721 | −26.7 / −27.1 dB | the same | −25.9 dB |
  | Small Stone off | 0.316 / 1.778 | −11.6 / −11.0 dB | −40.8 / −37.3 dB | −24 dB |
  | Small Stone on | 0.316 / 1.778 | −31.5 / −29.2 dB | −36.0 / −31.7 dB | −29.2 dB |

Phase 90 script has no reference: Phaser Nine at Color 0 keeps its modified stage 2, and its
snapshots fit our script structure at only −2 dB (scratch measurement), so neither its weights nor
its motion can be checked. Its weights are equal (0.5 / 0.5), so the dominance question above does
not arise, and A⁴ = (−A)⁴, so neither does its sign.

### Notch tracks

Prototype against Guitar Rig 6 (`evidence/fit/compare.txt`, produced by `evidence/fit/compare.py`).
Sweep ends, period, depth and peak: same multitone probes, same tracker (`track.py`). Trajectory:
the whole notch-1 path over one LFO period, from an impulse-train tracker (`itrack.py`, the
independent verifier's method) folded into 200 phase bins, ours against `trajectories.tsv` after
the best common phase shift; rms and largest error over the cycle. Notch frequencies in % error,
levels in dB error. The prototype runs at the rate Guitar Rig 6 was measured to run, so the period
column checks the prototype's LFO, not a knob law.

| Voicing / reference | Rates (Hz) | Notch-1 min | Notch-1 max | Trajectory rms / largest | Notch-2 min / max | Period | Resonance peak | Notch-1 depth |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Phase 90 block / Phaser Nine, Color 0.4217 | 0.143, 0.247, 1.071, 1.930, 3.720, 9.756 | −0.32 to +0.31 % | −0.75 to +0.62 % | 0.41 to 0.70 % / 1.18 % | (not a target) +0.28 to +0.80 % / +1.57 to +3.34 % | 0.00 % | −0.05 dB (+3.48 against +3.52/3.53) | −0.07 to −0.04 dB (−28.9) |
| Small Stone / Stoned Phaser, Color off | 0.159, 0.316, 1.778 | −0.02 to +0.53 % | −0.17 to −0.14 % | 0.12 to 0.21 % / 0.53 % | (n2 max leaves the 12 kHz band) −0.09 to −0.05 % at the bottom | 0.00 % | 0.00 dB (+1.94) | −0.03 to −0.01 dB (−12.1) |
| Small Stone Color / Stoned Phaser, Color on | 0.159, 0.316, 1.778 | −0.63, −0.89, **+2.84** % | −0.45 to +1.40 % | 0.30 to 0.44 % / 1.21 % | −2.58 to −0.20 % / +2.04 to +2.12 % | 0.00 % | −0.00 to +0.00 dB (+6.29 to +6.84) | −0.02 to +0.02 dB |
| Small Stone Color, Rate 0 (0.01 Hz, quasi-static) | 0.010 | −0.03 % | −0.72 % | | +1.26 % / +2.28 % | (set) | +0.01 dB | +0.00 dB |
| Phase 90 script / Phaser Nine, Color 0 (informational) | 0.143, 1.071, 9.756 | +4.9 to +5.3 % | +4.5 to +5.5 % | | | 0.00 % | −0.83 dB | much deeper |

This table is for the complex-response refit of the Small Stone (`compare.txt`). Before it
(`evidence/fit/compare_before_complex.txt`), Color off read notch 1's ends 2.2 to 3.1 % low,
notch 2 at the bottom 4.4 to 4.5 % high and the trajectory 1.24 to 1.25 % rms (3.36 % largest):
the structure lacked the wet highpass and had the weights the wrong way round. Color on read
0.80 to 0.85 % rms (1.53 % largest) with notch 2 at the top +0.73 to +0.87 %; its range now comes
from the complex snapshots instead of a balance of tracked maxima, which halves the trajectory
error, improves the null and the pitch match ("Motion"), and puts notch 2 at the top +2.0 to
+2.3 % high (a trade the previous fit made the other way). Phase 90 is unchanged. Earlier
history, all fitted and measured at 48 kHz only: `evidence/fit/compare_before_rate_fix.txt`.
Rates: the prototype at 48 kHz; the engine renders the same notches at 44.1 to 192 kHz
(`check_phaser.py` P9).

History: Phase 90 block, round 2 (square-root law) against round 3, per rate, both before the
discretisation change (`evidence/fit/traj_before_round3.txt`, `compare_before_round3.txt`,
`compare_before_rate_fix.txt`):

| Rate (Hz) | Sweep ends, worst, before / after | Trajectory rms before / after | Trajectory largest before / after | Depth before / after | Peak before / after |
| --- | --- | --- | --- | --- | --- |
| 0.143 | +0.46 / −0.51 % | 3.87 / 0.72 % | +6.31 / −1.27 % | 0.00 / −0.05 dB | −0.06 / −0.06 dB |
| 0.247 | +0.55 / +0.27 % | 3.84 / 0.72 % | +6.33 / −1.20 % | −0.04 / −0.04 dB | −0.06 / −0.06 dB |
| 1.071 | +0.10 / +0.44 % | 3.61 / 0.69 % | +6.53 / −1.08 % | −0.02 / −0.06 dB | −0.06 / −0.06 dB |
| 1.930 | −0.29 / +0.62 % | 3.15 / 0.62 % | +5.95 / +1.03 % | −0.02 / −0.06 dB | −0.06 / −0.06 dB |
| 3.720 | −1.40 / −0.75 % | 2.10 / 0.56 % | +4.05 / −1.15 % | −0.04 / −0.06 dB | −0.06 / −0.06 dB |
| 9.756 | +1.08 / +0.47 % | 1.15 / 0.38 % | +2.14 / +0.76 % | −0.06 / −0.07 dB | −0.06 / −0.06 dB |

Static response, Small Stone Color on, six frozen frames (sweep bottom, top and four points
between, `evidence/fit/fit_output.txt`), analog model: 0.02 to 0.09 dB rms, worst point 0.30 dB
in the top frame. The same structure fitted with 48 kHz TPT one-poles gave 0.05 to 0.25 dB rms
(worst 1.01 dB at 9.5 kHz), and before round 2 0.52 to 0.79 dB rms.

- Phase 90 block, refitted to the analog response (`fit_output.txt`), meets the round-3 bounds at
  all six rates: trajectory at most 0.70 % rms and 1.18 % at any point (bounds 1.5 % and 3 %),
  sweep ends within 0.75 % (1.5 %), depth within 0.07 dB (0.2 dB), resonance −0.05 dB (0.1 dB),
  period exact. The law's constants moved by under 2 % (RS 933.7 → 916.8 Ω, R1 1786.1 → 1777.7 Ω,
  γ 0.8915 → 0.8952, gate 3.535 → 3.542 Hz; m and κ along their flat directions); the lag moved
  from 11 736 to 14 746 Hz, the same 10.8 µs low-frequency delay now expressed in analog terms.
- **Phase 90 block notch 2** (not a target) is now 1.6 to 3.3 % *above* Guitar Rig 6 at the top
  of the sweep (7188 against 6956 Hz at noon), where the previous spec had it 3.1 to 4.3 % below.
  The previous deficit was the TPT warp, not the structure: at the sweep top the previous equations
  put notch 2 at 6977 Hz at 48 kHz, 6903 at 44.1 kHz and 7313 at 96 kHz against 7439 Hz for the same
  equations in analog, 6.2 %, 7.2 % and 1.7 % low [computed: `spec_check_before_rate_fix.txt`; the
  analog figure by evaluating them with s = j f].
  What is left is the structure: four equal stages with the block feedback put notch 2 at
  2.076·fc (bottom) to 2.115·fc (top) with the lag, a notch-2/notch-1 spacing of 4.61 to 4.71
  (analog, every rate; `spec_check.txt`), where Guitar Rig 6 measures 4.57 to 4.58
  (`targets.tsv`, `gr6_snapshots.tsv`). No sweep law moves the spacing, and β is pinned by the
  resonance. Guitar Rig computes its stages with `a = 2πf/fs − 1` at what measures as 96 kHz
  [decompiled: 0x2a9838], whose own warp narrows the spacing by a few tenths [inferred]; we keep
  the circuit's equal stages.
- Small Stone Color off, refitted on the complex response: wet input highpass at 44.2 Hz, dry
  0.5003 and wet 0.7500 (was 0.75 / 0.50), sweep 326.4 to 9086.3 Hz with a curvature term. Its
  trajectory reads 0.12 to 0.21 % rms (was 1.24 to 1.25 %), notch 1's ends within −0.17 to +0.53 %
  (were 2.2 to 3.1 % low) and notch 2 at the bottom within 0.1 % (was 4.4 % high): what the previous
  fit traded away was the highpass Guitar Rig has on its wet path.
- Phaser Nine at Color 0 is not a script reference: it keeps its modified stage 2, which our
  script (no feedback resistor, four plain stages) does not have. The +5 % is that difference.
- Small Stone Color on: the earlier notch-2 gap (+17 % at the sweep bottom) was the structure, and
  is gone. With the structure fitted in analog terms the two cells that were outside 3 % came in
  (notch 2 at the sweep top from −3.0 % to +0.7 to +0.9 %, the notch-1 minimum at 1.78 Hz from
  +4.2 % to +2.8 %); with the range from the complex snapshots notch 2 at the top reads +2.0 to
  +2.3 % and the notch-1 minimum at 1.78 Hz +2.8 %. That reading is where notch 1 fades below 3 dB
  prominence before the sweep bottom; Guitar
  Rig 6 realises the loop as factored second-order sections, which narrows the tracked sweep at fast
  rates while the TPT chain does not [inferred: analysis transcription,
  `evidence/stoned_color_structure.md`], and the tracker's own bias on this sharp corner is +6.2 %
  at 1.78 Hz (`selftest.txt`), for both readings alike.
- Trajectory tracker self-test (`evidence/fit/selftest_traj.txt`, `trajectory.py selftest`): on
  our own synthetic four-stage phaser with a known per-sample notch, tracked and folded like the
  Guitar Rig takes, smooth Phase 90-like sweeps read within 0.013 to 0.050 % rms and 0.26 % at any
  point (0.14 to 9.76 Hz, with the comparison's free phase shift; without it the error grows to
  0.36 % rms at 9.76 Hz, a common lag that cancels when both devices are tracked alike). Sharp
  Small Stone-like corners read 0.09 % rms at 0.32 Hz and 0.48 % rms, 1.9 % at the corner, at
  1.78 Hz. Our Guitar Rig takes track to the same notch values as the verifier's, sample for sample (four takes compared).
- Multitone tracker self-test (`evidence/fit/selftest.txt`): on a synthetic 4-stage phaser with known
  notch frequencies, smooth sweeps like the Phase 90 read within 0.07 % (min, max) and 0.00 %
  (period) from 0.25 to 9.76 Hz. Sharp RC-triangle turning points like the Small Stone read
  the minimum +1.1 % high at 0.32 Hz and +6.2 % high at 1.78 Hz (window smear), maximum −0.14 %
  and −0.75 %. Its depth floor is below −55 dB (the ideal synthetic phaser reads −55 to −72 dB),
  and it reads the prototype's closed-form −32.6 dB notch exactly, so the Phase 90 depth gap was
  not a tracker floor.

## Shared building block

**The voicings are analog transfer functions.** Each one below is written in the Laplace
variable s with one-pole sections of break frequency f (`(s − w)/(s + w)`, `w/(s + w)`,
w = 2π·f). Guitar Rig 6, the reference, runs these components at what measures as twice the
session rate and its sweep does not move with the session rate (96 kHz against 48 kHz:
0.03 to 0.07 % rms; [measured: `.audit/fx-verify/mod.md` defect 7]), so it is close to analog.
Every fit is now to the analog response, and the discretisation below makes the rendered
notches land on the analog ones at every sample rate.

**Discretisation: TPT one-poles with notch-matched coefficients.** Every one-pole is a TPT
(trapezoidal) lowpass:

```
v  = (x - s) * G                lp = v + s            s = lp + v        (state update)
P(x) = 2*lp - x                 (1 - s/w)/(1 + s/w): +1 at DC, -1 at HF
A(x) = x - 2*lp = -P(x)         the op-amp stage of the Phase 90: -1 at DC, +1 at HF
G  = g / (1 + g)
```

On the unit circle the trapezoidal map is s → j·tan(πf/fs), so a digital one-pole matches an
analog one at one frequency only: the coefficient `warped_g(f, fs, f_m) = tan(π f_m/fs)·f/f_m`
makes a one-pole of analog break f exact (magnitude and phase) at f_m. The old rule, f_m = f ("TPT keeps the break frequency
exact"), kept fc exact but not the notches: with four stages the notches sit well away from
fc, and the warp moved them with the sample rate. At 48 kHz the fit had absorbed the warp,
so the voicings held at 48 kHz only: Small Stone Color off's notch 1 at the sweep top fell
6.3 % at 96 kHz and 7.7 % at 192 kHz, the Phase 90's notch 2 moved by up to 7.3 %, and the
quasi-static notch-1 trajectory moved by up to 2.4 % rms [computed: the old prototype's
equations, `evidence/fit` scratch `before_p9.py`; the verifier measured 3.52 % rms against
Guitar Rig at 96 kHz]. Nothing else in the engine depended on the rate: the LFO, the gate
smoothing (`k = 1 − exp(−2π f/fs)`), the rate smoother and the Color network (poles at 50
and 268 Hz, where the warp is below 0.2 %) are rate-independent.

The stages are no longer forced to share one coefficient. Per sweep position the four stage
coefficients (and the Phase 90 lag) are solved so that the digital notches land on the analog
ones (prototype `stage_g`):

- **pair_g** (closed form): two coefficients g_a < g_b, each used for two stages (order
  a, b, a, b), whose four-stage chain has exactly the analog chain's phase −8·atan(f/fc) at two
  frequencies f_a, f_b. With c = 1/g the condition at one frequency is linear in c_a + c_b and
  c_a·c_b, so it is a 2×2 linear solve and a quadratic. f_b is capped at `MATCH_CAP`·fs = 0.25 fs
  [choice: above about fs/4 a first-order stage cannot follow the analog phase anyway; matching
  there would bend the whole band below. At 44.1 kHz and up no matched notch is capped]. If the
  pair has no real solution (never inside 22.05-192 kHz) both stages take `warped_g` at f_a.
- **Phase 90 script, Small Stone Color off**: pair_g at f_a = tan(π/8)·fc and f_b = tan(3π/8)·fc,
  where four analog stages give −180° and −540°. With the dry path these are the script's notches,
  so both are analog at every rate. Color off's wet highpass moves its notch 1 off f_a near the
  bottom of the sweep only, where the warp is below 10⁻⁴ (Voicing 2), so its notches hold too.
- **Small Stone Color on**: the same pair, then one common factor k on both coefficients so that
  notch 1 of the whole Color loop lands on the analog loop's notch 1 (1-D Newton on the notch
  condition). k = 1 where the analog response has no notch 1 (the bottom of the sweep, below
  about fc = 200 Hz, where k − 1 is under 2·10⁻⁴ anyway).
- **Phase 90 block**: the analog notches n1, n2 of the block loop (with its lag) are found first;
  stage 2 (S2) and the lag take `warped_g` at n1, stages 3 and 4 a pair_g matched at n1 and n2.
  Then three unknowns, stage 1's coefficient, the scale and the spread of the 3/4 pair, are solved
  (3-D Newton) so that both notches land on n1 and n2 and the notch-1 depth equals the analog.
- **The notch condition** at frequency f: `ln|H(f·e^ε)|² − ln|H(f·e^−ε)|² = 0`, ε = `NOTCH_EPS`
  = 10⁻⁴ [choice: a symmetric step small enough that the stationary point it defines is the true
  minimum to about 10⁻⁹ relative, large enough for double precision]. The same condition
  defines the analog notches, so the two agree to first order in ε.
- **Newton** (prototype `newton`, engine `solveNewton`): forward-difference Jacobian (step 10⁻⁷
  in ln g; 10⁻⁵ in ln f for the analog notches), the step scaled down as a whole until no unknown
  moves by more than 0.2, then halved until the residual's sum of squares falls (at most 30
  times), at most 40 iterations, converged below 10⁻¹² [choice: without the line search the
  block's depth condition makes the plain Newton step overshoot at 44.1 and 48 kHz].
- **Continuation**: the table is solved from the sweep bottom up, each entry starting from the
  previous entry's solution and keeping it where its solve does not converge [choice: started
  cold, the block solve at 22.05 kHz finds the mirrored spread and fails on 92 of 257 entries;
  with continuation every entry converges at 22.05-192 kHz].

**Coefficient table.** The solve runs once per voicing in `prepare()` at `TABLE_N + 1` = 257
sweep positions w = −1 … 1 (Phase 90: the gate-smoothed LFO u; Small Stone: the LFO v), each
through the sweep law to the analog fc. The table holds G = g/(1+g) for the four stages and
the lag. Per sample the sweep position is looked up and every G linearly interpolated:
`t = (w + 1)·N/2` clamped to [0, N], `i = min(floor t, N − 1)`, `G = T[i] + (t − i)(T[i+1] − T[i])`
[choice: N = 256 puts the interpolation error of fc below 2·10⁻⁵ relative (0.002 %) on the
widest sweep, Small Stone off; the four tables cost 41 KB and under 10 ms to build]. The engine
and the prototype build the same table; the prototype's `--depth` widens the table range to
±depth/0.5 (Small Stone: ±min(depth/0.5, 1.2)). There is no `tan()` per sample any more; the only
`tan()` calls are in `prepare()`, at matching frequencies clamped to 0.49·fs.

**What it buys, and what it cannot.** Against each voicing's own 48 kHz render
(`check_phaser.py` P9, engine, 44.1 / 88.2 / 96 / 192 kHz): notch-1 trajectory at noon, tracked
like P4, 0.05 % (Phase 90) and 0.17 % / 0.15 % (Small Stone off / on) rms at worst, where the
old structure was 0.7 % and 2.4 % rms (closed form); frozen notches below 10 kHz at five sweep
positions within 0.16 % (old: up to 7.7 %). The frozen magnitude response does not hold to
0.1 dB (old structure: 1.3 to 42 dB on the same measure). Against 192 kHz, 20 Hz to 10 kHz where
the response is within 20 dB of its peak, at sweep positions −1, −0.5, 0, 0.5, 1
[computed: `evidence/fit/d30.py`, `d30.txt`; closed form, it matches the bench row]:

| Voicing | 48 kHz: largest, where | 44.1 kHz: largest | Inside the top 3 dB, 48 / 44.1 kHz | Band of the difference (more than half the largest) |
| --- | --- | --- | --- | --- |
| Phase 90 script | +1.69 dB at 9.9 kHz, sweep top | +2.05 dB | +0.67 / +0.83 dB at 5.5 kHz, top | from 1.8 kHz (bottom) or 6.0 kHz (top) to 10 kHz |
| Phase 90 block | +1.40 dB at 8.1 kHz, top | +1.70 dB | +0.81 / +0.98 dB at 4.6 kHz, top | from 1.8 kHz (bottom) or 4.4 kHz (top) to 10 kHz |
| Small Stone off | +1.02 dB at 7.5 kHz, position +0.5 | +1.24 dB | +0.81 / +0.99 dB at 6.2 kHz, +0.5 | 1.0 to 10 kHz at the bottom, 5.4 to 8.7 kHz at +0.5 |
| Small Stone on | +1.66 dB at 3.5 kHz, top (the resonance) | +2.00 dB | +1.54 / +1.93 dB at 3.4 kHz, top | 2.7 to 4.6 kHz at the top, from 3.5 kHz upward at the bottom |

The lower rate is always the high one, and except for Color on, whose resonance sits in it, the
difference lies on the upper slopes of the response rather than inside its top 3 dB. Row P9
"frozen response" compares every rate against 48 kHz, so it reads the 48-against-192 kHz figure
(1.02 to 1.69 dB, under its 1.8 dB bar); 44.1 kHz against 192 kHz is over 1.8 dB for the script
(2.05) and Color on (2.00). The same limit moves the Color-on resonance at the sweep top by +2.6 %
at 48 kHz (3259 against 3176 Hz analog; peak height within 0.02 dB), where the old rule, exact at
fc, did not; matching at fc instead would put notch 2 1 % off at mid-sweep. Notch positions were
chosen.

Where that error comes from, and what would close it [computed: `evidence/fit/rate_limit.py`,
`rate_limit.txt`; closed form, it reproduces the bench row to the third decimal]:

- **It is the four stages.** Against the analog response the 48 kHz render is off by up to
  1.80 dB (script), 1.46 (block), 1.13 (Color off) and 1.72 (Color on), and 192 kHz by 0.11 dB
  at most, so the row is in effect the 48 kHz error. With the lag and the Color network made
  analog the stages alone give the same or more (block 1.85 dB at the sweep top: the lag's own
  0.46 dB partly cancels it). The Color network's TPT poles add 0.07 dB at most.
- **The 48 kHz chain lags.** At 9.94 kHz, sweep top, the digital wet path lags the analog one by
  5.5° (script), 7.3° (block) and 4.5° (Color on) at 48 kHz, 5.5 to 8.8° at 44.1 kHz. Color off
  leads instead (+8°), because there notch 2 is above the band and its matching point with it.
- **No coefficient choice fixes it.** Four free stage coefficients (and the lag) per sweep
  position and rate, fitted to the analog |H|, bring the row only to 1.45 dB (script), 1.01
  (block), 0.57 (Color off) and 0.69 (Color on), and give up exact notches to do so. Nor does a
  higher-order or complex-pole allpass: a stable allpass section only adds lag, and lag is
  what the chain has too much of.
- **A minimum-phase correction does, at a price.** Phase lead with a flat in-band magnitude needs
  a gain rise above the band (the gain-phase relation of a minimum-phase filter). The four stages
  refitted together with one second-order section on the wet output, outside every loop, whose
  gain above 10 kHz is held to L: L = 0 dB gives 0.18 to 0.94 dB, L = 1 dB 0.22 to 0.52, L = 3 dB
  0.06 to 0.28, L = 6 dB 0.03 to 0.12 (worst per voicing in `rate_limit.txt`). So
  0.1 dB costs a wet-path rise of about 6 dB between 10 kHz and Nyquist, an output filter that
  `docs/FX-ENGINES.md` §4.6.4 excludes, plus a joint eight- or nine-unknown solve per table entry
  in `prepare()`. Not done [choice: a change to the sound above 10 kHz and to the spec, left to a
  decision; oversampling, which would also close it, is ruled out by §4.6.4 for its latency].

Each stage is affine in its input for the current sample (`P(x) = (2G−1)·x + 2(1−G)·s`), so
a feedback loop around any number of stages is solved in closed form, with no unit delay.

Prototype details that are part of the spec: the LFO starts at phase 0, which is the bottom
of the sweep for both voicings (Phase 90 u = −1, Small Stone v = −1); the Phase 90 gate
smoothing is `s += k·(v − s)` with `k = 1 − exp(−2π·f_gate/fs)`, its state starting at the
first LFO value (a different start state changes only the first 0.2 s); `--depth` scales the
sweep about its centre (Phase 90: the smoothed LFO u is multiplied by depth/0.5; Small Stone:
the log half-range, capped at 1.2× [guess: keeps the top below 0.49·fs at 44.1 kHz]); the
command line defaults to 1.0 Hz and 0.32 Hz, noon on Guitar Rig is 1.071 Hz and 0.316 Hz;
`--mix` scales dry by `min(1, 2(1 − mix))` and wet by `min(1, 2·mix)` (Small Stone; Phase 90
uses `(1 − mix)·x + mix·wet`); the output is multiplied by the voicing's make-up gain
("Level", below). Depth and mix are pinned to stock (0.5) in the product.

### Engine behaviour outside the prototype

The prototype has no song position, no bypass and no bad input; the engine adds:

- **Song position** (the processor passes the host's LFO phase for each block's first sample).
  Two things are kept apart: a discontinuity in the host's own phase, and the engine's error
  against it. The engine keeps the host phase and rate of the last block that brought one and
  predicts this block's host phase as `previous + rate × (1 + D) × samples since / fs`, D the rate
  error the lock has learnt (below); the host's phase minus that prediction, wrapped to ±0.5 cycle,
  is the host's step.
  - A step of at least 0.01 cycle, at least 2 ms of LFO time and more than a rate error of 15 % could
    make since the last position (`kJumpCycles`, `kJumpLfoSeconds`, `kJumpDriftRate`; the last
    matters only for long blocks: 0.022 cycle for 1/4 notes at 4096 samples and 44.1 kHz) is a
    **jump**: transport start, locate, a loop that is not a whole number of
    cycles, a note-value change; switching sync on counts as one. [choice: the smallest real jump
    tested is 0.1 cycle, and at the fastest rate, 20 Hz, 2 ms is 0.04 cycle; what must not count is
    below 0.004 cycle: host jitter of ±8 samples is 0.18 ms at 44.1 kHz (0.0036 cycle at 20 Hz),
    and a tempo ramp mispredicts by half the rate change over one block, under 0.003 cycle for
    100 to 140 BPM in 4 s at 1/16 notes and 4096-sample blocks.] The phase is set to the host's
    and the sweep position glides from where it is (any glide still running included, so a second
    jump retargets from the current position) to the new phase's position: offset = current
    position − new position, added to the LFO value and faded out with a raised cosine,
    `0.5 − 0.5 cos(π n/N)`, over N samples counted per sample, so the glide does not depend on the
    host block. N is 10 ms for Phase 90 and Color off and 40 ms for Color on (`kGlideSeconds`)
    [choice, measured with P15 (tones of 82, 147, 220 and 330 Hz at −12 dBFS, jumps of 0.1, 0.25
    and 0.5 cycle from four sweep positions, blocks 16 to 4096 at 44.1 to 192 kHz): Color on
    sweeps its resonance through up to five octaves on a half-cycle jump, and at 10 ms that zipped
    at −31 dBFS once the block was long enough for the glide really to last 10 ms (round-2 N3);
    at 40 ms the worst tick is −81 dBFS. The glide is in the sweep variable, which for the Small
    Stone is log frequency: its fastest point moves π/2 × 4.98 octaves / 40 ms = 196 octaves per
    second on a half-cycle jump, about the LFO's own average sweep speed at 20 Hz (199). Phase 90 glides before
    its gate smoothing, which rounds the step anyway]. Time to within −40 dB of the voicing running
    at the new phase all along, for a half-cycle jump from the sweep bottom, blocks 16 to 4096 at 48
    and 192 kHz: Phase 90 script 172 ms, block 140 ms (the gate smoothing's 45 ms time constant
    dominates), Color off 8 ms, Color on 41 ms, each within 1.1 ms over every block size (P15).
  - Any smaller step is **no jump**: the LFO runs at the tempo's rate and its own error against
    the host is pulled in by bending the rate, never by a glide, with a second-order lock: a phase
    term `(|e| − d)·sign(e) / τ`, τ = 0.15 s (`kLockSeconds`), at most ±3 % of the rate
    (`kLockMaxNudge`), plus an integral D that learns a rate error the host does not report (a
    tempo parameter that lags the host's own), `D += (|e| − d)·sign(e)·Δt / (τ T_i · rate)`,
    T_i = 0.3 s (`kLockIntegralSeconds`, damping 0.71), D within ±10 % (`kLockMaxDrift`); nudge =
    phase term + D × rate. The dead band d = min(0.002 cycle, 0.5 ms of LFO time)
    (`kLockDeadCycles`, `kLockDeadLfoSeconds`) holds for both terms [choice: τ in the middle of 0.1
    to 0.25 s; the dead band takes host jitter of a few samples out of the LFO (±8 samples at
    44.1 kHz is 0.18 ms), so a constant rate error of any size up to 10 % leaves at most 0.002
    cycle standing where the old proportional lock, capped at 3 %, never closed a 5 % one (round-3
    N12). Without the dead band the error goes to zero, but jitter of ±8 samples at 4096-sample
    blocks then moves Color on's 8 Hz sweep enough to show −71 dBFS in P16's difference]. A jump
    keeps D. The nudge is set once per block. P16: song position jittered by ±2 and ±8 samples per
    block, tempo stepped by 1 BPM from 120 to 135, ramped from 100 to 140 BPM over 4 s, 1/4 and 1/16
    notes, 44.1 kHz/64 to 192 kHz/16: no glide, the high-passed difference from an exact host (or
    from the free-running LFO at the same rates) at most −86.4 dBFS, phase error after one second
    at most 0.0056 cycle (the 1/16-note ramp at 4096-sample blocks). P20: a song position 1, 5 and
    10 % faster and 10 % slower than the reported tempo, same notes and settings: no glide, phase
    error after 2 s at most 0.0024 cycle, at most −85.4 dBFS of difference from the voicing running
    free at the true rate (1/4 notes; at 1/16 Color on's sweep shows any LFO offset, 1e-4 cycle as
    −87 dBFS, as in P16).
  The first phase after `reset()` or the start of a model switch is taken directly. Free-running
  behaviour is unchanged (P2).
- **Model and Color switches** (all five voicings, Classic included): the outgoing voicing A keeps
  running on one copy of the input; the incoming one B is reset and runs on another copy, unheard,
  for 30 ms (`kSwitchWarmSeconds`: its states and the estimates below settle). On one held note two
  phasers can sit near antiphase, where every crossfade of the two passes a null that no gain
  repairs, and near it a large correcting gain is itself heard (round 3: blips of −14 to −26 dB
  for up to 5 ms, −25 dBFS off the note). So:
  - **Estimates.** Short-term powers and cross terms (two cascaded one-poles of 2.5 ms,
    `kSwitchStatSeconds`) of the analytic signals of A, B and the input, each from a 90° allpass
    pair (two chains of four first-order sections in z², ±1° from 20 Hz to 20 kHz at 44.1 kHz;
    Niemitalo's coefficients, `kQuadratureA`, `kQuadratureB`; their inputs come in over the
    warm-up, as a step would ring in their slow poles): a held note gives them no ripple at twice its
    frequency, which through the gains would put sidebands beside the note. In two bands split at
    1 kHz by a one-pole (`kSwitchSplitHz`), as `PowerCrossfade` in FxBlocks.
  - **Moment.** After the warm-up the fade waits, as `PowerCrossfade` does, for A and B to correlate
    no worse than −0.5 (`kSwitchMinCorrelation`) and, by the correlation's trend over the fade, not
    to fall below it; after 250 ms (`kSwitchWaitSeconds`) any moment that is good now, after
    280 ms (`kSwitchLongestWaitSeconds`) any moment. Request to fully new: 70 to 390 ms.
  - **Fade.** 40 ms (`kSwitchFadeSeconds`) on a smoother-step ramp r (continuous to the second
    derivative: a half-cycle turn of a held note in 40 ms stays 53 dB under it from 80 Hz off it
    on, where the smooth-step reaches −44 dB), with gains halfway between equal amplitude and equal
    power, √((1 − r) cos(πr/2)) and √(r sin(πr/2)) (−4.5 dB at the middle: where the estimates lag a
    pick attack, two correlated outputs rise 1.5 dB and two uncorrelated ones fall 1.5 dB).
  - **Bridge.** In a band whose two outputs correlate worse than −0.2 (in full from −0.7,
    `kSwitchBridgeFrom`, `kSwitchBridgeFull`, a smooth step between; this need followed through
    two 5 ms one-poles, `kSwitchTurnSmoothSeconds`, as two phasers near resonance can swing from
    in phase to antiphase within a few ms, and so fast a change of the gains is heard beside the
    note) and are both coherent with the input (|A X*|² / (|A|²|X|²) from 0.6 to 0.9,
    `kSwitchBridgeCoherence`: a held note, not a chord through two phasers whose partials turn
    apart; not smoothed, as the bridge's direction is not to be trusted where it falls), the input's
    quadrature pair turned to the phase midway between A and B (the bisector of their directions,
    kept on its side once heard; its weight also falls as sin²(angle/2) as the two come to one
    phase, where there is nothing to bridge and the bisector's normal, reversing, would flip it
    across: without that, a 3 kHz Stone on → off switch put −47 dBFS beside the note) joins with
    weight sin θ, θ = πr, at the level the band should have, and A and B take (1 ± cos θ)/2: with
    B = −A that is cos θ·A + sin θ·R, a turn of constant
    power from A to B instead of a pass through the null. A fade that starts through the bridge in
    the louder band takes 80 ms (`kSwitchTurnSeconds`), so that a 110 Hz note turned by half a
    cycle stays nearer under the notes beside it.
  - **Level.** Each band is scaled so that the power of its mix, predicted from the estimates,
    follows the straight line in dB between the two outputs', softly bounded at +1.4 dB
    (`kSwitchMaxGain` 1.18; was +18 dB). The bound is a trade: a correction inside a 40 ms fade is
    itself a bump of the note of that length, which spreads beyond 40 % of a 200 Hz note (at +6 dB a
    Stone on → off switch put −47 dBFS there where an uncorrected fade puts −69), while a smaller one
    leaves more of a partial cancellation standing (P19 at +1.2 dB: 3.09 dB under the lower level).
  [choice, measured with P19 (every ordered pair of the five voicings, held notes at 110, 256, 368
  and 659 Hz at −12 dBFS and a guitar-like take, rates 0.3 and 1.3 Hz, four LFO phases, followed
  until the output is the incoming voicing's): at most 3.00 dB under the lower and +1.25 dB over the
  higher of the two voicings' own levels (5 ms windows of the note's band; the take in P17's 20 ms
  frames), and off the note at most 0.5 dB over the higher of −60 dBFS, the voicings' own steady
  content and an ideal fade's (equal power on the same ramp, at the engine's own moment and length,
  no correction: on a 110 Hz note a 40 ms fade between two outputs that differ spreads beyond 40 %
  of the note by itself, −55.7 dBFS at worst). Of the round-3 tick probe's 720 switches, 54 put more
  than −60 dBFS beside the note; an ideal fade at the same moments puts 53 there, and in 48 the
  incoming voicing alone does after the switch (Small Stone Color on's own sweep at 200 to 256 Hz,
  −50 dBFS); one (Stone on → off, 200 Hz) is 7.9 dB over the ideal fade's. The round-3 dip probe's
  dips at 2479 Hz into Phase 90 script are the incoming voicing's own: started from reset, its gate
  smoothing primed at the sweep bottom puts its notch on the note for a moment, 44 dB under the
  lowest level its steady sweep reaches (the probe's reference); the output stays 27 dB above it.
  The 40 ms fade of P7 holds].
  Classic's own output is untouched (it is the same code path whenever no switch is in flight). A
  request that arrives before anything of the incoming voicing is heard replaces it (back to the
  outgoing voicing: the switch is dropped); one that arrives during the fade is taken when it ends.
  The engine holds two Phase 90 / Small Stone instances that share one set of coefficient tables,
  and a scratch buffer of four channels of the maximum block size, all allocated in `prepare()`.
- **Reset**: `reset()` clears every stage, lag and Color-network state, the LFO phase (to the
  sweep bottom), the gate smoothing, any glide and the phase lock, any switch in flight (the next
  block starts on the requested model) and the parameter smoothers (next `setParameters()`
  snaps). The processor calls it when a Phase 90 / Small Stone block is switched back on; the
  first block after it is the same as a freshly prepared engine's (P11: bit-identical; silent
  input gives exact silence where the frozen state used to replay at −3.2 dBFS, Color on, to
  −19.4 dBFS, block).
- **Bad input**: a sample that is not finite or whose magnitude is 10⁶ or more (no instrument
  produces it) is replaced by 0 before any model sees it, so it cannot enter a state (P12; one
  pair of ±3·10³⁸ used to latch Classic's `juce::dsp::Phaser` to NaN for good, round-2 N14).
  Everything below is untouched, so Classic stays bit-identical. Recursive states are still
  flushed once per block below 10⁻¹⁵ and dropped above 10⁶.

## Voicing 1: Phase 90

| Item | Value | Source |
| --- | --- | --- |
| Stages | 4 equal first-order allpass (analog; the discretised coefficients differ per stage to keep the notches analog, "Shared building block") | [circuit: https://dafx14.fau.de/papers/dafx14_felix_eichas_physical_modeling_of_the_.pdf section 3.1] |
| Capacitor C | 47 nF | [circuit: https://generalguitargadgets.com/pdf/ggg_p90_bom.pdf, six 0.047 µF; their role as allpass capacitors is read from the count] |
| Fixed resistor | 22 kΩ in parallel with each stage's JFET leg, so the floor is 1/(2π·22k·47n) = 153.9 Hz | [circuit: same parts list, five 22 kΩ; role read from the count] |
| Sweep law | `fc = (1/22k + 1/R(u)) / (2π·C)`, `R(u) = (RS^m + R_ch(u)^m)^(1/m)`, `R_ch(u) = R1·e^(−γ·u)`; u is the smoothed LFO in [−1, 1], +1 the top of the sweep | form [guess], chosen for its shape: Guitar Rig 6's trajectory at 0.14 Hz is straight in log frequency over the lower 85 % of its range with a sharp bottom turn, then bends to about 0.6 of that slope over the last 15 % [measured: evidence/fit/trajectories.tsv p9_block_p010]. An exponential channel resistance gives the straight part; the soft series limit RS gives the bend at the top. Device reading: a JFET's channel resistance climbs steeply towards pinch-off (sweep bottom) and cannot fall below a floor as the gate opens (sweep top) [inferred]. Laws tried on the same trajectories (round-3 exploration, scratch `$W/explore.py`, `explore2.py`; worst rate, each law and its LFO fitted to the trajectories alone): square-root conductance refitted 2.35 % rms / 5.8 % largest; power law of (1 − Vgs/Vp) 1.57 / 4.8 %; linear JFET conductance, R = R_on/(1 − Vgs/Vp) [literature: http://dafx.de/paper-archive/2005/P_155.pdf eq. 9-17], with a series resistor: no physical optimum (series resistance went negative); exponential with a plain series resistor (m = 1) 1.00 / 2.8 % but its top end +2.9 %; m free: this law |
| RS (series limit) | 916.8 Ω | [fit: evidence/fit/trajectories.tsv] |
| R1 (channel at mid-sweep) | 1777.7 Ω | [fit: evidence/fit/trajectories.tsv] |
| γ (channel slope) | 0.8952 per unit of u: R_ch spans e^(2γ) = 6.00× over the full LFO swing | [fit: evidence/fit/trajectories.tsv] |
| m (sharpness of the limit) | 3.876 | [fit: evidence/fit/trajectories.tsv]. Flat direction: the last two fit passes returned 3.737 and 3.876 at the same cost (0.000870), and RS, R1, γ moved by up to 0.65 % with it. fit.py's other start (m = 1) stops in a worse local minimum (cost 4.4× higher) |
| Stock sweep | quasi-static (u = ±1), at every sample rate: fc 931.6 to 3537.1 Hz, notch 1 419.4 to 1589.9 Hz, notch 2 1934 to 7481 Hz (block) [`spec_check.txt`]. Running at noon (1.071 Hz) the gate smoothing narrows notch 1 to 463.7 to 1529.6 Hz [`compare.txt`] | follows from the rows above |
| LFO shape | triangle whose halves are RC charge curves: `s(w) = (1 − e^(−κw))/(1 − e^(−κ))`, w ∈ [0, 1] the position within the half, κ = −0.007, i.e. a straight triangle | [fit: evidence/fit/trajectories.tsv]. The last fit passes return 0.010 and −0.007 at the same cost, so the fit resolves κ only to about ±0.01 and reads the reference's LFO as straight; Eichas' "slightly curved edges" [literature: Eichas, section 4.3] are not visible at this resolution |
| Gate smoothing | one-pole lowpass at 3.542 Hz on the LFO, before the law | [fit: evidence/fit/trajectories.tsv]; sets how the sweep narrows and rounds with rate |
| Rate range | existing `mod_rate`, 0.05 to 10 Hz, log | design choice. Guitar Rig 0.033 to 9.76 Hz [decompiled: Guitar Rig 6 0x2a891c]; AmpliTube 0.1 to 10 Hz [decompiled: AmpliTube 5 0x1788d0c] |
| Knob curve | keep tubamp's log mapping in Hz | design choice. Guitar Rig makes the period piecewise linear in the knob: 30 s at 0, 7 s at 0.1, 1.1 s at 0.4, 0.1025 s at 1 [decompiled: Guitar Rig 6 0x2a89b0]; measured 7.000, 4.050, 0.9337, 0.5181, 0.2688, 0.1025 s at 0.1, 0.25, 0.5, 0.75, 0.9, 1.0 [measured: evidence/fit/targets.tsv]. Noon is 1.071 Hz |
| Mix | dry 0.5, wet 0.5, fixed, then the make-up gain ("Level") | [guess]: Eichas figure 4 sums dry and wet through two resistors at Q5 but gives no values; Guitar Rig uses 0.5 and 0.5 [decompiled: Guitar Rig 6 0x2a8bcc], AmpliTube 1.0 and 1.0 [decompiled: AmpliTube 5 0x13d9218] |
| Script | no feedback: `wet = A⁴` | [circuit: Eichas section 3.1, the 1974 version has no feedback resistor] |
| Block node | the block-logo feedback resistor (R15 in Eichas' schematic) from the stage-4 output to the inverting input of the stage-2 op-amp (R6/R7 junction) | [circuit: Eichas figure 4, R15] |
| Block ratio β | 0.3957 = op-amp feedback resistor / block-logo feedback resistor. With a 10 kΩ op-amp feedback resistor that is a fitted equivalent of 25.3 kΩ for the feedback resistor: a fitted value, not a schematic value | [fit: evidence/fit/targets.tsv], to the measured resonance. No opened source gives the feedback resistor's value: Eichas gives no component values, and the GGG parts list numbers its parts differently from Eichas (its R28 is 56 kΩ, its R15 22 kΩ), so it does not identify the part. The 10 kΩ is unsourced too [guess]; only the ratio β enters the equations |
| Block equations | stage 2 becomes `S2 = ((1+β)w − 1)/(w + 1)` and stages 3, 4 sit in the loop: `wet = A · S2 · A² / (1 + β·L·A²)` | follows from the node and an ideal op-amp; Guitar Rig has the same form [decompiled: Guitar Rig 6 pool 0x2be05b0, 0x2a9050] |
| Feedback lag L | one-pole lowpass at 14 746 Hz in the feedback-resistor path (analog; a 10.8 µs delay at low frequencies) | [fit: evidence/fit/targets.tsv], to the median notch depth along the fitted sweep, now in the analog response (the discretisation keeps the notch-1 depth analog). The old value, 11 736 Hz, was the same 10.8 µs low-frequency delay as a TPT one-pole at 48 kHz. It stands in for what fills Guitar Rig's notch: a one-sample delay in its loop [decompiled: Guitar Rig 6 0x2a968c, 0x2a98e4] at what measures as a 96 kHz internal rate (10.4 µs). Its notch fills in as it rises, −30.3 dB near 510 Hz to −27.2 dB near 1380 Hz [measured: Phaser Nine Rate 0.5 take, binned per frame], which a gain imbalance cannot do. In a pedal the same role falls to the op-amp's bandwidth [guess]. Script mode has no feedback, so L does nothing there |
| Block result | quasi-static sweep ends, without the make-up [`spec_check.txt` less 1.50 dB]: resonance +3.52 dB at the bottom to +3.43 dB at the top (48 kHz; 192 kHz +3.38, analog +3.37), DC −1.32 dB, notch 1 at 0.4502·fc (bottom) to 0.4495·fc (top), −30.68 dB to −26.77 dB, notch 2 at 2.076·fc to 2.115·fc, notch-2/notch-1 spacing 4.61 to 4.71, the same at every sample rate. Notch-2 depth is not matched: −16.8 dB (bottom) to −20.0 dB (top) at 48 kHz, −21.7 dB analog at the top | closed form of the equation above; Guitar Rig 6 +3.52 dB peak, median notch depth −28.9 dB [measured: evidence/fit/targets.tsv], DC −1.4 dB, spacing 4.58 [measured: evidence/gr6_snapshots.tsv]. Without L the loop gives −32.60 dB at every frequency (analog) |
| Sample rate | the notches, the notch-1 depth and so the sweep are the same at every rate (P9: trajectory within 0.05 % rms, notches within 0.001 % between 44.1 and 192 kHz); the response above about 3 kHz is not (up to 1.7 dB, "Shared building block") | notch-matched discretisation |
| Saturation | none in v1 | both commercial models are linear: harmonics of a −3 dBFS sine below −150 dB relative [measured: evidence/gr6_thd.tsv, verification.md C09]; no signal term in AmpliTube's JFET resistance [decompiled: AmpliTube 5 0xd8984c]. The hardware does distort [literature: Eichas, section 3.1] |
| Level | make-up gain ×1.3152 (+2.38 dB) script, ×1.3137 (+2.37 dB) block | [choice: unity loudness against bypass on guitar, averaged over registers; see "Level" below]. Guitar Rig adds ×1.862 (+5.4 dB) [decompiled: Guitar Rig 6 0x2a8e5c]; ours stays 3.0 dB (block) below it |
| Filtering | none | Guitar Rig's 16.4 kHz lowpass is not reproduced; a linear TPT phaser cannot alias |
| Oversampling | none | see "Shared building block"; the discretisation, not oversampling, keeps the notches analog |
| Stereo | one LFO for both channels | Guitar Rig's Rotate defaults to zero offset [decompiled: Guitar Rig 6 0x2a92b8] |
| Player controls | Speed (`mod_rate`), Script/Block switch | the pedal has one knob; Depth and Mix are pinned to stock |

Block-logo loop, per sample (`G1 … G4`, `GL` the table's coefficients at this sample;
`a3 = 1 − 2G3`, `a4 = 1 − 2G4`, `k3 = −2(1−G3)·s3`, `k4 = −2(1−G4)·s4`; `sl` the lag state):

```
v1 = A1(x)                                       ; stage 1, G1
s2 = (1 + beta)*v1 - (2 + beta)*lp2(v1)          ; stage 2's lowpass, G2
y4 = (a4*(a3*(s2 - beta*(1 - GL)*sl) + k3) + k4) / (1 + beta*GL*a3*a4)
l  = lpL(y4)                       ; = GL*y4 + (1 - GL)*sl, then update sl
u  = s2 - beta*l ;  y3 = A3(u) ;  y4 = A4(y3)
out = M*(0.5*x + 0.5*y4)                         ; M the make-up gain
```

Why the earlier voicing missed: the gate smoothing (then 6.0 Hz) and the sweep ends had been
fitted to single-frame extremes of a jittery tracker (444 to 1544 Hz at noon, 712 to 1157 Hz
at the fastest rate), which read the sweep wider than it is. A 6 Hz smoothing narrows the
sweep too little as the rate rises, so the error grew with rate (−0.9/+1.2 % at 0.14 Hz,
−7.9/+4.4 % at 3.7 Hz, −5.7/+7.8 % at 9.8 Hz; `evidence/fit/compare_before.txt`). The first refit
moved it to 4.41 Hz and rescaled the then square-root law. Round 2 then matched the sweep ends
within 1.4 %, but between them its square-root law missed by up to 6.6 % (3.1 to 3.9 % rms up to
1.9 Hz, `traj_before_round3.txt`): it was above Guitar Rig 6 in the lower half of the range and
below it in the upper half, because a square-root conductance on a curved triangle bends the
whole log-frequency path, while Guitar Rig's is straight until near the top. Round 3 fitted the
whole trajectory, not the extremes, with the law above (the LFO curvature and the gate smoothing
refitted with it); the worst point is now 1.27 % and the ends moved closer, not further.

## Voicing 2: Small Stone

No primary circuit source for the Small Stone could be opened, so these constants are fitted
to Guitar Rig 6's "Stoned Phaser". The Color-on structure follows what its code does
(`evidence/stoned_color_structure.md`); the constants are ours, fitted to measurements.

| Item | Color off | Color on | Source |
| --- | --- | --- | --- |
| Stages | 4, one break frequency | 4, one break frequency | four stages as two second-order sections, all with one `g` [decompiled: Guitar Rig 6 0x6985b4, 0x698600]; exactly two notches measured [measured: verification.md, Part 1]. Our stage coefficients differ per stage only to keep the notches analog ("Shared building block") |
| Sweep law | `ln fc = (ln lo + ln hi)/2 + ln(hi/lo)/2 · v + curve·(v² − 1)`, v in [−1, 1]: exponential, plus a curvature that leaves the ends at lo and hi | exponential (curve 0) | form: `centre · 32^x` [decompiled: Guitar Rig 6 0x2d9774, 0x2d9f7c]; log-f triangle fits the track (R² 0.993 against 0.85 for linear f) [measured: verification.md C12]. Color off curve = 0.0128 [fit: evidence/fit/snapshots.tsv]: against the break frequency of 277 complex snapshots the pure exponential leaves a systematic error, +0.8 % at both turning points and −0.4 to −0.6 % in mid-sweep, the same at 0.16, 0.32 and 1.78 Hz; the curvature term takes the rms from 0.43 % to 0.18 %, and the pitch deviation at 1.78 Hz from +16 % to +13.6 % of Guitar Rig's. Fitted the same way, Color on's curvature is 0.003 and does nothing, so Color on keeps the plain law |
| Sweep range | 326.4 Hz to 9086.3 Hz | 98.5 Hz to 3105.4 Hz | Both [fit: evidence/fit/snapshots.tsv] by `fit.py`: the break frequency of every complex snapshot (structure fixed), then the law at κ = 0.702 with one start phase per take (Guitar Rig's LFO starts within 0.001 cycle of 0). Color off: three takes (0.16, 0.32, 1.78 Hz), 0.18 % rms; was 347.7 to 8931.5 Hz from the notch-1 trajectories under the wrong structure, and before that 355 / 8300 Hz (48 kHz only). Color on: two takes (0.16, 0.32 Hz), 0.27 % rms, where the previous range (99.4 to 3068.3 Hz: the frozen sweep-bottom frame and a balance of tracked notch maxima, `fit.py --range-from-tracks`) read 0.89 % rms at start phase 0 (fit.py run before the change; the new range reads 0.63 % at phase 0). Checked against the notch trajectories (P4): Color off 0.12 to 0.21 % rms (was 1.24 to 1.25), Color on 0.30 to 0.44 % rms (was 0.80 to 0.85) |
| LFO shape | RC triangle, κ = 0.702 (the same formula as the Phase 90's) | same | [fit: evidence/fit/trajectories.tsv, the Color-off rows, with the previous sweep range]; the complex snapshots, κ free, give 0.704 at the same cost (its flat direction), so it was kept. It was 0.70 before that [fit: evidence/phasis_tracks.tsv column stoned_phaser; original script not kept], best of the shapes tried, R² 0.9998 [measured: verification.md C12]. Guitar Rig's LFO code has no Color branch [decompiled: Guitar Rig 6 0x6983fc-0x698558], so Color on uses the same κ |
| Rate range | existing `mod_rate` | same | Guitar Rig 0.01·1000^knob Hz: 0.1585, 0.3162, 1.778 Hz at 0.4, 0.5, 0.75 [measured: evidence/fit/targets.tsv]. Noon is 0.316 Hz. Color does not change the rate [decompiled: Guitar Rig 6 0x611e80-0x611fa4; measured period error 0.00 %] |
| Feedback | none | c = 0.7548 around all four stages, through the network's feedback branch F, no delay | [fit: evidence/fit/targets.tsv]. Taken from the fourth stage, injected before the first [decompiled: Guitar Rig 6 0x6983a4, 0x6985a0] |
| Color network poles | none | f_p1 = 49.8 Hz, f_p2 = 267.9 Hz, shared by both branches | [fit: evidence/fit/targets.tsv]. One second-order network with two outputs on a shared denominator [decompiled: Guitar Rig 6 0x6980b0-0x6982e4] |
| Feedback branch F | none | `s (s + w_zf) / ((s + w_p1)(s + w_p2))`, f_zf = 12.0 Hz: zero at DC, unity well above 300 Hz | [fit: evidence/fit/targets.tsv]. Guitar Rig forms it as `1 − z⁻¹·BB` with BB unity at DC [decompiled: Guitar Rig 6 0x69859c-0x6985a8] |
| Wet input highpass | first order, `s / (s + w_hp)`, f_hp = 44.2 Hz, before the stages | (the input branch Ha below) | [fit: evidence/fit/snapshots.tsv]: with it the complex snapshots fit to −46.7 dB re \|H\|, without it −14.8 dB. Guitar Rig's input branch reduces to a first-order highpass near 44 Hz with gain 1.5 on the wet path when Color is off [decompiled: `evidence/stoned_color_structure.md`]; the gain is the 0.75 below. It moves notch 1 up near the bottom of the sweep (148.7 Hz at f_lo, 0.456·fc instead of tan(π/8) = 0.414) and takes the wet out at DC (−5.5 dB DC gain, the dry alone). TPT one-pole, exact at its pole |
| Input branch Ha | none | `(s + w_za)(s + w_zb) / ((s + w_p1)(s + w_p2))`, f_za = 12.7 Hz, f_zb = 862.1 Hz: unity at HF, +10.2 dB shelf below about 270 Hz, cut below 50 Hz. Wet path only | [fit: evidence/fit/targets.tsv]. In Guitar Rig the dry is the unfiltered input [decompiled: Guitar Rig 6 0x6983b0] |
| Mix | dry 0.5003, wet 0.7500, then ×1.0186 (+0.16 dB) | dry 0.1318, wet 0.4395, then ×0.9237 (−0.69 dB) | Color off: [fit: evidence/fit/snapshots.tsv], from the phase: the earlier fit to the median notch depth and peak gave dry 0.749, wet 0.501, the same magnitudes with the weights the wrong way round (section "Motion"). Color on: [fit: evidence/fit/targets.tsv]; the complex snapshots give dry/wet 0.2980 against 0.2999. Make-up: [choice], "Level" below |
| Saturation | none in v1 | none in v1 | harmonics below −120 dB [measured: evidence/gr6_thd.tsv]. The OTA stage is a tanh device [literature: http://dafx.de/paper-archive/2005/P_155.pdf, eq. 1-8] |
| Player controls | Rate (`mod_rate`), Color switch | | the pedal has one knob and one switch |

### Color on in words

The four stages are the same as with Color off; one LFO moves all of them together over a
lower range. Two things are added, both built from one small network with two poles (50 Hz and
268 Hz):

1. The wet path's input is shaped by the network's input branch Ha: low frequencies (about 50
   to 300 Hz) are lifted by about 10 dB relative to the highs, and the deep bass below 50 Hz is
   cut. The dry path is not filtered.
2. The output of the fourth stage is fed back, with gain c, to the input of the first stage,
   through the network's feedback branch F. F passes everything above a few hundred hertz and
   rolls off towards DC as a second-order highpass, so the resonance acts on the mids and highs
   but the loop has no DC gain.

The mix becomes wet-heavy: 0.4395 of the loop output plus 0.1318 of the dry input.

```
H(s) = dry + wet · Ha(s) · P(s)⁴ / (1 − c · F(s) · P(s)⁴)
P(s) = (1 − s/w_c) / (1 + s/w_c)          w_c = 2π·fc, fc from the LFO
Ha(s) = (s + w_za)(s + w_zb) / ((s + w_p1)(s + w_p2))
F(s)  = s (s + w_zf)       / ((s + w_p1)(s + w_p2))
```

Discretisation: the four stages take their coefficients from the table ("Shared building
block"). The Color network's one-poles are TPT lowpasses `lp = G·(in − s) + s`, `s ← 2·lp − s`,
`G = g/(1 + g)`, `g = tan(π·f/fs)` (each exact at its own pole; below 270 Hz the warp is under
0.2 % at 44.1 kHz). Each branch is two first-order sections in series, so each is two one-poles:

```
Ha:  xa = x  − ka·lp_p1a(x)       ka = 1 − f_za/f_p1      (shelf: f_za/f_p1 at DC, 1 at HF)
     xs = xa − kb·lp_p2a(xa)      kb = 1 − f_zb/f_p2      (shelf: f_zb/f_p2 at DC, 1 at HF)
F:   h  = y4 − lp_p1f(y4)                                  (highpass at f_p1)
     e  = h  − kf·lp_p2f(h)       kf = 1 − f_zf/f_p2      (shelf: f_zf/f_p2 at DC, 1 at HF)
```

The loop is solved per sample without a delay. Each stage is affine in its input
(`P_k(u) = p_k·u + 2(1 − G_k)·s_k`, `p_k = 2G_k − 1`), so the chain is `y4 = p⁴·u + K` with
`p⁴ = p1·p2·p3·p4` and `K` the four stage states pushed through the chain
(`K = 0; K = p_k·K + 2(1 − G_k)·s_k` for k = 1 … 4). F is affine in y4 for the current sample:
`F(y4) = aF·y4 + bF` with `aF = (1 − G1)(1 − kf·G2)` and
`bF = −(1 − kf·G2)(1 − G1)·s1f − kf(1 − G2)·s2f` (G1, G2 for f_p1, f_p2; s1f, s2f the feedback
branch states). Then:

```
y4  = (p⁴·(xs + c·bF) + K) / (1 − c·aF·p⁴)
e   = F(y4)          ; run the two feedback one-poles on y4, updating s1f, s2f
u   = xs + c·e ;  y4 = P4(P3(P2(P1(u))))   ; updates the stage states
out = M·(dry·x + wet·y4)                 ; M the make-up gain
```

`c·aF·|p⁴| ≤ 0.755` always, so the divisor never reaches zero. Guitar Rig puts one sample of delay
into the filtered part of its loop; we do not, and the fitted constants absorb the difference.

Color off, per sample: `u = x − lp_hp(x)` (the wet input's highpass, one TPT one-pole), then the
four stages, `out = M·(dry·x + wet·y4)`. Its notch 2 at the sweep bottom is now 803 Hz against
Guitar Rig's 805 Hz, a notch ratio of 5.40 (the previous structure, without the highpass, had
840 Hz and the ideal 5.83). The pair design of the stages ("Shared building block") still
matches the four analog stages at tan(π/8)·fc and tan(3π/8)·fc; the highpass moves notch 1 off the
first of those points only near the bottom of the sweep, below 400 Hz, where the stages' warp is
under 10⁻⁴, so the rendered notches stay on the analog ones (P3, P9).

## Level

**Rule [choice]: every Phase 90 and Small Stone voicing is unity loudness against bypass on guitar
material, averaged over registers.** The make-up is the negated mean, over three guitar-like takes
(`refmeasure.guitar`: a mixed take, a low riff on 82 to 147 Hz and a high-register take on 330 to
784 Hz, 12 s each), of the ungated ITU-R BS.1770 K-weighted loudness of the output against the
input, at the voicing's starting rate, rounded to 0.01 dB (`check_phaser.py` P18, bar ±0.25 dB).
Why this and not the reference's level: no hardware was measured, so "the pedal's level" is only
Guitar Rig's, and Guitar Rig's own make-up is a choice of its own (Phaser Nine ×1.862, about
+3 dB over bypass on guitar; its Stoned Phaser is within 0.7 dB of unity). A phaser that jumps in
level when engaged, or between its five voicings, gets judged on the jump. Classic stays as it
was (frozen).

| Voicing | Before (make-up) and mixed / low / high, register mean | Make-up now | Now: mixed / low / high, register mean |
| --- | --- | --- | --- |
| Phase 90 script | +2.70 dB: +0.59 / +1.73 / −1.36, **+0.32 dB** | ×1.3152 (+2.38 dB) | +0.27 / +1.40 / −1.68, 0.00 dB |
| Phase 90 block | +1.50 dB: −1.01 / −0.50 / −1.10, **−0.87 dB** | ×1.3137 (+2.37 dB) | −0.15 / +0.37 / −0.24, −0.01 dB |
| Small Stone off | +0.50 dB: +0.42 / +1.13 / −0.59, **+0.32 dB** (old structure) | ×1.0186 (+0.16 dB) | +0.12 / +0.74 / −0.86, 0.00 dB |
| Small Stone on | 0 dB: +1.51 / +2.85 / −2.32, **+0.68 dB** | ×0.9237 (−0.69 dB) | +0.83 / +2.16 / −3.01, 0.00 dB |

(The previous rule took the mean of pink noise and a plucked riff at 0.1 dB and skipped make-ups
under 0.25 dB, so on guitar no voicing was within its own figures across registers; round-2 P6,
N8.) What one fixed gain cannot remove is the register dependence, which is the voicing: the
Color-on bass shelf lifts a low riff by +2.2 dB and leaves a high take −3.0 dB; Phase 90 script
spans +1.4 to −1.7 dB, block +0.4 to −0.2 dB. Against Guitar Rig the level is now: Phaser Nine
about 3.0 dB louder than our block (it was 3.9), Stoned Phaser Color off 0.16 dB quieter, Color on
0.69 dB louder.

**Headroom.** Nothing saturates and there is no limiter: a full-scale input can leave the engine
above 0 dBFS. Worst peak of the output for a full-scale input (squares of 41.2, 82.4 and 110 Hz,
sines of 97 Hz to 3.3 kHz, a guitar-like take peaking at 0 dBFS; starting rate and 10 Hz; P18):
Phase 90 script 1.32 (+2.4 dB, 1.3 kHz sine), block 1.97 (+5.9 dB, 1.3 kHz sine, the resonance),
Small Stone off 1.93 (+5.7 dB, 41 Hz square), Small Stone on 4.77 (**+13.6 dB**, 82 Hz square at
10 Hz). The processor's documentation should say that the phaser needs up to 14 dB of headroom
after it (6 dB for every voicing but Small Stone Color on), and that the chain after it sees these
peaks.

## Peaks on a low square wave

Small Stone Color on peaks well above its input on a low square wave: the input branch lifts the
bass by 10 dB and the resonant loop rings on every edge. Same signal through Guitar Rig 6 (black
box, `auhost`, the study's Color-on state at 0.316 Hz) and the engine, peak and rms of the output
against the input after the first second, −6 dBFS squares (scratch `squares.py`; ours with and
without its −0.69 dB make-up):

| Signal | Guitar Rig 6 | ours (make-up removed) | ours |
| --- | --- | --- | --- |
| 82.4 Hz square | +12.72 / +2.24 dB | +13.00 / +2.24 dB | +12.31 / +1.55 dB |
| 41.2 Hz square | +9.20 / +0.84 dB | +9.10 / +0.85 dB | +8.41 / +0.16 dB |
| 110 Hz square | +12.20 / +2.56 dB | +12.51 / +2.56 dB | +11.82 / +1.87 dB |

The reference does the same within 0.3 dB, so the peak is the voicing, not an error in ours
[measured]. The +14.1 dB the verifier read was a full-scale 82 Hz square at another rate and
LFO position; the model is linear, so the level does not change the ratio.

## Mapping to tubamp parameters

| Parameter | Phase 90 | Small Stone |
| --- | --- | --- |
| `mod_rate` (existing) | Speed, Hz | Rate, Hz |
| `mod_phaser_model` (new) | 1 | 2 |
| `mod_phaser_color` (new) | Off = script, On = block | Color |
| `mod_depth`, `mod_mix` (existing) | not read; UI greys them | not read |

The prototype accepts `--depth` and `--mix` (0.5 = stock) for experiments only. Stock is pinned
because the generic defaults (depth 0.4, mix 0.35) are not the stock pedal.

Audition files (`prototype/audition.py`, copies in `audition/`, levels in `audition/peaks.txt`):
noise and a plucked-string riff through each voicing, plus the dry riff and noise.
