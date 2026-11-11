# Phaser refit: independent verification

Verifier's own measurement of `prototype/phaser_ref.py` and `voicings.md` against Guitar Rig 6,
after the two fix rounds. Nothing was fixed or edited. Date: 2026-11-11. All at 48 kHz.

Scratch (scripts, logs, small result files, no WAVs): `$RE_STATE/work/phaser-refit-verify/`.
Guitar Rig 6 states were built with `tools/re/ngrc_xml.py` + `tools/re/gr6state.py`, not with the
authors' `grstate.py`.

## Result in short

- Every stated accuracy figure is confirmed. My error table lands within 0.05 % of theirs on the
  Phase 90 sweep ends and within the trackers' errors elsewhere.
- The stated figures cover the sweep ends only. Between the ends the Phase 90 block sweep is still
  up to 6.6 % away from Guitar Rig 6 (3.1 to 3.8 % rms at rates up to 1.9 Hz). The refit did not
  change that.
- The reference settings are the authors' choice, not the plugin's INIT component presets.
- No constant in the prototype or in voicings.md equals a constant in the Guitar Rig 6 binary,
  apart from trivial values. One constant (Phase 90 LFO curvature 0.36) comes from the AmpliTube 5
  binary.
- `fit.py` regenerates the fitted constants from `targets.tsv`: Small Stone Color to every printed
  digit, Phase 90 sweep constants to three digits.

## 1. Tracker and its self-test

`itrack.py` (scratch). Principle: an impulse train, not a multitone.

- Probe: one single-sample impulse every P samples (P = 512 for the Phase 90, 2048 for the Small
  Stone, 8192 for the frozen frames). M files with the train shifted by P/M are interleaved, so
  snapshots are 1.3 to 2.7 ms apart.
- Each impulse returns a complete impulse response at one instant of the LFO. Its DTFT on a log
  grid (ratio 1.0035, 40 Hz to 16 kHz) is |H| at that instant. No analysis window sets the
  frequency resolution.
- Notches: local minima in the band (300 Hz to 12 kHz for the Phase 90, 60 Hz to 12 kHz for the
  Small Stone) with 3 dB prominence on both sides, refined by a golden-section search on the DTFT.
- Sweep ends: per cycle, a two-sided quadratic with a common junction is fitted to ±12 snapshots
  around the turning point, and the extreme of the fitted curve is taken. It fits a smooth turn and
  a sharp one alike. Median over cycles. No single frame sets a sweep end.
- Period: level crossings of ln f at 11 levels, both directions, least squares.
- Depth and peak: medians over whole cycles of the notch-1 level and of the highest |H| in band.

Self-test on my own synthetic four-stage phaser (`synth.py`: transposed direct-form allpass
sections, known frozen notches `fs/π·atan(g·tan((2k−1)π/8))`), error against the true value:

| Sweep | Rate Hz | Notch 1 min | Notch 1 max | Notch 2 min | Period | Depth (true) | Peak |
| --- | --- | --- | --- | --- | --- | --- | --- |
| smooth (Phase 90-like) | 0.247 | +0.001 % | −0.001 % | +0.001 % | 0.0000 % | +0.02 dB (−29.12) | 0.00 dB |
| smooth | 1.071 | +0.004 % | −0.003 % | +0.003 % | 0.0000 % | +0.07 dB | 0.00 dB |
| smooth | 3.72 | +0.003 % | −0.002 % | +0.001 % | 0.0000 % | −0.04 dB | 0.00 dB |
| smooth | 9.756 | +0.006 % | −0.004 % | +0.003 % | 0.0000 % | −0.02 dB | 0.00 dB |
| sharp (Small Stone-like) | 0.316 | +0.016 % | −0.004 % | −0.004 % | 0.0000 % | 0.00 dB (−12.04) | 0.00 dB |
| sharp | 1.778 | +0.37 % | +0.003 % | +0.001 % | 0.0000 % | −0.02 dB | 0.00 dB |

Worst error: 0.37 % (Small Stone-like minimum at 1.78 Hz, where the low notch moves during its own
impulse response). Everything else is within 0.02 %. For comparison, a plain 3-frame running median
without the fit reads the sharp turning points 0.6 to 1.4 % short.

The first version of the sweep-end fit had two defects (window centred on a coarse phase bin; value
taken at the junction, not at the curve's extreme). They showed as a 0.5 % disagreement with
`targets.tsv` at the slowest Phase 90 rate and a 0.14 % self-test error. Both were fixed, the saved
tracks were re-analysed and the self-test above is the final one.

Uncertainty used below: Phase 90 sweep ends ±0.05 %, Small Stone sweep ends ±0.05 % (minimum at
1.78 Hz ±0.4 %), period ±0.001 %, notch depth ±0.07 dB, peak ±0.01 dB. The authors' tracker reads
the Small Stone minimum 1.1 % high at 0.32 Hz and 6.2 % high at 1.78 Hz (their `selftest.txt`);
mine does not have that bias.

Not validated: the bottom of a Small Stone sweep at 5 Hz. The snapshot is no longer a frozen
response there and the notch assignment fails.

## 2. Set-up checks

- State builders. `gr6state.py` (INIT document from the `.ngrc`) and the authors' `grstate.py`
  (component cut from a rack preset) give bit-identical audio for the same settings: maximum
  difference 0.0 on 3 s of noise, Phaser Nine block and Stoned Phaser Color on.
- Determinism. Two renders of the same file are bit-identical. The LFO starts at the same phase.
- Rack gate. The default state has the rack noise gate on (threshold 0.1). On a multitone like the
  authors' probe it changes only the first 0.25 s (−155 dB afterwards), so their takes are not
  affected. It chops an impulse train, so my states set `Rack.G=0`.
- Linearity. Impulses at 0.25 and 0.025 scale to −132 dB (Phaser Nine) and −81 dB rms (Stoned
  Phaser Color on).
- Make-up gain. Phaser Nine with Dry/Wet 0 measures +5.40 dB at 100 Hz to 1 kHz. The authors'
  −5.40 dB is right. It also rolls off: −0.12 dB at 3 kHz, −0.8 dB at 8 kHz, −1.7 dB at 12 kHz.
  The prototype has no such filter (stated in voicings.md as a design choice).
- What "Color 0.4217" means. It is the value written by the component's coded default setter
  (`OrangePhaserAU::setDefaultParameterValues` 0x1391b8: stages 4, Color 0x3ed7e910 = 0.4217). It is
  not the plugin's INIT component preset. `Phaser Nine Factory.ngrc` INIT stores Notches 7,
  Color 0.5027, Frequency 0.4758, Depth 0.5, Rate 0.5. The authors' "block" is Notches 4,
  Color 0.4217, Frequency 0.5, Depth 0.5. Measured at Rate 0.5:

| Setting | Notches | Notch 1 Hz | Peak dB |
| --- | --- | --- | --- |
| authors' block (n=4, c=0.4217, e=0.5) | 2 | 465 to 1522 | +3.53 |
| INIT component preset (n=7, c=0.5027, e=0.4758) | 3 | 488 to 1661 | +3.94 |
| n=4 with INIT Color and Frequency | 2 | 444 to 1507 | +4.36 |
| n=4, c=0.4217, INIT Frequency 0.4758 | 2 | 433 to 1471 | +3.54 |

- Same for the Stoned Phaser. The authors use Color Strength 0.74 (the DSP class default). The INIT
  component preset stores 0.40. Color on at 0.40 peaks at +3.16 dB, against +6.82 dB at 0.74.
  The voicing matches Guitar Rig 6 at these chosen settings. Which one a user gets when the
  component is dropped into a rack could not be checked without the GUI.

## 3. Error table: prototype against Guitar Rig 6

Mine, with theirs (`evidence/fit/compare.txt`) in brackets. The prototype ran at the rate I
measured on Guitar Rig 6. "held out" rows are knob positions that are not in `targets.tsv`.

| Take | Rate Hz | Notch 1 min GR / proto Hz | err % (theirs) | Notch 1 max GR / proto Hz | err % (theirs) | Notch 2 min err % (theirs) | Notch 2 max err % (theirs) | GR period s | Depth err dB (theirs) | Peak GR / proto dB | Peak err dB (theirs) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| P90 block, Rate 0.10 | 0.1429 | 423.6 / 425.1 | +0.36 (+0.34) | 1590.4 / 1597.9 | +0.47 (+0.46) | +0.25 (+0.24) | −3.42 (−3.44) | 7.00000 | −0.04 (0.00) | 3.53 / 3.47 | −0.06 (−0.06) |
| P90 block, Rate 0.25 | 0.2469 | 428.1 / 429.4 | +0.32 (+0.33) | 1581.3 / 1589.6 | +0.53 (+0.55) | +0.20 (+0.21) | −3.33 (−3.34) | 4.05000 | −0.04 (−0.04) | 3.53 / 3.47 | −0.06 (−0.06) |
| P90 block, Rate 0.50 | 1.0710 | 464.1 / 464.3 | +0.06 (+0.07) | 1523.1 / 1524.6 | +0.10 (+0.10) | −0.10 (−0.14) | −3.43 (−3.44) | 0.93375 | −0.06 (−0.02) | 3.53 / 3.47 | −0.06 (−0.06) |
| P90 block, Rate 0.60 | 1.3029 | 474.6 / 474.5 | −0.03 (held out) | 1506.0 / 1506.5 | +0.03 | −0.22 | −3.41 | 0.76750 | −0.06 | 3.53 / 3.47 | −0.06 |
| P90 block, Rate 0.75 | 1.9300 | 503.9 / 502.4 | −0.30 (−0.29) | 1461.9 / 1458.0 | −0.26 (−0.26) | −0.53 (−0.52) | −3.46 (−3.47) | 0.51813 | −0.07 (−0.02) | 3.53 / 3.47 | −0.06 (−0.06) |
| P90 block, Rate 0.90 | 3.7209 | 584.9 / 581.0 | −0.67 (−0.65) | 1351.9 / 1332.9 | −1.41 (−1.40) | −1.03 (−1.07) | −4.02 (−4.03) | 0.26875 | −0.10 (−0.04) | 3.53 / 3.47 | −0.06 (−0.06) |
| P90 block, Rate 0.95 | 5.3872 | 644.7 / 641.6 | −0.48 (held out) | 1251.1 / 1245.5 | −0.45 | −0.96 | −2.75 | 0.18562 | +0.01 | 3.53 / 3.47 | −0.06 |
| P90 block, Rate 0.97 | 6.5628 | 677.0 / 675.5 | −0.22 (held out) | 1196.8 / 1199.7 | +0.25 | −0.77 | −1.89 | 0.15237 | 0.00 | 3.53 / 3.47 | −0.06 |
| P90 block, Rate 1.00 | 9.7561 | 737.4 / 739.5 | +0.28 (+0.29) | 1106.6 / 1118.4 | +1.07 (+1.08) | −0.42 (−0.43) | −0.80 (−0.82) | 0.10250 | −0.01 (−0.06) | 3.53 / 3.47 | −0.06 (−0.06) |
| SS off, Rate 0.40 | 0.1585 | 148.8 / 147.1 | −1.11 (−1.10) | 3785.6 / 3744.6 | −1.08 (−1.09) | +6.54 (+6.50) | leaves band | 6.30957 | +0.05 (+0.05) | 1.94 / 1.94 | 0.00 (0.00) |
| SS off, Rate 0.50 | 0.3162 | 148.8 / 147.1 | −1.14 (−1.06) | 3785.6 / 3744.6 | −1.08 (−1.10) | +6.53 (+6.43) | leaves band | 3.16228 | +0.05 (+0.04) | 1.94 / 1.94 | 0.00 (0.00) |
| SS off, Rate 0.60 | 0.6310 | 148.9 / 147.2 | −1.16 (held out) | 3785.4 / 3744.6 | −1.08 | +6.53 | leaves band | 1.58489 | +0.05 | 1.94 / 1.94 | 0.00 |
| SS off, Rate 0.75 | 1.7783 | 149.3 / 147.7 | −1.10 (−0.57) | 3785.7 / 3744.4 | −1.09 (−1.14) | +6.49 (+6.49) | leaves band | 0.56234 | +0.07 (+0.09) | 1.93 / 1.94 | 0.00 (0.00) |
| SS off, Rate 0.90 | 5.0119 | 150.2 / 148.6 | −1.08 (held out) | 3783.7 / 3743.3 | −1.07 | +6.51 | leaves band | 0.19953 | +0.19 | 1.93 / 1.93 | +0.01 |
| SS on, Rate 0.40 | 0.1585 | fades, see below | (+1.09) | 1301.1 / 1329.2 | +2.16 (+1.85) | +0.69 (−0.16) | −3.00 (−2.98) | 6.30957 | −0.05 (−0.06) | 6.83 / 6.84 | +0.01 (0.00) |
| SS on, Rate 0.50 | 0.3162 | fades, see below | (+0.82) | 1301.1 / 1329.2 | +2.16 (+2.45) | +0.89 (−1.37) | −3.00 (−2.98) | 3.16228 | −0.05 (−0.05) | 6.83 / 6.83 | 0.00 (+0.01) |
| SS on, Rate 0.60 | 0.6310 | not measured | (held out) | 1300.8 / 1329.3 | +2.19 | +0.83 | −3.00 | 1.58489 | −0.05 | 6.80 / 6.80 | +0.01 |
| SS on, Rate 0.75 | 1.7783 | fades, see below | (+4.19) | 1301.0 / 1329.6 | +2.20 (+2.80) | +1.29 (+1.68) | −2.99 (−3.03) | 0.56234 | −0.06 (−0.09) | 6.55 / 6.63 | +0.07 (−0.02) |
| SS on, Rate 0.90 | 5.0119 | not valid | (held out) | 1301.3 / 1329.3 | +2.16 | not valid | −3.00 | 0.19953 | −0.02 | 6.46 / 6.38 | −0.08 |

Prototype period error against Guitar Rig 6: below 0.0001 % in every row.

Small Stone Color on, notch-1 minimum. Notch 1 does not turn at the sweep bottom: it loses its
3 dB prominence on the way down and regains it on the way up. So this "minimum" is a property of
the 3 dB rule. I read the frequency at which the prominence crosses the threshold, interpolated
between snapshots (cycle-to-cycle spread below 0.02 %):

| Rate Hz | Threshold | Falling: GR / proto Hz | err % | Rising: GR / proto Hz | err % | Theirs |
| --- | --- | --- | --- | --- | --- | --- |
| 0.1585 | 3 dB | 137.8 / 140.3 | +1.83 | 141.7 / 143.2 | +1.03 | +1.09 |
| 0.3162 | 3 dB | 136.2 / 139.2 | +2.21 | 144.0 / 144.8 | +0.56 | +0.82 |
| 1.7783 | 3 dB | 122.8 / 128.9 | +4.95 | 162.4 / 158.7 | −2.32 | +4.19 |
| 1.7783 | 2 dB | 104.8 / 108.4 | +3.46 | 139.9 / 136.2 | −2.62 | |
| 1.7783 | 4 dB | 145.3 / 153.6 | +5.74 | 186.3 / 182.5 | −2.04 | |

The reading moves from 105 to 145 Hz when the threshold goes from 2 to 4 dB. The error between the
two devices is stable: under 2.3 % at the slow rates, 3.5 to 5.7 % on the falling side at 1.78 Hz.

### Verdict per stated figure

| Stated | Verdict | My number | Tracker uncertainty |
| --- | --- | --- | --- |
| Phase 90 block, notch-1 min and max within 1.40 % at six rates | CONFIRMED | min −0.67 to +0.36 %, max −1.41 to +1.07 %; worst −1.41 % (max at 3.72 Hz). Three held-out rates: within 0.48 % | ±0.05 % |
| Phase 90 block notch depth within 0.06 dB | CONFIRMED within tracker error | −0.10 to −0.01 dB; worst −0.10 dB at 3.72 Hz. By notch frequency (420 to 1600 Hz, six bands) the prototype is 0.06 to 0.12 dB deeper in every band, and follows the fill-in (−30.5 dB at 470 Hz to −27.2 dB at 1400 Hz) | ±0.07 dB |
| Phase 90 block resonance within 0.06 dB | CONFIRMED | −0.06 dB at all nine rates (3.53 against 3.47) | ±0.01 dB |
| Small Stone Color off within 1.14 % | CONFIRMED | min −1.14 to −1.10 %, max −1.09 to −1.08 %. The offset is the same at every rate (held out, 0.63 and 5.0 Hz: −1.16 and −1.08 %); their −0.57 % at 1.78 Hz is their tracker's bias | ±0.05 %, ±0.4 % for the minimum at 1.78 Hz |
| Color off second notch about 6.5 % high | CONFIRMED | +6.49 to +6.54 % at the sweep bottom | ±0.05 % |
| Color on within 3.03 % on both notches at three rates | CONFIRMED | notch 1 max +2.16 to +2.20 %; notch 2 max −3.00 to −2.99 %; notch 2 min +0.69 to +1.29 %; notch 1 fade point +0.56 to +2.21 % at 0.159 and 0.316 Hz | ±0.05 % |
| except notch-1 minimum at 1.78 Hz (+4.19 %) | CONFIRMED as an exception | +4.95 % falling, −2.32 % rising. The quantity depends on the 3 dB rule | ±0.1 %, plus the rule |
| Color-on static response 0.05 to 0.25 dB rms | CONFIRMED | 0.05 to 0.21 dB (section 5) | ±0.02 dB |
| Periods within 0.01 % | CONFIRMED | below 0.0001 %. It is a tautology: the prototype is run at the measured rate | ±0.001 % |

My Guitar Rig 6 readings against `targets.tsv`: Phase 90 within 0.07 % at all six rates (noon:
464.1 to 1523.1 Hz, theirs 464.1 to 1522.8; Rate 0.10: 423.6 to 1590.4, theirs 423.6 to 1590.2).
Small Stone Color off: 148.8 to 149.3 Hz and 3785.6 Hz at every rate; theirs 150.5 to 3780.9 at
0.32 Hz and 157.0 to 3759.1 at 1.78 Hz, which is their window smear. Periods agree to
five digits, except their 6.3087 s for Color on at Rate 0.40 (mine 6.30957 s).

## 4. What the stated figures do not cover

- Phase 90 block, between the sweep ends. Prototype notch 1 against Guitar Rig 6 at every snapshot
  over whole cycles, after the best common time shift:

| Rate Hz | rms % | max % |
| --- | --- | --- |
| 0.143 | 3.80 | +6.30 |
| 0.247 | 3.77 | +6.40 |
| 1.071 | 3.54 | +6.62 |
| 1.930 | 3.10 | +5.97 |
| 3.721 | 2.08 | +3.97 |
| 9.756 | 1.14 | +2.03 |

  At 1.071 Hz, counted from Guitar Rig's minimum: +5.0 % at 15 to 20 % of the cycle, −5.5 % at
  40 %, −4.2 % at 65 %, +6.4 % at 95 %. The prototype's law is too straight: it is above Guitar
  Rig 6 in the lower half of the range and below it in the upper half. The first verification
  found 3.6 % rms and +6.7 % at this rate before the refit. The refit moved the ends and left the shape.
  voicings.md names the cause (square-root law against Guitar Rig's voltage-to-frequency map) but
  gives no mid-sweep number.
- Small Stone, same measure: Color off 1.9 % rms, −3.4 % max; Color on 1.1 % rms, +2.4 % max.
- Whole response while the LFO runs: rms of the dB difference over 100 Hz to 8 kHz per snapshot,
  best common time shift. Small Stone Color on: median 0.11 to 0.16 dB, worst snapshot 0.30 to
  0.34 dB (sweep top), at 0.159, 0.316 and 1.78 Hz. Color off at 0.316 Hz: 0.48 median, 0.78 worst.
  Phase 90 block: 0.99 median, 2.97 worst at 1.071 Hz; 0.47 and 0.96 at 9.76 Hz. The Phase 90
  number is large because the notch is deep and narrow and sits a few percent off mid-sweep.
- Sample rate. All figures are 48 kHz figures. The equations of voicings.md evaluated at other
  rates (not measured on Guitar Rig 6): Phase 90 block at the sweep top, notch 1 1613.9 / 1609.4 /
  1591.1 Hz at 44.1 / 48 / 96 kHz, notch 2 6977 / 7054 / 7401 Hz, depth −27.0 / −26.8 / −26.0 dB.
  Small Stone Color on at f_hi: 1332 / 1329 / 1319 Hz and 6897 / 6965 / 7270 Hz. TPT keeps the
  break frequency, not the notch frequency. f_hi = 3133.5 was chosen to split an error that exists
  at 48 kHz only.

## 5. Static Color-on response (step 4)

Stoned Phaser, Color on, Rate 0 (0.01 Hz), one 101.5 s render, one impulse every 8192 samples.
Each snapshot is a frozen response. 50 snapshots from t = 2 s to 100 s (sweep bottom at 100 s, top
at 50 s). Prototype: frozen response from the voicings.md equations, which equal `phaser_ref.py` to
2e-11 dB (section 6). rms of the dB difference, 100 Hz to 8 kHz, log grid:

| How the prototype's break frequency is chosen | Median | Range |
| --- | --- | --- |
| free, best fit (the authors' definition) | 0.065 dB | 0.048 to 0.206 dB |
| prototype's first notch placed on Guitar Rig's | 0.15 dB | 0.05 to 0.35 dB |
| prototype's own LFO at the same time (spec as written) | 0.12 dB | 0.06 to 0.29 dB |

- Sweep bottom: 0.055 dB (theirs 0.05). Sweep top: 0.206 dB, worst point −0.51 dB at 5.5 kHz
  (theirs 0.25 dB over a band that reaches 9.5 kHz).
- Their two stock-window frames in `targets.tsv` agree with my snapshots at the same times to
  0.007 dB rms (bottom) and 0.010 dB rms (top) over their 56 frequencies. The targets are real.
- The mid and narrow Sweep Min/Max windows were not re-measured. The stock sweep passes through the
  same break frequencies.

## 6. Constants, tags and fit.py (step 5)

Prototype against voicings.md:

- No numeric difference. All 24 constants are equal: 47 nF, 22 kΩ, R0 539.2, x 0.7680 and 0.2139,
  β 0.3957, κ 0.36, gate 4.406 Hz, lag 11818 Hz; Small Stone 355 / 8300 Hz, 0.75 / 0.50; Color
  99.0 / 3133.5 Hz, c 0.7553, 0.1266 / 0.4289, 53.0, 266.0, 18.3, 16.5, 885.2 Hz; κ 0.70.
- The equations of voicings.md are complete. I typed them and the constants from the document
  alone (`docform.py`) and compared with the impulse response of the prototype at frozen break
  frequencies: maximum difference 2e-11 dB for Phase 90 script and block, Small Stone Color off
  and on, 20 Hz to 20 kHz.
- In the prototype, not in voicings.md:
  - the gate smoothing is `s += k·(v − s)` with `k = 1 − exp(−2π·4.406/fs)`, and its state starts
    at the first LFO value;
  - LFO polarity: phase 0 is the sweep bottom for both voicings (Phase 90: `x = centre − half·v`);
  - the lag frequency is clamped to 0.49·fs;
  - CLI default rates 1.0 Hz and 0.32 Hz (voicings.md gives noon as 1.071 and 0.316 Hz);
  - `P90_KAPPA` carries the tag `[AmpliTube LFO]`, which is not one of the three tags.
- Numbers in voicings.md that the equations do not give at 48 kHz:
  - "notch 1 419 to 1589 Hz quasi-static" and "−26.8 dB at 1588 Hz": the top is 1609 Hz
    (0.456·fc). 0.450·fc is the analog ratio. Without the lag it is 1612 Hz.
  - "notch 2 at 2.07·fc, spacing 4.60": 2.07 and 4.59 at the bottom, 2.00 and 4.38 at the top.
  - Checked and right: 154 Hz floor, K 6.28 kHz, fc 930 to 3530 Hz, peak +3.51 to +3.37 dB, DC
    −1.33 dB, −30.7 dB at the bottom, −32.6 dB without the lag, 10.7 µs, +10.4 dB shelf,
    `c·aF·p⁴ ≤ 0.76` (0.741).

`[circuit]` tags, sources opened by me:

| Tag | Source says | Holds? |
| --- | --- | --- |
| Four equal first-order stages [Eichas, DAFx-14, 3.1] | four allpass filters in cascade, all driven by one LFO, eq. 17 | yes |
| 47 nF [GGG parts list, six 0.047 µF] | C2, C3, C4, C5, C9, C10 are 0.047 µF | yes; role from the count, as tagged |
| 22 kΩ [same list, five 22 kΩ] | R3, R4, R5, R6, R15 are 22 k | yes; role from the count, as tagged |
| Script has no feedback [Eichas 3.1] | the 1974 version "did not include the feedback path" with R15 | yes |
| Block node [Eichas figure 4, R15] | R15 runs from the stage-4 output (B) to the inverting node of the second allpass stage | yes |

- Neither source gives a value for the feedback resistor in that role. Eichas gives no component
  values at all.
- The name "R28" (prototype lines 8 and 37; voicings.md rows "Block ratio β" and "Feedback lag L")
  is in no opened source. Eichas calls the feedback resistor R15. In the cited parts list R28 is
  56 kΩ and R15 is 22 kΩ. The two documents number parts differently (Eichas' shunts R8, R11, R14
  are 10 k in the parts list), so the list does not identify the feedback resistor. If it is the
  fifth 22 k, β would be 10/22 = 0.45 against the fitted 0.3957. voicings.md tags β as a fit,
  which is correct.
- `fit.py` docstring: "kappa stays at its circuit value". It is not a circuit value (section 7).

`fit.py`, run unmodified from `targets.tsv` (full run 3 min; it needs the probe files, which I
regenerated with `track.py gen` and deleted):

| Constant | voicings.md | fit.py rerun | Digits |
| --- | --- | --- | --- |
| β | 0.3957 | 0.3957 | 4 |
| x_off, x_on | 0.7680, 0.2139 | 0.7682, 0.2142 | 3 |
| gate | 4.406 Hz | 4.400 Hz | 3 |
| R0 | 539.2 Ω | 538.8 Ω | 3 |
| feedback lag | 11818 Hz | 11818 Hz | 5 |
| Small Stone dry, wet (off) | 0.75, 0.50 | 0.7494, 0.5008 | 2, as rounded |
| Color c, dry, wet | 0.7553, 0.1266, 0.4289 | same | 4 |
| Color f_p1, f_p2, f_zf, f_za, f_zb | 53.0, 266.0, 18.3, 16.5, 885.2 | 52.99, 266.0, 18.32, 16.48, 885.2 | 3 to 4 |
| Color f_lo, f_hi | 99.0, 3133.5 | 99.0, 3133.5 | 3, 5 |

- The Phase 90 fit starts from the prototype's current constants. From two other starts
  (`fit_start.py`: x 0.85 / 0.30, gate 7; x 0.70 / 0.15, gate 3) it returns x_off 0.7670 to 0.7673,
  x_on 0.2118 to 0.2126, gate 4.407 to 4.412, R0 540.7 to 541.4, lag 11813 to 11814, with the same
  cost (0.82 %). So "a rerun lands within 0.2 %" holds from the published start only; x_on moves
  1.0 % between starts. The minimum is flat; the sweep is the same.
- The Color structure fit starts from three generic points and lands on the published constants.
- Not regenerated by fit.py, as voicings.md says: Small Stone 355 / 8300 Hz and κ 0.70 (scripts not
  kept), κ 0.36 (not fitted). My numbers support 355 / 8300 to −1.1 % and κ 0.70 (mid-rise
  fraction 0.573 against 0.575).

## 7. Fence check (step 6)

Structure. The prototype's Color-on form is the binary's form as described in
`evidence/stoned_color_structure.md`: one shared break frequency, feedback from the fourth stage to
the first, one two-pole network with two outputs, `F = 1 − BB` with BB unity at DC, input shaping
on the wet path only, dry unfiltered. The prototype drops the unit delay. That is structure, which
is allowed.

Constants. I read every floating-point immediate and literal-pool load in 20 Guitar Rig 6
functions (StonedPhaser, RigPhaserBT, PhaserMXR, OrangePhaserAU: 236 values, 83 distinct) and
compared them, also as 2π·x and 1/x, with every constant of the prototype and voicings.md and with
the derived ka, kb, kf, 1+β, 2+β.

- As plain values, no prototype constant is within 1 % of a Guitar Rig 6 constant, except the two
  below. The transformed forms give two chance hits with no meaning (1/(2π·β) = 0.402 near a 0.4 in
  the rate law; 1/(2π·1.2) = 0.1326 near the 2/15 of a tangent series).
- 0.5 (Phase 90 dry and wet, Small Stone wet): equal to Guitar Rig's 0.5. Trivial.
- Small Stone κ = 0.70 equals a 0.7 in `calculateFilterCoeffs` (0x6980b8) and
  `updateBaseCoefficients` (0x2d9310). There it scales Color strength (`min(|c|/0.7, 1)`); it has
  nothing to do with the LFO shape. I read it as a coincidence. The script that fitted κ was not
  kept, so the provenance cannot be re-run; the measured shape supports the value.
- Guitar Rig's own values against ours, none equal: sweep limits 93 / 2960 Hz and 307 / 8500 Hz
  (0x611ea0 to 0x611eb8) against 99.0 / 3133.5 and 355 / 8300; c 0.74 against 0.7553; dry 0.13 and
  wet 0.87 × 0.501 = 0.436 against 0.1266 and 0.4289; network poles 50 Hz (clamped) and 267.5 Hz,
  worked out from the binary's element values at c = 0.74, against 53.0 and 266.0; Phaser Nine
  feedback 0.95 × 0.4217 = 0.4006 and stage-2 0.2030 against β 0.3957; control lowpass 3.65 Hz
  against gate 4.406 Hz. These are what a fit to that device should give: close, not identical.
- The Color constants are produced by `fit.py` from measured frames, and those frames match my own
  measurement to 0.01 dB.

Crossing or near the line:

1. Phase 90 κ = 0.36 is derived from the AmpliTube 5 binary: square wave into a one-pole at
   0.1135332 × rate (0xd89c44, 0x3de88414), so κ = π × 0.1135 = 0.357, rounded. It appears as
   `P90_KAPPA = 0.36` (prototype line 38) and in the "LFO shape" row of voicings.md, tagged
   `[guess]`. It is one scalar, not a table, and not from Guitar Rig. It is still a vendor literal.
   The fit shows it is inert (free κ gives 0.45 with the same error).
2. `evidence/tools/gr6_mxr.py` (58 lines) and `gr6_mxr_static.py` (32 lines) transcribe NI's
   Phaser Nine loop with its literals (49157.8, −417070, 1179910, −1112860, 12652.6, −37689.5,
   1.2030238, 1.862087, 0.1692, 3.145). Marked analysis only. Already flagged in the first
   verification. Not used by the prototype.
3. `$RE_STATE/work/phaser-recheck/ana/gr6_color_read.py` (34 lines, scratch, outside the repo)
   transcribes `calculateFilterCoeffs` with its pool values. Marked analysis only.
4. voicings.md quotes Guitar Rig numbers in its Source column (rate law, 1.862, 0.5 / 0.5,
   16.4 kHz). They are references, not our constants.

Nothing from items 2 to 4 is in `phaser_ref.py` or in the voicing tables as a constant to build.

## 8. Audition WAVs (step 7)

All ten files: 48 kHz, mono, float. Notch movement read from each file against its dry file
(short-time cross-spectrum, 1/6-octave smoothing, 21 ms frames for the Phase 90 and 85 ms for the
Small Stone; period by phase folding). This reader is coarse: it rounds off sharp turning points and
its period resolution is about 0.3 % (Phase 90) to 3 % (Small Stone, three cycles in the file).
"Expected" is the prototype at the same rate by the impulse tracker.

| File | Peak dBFS | rms dBFS | Samples ≥ 1.0 | File − prototype(dry) | Notch 1 Hz (expected) | Notch 2 Hz (expected) | Period s (expected) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| dry_noise | −8.33 | −22.00 | 0 | | | | |
| phase90-script_noise | −9.46 | −23.71 | 0 | 2e-8 | 447 to 1389 (427 to 1405) | 2498 to 7463 (2469 to 7537) | 0.933 (0.934) |
| phase90-block_noise | −8.77 | −22.63 | 0 | 2e-8 | 484 to 1534 (464 to 1525) | 2130 to 6689 (2129 to 6715) | 0.935 (0.934) |
| smallstone_noise | −7.08 | −21.49 | 0 | 3e-8 | 158 to 3342 (147 to 3745) | 935 to 10494 (856 to band edge) | 3.153 (3.163) |
| smallstone-color_noise | −4.24 | −19.52 | 0 | 5e-8 | 148 to 1248 (fades near 142, to 1329) | 485 to 6607 (433 to 6965) | 3.128 (3.163) |
| dry_riff | −6.02 | −30.41 | 0 | | | | |
| phase90-script_riff | −7.24 | −33.42 | 0 | 2e-8 | 447 to 1389 | 2523 to 7463 | 0.933 |
| phase90-block_riff | −5.36 | −32.33 | 0 | 4e-8 | 447 to 1534 | 2130 to 6623 | 0.935 |
| smallstone_riff | −5.19 | −31.11 | 0 | 2e-8 | 154 to 3426 | 944 to 10287 | 3.153 |
| smallstone-color_riff | −3.69 | −31.31 | 0 | 3e-8 | 143 to 1248 | 530 to 6381 | 3.066 |

- No file is silent. No file reaches full scale. Levels equal `audition/peaks.txt`.
- Every file equals the current prototype applied to the dry file to float32 rounding. The files
  are the round-2 voicings.
- Rates and ranges match the voicings within the reader's resolution. The riffs are quiet
  (−31 to −33 dBFS rms), as before.
- The earlier +1.19 dBFS overshoot of `smallstone-color_noise.wav` is gone (−4.24 dBFS).

## 9. Can a C++ implementer build from voicings.md as written?

Yes. The equations and constants in voicings.md are complete and unambiguous: typed from the
document alone they reproduce the prototype to 1e-11 dB, and the delay-free loop solutions are
written out. What the implementer would get wrong or should know: (1) the Phase 90 sweep will match
Guitar Rig 6 at its two ends and miss by up to 6.6 % in between, because the square-root law is
straighter than the reference; the accuracy table does not say so. (2) The "stock sweep" row says
notch 1 reaches 1589 Hz; the equations give 1609 Hz at 48 kHz, so a unit test written from that
row fails by 1.3 %. (3) The accuracy holds at 48 kHz. At 96 kHz the Phase 90 block top moves −1.1 %
(notch 1) and +4.9 % (notch 2) and its notch is 0.8 dB shallower; the Color f_hi was tuned to a
48 kHz compromise. (4) The gate smoothing form and its initial state, the LFO polarity and start
phase, and the lag clamp are only in the prototype; a different start state changes the first
0.2 s, nothing else. (5) The feedback resistor is called R28 in the text and R15 in the only
circuit source; β is a fit to Guitar Rig 6 and no circuit value backs it. (6) The reference is
Guitar Rig 6 at Color 0.4217 with four stages, and at Color Strength 0.74; the plugin's INIT
component presets differ, so an A/B against a freshly loaded component will not match. (7) κ = 0.36
is an AmpliTube constant and should be replaced or refitted before it ships. (8) Guitar Rig's
Phaser Nine rolls off the top (−0.8 dB at 8 kHz); ours does not, by design.

## 10. Not done, and tool notes

- Guitar Rig 6 was not rendered at 44.1 or 96 kHz. The sample-rate figures are from the equations.
- The mid and narrow Sweep Min/Max static frames were not re-measured.
- The Color-on fade point was not measured at the held-out rates. The Small Stone sweep bottom at
  5 Hz could not be read (tracker not valid there).
- No listening. No GUI. AmpliTube 5 was not loaded; κ's origin is taken from the evidence notes and
  `claims.tsv` C15.
- One of 32 Guitar Rig renders failed when three ran in parallel and passed on a serial re-render
  (the authors saw the same).
- WebFetch could not parse the two PDFs; I read the saved PDFs instead.
- Disk: scratch stayed under 300 MB; every WAV was deleted after its numbers were written.

Reproduce (scratch): `selftest.py`; `run.py <take> both`; `heldout.py`; `final_table.py`;
`run2.py` then `fade.py`, `dyn.py`; `traj.py`; `static_gr.py on_stock "Rt=0 Cl=1"` then
`static_cmp.py`; `doccheck.py`; `fit_start.py`; `fence.py`; `init_check.py`; `audition_check.py`.
