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
| Phase 90 block / Phaser Nine, Color 0.4217 | 0.143, 0.247, 1.071, 1.930, 3.720, 9.756 | −0.32 to +0.32 % | −0.75 to +0.62 % | 0.38 to 0.72 % / 1.27 % | (not a target) −0.77 to +0.23 % / −4.24 to −1.37 % | 0.00 % | −0.06 dB (+3.46/3.47 against +3.52/3.53) | −0.07 to −0.04 dB (−28.9) |
| Small Stone / Stoned Phaser, Color off | 0.159, 0.316, 1.778 | −1.10 to −0.57 % | −1.14 to −1.09 % | 1.89 to 1.90 % / 3.57 % | (leaves the band) | 0.00 % | 0.00 dB (+1.94) | +0.04 to +0.09 dB (−12.1) |
| Small Stone Color / Stoned Phaser, Color on | 0.159, 0.316, 1.778 | +0.82, +1.09, **+4.19** % | +1.85 to +2.80 % | 1.08 to 1.12 % / 2.21 % | −1.37 to +1.68 % / −2.98 to **−3.03** % | 0.00 % | −0.02 to +0.01 dB (+6.29 to +6.84) | −0.09 to −0.05 dB |
| Small Stone Color, Rate 0 (0.01 Hz, quasi-static) | 0.010 | +0.36 % | +1.41 % | | +1.72 % / −2.55 % | (set) | +0.01 dB | −0.08 dB |
| Phase 90 script / Phaser Nine, Color 0 (informational) | 0.143, 1.071, 9.756 | +4.9 to +5.3 % | +4.5 to +5.5 % | | | 0.00 % | −0.83 dB | much deeper |

Phase 90 block, round 2 (square-root law) against round 3 (this spec), per rate
(`evidence/fit/traj_before_round3.txt`, `compare_before_round3.txt`, `compare.txt`):

| Rate (Hz) | Sweep ends, worst, before / after | Trajectory rms before / after | Trajectory largest before / after | Depth before / after | Peak before / after |
| --- | --- | --- | --- | --- | --- |
| 0.143 | +0.46 / −0.51 % | 3.87 / 0.72 % | +6.31 / −1.27 % | 0.00 / −0.05 dB | −0.06 / −0.06 dB |
| 0.247 | +0.55 / +0.27 % | 3.84 / 0.72 % | +6.33 / −1.20 % | −0.04 / −0.04 dB | −0.06 / −0.06 dB |
| 1.071 | +0.10 / +0.44 % | 3.61 / 0.69 % | +6.53 / −1.08 % | −0.02 / −0.06 dB | −0.06 / −0.06 dB |
| 1.930 | −0.29 / +0.62 % | 3.15 / 0.62 % | +5.95 / +1.03 % | −0.02 / −0.06 dB | −0.06 / −0.06 dB |
| 3.720 | −1.40 / −0.75 % | 2.10 / 0.56 % | +4.05 / −1.15 % | −0.04 / −0.06 dB | −0.06 / −0.06 dB |
| 9.756 | +1.08 / +0.47 % | 1.15 / 0.38 % | +2.14 / +0.76 % | −0.06 / −0.07 dB | −0.06 / −0.06 dB |

Static response, Small Stone Color on, six frozen frames (sweep bottom, top and four points
between, `evidence/fit/fit_output.txt`): 0.05 to 0.25 dB rms, worst point 1.01 dB at 9.5 kHz in
the top frame. The structure before round 2 gave 0.52 to 0.79 dB rms on the same frames.

- Phase 90 block meets the round-3 bounds at all six rates: trajectory at most 0.72 % rms and
  1.27 % at any point (bounds 1.5 % and 3 %), sweep ends within 0.75 % (1.5 %), depth within
  0.07 dB (0.2 dB), resonance −0.06 dB (0.1 dB), period exact. The ends did not get worse to buy
  the middle: the worst end went from 1.40 % to 0.75 %. One thing did get worse: notch 2's top,
  which is not a target, moved from −3.4 % to −4.2 % at the slowest rate (−3.1 % at noon): notch 1's
  top there now sits 0.5 % low instead of 0.5 % high, and our notch-2/notch-1 spacing at the top
  (4.39, `spec_check.txt`) is narrower than Guitar Rig's (6956/1522.8 = 4.57 at noon,
  `targets.tsv`), which no sweep law can change.
- Small Stone, same trajectory measure: Color off 1.9 % rms, Color on 1.1 % rms, both under the
  3 % that would have triggered a change, so the Small Stone was not touched in round 3. Most of the
  Color-off figure is the known −1.1 % range offset plus a sweep top that is too low:
  `fit.py` refits f_lo, f_hi and κ to the Color-off trajectories as a check and gets 353.2 Hz,
  8544 Hz and 0.710, which would bring it to 0.75 to 0.93 % rms (`fit_output.txt`); not applied.
- Phaser Nine at Color 0 is not a script reference: it keeps its modified stage 2, which our
  script (no feedback resistor, four plain stages) does not have. The +5 % is that difference.
- Small Stone Color on: the earlier notch-2 gap (+17 % at the sweep bottom) was the structure,
  and is gone. Two cells stay just outside 3 %, and both are differences in how Guitar Rig 6
  computes, not in the circuit:
  - Notch 2 at the sweep top, −3.0 %. Guitar Rig 6 runs this component at twice the session
    rate: our structure fits the six static frames to 0.01 dB rms at 96 kHz but only 0.05 to
    0.25 dB at 48 kHz, and at 96 kHz the top frame's notches land at 1297 / 7158 Hz against
    1315 / 6889 Hz at 48 kHz (Guitar Rig 6: 1306 / 7128). At 48 kHz four first-order allpasses
    cannot widen the notch-2/notch-1 spacing at the top of the sweep (the feedback gain moves it
    by 0.1 %), so f_hi is set to split the error: notch 1 up to +2.8 %, notch 2 down to −3.0 %.
    Running the Small Stone at 2× would close it; it is not in the spec.
  - Notch-1 minimum at 1.78 Hz, +4.2 %. Notch 1 fades below 3 dB prominence before the sweep
    bottom, so this is where detection stops, and it moves little with f_lo. Guitar Rig 6
    realises the loop as factored second-order sections; with the same loop and the same
    break-frequency trajectory that form narrows the tracked sweep at fast rates (notch-1 top
    −1.6 %, minimum +11 % from 0.16 to 1.78 Hz, as measured) while the TPT chain does not
    (0.0 %, +14 %) [inferred: analysis transcription, `evidence/stoned_color_structure.md`].
    The tracker's own bias on this sharp corner is +6.2 % at 1.78 Hz (`selftest.txt`), and
    applies to both readings, which are tracked identically.
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

One allpass stage is a TPT (trapezoidal) one-pole. With break frequency `fc`:

```
g  = tan(pi * fc / fs)          G = g / (1 + g)
v  = (x - s) * G                lp = v + s            s = lp + v        (state update)
P(x) = 2*lp - x                 (1 - s/wc)/(1 + s/wc): +1 at DC, -1 at HF
A(x) = x - 2*lp = -P(x)         the op-amp stage of the Phase 90: -1 at DC, +1 at HF
```

Each stage is affine in its input for the current sample (`P(x) = (2G-1)*x + 2(1-G)*s`), so
a feedback loop around any number of stages is solved in closed form, with no unit delay.
All four stages share one `g`, recomputed every sample. A cheaper update (every 16 samples
with linear interpolation) is plausible at LFO rates up to 10 Hz but untested [guess].

Prototype details that are part of the spec: `fc` (and the Phase 90 feedback lag frequency) is
clamped to 0.49·fs before `tan` [guess: keeps `tan` finite, never reached at 44.1 kHz and up];
the LFO starts at phase 0, which is the bottom of the sweep for both voicings (Phase 90 u = −1,
Small Stone v = −1); the Phase 90 gate smoothing is `s += k·(v − s)` with
`k = 1 − exp(−2π·f_gate/fs)`, its state starting at the first LFO value (a different start state
changes only the first 0.2 s); `--depth` scales the sweep about its centre (Phase 90: the
smoothed LFO u is multiplied by depth/0.5; Small Stone: the log half-range, capped at 1.2× [guess:
keeps the top below 0.49·fs at 44.1 kHz]); the command line defaults to 1.0 Hz and 0.32 Hz,
noon on Guitar Rig is 1.071 Hz and 0.316 Hz;
`--mix` scales dry by `min(1, 2(1 − mix))` and wet by `min(1, 2·mix)` (Small Stone; Phase 90
uses `(1 − mix)·x + mix·wet`). Depth and mix are pinned to stock (0.5) in the product.

Why TPT: the break frequency is exact at every sample rate. Guitar Rig's Phase 90 model
uses `a = 2*pi*f/fs - 1` [decompiled: Guitar Rig 6 0x2a9838]; its sweep is the same at 48
and 96 kHz and 0.2 to 0.9 % higher at 44.1 kHz [measured: verification.md C08], which fits
running the code at twice the host rate [inferred, not read in code]. AmpliTube uses the
bilinear transform without prewarp [decompiled: AmpliTube 5 0xd8984c].

## Voicing 1: Phase 90

| Item | Value | Source |
| --- | --- | --- |
| Stages | 4 equal first-order allpass | [circuit: https://dafx14.fau.de/papers/dafx14_felix_eichas_physical_modeling_of_the_.pdf section 3.1] |
| Capacitor C | 47 nF | [circuit: https://generalguitargadgets.com/pdf/ggg_p90_bom.pdf, six 0.047 µF; their role as allpass capacitors is read from the count] |
| Fixed resistor | 22 kΩ in parallel with each stage's JFET leg, so the floor is 1/(2π·22k·47n) = 153.9 Hz | [circuit: same parts list, five 22 kΩ; role read from the count] |
| Sweep law | `fc = (1/22k + 1/R(u)) / (2π·C)`, `R(u) = (RS^m + R_ch(u)^m)^(1/m)`, `R_ch(u) = R1·e^(−γ·u)`; u is the smoothed LFO in [−1, 1], +1 the top of the sweep | form [guess], chosen for its shape: Guitar Rig 6's trajectory at 0.14 Hz is straight in log frequency over the lower 85 % of its range with a sharp bottom turn, then bends to about 0.6 of that slope over the last 15 % [measured: evidence/fit/trajectories.tsv p9_block_p010]. An exponential channel resistance gives the straight part; the soft series limit RS gives the bend at the top. Device reading: a JFET's channel resistance climbs steeply towards pinch-off (sweep bottom) and cannot fall below a floor as the gate opens (sweep top) [inferred]. Laws tried on the same trajectories (round-3 exploration, scratch `$W/explore.py`, `explore2.py`; worst rate, each law and its LFO fitted to the trajectories alone): square-root conductance refitted 2.35 % rms / 5.8 % largest; power law of (1 − Vgs/Vp) 1.57 / 4.8 %; linear JFET conductance, R = R_on/(1 − Vgs/Vp) [literature: http://dafx.de/paper-archive/2005/P_155.pdf eq. 9-17], with a series resistor: no physical optimum (series resistance went negative); exponential with a plain series resistor (m = 1) 1.00 / 2.8 % but its top end +2.9 %; m free: this law |
| RS (series limit) | 933.7 Ω | [fit: evidence/fit/trajectories.tsv] |
| R1 (channel at mid-sweep) | 1786.1 Ω | [fit: evidence/fit/trajectories.tsv] |
| γ (channel slope) | 0.8915 per unit of u: R_ch spans e^(2γ) = 5.95× over the full LFO swing | [fit: evidence/fit/trajectories.tsv] |
| m (sharpness of the limit) | 3.892 | [fit: evidence/fit/trajectories.tsv]. Flat direction: a second fit pass returned 3.866 with the same cost. fit.py's other start (m = 1) stops in a worse local minimum (cost 4.6× higher) |
| Stock sweep | quasi-static (u = ±1, 48 kHz): fc 930.8 to 3487.3 Hz, notch 1 419.5 to 1589.6 Hz, notch 2 1924 to 6977 Hz [`spec_check.txt`]. Running at noon (1.071 Hz) the gate smoothing narrows notch 1 to 463.7 to 1529.5 Hz [`compare.txt`] | follows from the rows above |
| LFO shape | triangle whose halves are RC charge curves: `s(w) = (1 − e^(−κw))/(1 − e^(−κ))`, w ∈ [0, 1] the position within the half, κ = −0.002, i.e. a straight triangle | [fit: evidence/fit/trajectories.tsv]. The two fit passes return 0.008 and −0.002 at the same cost, so the fit resolves κ only to about ±0.01 and reads the reference's LFO as straight; Eichas' "slightly curved edges" [literature: Eichas, section 4.3] are not visible at this resolution |
| Gate smoothing | one-pole lowpass at 3.535 Hz on the LFO, before the law | [fit: evidence/fit/trajectories.tsv]; sets how the sweep narrows and rounds with rate |
| Rate range | existing `mod_rate`, 0.05 to 10 Hz, log | design choice. Guitar Rig 0.033 to 9.76 Hz [decompiled: Guitar Rig 6 0x2a891c]; AmpliTube 0.1 to 10 Hz [decompiled: AmpliTube 5 0x1788d0c] |
| Knob curve | keep tubamp's log mapping in Hz | design choice. Guitar Rig makes the period piecewise linear in the knob: 30 s at 0, 7 s at 0.1, 1.1 s at 0.4, 0.1025 s at 1 [decompiled: Guitar Rig 6 0x2a89b0]; measured 7.000, 4.050, 0.9337, 0.5181, 0.2688, 0.1025 s at 0.1, 0.25, 0.5, 0.75, 0.9, 1.0 [measured: evidence/fit/targets.tsv]. Noon is 1.071 Hz |
| Mix | dry 0.5, wet 0.5, fixed | [guess]: Eichas figure 4 sums dry and wet through two resistors at Q5 but gives no values; Guitar Rig uses 0.5 and 0.5 [decompiled: Guitar Rig 6 0x2a8bcc], AmpliTube 1.0 and 1.0 [decompiled: AmpliTube 5 0x13d9218] |
| Script | no feedback: `wet = A⁴` | [circuit: Eichas section 3.1, the 1974 version has no feedback resistor] |
| Block node | the block-logo feedback resistor (R15 in Eichas' schematic) from the stage-4 output to the inverting input of the stage-2 op-amp (R6/R7 junction) | [circuit: Eichas figure 4, R15] |
| Block ratio β | 0.3957 = op-amp feedback resistor / block-logo feedback resistor. With a 10 kΩ op-amp feedback resistor that is a fitted equivalent of 25.3 kΩ for the feedback resistor: a fitted value, not a schematic value | [fit: evidence/fit/targets.tsv], to the measured resonance. No opened source gives the feedback resistor's value: Eichas gives no component values, and the GGG parts list numbers its parts differently from Eichas (its R28 is 56 kΩ, its R15 22 kΩ), so it does not identify the part. The 10 kΩ is unsourced too [guess]; only the ratio β enters the equations |
| Block equations | stage 2 becomes `S2 = ((1+β)w − 1)/(w + 1)` and stages 3, 4 sit in the loop: `wet = A · S2 · A² / (1 + β·L·A²)` | follows from the node and an ideal op-amp; Guitar Rig has the same form [decompiled: Guitar Rig 6 pool 0x2be05b0, 0x2a9050] |
| Feedback lag L | one-pole lowpass at 11 736 Hz in the feedback-resistor path (with TPT at 48 kHz: a 10.8 µs delay at low frequencies) | [fit: evidence/fit/targets.tsv], to the median notch depth along the fitted sweep. It stands in for what fills Guitar Rig's notch: a one-sample delay in its loop [decompiled: Guitar Rig 6 0x2a968c, 0x2a98e4] at what measures as a 96 kHz internal rate. Its notch fills in as it rises, −30.3 dB near 510 Hz to −27.2 dB near 1380 Hz [measured: Phaser Nine Rate 0.5 take, binned per frame], which a gain imbalance cannot do. In a pedal the same role falls to the op-amp's bandwidth [guess]. Script mode has no feedback, so L does nothing there |
| Block result | at 48 kHz, quasi-static sweep ends [`spec_check.txt`]: resonance +3.51 dB at the bottom to +3.37 dB at the top (median over a running sweep +3.47, `compare.txt`), DC −1.33 dB, notch 1 at 0.4506·fc (bottom) to 0.4558·fc (top) (analog 0.4502), −30.69 dB at 419.5 Hz to −26.77 dB at 1589.6 Hz (median over a running sweep −28.9), notch 2 at 2.067·fc to 2.001·fc (analog 2.073), notch-2/notch-1 spacing 4.59 to 4.39 | closed form of the equation above (the TPT warp moves the notch-to-fc ratios with fc); Guitar Rig 6 +3.52 dB peak, median notch depth −28.9 dB [measured: evidence/fit/targets.tsv], DC −1.4 dB, spacing 4.58 [measured: evidence/gr6_snapshots.tsv]. Without L the loop gives −32.60 dB at every frequency (analog) |
| Sample rate | the fit and every accuracy figure are at 48 kHz. The same equations at 44.1 / 48 / 96 kHz put the top of the block sweep at notch 1 1593.9 / 1589.6 / 1571.9 Hz, notch 2 6903 / 6977 / 7313 Hz, depth −26.97 / −26.77 / −26.02 dB [`spec_check.txt`; not measured on Guitar Rig at other rates] | TPT keeps the break frequency exact, not the notch frequency |
| Saturation | none in v1 | both commercial models are linear: harmonics of a −3 dBFS sine below −150 dB relative [measured: evidence/gr6_thd.tsv, verification.md C09]; no signal term in AmpliTube's JFET resistance [decompiled: AmpliTube 5 0xd8984c]. The hardware does distort [literature: Eichas, section 3.1] |
| Level | unity: no make-up gain | Guitar Rig adds ×1.862 (+5.4 dB) [decompiled: Guitar Rig 6 0x2a8e5c]; we leave level to the chain |
| Filtering | none | Guitar Rig's 16.4 kHz lowpass is not reproduced; a linear TPT phaser cannot alias |
| Oversampling | none | see "Shared building block" |
| Stereo | one LFO for both channels | Guitar Rig's Rotate defaults to zero offset [decompiled: Guitar Rig 6 0x2a92b8] |
| Player controls | Speed (`mod_rate`), Script/Block switch | the pedal has one knob; Depth and Mix are pinned to stock |

Block-logo loop, per sample (`a = 1 − 2G`, `k3 = −2(1−G)·s3`, `k4 = −2(1−G)·s4`; `GL`, `sl` are
the G and state of the 11 736 Hz lag one-pole):

```
v1 = A1(x)
s2 = (1 + beta)*v1 - (2 + beta)*lp2(v1)
y4 = (a*a*(s2 - beta*(1 - GL)*sl) + a*k3 + k4) / (1 + beta*GL*a*a)
l  = lpL(y4)                       ; = GL*y4 + (1 - GL)*sl, then update sl
u  = s2 - beta*l ;  y3 = A3(u) ;  y4 = A4(y3)
out = 0.5*x + 0.5*y4
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
| Stages | 4 equal | 4 equal, one shared break frequency | four stages as two second-order sections, all with one `g` [decompiled: Guitar Rig 6 0x6985b4, 0x698600]; exactly two notches measured [measured: verification.md, Part 1] |
| Sweep law | exponential: `fc = sqrt(lo·hi) · (hi/lo)^(v/2)`, v in [−1, 1] | same | form: `centre · 32^x` [decompiled: Guitar Rig 6 0x2d9774, 0x2d9f7c]; log-f triangle fits the track (R² 0.993 against 0.85 for linear f) [measured: verification.md C12] |
| Sweep range | 355 Hz to 8300 Hz | 99.0 Hz to 3133.5 Hz | Color off: [fit: evidence/gr6_snapshots.tsv, evidence/ss_color_fit.tsv; original script not kept], confirmed within 1.14 % on notch 1's ends at three rates; `evidence/fit/fit.py` re-derives 353.2 / 8544 Hz from `evidence/fit/trajectories.tsv` as a check (printed in `fit_output.txt`, not applied: the Small Stone was left alone in round 3). Color on: [fit: evidence/fit/targets.tsv]: f_lo is the break frequency of the frozen sweep-bottom frame, f_hi the value that centres the notch-1/notch-2 maximum errors (see "Measured accuracy") |
| LFO shape | RC triangle, κ = 0.70 (the same formula as the Phase 90's) | same | [fit: evidence/phasis_tracks.tsv column stoned_phaser; original script not kept]; best of the shapes tried, R² 0.9998 [measured: verification.md C12]; `evidence/fit/fit.py` re-derives 0.710 from the Color-off rows of `evidence/fit/trajectories.tsv` (check). Guitar Rig's LFO code has no Color branch [decompiled: Guitar Rig 6 0x6983fc-0x698558] |
| Rate range | existing `mod_rate` | same | Guitar Rig 0.01·1000^knob Hz: 0.1585, 0.3162, 1.778 Hz at 0.4, 0.5, 0.75 [measured: evidence/fit/targets.tsv]. Noon is 0.316 Hz. Color does not change the rate [decompiled: Guitar Rig 6 0x611e80-0x611fa4; measured period error 0.00 %] |
| Feedback | none | c = 0.7553 around all four stages, through the network's feedback branch F, no delay | [fit: evidence/fit/targets.tsv]. Taken from the fourth stage, injected before the first [decompiled: Guitar Rig 6 0x6983a4, 0x6985a0] |
| Color network poles | none | f_p1 = 53.0 Hz, f_p2 = 266.0 Hz, shared by both branches | [fit: evidence/fit/targets.tsv]. One second-order network with two outputs on a shared denominator [decompiled: Guitar Rig 6 0x6980b0-0x6982e4] |
| Feedback branch F | none | `s (s + w_zf) / ((s + w_p1)(s + w_p2))`, f_zf = 18.3 Hz: zero at DC, unity well above 300 Hz | [fit: evidence/fit/targets.tsv]. Guitar Rig forms it as `1 − z⁻¹·BB` with BB unity at DC [decompiled: Guitar Rig 6 0x69859c-0x6985a8] |
| Input branch Ha | none | `(s + w_za)(s + w_zb) / ((s + w_p1)(s + w_p2))`, f_za = 16.5 Hz, f_zb = 885.2 Hz: unity at HF, +10.4 dB shelf below about 300 Hz, cut below 50 Hz. Wet path only | [fit: evidence/fit/targets.tsv]. In Guitar Rig the dry is the unfiltered input [decompiled: Guitar Rig 6 0x6983b0] |
| Mix | dry 0.75, wet 0.50 | dry 0.1266, wet 0.4289 | Color off: [fit: evidence/fit/targets.tsv] (median notch depth −12.09 dB and peak +1.94 dB give dry 0.749, wet 0.501). Color on: [fit: evidence/fit/targets.tsv] |
| Saturation | none in v1 | none in v1 | harmonics below −120 dB [measured: evidence/gr6_thd.tsv]. The OTA stage is a tanh device [literature: http://dafx.de/paper-archive/2005/P_155.pdf, eq. 1-8] |
| Player controls | Rate (`mod_rate`), Color switch | | the pedal has one knob and one switch |

### Color on in words

The four stages are the same as with Color off; one LFO moves all of them together over a
lower range. Two things are added, both built from one small network with two poles (53 Hz and
266 Hz):

1. The wet path's input is shaped by the network's input branch Ha: low frequencies (about 50
   to 300 Hz) are lifted by about 10 dB relative to the highs, and the deep bass below 50 Hz is
   cut. The dry path is not filtered.
2. The output of the fourth stage is fed back, with gain c, to the input of the first stage,
   through the network's feedback branch F. F passes everything above a few hundred hertz and
   rolls off towards DC as a second-order highpass, so the resonance acts on the mids and highs
   but the loop has no DC gain.

The mix becomes wet-heavy: 0.4289 of the loop output plus 0.1266 of the dry input.

```
H(s) = dry + wet · Ha(s) · P(s)⁴ / (1 − c · F(s) · P(s)⁴)
P(s) = (1 − s/w_c) / (1 + s/w_c)          w_c = 2π·fc, fc from the LFO
Ha(s) = (s + w_za)(s + w_zb) / ((s + w_p1)(s + w_p2))
F(s)  = s (s + w_zf)       / ((s + w_p1)(s + w_p2))
```

Discretisation: every one-pole is a TPT lowpass `lp = G·(in − s) + s`, `s ← 2·lp − s`,
`G = g/(1 + g)`, `g = tan(π·f/fs)` (prewarped at its own frequency). Each branch is two
first-order sections in series, so each is two one-poles:

```
Ha:  xa = x  − ka·lp_p1a(x)       ka = 1 − f_za/f_p1      (shelf: f_za/f_p1 at DC, 1 at HF)
     xs = xa − kb·lp_p2a(xa)      kb = 1 − f_zb/f_p2      (shelf: f_zb/f_p2 at DC, 1 at HF)
F:   h  = y4 − lp_p1f(y4)                                  (highpass at f_p1)
     e  = h  − kf·lp_p2f(h)       kf = 1 − f_zf/f_p2      (shelf: f_zf/f_p2 at DC, 1 at HF)
```

The loop is solved per sample without a delay. Each stage is affine in its input
(`P(u) = p·u + k_i`, `p = 2G − 1`), so the chain is `y4 = p⁴·u + K` with `K` the four stage
states pushed through the chain. F is affine in y4 for the current sample:
`F(y4) = aF·y4 + bF` with `aF = (1 − G1)(1 − kf·G2)` and
`bF = −(1 − kf·G2)(1 − G1)·s1f − kf(1 − G2)·s2f` (G1, G2 for f_p1, f_p2; s1f, s2f the feedback
branch states). Then:

```
y4  = (p⁴·(xs + c·bF) + K) / (1 − c·aF·p⁴)
e   = F(y4)          ; run the two feedback one-poles on y4, updating s1f, s2f
u   = xs + c·e ;  y4 = P4(P3(P2(P1(u))))   ; updates the stage states
out = dry·x + wet·y4
```

`c·aF·p⁴ ≤ 0.76` always, so the divisor never reaches zero. Guitar Rig puts one sample of delay
into the filtered part of its loop; we do not, and the fitted constants absorb the difference.

Notch 2 with Color off sits 6 to 7 % above Guitar Rig's at the sweep bottom (857 against
805 Hz): Guitar Rig's notch ratio is about 5.4, ours is the ideal 5.83. The cause is now known:
with Color off Guitar Rig's input branch is a first-order highpass near 44 Hz on the wet path
(`evidence/stoned_color_structure.md`), which shifts notch 1 upward. Color off is not changed
in this round.

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
