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
- `[fit: evidence/fit/targets.tsv]`, `[fit: evidence/fit/trajectories.tsv]`: fitted to Guitar Rig 6
  by `evidence/fit/fit.py`, which regenerates it from that file (a rerun lands within 0.2 %,
  optimizer tolerance; flat directions are named where they occur). Two older Small Stone values
  name their own file; their original fit scripts were not kept, and `fit.py` now re-derives them
  from `trajectories.tsv` as a check.
- `[guess]`: neither; the reason for the choice is given.
- `[choice]`: a design decision of ours (discretisation, make-up, glide), with its reason and,
  where it rests on one, the measurement.
Structural statements keep the `[literature]`, `[decompiled]` and `[measured]` tags of
`tools/re/README.md`.

## Measured accuracy

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
| Small Stone / Stoned Phaser, Color off | 0.159, 0.316, 1.778 | −3.13 to −2.42 % | −2.28 to −2.24 % | 1.24 to 1.25 % / 3.36 % | (n2 max leaves the 12 kHz band) +4.4 to +4.5 % at the bottom | 0.00 % | 0.00 dB (+1.94) | +0.04 to +0.09 dB (−12.1) |
| Small Stone Color / Stoned Phaser, Color on | 0.159, 0.316, 1.778 | −0.64, −0.90, **+2.78** % | −1.29 to +0.25 % | 0.80 to 0.85 % / 1.53 % | −1.85 to −0.27 % / +0.73 to +0.87 % | 0.00 % | −0.00 to +0.01 dB (+6.29 to +6.84) | +0.03 to +0.06 dB |
| Small Stone Color, Rate 0 (0.01 Hz, quasi-static) | 0.010 | −0.00 % | −0.89 % | | +1.46 % / +1.36 % | (set) | +0.01 dB | +0.17 dB |
| Phase 90 script / Phaser Nine, Color 0 (informational) | 0.143, 1.071, 9.756 | +4.9 to +5.3 % | +4.5 to +5.5 % | | | 0.00 % | −0.83 dB | much deeper |

This table is for the notch-matched discretisation and the analog refit (`compare.txt`); the
previous spec's numbers, all fitted and measured at 48 kHz only, are in
`evidence/fit/compare_before_rate_fix.txt` (trajectory rms 0.38 to 0.72 % block, 1.89 to 1.90 %
Color off, 1.08 to 1.12 % Color on; Color-on notch-2 top −3.0 %, notch-1 minimum at 1.78 Hz
+4.19 %). Every voicing's trajectory got closer to Guitar Rig 6. Color off traded a range offset
for shape: notch 1's ends now sit 2.2 to 3.1 % low (they were 0.6 to 1.1 % low) while the whole
trajectory is a third closer; its remaining error is a structure Guitar Rig has and we do not
(below). Rates: the prototype at 48 kHz; the engine renders the same notches at 44.1 to 192 kHz
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
- Small Stone Color off, now refitted (f_lo, f_hi, κ from the three Color-off trajectories,
  applied this time because the old 48 kHz fit no longer holds once the warp is gone): 1.24 to
  1.25 % rms (was 1.9 %). Its notch 1's ends sit 2.2 to 3.1 % low and notch 2 at the bottom 4.4 %
  high: the shape of Guitar Rig's Color-off trajectory, a first-order highpass near 44 Hz on its
  wet path (below), is not in our structure, and the least-squares fit trades the ends for the
  middle.
- Phaser Nine at Color 0 is not a script reference: it keeps its modified stage 2, which our
  script (no feedback resistor, four plain stages) does not have. The +5 % is that difference.
- Small Stone Color on: the earlier notch-2 gap (+17 % at the sweep bottom) was the structure, and
  is gone. With the structure fitted in analog terms the two cells that were outside 3 % are in:
  notch 2 at the sweep top went from −3.0 % to +0.7 to +0.9 %, which the previous spec could not
  close at 48 kHz ("Guitar Rig 6 runs this component at twice the session rate": the analog fit is
  what its 96 kHz response is close to), and the notch-1 minimum at 1.78 Hz went from +4.2 % to
  +2.8 %. That reading is where notch 1 fades below 3 dB prominence before the sweep bottom; Guitar
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
  where four analog stages give −180° and −540°. With the dry path these are the notches, so both
  notches are analog at every rate.
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
0.1 dB: four first-order sections at 44.1 or 48 kHz cannot follow an analog chain's phase near
10 kHz (the best four free real coefficients still miss it by 4 to 6° at 10 kHz), so the
responses differ by 1.1 dB (Small Stone off) to 1.7 dB (Phase 90 script) at the top of the sweep,
on the slopes above 3 kHz, between 48 and 192 kHz (old structure: 1.3 to 42 dB on the same
measure). The same limit moves the Color-on resonance at the sweep top by +2.7 % at 48 kHz
(3288 against 3203 Hz analog; peak height within 0.01 dB), where the old rule, exact at fc, did
not; matching at fc instead would put notch 2 1 % off at mid-sweep. Notch positions were chosen.
Closing the rest needs oversampling, which `docs/FX-ENGINES.md` §4.6.4 rules out (latency).

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

- **Song-position jumps** (transport start, locate, loop, a sync-value change). The phase is set
  to the host's, but the sweep position glides from where it was to the new phase's position:
  offset = (old position + any glide still running) − new position, added to the LFO value and
  faded out with a raised cosine, `0.5 − 0.5 cos(π n/N)`, over N = 10 ms of samples
  (`kGlideSeconds`) [choice: the worst case, a full bottom-to-top jump of Small Stone Color on with a
  220 Hz tone at −12 dBFS, leaves −70.5 dBFS above 2 kHz at 10 ms against −64.3 at 5 ms and
  −35.6 at 3 ms; 10 ms is the shortest with 10 dB in hand under the −60 dBFS bar, and still a
  fifth of the fastest LFO period, 50 ms at 20 Hz]. Before: −16.4 dBFS (Color on) and −20.3 dBFS
  (Color off) on the same signal (P10). The Phase 90 glides too, before its gate smoothing.
  Offsets below 10⁻⁶ of the sweep range (`kGlideThreshold`; the per-block rounding of a running
  transport) are taken directly [choice: a 10⁻⁶ step is 2·10⁻⁶ in fc]. The first phase after
  `reset()` or a model switch is taken directly. Free-running behaviour is unchanged (P2).
- **Reset**: `reset()` clears every stage, lag and Color-network state, the LFO phase (to the
  sweep bottom), the gate smoothing, any glide, the switcher (next block starts on the requested
  model) and the parameter smoothers (next `setParameters()` snaps). The processor calls it when a
  Phase 90 / Small Stone block is switched back on; the first block after it is the same as a
  freshly prepared engine's (P11: bit-identical; silent input gives exact silence where the frozen
  state used to replay at −3.2 dBFS, Color on, to −19.4 dBFS, block).
- **Non-finite input**: a NaN or infinite input sample is replaced by 0 before any model sees it,
  so it cannot enter a state (P12; Classic's `juce::dsp::Phaser` used to latch on one). Finite
  input is untouched, so Classic stays bit-identical. Recursive states are still flushed once per
  block below 10⁻¹⁵ and dropped above 10⁶.

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
| Level | make-up gain ×1.365 (+2.70 dB) script, ×1.189 (+1.50 dB) block | [choice: unity loudness when engaged, as the pedals are designed to be; see "Level" below]. Guitar Rig adds ×1.862 (+5.4 dB) [decompiled: Guitar Rig 6 0x2a8e5c]; ours stays 3.9 dB (block) below it |
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
| Sweep law | exponential: `fc = sqrt(lo·hi) · (hi/lo)^(v/2)`, v in [−1, 1] | same | form: `centre · 32^x` [decompiled: Guitar Rig 6 0x2d9774, 0x2d9f7c]; log-f triangle fits the track (R² 0.993 against 0.85 for linear f) [measured: verification.md C12] |
| Sweep range | 347.7 Hz to 8931.5 Hz | 99.4 Hz to 3068.3 Hz | Color off: [fit: evidence/fit/trajectories.tsv] by `fit.py` (`ss_off_fit`: notch 1 = tan(π/8)·fc, least squares on the three Color-off trajectories with f_lo, f_hi and κ free; three starts land on the same values). Before the discretisation change these were 355 / 8300 Hz from an older 48 kHz fit (original script not kept), which held at 48 kHz only. Color on: [fit: evidence/fit/targets.tsv]: f_lo is the break frequency of the frozen sweep-bottom frame, f_hi the value that centres the notch-1/notch-2 maximum errors (see "Measured accuracy") |
| LFO shape | RC triangle, κ = 0.702 (the same formula as the Phase 90's) | same | [fit: evidence/fit/trajectories.tsv, the Color-off rows, with the sweep range]; it was 0.70 [fit: evidence/phasis_tracks.tsv column stoned_phaser; original script not kept], best of the shapes tried, R² 0.9998 [measured: verification.md C12]. Guitar Rig's LFO code has no Color branch [decompiled: Guitar Rig 6 0x6983fc-0x698558], so Color on uses the same κ |
| Rate range | existing `mod_rate` | same | Guitar Rig 0.01·1000^knob Hz: 0.1585, 0.3162, 1.778 Hz at 0.4, 0.5, 0.75 [measured: evidence/fit/targets.tsv]. Noon is 0.316 Hz. Color does not change the rate [decompiled: Guitar Rig 6 0x611e80-0x611fa4; measured period error 0.00 %] |
| Feedback | none | c = 0.7548 around all four stages, through the network's feedback branch F, no delay | [fit: evidence/fit/targets.tsv]. Taken from the fourth stage, injected before the first [decompiled: Guitar Rig 6 0x6983a4, 0x6985a0] |
| Color network poles | none | f_p1 = 49.8 Hz, f_p2 = 267.9 Hz, shared by both branches | [fit: evidence/fit/targets.tsv]. One second-order network with two outputs on a shared denominator [decompiled: Guitar Rig 6 0x6980b0-0x6982e4] |
| Feedback branch F | none | `s (s + w_zf) / ((s + w_p1)(s + w_p2))`, f_zf = 12.0 Hz: zero at DC, unity well above 300 Hz | [fit: evidence/fit/targets.tsv]. Guitar Rig forms it as `1 − z⁻¹·BB` with BB unity at DC [decompiled: Guitar Rig 6 0x69859c-0x6985a8] |
| Input branch Ha | none | `(s + w_za)(s + w_zb) / ((s + w_p1)(s + w_p2))`, f_za = 12.7 Hz, f_zb = 862.1 Hz: unity at HF, +10.2 dB shelf below about 270 Hz, cut below 50 Hz. Wet path only | [fit: evidence/fit/targets.tsv]. In Guitar Rig the dry is the unfiltered input [decompiled: Guitar Rig 6 0x6983b0] |
| Mix | dry 0.75, wet 0.50, then ×1.059 (+0.50 dB) | dry 0.1318, wet 0.4395, no make-up | Color off: [fit: evidence/fit/targets.tsv] (median notch depth −12.09 dB and peak +1.94 dB give dry 0.749, wet 0.501). Color on: [fit: evidence/fit/targets.tsv]. Make-up: [choice], "Level" below |
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

Notch 2 with Color off sits 4.4 to 4.5 % above Guitar Rig's at the sweep bottom (840 against
805 Hz; it was 6 to 7 % with the previous constants): Guitar Rig's notch ratio is about 5.4,
ours is the ideal 5.83. The cause is known: with Color off Guitar Rig's input branch is a
first-order highpass near 44 Hz on the wet path (`evidence/stoned_color_structure.md`), which
shifts notch 1 upward. It is not in this spec.

## Level

Engaging a pedal should be about unity loudness, as the pedals are built to be (both have a
make-up stage after the mix), and a player switching from Classic to Phase 90 should not hear a
drop. Loudness against bypass, ITU-R BS.1770 K-weighting, ungated, whole take, engine renders at
48 kHz, noon rates, before any make-up (scratch `loud4c.py`):

| Voicing | pink noise −18 dBFS | plucked riff (`audition.py`) | fxbench music |
| --- | --- | --- | --- |
| Classic (defaults 1 Hz, depth 0.4, mix 0.35) | −1.83 dB | −1.95 dB | −2.30 dB |
| Phase 90 script | −2.40 dB | −3.05 dB | −2.03 dB |
| Phase 90 block | −1.55 dB | −1.54 dB | −1.83 dB |
| Small Stone off | −0.16 dB | −0.80 dB | +0.03 dB |
| Small Stone on | +0.99 dB | −0.76 dB | +1.38 dB |

A 50/50 phaser loses about 1.5 to 3 dB to its notches; the Small Stone's dry-heavy mix (off) and
its resonance (on) roughly make up their own. Decision [choice]: a fixed make-up per voicing,
the negated mean of the noise and riff figures rounded to 0.1 dB, and none under 0.25 dB: Phase 90
script +2.70 dB (×1.365), block +1.50 dB (×1.189), Small Stone off +0.50 dB (×1.059), Small Stone
on 0 dB. Measured again with the make-up in the engine (noise / riff / music): script +0.30 /
−0.34 / +0.67 dB, block −0.04 / −0.03 / −0.32 dB, Small Stone off +0.34 / −0.31 / +0.53 dB,
Small Stone on +0.99 / −0.76 / +1.38 dB (its level depends most on the programme, through the
bass shelf and the resonance). The script and block positions of the Phase 90 now match each
other within 0.4 dB. Classic stays as it was (frozen),
so switching Classic → Phase 90 now gains about 1.5 dB instead of losing up to 1 dB. Guitar Rig
adds +5.4 dB to its Phaser Nine (about +3.9 dB over bypass); we do not follow it there.

## Peaks on a low square wave

Small Stone Color on peaks well above its input on a low square wave: the input branch lifts the
bass by 10 dB and the resonant loop rings on every edge. Same signal through Guitar Rig 6
(black box, `auhost`, the study's Color-on state at 0.316 Hz) and the prototype, peak and rms of
the output against the input after the first second, −6 dBFS squares (scratch `sq4b.py`):

| Signal | Guitar Rig 6 | ours (this spec) | ours (previous spec) |
| --- | --- | --- | --- |
| 82.4 Hz square | +12.72 / +2.21 dB | +13.00 / +2.21 dB | +12.87 / +2.22 dB |
| 82.4 Hz square, band-limited | +10.96 / +2.21 dB | +11.04 / +2.21 dB | +10.92 / +2.22 dB |
| 41.2 Hz square | +9.20 / +0.83 dB | +9.10 / +0.85 dB | +9.08 / +0.85 dB |
| 110 Hz square | +12.20 / +2.52 dB | +12.51 / +2.52 dB | +12.39 / +2.55 dB |

The reference does the same within 0.3 dB, so the peak is the voicing, not an error in ours
[measured]. The +14.1 dB the verifier read was a full-scale 82 Hz square at another rate and
LFO position; the model is linear, so the level does not change the ratio. Nothing saturates
in v1; the chain's later stages see these peaks.

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
