# Drive and EQ: adversarial verification

Verifier build: `build-verify/drive-eq/` (Release) and `build-verify/drive-eq/asan/`
(`-fsanitize=address,undefined`), worktree at 157ab02, Apple M4 Pro, arm64. Nothing under
`src/`, `ui/`, `tests/`, `tools/` or `docs/` was changed.

Harness: `build-verify/drive-eq/harness/procrun.cpp` drives the whole
`TubampAudioProcessor` headless (chain order, parameter events and ramps, block-size
patterns, parameter fuzzing, allocation counting through `malloc_logger`, latency log).
It links against the object files of `tubamp_golden` (`harness/build.sh`). Python drivers
are `harness/t_*.py`; raw results are in `build-verify/drive-eq/results/`. Unless a line
says "engine alone" (then `tubamp_fxbench`), every number below was measured through the
full processor.

Detectors were shown to fail before they were trusted:

- Alias detector: the same script reads -18 to -20 dBc for Classic (2x, no ADAA).
- Allocation counter: a deliberate `malloc` per block inside the audio call is counted 75 of 75;
  the first version of the hook crashed the harness and its results were discarded.
- Latency: a first method (cepstral minimum-phase excess delay) flagged the Fuzz as 1 sample
  wrong. That was the method (three in-band 720 Hz poles), not the engine; the figures below
  use the phase against an analog model of each circuit.

## Verdicts

| Claim | Verdict | Number |
|---|---|---|
| D-a alias | VERIFIED as stated; weaker outside the stated conditions | -81.1 / -88.7 / -86.0 dBc reproduced. Worst found: Screamer -47 dBc, Distortion -70 dBc, Fuzz -40 dBc (16-17 kHz tones); guitar range -73 / -78 / -68 dBc |
| D-b character | VERIFIED for Screamer; Distortion and Fuzz circuit-consistent but off the commercial references | Screamer peak ~780 Hz, 110 Hz -11.0, 5 kHz -10.3 dB, H3 -21 dB, H2 < -160 dB |
| D-c level, DC, clicks | PARTLY: tone-knob moves are NOT click-free | Screamer tone sweep -59 dBc under a 98 Hz note (team bar -80 dBc) |
| D-d latency | VERIFIED; the 0.5 dB split bar holds only for an idealised flat lane | worst rounding error 0.36 sample; real split sum 2.3 dB short at 10 kHz (44.1 kHz) |
| D-e switches, Classic | VERIFIED | 240 switches, worst step +1.5 dB re steady; golden 339 rows, 0 unexpected |
| E-a Classic exact | VERIFIED | 0 differing samples in every flat and static case |
| E-b automation | VERIFIED for discontinuities | worst -68.6 dBFS (rapid toggling), -76.6 dBFS (single step) |
| E-c Parametric | VERIFIED | bell worst 1.91 dB (at 2.85 kHz, not near Nyquist); engine residual -152 dBc |
| E-d Stack | VERIFIED, bar judged acceptable | 0.36 dB to 5 kHz, 1.66 dB to 10 kHz, 13.6 dB at 20 kHz (44.1 kHz) |
| E-e Graphic | VERIFIED | all bands +12 gives +18.4 dB; single band 12.00 dB, 0.99-1.01 octave |
| E-f float noise | CONFIRMED, and wider than stated | fresh instance too; Parametric/Graphic are never bit-exact in the plugin (-152 dB) |
| X-a robustness | NOT VERIFIED for NaN/Inf; verified for finite input and allocations | one NaN latches the output forever; 0 allocations; 0 sanitizer reports in 688 runs |
| X-b CPU | Distortion over budget | Screamer 1.75 %, Distortion 3.0-3.4 %, Fuzz 3.0-3.1 % (48 kHz, block 64, stereo) |

## Drive

### D-a Aliasing

Method: 1 s exactly periodic analysis window after 2 s of settling, rectangular window,
1 Hz bins. Every bin below 20 kHz that is not a multiple of the input's fundamental (or of
the common base of a two-tone or harmonic stack) is alias. "dBc" is re the strongest
legitimate component at the output. `harness/t_alias1.py` (2880 runs), `t_alias2.py`
(1200 runs); `results/alias_single.tsv`, `alias_multi.tsv`.

Stated conditions (1244, 3137, 7500 Hz, -6 dBFS, gain 36, tone 12 kHz, 44.1 and 48 kHz):
Screamer -81.1, Distortion -88.7, Fuzz -86.0 dBc. Classic under the same detector: -18.2 dBc.

Attack results (worst alias sum, dBc; absolute level in brackets where it matters):

| Case | Screamer | Distortion | Fuzz |
|---|---|---|---|
| single tone to 7.9 kHz, any level -30..0 dBFS, tone 12 kHz | -80.3 (7.5 kHz, -18 dBFS) | -85.9 (6.1 kHz, 0 dBFS) | -76.0 (331 Hz, 0 dBFS) |
| same, tone 500 Hz | -79.9 | -77.9 (7.5 kHz, 0 dBFS) | -68.5 (6.1 kHz, 0 dBFS; -97 dBFS) |
| guitar-like harmonic stacks (83, 331, 991, 1319 Hz), re total output | -73.1 (991 Hz, 0 dBFS) | -79.5 | -82.8 |
| two tones up to 8 kHz | -77.2 | -81.3 | -82.6 |
| two tones 10 + 11 kHz | -64.0 | -74.3 | -51.1 (alias at 237 Hz) |
| two tones 14.5 + 16 kHz | -61.6 | -65.8 | -58.5 |
| single tone 16 kHz (near 44.1 kHz fs/3 region) | -55.8 tone 12 k, -47.3 tone 500 (-81 dBFS, alias at 389 Hz) | -87 | -39.8 at 0 dBFS, tone 500 (-80 dBFS) |
| single tone 17-19 kHz | -52.8 | -69.6 | -49.2 |
| gain 0 and 18 | -54.8 (19 kHz) | -80.5 | -77.2 |

88.2 and 96 kHz equal 44.1 and 48 kHz to 0.1 dB (same internal rate). The weak cases are
tones above 10 kHz whose high odd harmonics fold about the internal 176.4 kHz rate; they are
outside what a guitar produces and sit at or below -72 dBFS absolute. For the study's
comparison case (5 kHz, -6 dBFS, full drive, sum to 20 kHz) the new types read -90.1, -89.2
and -97.0 dBc.

### D-b Character (`t_char.py`, `t_char2.py`, `results/character*.txt`)

Screamer, 48 kHz:

- Small-signal response at d = 0.5, T = 0.5: peak near 780-812 Hz, 110 Hz -11.0 dB, 5 kHz
  -10.3 dB re peak (GR6 Skreamer 720 Hz, -10.3, -11.1; AT5 -11.2, -11.2).
- Input for 1 dB compression re 2.5 kHz: 110 Hz -16.4 dB, 640 Hz -3.2 dB: a first-order
  720 Hz high-pass ahead of the clipper (GR6 Skreamer AU with its 587 Hz corner: -15.0, -2.7).
- 1 kHz at -18 dBFS, mid drive: H3 -21.1 dB, H2 below -160 dB, THD 5.7 / 9.3 / 7.2 % at
  -30 / -18 / -6 dBFS (references H3 -18.5 to -21.1; THD 7-10 / 9-13 / 7-14 %).
- Clean mix: output peak grows 17.7 dB (mid) and 10.8 dB (high drive) for a 36 dB input step
  (Skreamer Deluxe 16.0 / 11.1, AT5 11.9 / 10.7, GR6 classic 7.0 / 2.9).
- Gain at 1 kHz, low / mid / high: 15.1 / 20.6 / 34.5 dB (references 9-14 / 15-23 / 27-29).
- Against my own nodal model of the TS-808 (diode small-signal conductance included), 60 Hz
  to 10 kHz: within 0.38 dB for d of 0 and 0.5; -0.82 dB at 9.9 kHz for d = 1 (48 kHz),
  -0.90 dB at 44.1 kHz. The engine scales the clipped path by a constant instead of letting
  the diodes load the 51 pF pole.

Distortion: gain 0 is unity and clean; small-signal gain at 1 kHz is 0 / 9 / 17.7 / 34.6 /
42.1 / 57.9 dB at knob 0 / 1 / 3 / 12 / 18 / 36 dB. Filter knob -3 dB at 506 Hz (T = 0),
4.3 kHz (centre), above 20 kHz (T = 1). At the starting point (gain 18) THD is 26 / 31 / 32 %
at -30 / -18 / -6 dBFS and the output moves 1.5 dB for a 36 dB input change (AT5 RAT at mid:
2.3 / 11.9 / 15.2 %, 15.4 dB; its mid gain is 17.7 dB against our 42 dB). The bass roll-off
with gain (110 Hz -15.5 dB re peak at mid) follows the 47 ohm / 2.2 uF leg.

Fuzz: THD 74-77 %, H3 -5.3 dB (AT5 Big Muff 77-87 %, -4.2 dB); output moves 3.3 / 0.6 dB
for 36 dB of input. Tone stack matches my nodal solve of the cited network within 0.08 dB;
at centre the response at 900 Hz is 6.0 dB below the line joining 220 Hz and 5 kHz, minimum
-16.5 dB at 1005 Hz. The cited paper's own values give 3.4 dB; the 6 dB comes from the
Ram's Head values substituted in the code. Pre-clip (P1dB method): 640 Hz -1.3, 1.28 kHz
-9.0, 2.5 kHz -23.6 dB re 110 Hz; both reference Muffs are flat to 900 Hz and -13 dB at
2.5 kHz. Small-signal peak is at 90 Hz (references 220 Hz), 5 kHz is -49 dB re peak
(references -26 to -33 dB).

Component values, read in the cited public sources by a helper agent (PDFs via pdftotext):

- TS-808 (Yeh, Abel, Smith, DAFx-07): R1 4.7 k, Cz 0.047 uF, Cc 51 pF, R2 51 k + 500 k;
  tone stage 1 k, 0.22 uF, 220 ohm, 0.22 uF, 20 k, 10 k. All as coded. Pot taper A500k from
  sandelinos.me. Diode type is not in Yeh; sandelinos names MA150, the code comment says
  1S1588/1N914.
- RAT (madbeanpedals Runt2020 BOM): 47R, 560R, 2u2, 4u7, GAIN 100kA, 100 pF, 1 k, 1n914,
  CUT 100kA, 1k5, 3n3, LM308, 30 pF. All as coded; the BOM gives values, not wiring.
  cushychicken confirms the 32 kHz to 475 Hz filter range.
- Big Muff (guitarscience bigmuff.pdf): 470 k, 470 pF, 10 k, 39 k, 1 uF as coded; the stages
  are BJT stages, which the code idealises as op-amp gains. Ram's Head stack 39 k / 10 nF /
  4 nF / 22 k is on the coda-effects page; SUSTAIN B100K and 1N914 on the PedalPCB BOM.
- electrosmash.com could not be opened.

### D-c Level, DC, knob moves (`t_dc.py`, `t_zip.py`)

- Not silent at gain 0: small-signal gain at 1 kHz is Screamer +15 dB, Distortion 0 dB,
  Fuzz +21 dB, Classic / Boost / Treble 0 dB.
- Starting points, guitar-like stack at -18.6 dBFS rms in: Classic -7.8, Screamer -12.3,
  Distortion -13.6, Fuzz -13.8, Boost -6.6, Treble -18.1 dBFS rms. The three pedals sit
  within 1.5 dB; Distortion and Fuzz give -12.0 dBFS peak for a -18 dBFS sine.
- DC with a +0.2 offset on the input at gain 36: at most -155 dBFS. Silence in gives exact 0.
- Gain and level moves: no excess over the steady state in the saturated types; clean types
  are at the spectral skirt of a 20 ms ramp.
- Tone moves zipper in every new type that runs its tone section as a direct-form filter
  stepped every 32 samples. Tone 500 to 12000 Hz under a 98 Hz note, line level above 3 kHz
  (Hann 2048 STFT) re the output peak, 0.5 s sweep: Screamer -59.1 dBc, Treble -76.7,
  Boost -85.6, Distortion -95.0, Classic -97.4. Time-domain peak above 3 kHz re the note:
  Screamer -32 dB (50 ms sweep), -44 dB (0.5 s), -51 dB (2 s); Treble -32 / -49 / -61;
  Boost -44 / -58 / -69; Classic -68 / -86 / -92. `check_drive.py` D17 uses a 1 kHz
  carrier, which masks it.

### D-d Latency (`t_lat2.py`, `t_lat3.py`, `t_lat4.py`)

Pure delay at 1 kHz relative to an analog model of each circuit, in samples, against the
value the processor reports:

| Rate | Classic | Screamer | Distortion | Fuzz | Boost, Treble |
|---|---|---|---|---|---|
| 44.1 kHz | 3.14 / 3 | 4.69 / 5 | 5.12 / 5 | 4.94 / 5 | 0.00 / 0 |
| 48 kHz | 3.13 / 3 | 4.69 / 5 | 5.12 / 5 | 4.94 / 5 | 0.00 / 0 |
| 96 kHz | 3.11 / 3 | 3.64 / 4 | 4.76 / 5 | 4.14 / 4 | 0.00 / 0 |
| 192 kHz | 3.09 / 3 | 1.00 / 1 | 3.79 / 4 | 2.01 / 2 | 0.00 / 0 |

Worst error 0.36 sample (Screamer, 96 kHz). The delay rises with frequency (Classic 3.41 at
10 kHz and 4.03 at 16 kHz at 44.1 kHz). Three drives in series report 15 at 48 kHz.

The report follows `drive_type` and `drive_on` in the block where the parameter changes
(48 kHz log: 3, 5, 0, 3, off 0, on 3, Fuzz 5; 96 kHz: 3, 4, 5, 4, 0, 3, 0, 3, 4).

Split, drive in lane A, lane B empty. The lane wait itself is exact (measured sum equals
lane A plus a dry signal delayed by the reported integer, 0.000 dB). The team's figure is
the comb for a hypothetical flat lane, and I reproduce it: -0.37 dB at 10 kHz and -8.22 dB
at 16 kHz at 44.1 kHz; -0.25 and -3.88 dB at 48 kHz. What is heard with Classic at gain 0
and its most open tone (12 kHz), sum re dry-only, against a perfectly aligned sum:

| | 5 kHz | 8 kHz | 10 kHz | 12 kHz | 14 kHz | 16 kHz |
|---|---|---|---|---|---|---|
| 44.1 kHz heard | -0.44 | -1.86 | -4.49 | -10.55 | -10.96 | -5.78 |
| 44.1 kHz aligned | -0.27 | -1.01 | -2.18 | -4.26 | -6.71 | -7.58 |
| 48 kHz heard | -0.45 | -1.82 | -4.12 | -8.74 | -12.15 | -7.62 |
| 48 kHz aligned | -0.30 | -1.11 | -2.30 | -4.26 | -6.49 | -7.57 |

The tone filter's own phase lag adds to the uncompensated fraction, so the real excess loss
is 2.3 dB at 10 kHz and 6.3 dB at 12 kHz (44.1 kHz), not 0.4 dB. At 96 kHz it is 0.43 dB at
10 kHz. Audibility: through a guitar cab (the cab block's high cut defaults to 8 kHz, and a
guitar speaker is far down by 6-8 kHz) the excess is 0.2 dB at 5 kHz and 0.85 dB at 8 kHz,
which is inaudible. Without a cab (clean parallel blend, bass DI) it is a slight dulling
above 9 kHz that an A/B on bright material would show. No cab IR is in the repo, so this was
reasoned, not measured.

### D-e Type switches, Classic (`t_sw.py`)

240 switches (every ordered pair, 44.1 / 48 / 96 kHz, blocks 37 / 64 / 256 / 512, with and
without the starting-point knobs) under a 98 + 220 Hz chord: the largest sample step during
a switch is at most +1.5 dB above the louder steady state; one case above +1 dB, none above
+3 dB; 0 non-finite. The switch fades through the dry input, so the level dips by up to
15.8 dB for about 40 ms (Boost to Distortion).

Classic: `tubamp_golden --verify tests/golden/fx-golden.tsv` through the processor: 339 rows,
270 identical, 69 changed as expected (drive rows: audio same, latency +3), 0 unexpected.
`--legacy-check`: 168 renders, 0 differences. `tubamp_fxbench drive --classic-null` at
44.1 / 48 / 96 / 192 kHz, blocks 1 / 37 / 512, three settings: 0 differing samples in all 36.

## EQ

### E-a Classic (`t_eq_exact.py`, `t_eq_near.py`)

- Static non-flat: 60 fxbench nulls (4 rates, blocks 1 / 64 / 512, five settings including
  5.0001), 0 differ; golden rows `eq*_shape`, `eq*_cut` identical through the processor.
- Exactly flat: eq, eq2, eq3 at 44.1 / 48 / 96 / 192 kHz, stereo block 512 and mono block 37,
  fresh and after a state round trip: 0 differing samples against an empty chain. Amp tone
  stack at 5/5/5 against `amp_eq_on` off with the A2 model: 0 differing samples at all four
  rates.
- Near flat (engine alone, exact floats): bass 5.0001 or 4.9999 leaves a -90.5 dB residual,
  mid 5.0001 -96.7 dB, bass 5.1 -46.3 dB.
- Crossing flat under a 98 Hz -12 dBFS note: ramps through 5.0, steps 5 / 5.0001 / 5 /
  4.9999 / 5 and toggling 5 with 5.0001 every two blocks stay at or below -111 dBFS above
  1 kHz.

### E-b Automation (`t_eq_click.py`, `t_eq_over.py`; 532 cases)

Line level above 1 kHz under a 98 Hz -12 dBFS note, block 64:

| Move | Classic | Parametric | Stack | Graphic |
|---|---|---|---|---|
| instant full-range step, worst | -92.4 | -81.7 (Q 8 to 0.5) | -83.0 | -76.6 (one band, others at +12) |
| 50 ms ramp, worst | -94.1 | -94.2 | -102.1 | -92.5 |
| 0.5 s ramp | at the steady floor (-102 dBFS) in every type |
| toggled between extremes every 4 ms | -77.5 | -73.9 | -79.7 | -68.6 |

One case is much higher and is not a discontinuity: mid frequency jumped from 100 Hz to
8 kHz with the bell at +15 dB, Q 8, sitting on the note: -26.0 dBFS (step), -32.7 dBFS
(50 ms). The note is at +3 dBFS under that bell and loses 15 dB in 10 ms. With the bell at
-15 dB it is -58.2 dBFS.

Under noise, toggling any knob between its extremes (blocks 1 to 512, 48 and 192 kHz, all
four Stack voices, mid frequency at Q 8) never raises the output peak above the louder of
the two static settings (+0.0 dB), and nothing went non-finite. A click under noise cannot
be separated spectrally, so that is the only noise test made.

Type and voice switches under the note: at most -86.4 dBFS above 1 kHz.

### E-c Parametric (`t_eq_resp.py`, `t_eq_resid.py`)

- Bell against the analog prototype, 20 Hz to min(20 kHz, 0.45 fs), f0 100 Hz to 8 kHz,
  Q 0.5 to 8, +-15 and +-3 dB: worst 1.91 dB at 44.1 kHz, 1.54 dB at 48 kHz, 0.29 dB at
  96 kHz, 0.07 dB at 192 kHz. The 1.91 dB is the Q 0.5 bell at 8 kHz and it sits at
  2.85 kHz (bandwidth guard), not near Nyquist. For f0 up to 1 kHz the worst is 0.27 dB.
  Half-gain Q measured over set Q is 1.00 to 1.02 where the upper edge is below Nyquist.
- Shelves within 0.04 dB at every rate. Cuts -3.01 dB at the set frequency, -12.3 dB one
  octave out, -24.1 dB two octaves out; the high cut is not decramped (8 kHz cut reads
  -21.3 dB at 16 kHz at 44.1 kHz, analog -12.3 dB). Tilt -5.99 / 0.00 / +6.00 dB at 20 Hz /
  700 Hz / 20 kHz.
- Parametric against Classic for one band at 10: at most 0.005 dB apart.
- Residual, 82.4 Hz at -20 dBFS, fundamental fitted and removed. Engine alone at 192 kHz:
  Parametric -151.7 dBc, with low cut 21 Hz + bass 10 + mid 10 at 100 Hz Q 8 -153.0 dBc,
  Stack -147.1, Graphic -152.1, Classic -68.7 dBc. Through the processor the floor is the
  always-on float DC blocker: empty chain -139.4 dBc at 48 kHz and -132.6 dBc at 192 kHz;
  the new types read -130 to -138 dBc there.

### E-d Stack (`t_eq_stack.py`)

Error against the analog curve over a 3x3x3 knob grid, every voice: 44.1 kHz 0.36 dB to
5 kHz, 1.66 dB from 5 to 10 kHz, up to 4.6 dB at 15 kHz and 13.6 dB at 20 kHz; 48 kHz 0.31 /
1.37 / 3.6 / 9.1 dB; 96 kHz 0.07 / 0.31 / 0.7 / 1.4 dB. Judgment: a tone stack is used ahead
of a power stage and cab that pass little above 6 kHz, and the error below 5 kHz is a third
of a dB, so the bar is reasonable. Used as a free-standing EQ on a full-range signal the top
octave is wrong at 44.1 kHz.

Voicings at noon, makeup included (80 Hz / dip / 5 kHz): Tweed +6.2 / 773 Hz -5.7 / +3.0;
Blackface +4.2 / 409 Hz -9.4 / +8.5; British +4.8 / 732 Hz -4.4 / +1.4; Top Boost +8.2 /
667 Hz -10.7 / +6.3 dB. The controls interact: British mid 0 to 10 moves the dip from 849 to
601 Hz (-9.4 to -0.1 dB); treble at 10 moves it to 541 Hz; bass at 0 takes 80 Hz from +4.8
to -5.7 dB. Blackface is the deepest and lowest scoop and British the shallowest and highest,
which is the right ordering for the circuits named. Top Boost uses the Fender-Marshall
topology with a fixed 10 k in place of a mid pot.

### E-e Graphic

Single band at +12: peak 12.00 dB at the centre, half-gain width 0.99 to 1.01 octave (0.94 at
192 kHz for 100 Hz, a bin-resolution effect), error against analog at most 0.26 dB (6.4 kHz
band at 44.1 kHz). All seven at +12: +15.2 / +17.7 / +18.3 / +18.4 / +18.3 / +17.8 / +15.1
at the centres (6.4 dB of build-up, 5 dB between the middle and the ends). 400 + 800 at +12:
+14.6 dB. Alternating +12 / -12: +9.9 / -7.4 / +7.9 / -7.8 / +8.0 / -7.3 / +9.9. All at zero
is not bit-exact in the plugin (E-f).

### E-f Default float noise

Read back from the parameter atomics: `eq_tilt` 8.94e-08, `eq_level` and every graphic band
1.79e-07, `drive_level` 3.58e-07, `mix_alevel` 8.94e-07; `eq_bass`, `drive_gain`, the cut
frequencies and Q are exact. This is so on a fresh instance, after a state round trip, and
after the host writes 0 (the range snaps `start + step * k` with a fused multiply-add, so any
stepped range with a non-zero start is affected). Parametric and Graphic at their defaults
differ from the input in 25-50 % of samples at -151 to -158 dB re the signal at every rate;
the fxbench checks E4 and E17 pass only because fxbench feeds an exact 0.0. Also touched:
the tilt and level stages are never skipped (Parametric at defaults costs 0.044 % instead of
nothing), low graphic bands run as 1.8e-7 dB bells, and any "exactly at default" fast path
keyed on drive level, mix levels or other ranges that do not start at zero will never fire.

## Both

### X-a Robustness (`t_stress.py`)

- Allocations inside `processBlock`: 0 in 1524 Release runs (counter validated above).
- Finite input (+24 dBFS noise, 1e-40, DC at 1.0, +-100 spikes, silence): 1248 static runs
  (22.05 to 192 kHz, mono and stereo, blocks 1 / 64 / 4096, six drive types x four EQ types,
  every parameter at both extremes) and 276 fuzz runs (35 % of blocks change 1-3 random
  drive / EQ / amp-EQ parameters to minimum, maximum or random, including types, voices and
  on/off; blocks 1 to 4096 including blocks larger than prepared; split chains; amp with the
  A2 model): 0 non-finite samples.
- ASan + UBSan: 480 static and 208 fuzz runs with the full nasty input: 0 reports, 0 crashes.
- NaN, +-Inf: NOT handled. Ten NaN samples at the plugin input leave the output NaN for the
  rest of the run with an EMPTY chain: the processor's always-on DC blocker latches it and
  nothing sanitises the input. Engine alone: Classic drive, Classic EQ, Parametric and Stack
  latch NaN and Inf for good (`flushed()` and the state flush do not test for NaN, against
  section 2.2 "a bad sample cannot live in a loop"); Graphic only escapes while every band
  is an identity. Screamer, Distortion, Fuzz, Boost and Treble recover. A finite 3e38 sample
  also latches Classic drive and Classic EQ.
  Reproduce: `procrun --chain "" --in <2 s sine with 10 NaN>`; `tubamp_fxbench eq nan.wav
  out.wav type=1 bass=7`.

Also found here (`t_reenable.py`, `results/reenable_nan.txt`): a bypassed block keeps its state frozen
and releases it when re-enabled. Note playing, `drive_on` or `eq_on` off, input goes silent,
on again 1 s later: the output bursts at -8.3 dBFS (Screamer), -9.7 (Classic drive), -11.3
(Fuzz, 18 ms later), -12 to -14.5 (others), +1.7 dBFS (Classic EQ, bass 10), +14.0 dBFS
(Parametric, bass 10 and a +15 dB Q 8 bell on the note), -4.9 dBFS (Graphic). Toggling on
under the note is a hard switch: the sample step is 23 to 45 dB above the steady maximum.
The frozen-state behaviour predates this work for Classic; the new engines inherit it.

### X-b CPU (`t_cpu.py`, `results/cpu.txt`)

Percent of one core, whole processor minus the empty chain (0.067 % at 48 kHz), stereo,
minimum of 5. The machine was under load from other builds (load average 13-16), so treat
these as upper figures; `tubamp_fxbench --bench` under the same load agrees (Screamer 1.73,
Distortion 2.98 at gain 18 and 3.42 at gain 6, Fuzz 2.97).

| | 48 kHz, block 64 | 96 kHz, block 64 | silence, 48 kHz | budget |
|---|---|---|---|---|
| Classic drive | 0.17 | 0.33 | 0.14 | 0.35 |
| Screamer | 1.75 (mono 0.90) | 1.66 | 1.19 | 2.0 |
| Distortion | 3.36 (1.42 at gain 36) | 3.41 | 2.41 | 2.0 |
| Fuzz | 3.06 | 3.20 | 1.92 | 3.0 |
| Boost / Treble | 0.02 | 0.05 | 0.02 | 0.05 |
| Classic EQ | 0.05 | 0.12 | 0.05 | 0.10 |
| Parametric | 0.13 | 0.28 | 0.10 | 0.30 |
| Stack | 0.03 | 0.07 | 0.03 | 0.15 |
| Graphic | 0.17 | 0.36 | 0.14 | 0.30 |

Silence and a decay into silence cost less than signal in every type: no denormal spike.

### X-c Against the study's commercial units

| Quantity | New | References (study tables) |
|---|---|---|
| Alias sum, 5 kHz -6 dBFS full drive | -90 / -89 / -97 dBc | pedal models -17 to -39; NI Driver -72, Dirt -75, HG-2 -109 |
| TS peak, 110 Hz, 5 kHz | 780 Hz, -11.0, -10.3 dB | 720 Hz, -10.3 to -11.2, -10.8 to -11.2 |
| TS H3, THD at -18 dBFS | -21.1 dB, 9.3 % | -18.5 to -21.1 dB, 9.4 to 12.7 % |
| TS gain at 1 kHz, low / mid / high | 15 / 21 / 35 dB | 9-14 / 15-23 / 27-29 |
| RAT gain at 1 kHz, low / mid / high | 0 / 42 / 58 dB | AT5 -5 / 18 / 40 |
| RAT output range at mid | 1.5 dB | AT5 15.4 dB |
| Muff pre-clip at 2.5 kHz | -23.6 dB | -13 dB (both) |
| Muff THD at -18 dBFS | 75 % | 84 % |
| Latency, samples | 5 / 5 / 5 | 3 to 7; Deluxe 18, Dirt 19, HG-2 32 |
| CPU, % | 1.75 / 3.4 / 3.1 | 0.67 to 1.9; Deluxe 4.4 |
| EQ bell error at 44.1 kHz | 1.91 dB (Q 0.5), 0.07 dB (Q 8) | cookbook 6.38 dB; Solid EQ decramped |
| EQ residual | -152 dBc | Classic -69 dBc at 192 kHz |

Alias control is ahead of every pedal model measured and behind only HG-2. The Screamer is
inside the spread of three commercial Tube Screamers on every figure except top-end gain.
The EQ is numerically clean. What a professional would still fault:

- Every type is perfectly symmetric (H2 below -150 dB everywhere). There is no asymmetric
  clipper, no bias, no op-amp slew, no transistor-stage nonlinearity in the Fuzz.
- Distortion is fully saturated from about a sixth of the gain knob upward and has no
  touch dynamics at its starting point; the first 3 dB of the knob span 18 dB of gain.
- Fuzz is darker ahead of the clippers than both reference Muffs and has no low roll-off.
- Tone knobs zipper; bypass is a hard switch that replays frozen state; a NaN kills the
  plugin until it is re-prepared.
- EQ: one sweepable band, fixed shelf corners, 12 dB/oct cuts only, no ripple correction in
  the Graphic, Stack not oversampled, four voicings with one topology.

## Defects, most serious first

1. Non-finite input latches for good (processor DC blocker; Classic drive; Classic,
   Parametric, Stack and non-flat Graphic EQ). See X-a.
2. Re-enabling a bypassed drive or EQ replays its frozen state as a burst at the level the
   signal had when it was bypassed (up to +14 dBFS in an extreme Parametric setting, -8 to
   -14 dBFS for drives); on/off under signal is a hard switch. See X-a.
3. Tone-knob zipper in Screamer (-59 dBc), Treble (-77 dBc) and Boost (-86 dBc) under a low
   note; `t_zip.py`, or `procrun --chain drive --set drive_type=1 --set drive_gain=0
   --rampl drive_tone=48000:72000:500:12000` on a 98 Hz -40 dBFS sine.
4. Distortion costs 3.0-3.4 % against a 2.0 % budget; Fuzz is at its 3.0 % limit.
5. Distortion gain law and missing dynamics at the starting point (D-b).
6. Fuzz pre-clip response 10 dB darker at 2.5 kHz than both references; peak at 90 Hz (D-b).
7. Split-lane sum with a Classic drive: 2.3 dB (44.1 kHz) and 1.8 dB (48 kHz) more loss at
   10 kHz than an aligned sum; the 0.5 dB bar is met only by a lane without the tone filter.
8. Parametric and Graphic defaults are not bit-exact in the plugin and cannot be made so by
   the host (E-f); tilt and level stages always run.
9. Parametric bell at Q 0.5, 8 kHz, +15 dB is 1.9 dB hot at 2.85 kHz at 44.1 kHz.
10. Screamer at full drive is 0.8-0.9 dB low at 10 kHz against the circuit.
11. A type switch drops to the dry signal for about 40 ms (up to 15.8 dB dip).
12. Graphic: 6.4 dB build-up with all bands raised.

## Not tested

- No listening test; every audibility statement is reasoned from level and frequency.
- No real DAW, no `auval`, no Intel build; arm64 Release and ASan only.
- No recorded guitar: synthetic harmonic stacks and two-tones stand in for it.
- Commercial units were not re-measured; the study's tables are quoted as they stand.
- 176.4 kHz was not run; 192 kHz was run for latency, stress, EQ response and residual only.
- Clicks under noise: only the overshoot test above.
- The UI applying per-type starting points when a type is picked.
- Cab interaction for the split-lane curve (no IR in the repo).
- CPU on an idle machine.
