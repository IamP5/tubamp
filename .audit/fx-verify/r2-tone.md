# Drive and EQ: adversarial verification, round 2

Verifier scratch: `build-verify/tone-r2/` (scripts `t_*.py`, library `r2lib.py`, result tables
`res_*.tsv`). Worktree at 93b05cf, Apple M4 Pro (12 cores), arm64. No tracked file was changed and
nothing in `build/` or `build-fx/` was rebuilt.

Every statement is labelled **[M]** measured (script named), **[I]** inferred, or **[G]** guess.

## Method

- Engine alone: `build/tubamp_fxbench_artefacts/Release/tubamp_fxbench` (13:38, after the last
  change to `Drive.cpp`, `Eq.cpp` and the adapters), driven by my own WAVs and timelines.
- Whole processor: round 1's `procrun.cpp` (written by a verifier) relinked into
  `build-verify/tone-r2/procrun` against the object files of `build/` (PluginProcessor.cpp.o 14:11,
  sources 14:04 to 14:09), read-only.
- "Before the fixes": round 1's own bench build at 157ab02
  (`build-verify/drive-eq/tubamp_fxbench_artefacts/...`). "Before the branch": `--legacy`.
- Own signals: sines, a 12-harmonic guitar-like stack with scrambled phases (an asymmetric
  waveform), an inharmonic decaying pluck, band-limited noise, random-phase multitones for transfer
  functions. Own analysis: WAV I/O, 1 Hz rectangular spectra, Hann STFT, zero-phase FFT filters.
  None of the team's check scripts and none of round 1's analysis code was run.
- Detectors were shown to fail first: the alias detector reads -24.8 dBFS for Classic drive (5 kHz,
  -6 dBFS, gain 36); the retarget scan reads -6.4 dBFS on the pre-fix engine (the round-1 jump); the
  WAV path was checked to deliver NaN, Inf and 3e38 to the engine.
- Three of my own first results were wrong and were corrected before use. (1) A retarget click
  9.7 dB above "the same move from rest" was the phase of the note at the move: with the reference
  scanned over one period both read -68.6 dBFS. (2) `--rt-check` failed 97 of 216 runs with
  "malloc 0, free N": with a WAV input the bench's logger counts frees from the reader's background
  thread (0, 6 and 50 on three identical runs, the frozen legacy drive included); with generated
  inputs it is 0 in 1440 runs. (3) A Classic latency of 3.85 samples included the tone filter's own
  group delay, which my first model left out.

## Round-1 defects

| # | Round-1 defect | Status | Number (worst case) | Script |
|---|---|---|---|---|
| D1 | Drive: zipper on the tone knobs (Screamer -59.1, Treble -76.7, Boost -85.6 dBc) | FIXED | Round-1 condition (98 Hz -40 dBFS, gain 0, tone 500 to 12000 in 0.5 s): Screamer -125.0 dBc against a -134 dBc floor. Worst over 1650 moves where the circuit is near linear or clean (steps, 50 ms and 0.5 s ramps, four retargets 7 ms apart, 44.1 to 192 kHz, blocks 1 to 2048, 41 / 98 / 196 Hz notes): -100.7 dBc (Screamer, tone 12000 to 500 step, 196 Hz at -50 dBFS, gain 18, 44.1 kHz, block 2048). At guitar level in saturation a move of any knob of the three pedals rises at most 1.6 dB above the louder steady spectrum (Distortion gain 0 to 36 under the stack) [M] | `t_dzip.py`, `t_dzip2.py` |
| D2 | Drive: Treble smoother never settled (CPU after a move) | FIXED | 30 s run with all three knobs stepped at 0.5 s against the settled values: Treble x1.02, every other type x0.99 to x1.01 [M] | `t_cpu.py` |
| D3 | Drive: Distortion dynamics and reference-voltage calibration | FIXED | At the starting point (gain 18) the output moves 9.3 dB (110 Hz stack) and 10.4 dB (E5 chord) for input -30 to -12 dBFS peak; was 1.5 to 2.6. 1 kHz small-signal gain 0.0 / 15.1 / 31.0 / 46.4 / 57.9 dB at knob 0 / 9 / 18 / 27 / 36 (the last is 4 dB short of the stated 62: the op-amp pole). -18 dBFS 1 kHz gives -12.05 dBFS peak [M] | `t_dyn.py`, `t_dresp.py`, `t_small.py` |
| D4 | Drive: non-finite input | FIXED | 234 engine runs (six types; one NaN, a 700-sample NaN run, +Inf, -Inf, mixed, 3e38, 1e30, denormals, DC, full-scale and +12 dBFS squares, Nyquist; blocks 1 / 37 / 512; mono and stereo): 0 non-finite output samples; every NaN and Inf render is bit-identical to the render with zeros in their place. Through the processor a 10-sample NaN run is within 1e-3 of the clean render after at most 16.4 ms [M] | `t_robust.py`, `t_huge.py` |
| D5 | Drive: every type perfectly symmetric | PARTLY | Fuzz H2 is now -50.0 dB re H1 at 1 kHz -18 dBFS (-49.4 to -60.2 over eight tone / level / gain cases) under an H3 of -5 dB; steady DC at most -158 dBFS. Present and derived from the bias, but too small to hear [M, last clause G]. Screamer and Distortion stay symmetric (H2 at most -162 dB), which is what those circuits do [I] | `t_dresp.py`, `t_misc.py` |
| D6 | Drive: aliasing off the team's test grid | NOT FIXED (declared) | Tones 13 to 19.5 kHz at 44.1 kHz: Screamer -47.2 dBc (-82.9 dBFS), Fuzz -43.1 dBc (-83.3 dBFS; 16 kHz at 0 dBFS, tone 500), Distortion -69.0 dBc: the round-1 figures. Loudest in absolute terms: -65.2 dBFS (Screamer, 19.47 kHz at 0 dBFS). Inside 1 to 12 kHz see "What holds" [M] | `t_alias_hi.py` |
| D7 | Drive: Screamer response at 10 kHz | NOT FIXED (declared) | Against my own analog expression with the diodes' small-signal loading: -0.83 dB at 10 kHz at full drive (48 and 96 kHz), -0.91 dB at 44.1 kHz; -0.37 to -0.47 dB at drive 0 and 0.5; -0.32 dB at 5 kHz at full drive [M] | `t_dresp.py` |
| E1 | EQ: non-finite input latches | FIXED | 390 engine runs over ten EQ configurations: 0 non-finite samples; NaN and Inf renders bit-identical to zeroed input. One 3e38 sample: Classic within 1e-3 of the clean render after 18 ms, the new types unaffected. Through the processor a NaN run costs at most 36.5 ms [M] | `t_robust.py`, `t_huge.py` |
| E2 | EQ: neutral detection under host float noise | FIXED | Engine with the host's noise values (tilt 8.94e-8, level 1.79e-7, knobs 5 +- 4.8e-7, bands +- 1.79e-7), fresh and after moving away and back, 44.1 / 48 / 96 / 192 kHz, blocks 37 and 512: 0 samples differ from the input. Through the processor a fresh Parametric, a fresh Graphic, both after a state round trip, both after the host writes 0, and three Parametric in series: 0 samples differ from an empty chain at all four rates [M] | `t_neutral.py` |
| E3 | EQ: Classic retarget jump | FIXED | 5728 retarget times across the end of the ramp, sample by sample at block 1 and every 7 samples at blocks 37 / 64 / 77 / 100: worst -68.6 dBFS (treble 5 to 10 then to 0), equal to the same move from rest at its worst phase (-68.6 dBFS). The pre-fix engine reads -6.4 dBFS in the same scan [M] | `t_eqretarget.py`, `t_eqretarget_ref.py` |
| E4 | State replay when a block is re-enabled | PARTLY | Through the processor, every new EQ and drive type, blocks 37 / 64 / 512: re-enabled into silence gives -223 dBFS or less; under a held note no sample step above the steady one, overshoot at most +0.4 dB (sine) and +2.9 dB (guitar stack on a Q 8 bell), click above 1 kHz at most -62.0 dBFS. Classic EQ still replays: +0.9 dBFS into silence, a step 34.9 dB above the steady one and a -2.5 dBFS click under the note. Classic drive: 0.0 dBFS into silence, step +27.1 dB. Declared ("Classic types switch as before") [M] | `t_reenable.py` |
| E5 | EQ: wide-bell accuracy (1.91 dB at Q 0.5, 8 kHz, 44.1 kHz) | FIXED | 2904 bells (f0 100 Hz to 8 kHz; Q 0.3 to 10, judged against the clamped 0.5 to 8; +15 / -15 / +6 / -3 / +1.5 / -0.9 dB): worst 0.27 dB at 44.1 kHz, 0.26 dB at 48 kHz, 0.01 dB at 96 kHz, 0.65 dB at 22.05 kHz; Q 6 to 8: 0.05 dB at 44.1 kHz [M] | `t_eqresp.py` |
| E6 | Restated row E6 (+10.0 dB at 150 Hz, was +10.1) | SOUND | Parametric bass 10 reads +10.000 dB at 150 Hz at 44.1 / 48 / 96 / 192 kHz; the frozen Classic reads +9.998 to +10.021. A +20 dB shelf with its corner at 150 Hz gives half its dB there [M, I] | `t_eqresp.py` |
| E13 | Restated row E13 (Stack against analog, 1.7 / 0.35 dB) | NOT FIXED: the bar was moved to the measurement | 6x6x6 knob grid, four voices, against the closed form in s: 0.36 dB to 5 kHz and 1.66 dB from 5 to 10 kHz at 44.1 kHz; 1.37 dB at 48 kHz; 0.31 dB at 96 kHz. The new bars sit 0.04 dB above what the design gives. Above 10 kHz it is still 13.0 dB off at 44.1 kHz and 9.1 dB at 48 kHz [M] | `t_eqgs.py` |

Count: 9 fixed or sound (D1 to D4, E1 to E3, E5, E6), 2 partly (D5, E4), 3 not fixed (D6, D7, E13).

## New findings, most audible first

None of these was introduced by the two fix commits: wherever I could compare, the pre-fix engine
gives the same number. They are defects the first round did not find, or found only half of.

| # | Finding | Number | Heard as | Script |
|---|---|---|---|---|
| N1 | A drive type switch fades through the dry input. When the block's output sits below its input, the dry guitar comes through | Level -20 dB on both types, guitar stack at -12 dBFS peak: +19.0 dB (5 ms rms) and +18.8 dB (peak) above the louder steady state for about 40 ms (Fuzz to Treble); all 30 ordered pairs exceed +7 dB. At the starting points it is a dip instead, deepest -9.9 dB (round 1 saw the dip only) [M] | A loud blip on a type or preset change | `t_dswitch.py` |
| N2 | Parametric: Q moved fast on a boosted bell that sits on a held note overshoots | Mid +15 dB, Q 8 to 0.5 on a 110 Hz -12 dBFS note: peak +14.6 dBFS against +3.0 dBFS before and after, i.e. +11.6 dB (step); +8.5 dB (50 ms move); +1.4 dB (0.3 s); +0.4 dB (1 s). Guitar stack: +10.0 dB. A -15 dB cut overshoots +11.3 dB the same way. Same in the pre-fix engine [M]. Cause: the bell's mix term k (A^2 - 1) follows Q at once, the band-pass state it multiplies does not [I] | A thump when Q is clicked or automated; not when dragged slowly | `t_eqover.py`, `t_eqchirp.py` |
| N3 | Classic EQ and the amp tone stack: the mid knob clicks when it passes 5 (where the code switches between boost Q 0.7 and cut Q 1.5) | Under an 82.41 Hz -12 dBFS note, peak above 1 kHz: mid 10 to 0 -59.9 dBFS at 48 kHz, -54.1 at 96 kHz, -51.7 at 192 kHz (block 64); 8 to 2 -64.1; 7 to 3 -67.5; 6 to 4 -73.4; 0 to 10 -68.6. Moves that do not pass 5: -84.8 dBFS or lower. The click sits 8.4 ms after the step, where the smoothed knob crosses 5. Same through the processor for the EQ block and for the amp tone stack without a model (-59.9 / -54.2 dBFS). The team's bound is -80 dBFS and its smoke test steps 5 to 0 only. Pre-fix engine identical; the pre-branch code read -17.6 [M] | A faint tick on a clean low note; masked by anything with harmonics [G] | `t_eqmid.py`, `t_eq192.py`, `t_proc_mid.py` |
| N4 | EQ type switch under a strong low boost | Parametric (+15 dB at 98 Hz) to Graphic under a 98 Hz -12 dBFS note: -54.8 dBFS above 1 kHz at 96 kHz, -60.9 at 48 kHz (round 1: -86.4 with milder settings); deepest dip over 72 switches -9.1 dB. Pre-fix identical [M]. The linear 20 ms fade has corners [I] | A soft tick plus a dip on a type change | `t_eqswitch.py`, `t_small.py` |
| N5 | Screamer and Distortion put out DC on asymmetric (guitar-like) waveforms; no output coupling is modelled | Sustained stack at -12 dBFS: Distortion -42.7 dBFS at its starting point, -34.8 dBFS at gain 36 (22.4 dB under its own rms), -26.6 dBFS at gain 36 and level 0; Screamer -38.2 dBFS. Fuzz -158 dBFS (its 1.9 Hz stage). The frozen Classic does the same (-35.6 dBFS at gain 12, -13.4 at gain 36). Only the processor's end-of-chain blocker removes it [M]; an amp model placed after the pedal is driven off centre by it [I] | Not heard directly | `t_misc.py`, `t_dresp.py` |
| N6 | Loudness at the starting points against bypass | -12 dBFS strum, out minus in, rms / A-weighted: Screamer +6.8 / +11.7 dB, Distortion +4.0 / +11.1 dB, Fuzz +3.9 / +3.1 dB, frozen Classic +10.7 / +10.5 dB. Quiet picking (-30 dBFS): Fuzz +21.1 dB. Distortion and Fuzz are matched in peak at 1 kHz (-12.05 / -11.98 dBFS) and in rms on the stack, but are 8 dB(A) apart on the stack and 5 dB(A) on the chord [M] | Engaging a pedal is a clear level jump; the Fuzz sounds duller and quieter than the Distortion at their starting points [G] | `t_dyn.py`, `t_small.py` |
| N7 | Fuzz sustain knob: all of the clean-up is in its first twelfth | Output change for 18 dB of input: 15.8 dB at knob 0, 9.0 at 3, 4.4 at 6, 2.6 at 9, 0.7 at 18 (the stated figure, confirmed), 0.2 at 36. Level from knob 6 to 36: +1.8 to +5.1 dB re input. The sustain itself keeps growing: when a pluck has fallen 40 dB the output is down 12.0 dB at knob 9, 3.0 at 18, 0.6 at 36 [M] | A usable Muff law; "cleans up at the bottom" holds only at the very bottom | `t_dyn.py` |
| N8 | Screamer CPU while the gain knob is automated | 2.40 to 2.45 % against 1.73 % static and a 2.0 % budget (x1.4); other types stay inside their budgets (Fuzz 3.36 %, Distortion 1.85 %). Measured under machine load, see CPU [M] | Not audible | `t_cpu.py` |
| N9 | The smoke test's Classic knob-step row at 192 kHz (-79.6 against -80) | Reproduced with the engine alone: -79.6 dBFS (block 512), -79.0 (block 64). The pre-fix engine reads the same to 0.1 dB; the pre-branch code reads -33.6. So it is neither old plugin behaviour nor a product of the fix round: it is the Classic smoothing as first built, 46 dB better than before the branch. The steady floor above 1 kHz at 192 kHz is already -90.4 dBFS (float shelf at 150 Hz) [M]; the bound was set from the lower rates [I] | Not audible | `t_eq192.py` |
| N10 | `tubamp_fxbench --rt-check` with a WAV input fails at random | "malloc 0, free N" with N = 0, 6, 50 on three identical runs, and on `--legacy`. A tool defect, not an engine one [M] | n/a | `t_null.py`, `t_rt.py` |
| N11 | Finite absurd input | One 1e30 sample: Classic EQ stays above +12 dBFS for 173 ms (frozen linear filter, no overflow so no reset). One 3e38 sample into an empty chain: the processor output is above +12 dBFS for the remaining 1.6 s of the render (its DC blocker; outside these engines) [M] | Pathological input only | `t_huge.py` |

## What holds (attacks that found nothing)

- Aliasing, tones and two-tone pairs from 1 to 12 kHz, every gain in 6 dB steps, three levels, two
  tone settings, 44.1 / 48 / 96 / 192 kHz, 7728 runs, loudest inharmonic line in 20 Hz to 20 kHz:
  worst -68.3 dBc (Screamer, 11058 + 11931 Hz, gain 36, 44.1 kHz, -96.7 dBFS); loudest in absolute
  terms -78.1 dBFS (Distortion, 8051 + 9021 Hz at 0 dBFS, 44.1 kHz, -73.0 dBc). Up to 5 kHz:
  -87.5 dBc or better for Screamer and Distortion. The Fuzz reads -39.4 dBc at gain 0 and -65.3 dBc at
  gain 6, where the products sit at -111.6 and -129.3 dBFS; from gain 12 up it is -76.9 dBc or
  better (-94.2 dBFS). 96 and 192 kHz equal 48 kHz
  within 0.5 dB. [M] `t_alias.py`, `res_alias.tsv`
- Knob moves under a real signal at saturation: no non-finite output, no overshoot above 0.74 dB on
  steps and retargets; block-rate wiggle (180 runs: 16 / 64 / 256-sample toggles of gain, tone,
  level) settles on the mean setting without splatter. [M] `t_dzip2.py`
- Latency, reported against measured (phase against my analog models at 1 kHz), 44.1 / 48 / 88.2 /
  96 / 176.4 / 192 kHz: Screamer 4.69 / 4.68 / 3.64 / 3.66 / 1.00 / 1.01 (5 / 5 / 4 / 4 / 1 / 1),
  Distortion 5.11 / 5.10 / 4.87 / 4.83 / 3.73 / 3.82 (5 / 5 / 5 / 5 / 4 / 4), Fuzz 4.94 / 4.94 /
  4.14 / 4.14 / 2.00 / 2.00 (5 / 5 / 4 / 4 / 2 / 2), Boost and Treble 0. Worst error 0.36 sample
  (Screamer, 88.2 kHz). Classic reads 3.85 at 48 kHz including its tone filter's own 0.71 samples,
  i.e. 3.14 against a reported 3 [M; the subtraction is I]. `t_dlat.py`
- Silence in: exact zeros out of every drive type at gain 36, level 12. 60 s of silence then a pluck:
  bit-identical to the pluck after 0.5 s of silence, with and without FTZ, every drive and EQ type;
  no denormal cost (0.83x to 1.10x). [M] `t_misc.py`, `t_dresp.py`
- First 50 ms after reset: peak within +0.32 dB of the settled cycle in every type. The one large
  sample step (Distortion +13.5 dB at 0.15 ms) is the test sine's onset through the pre-clip treble
  emphasis, bounded by the clipper [M; the cause is I]. `t_misc.py`, `t_misc2.py`
- Classic bit-identity, my input (two plucks, noise, a gap, a hot passage) and settings:
  `--classic-null` 234 runs (44.1 / 48 / 96 kHz, blocks 1 / 37 / 512, mono and stereo, six drive and
  seven EQ settings, the drive's including automation and on/off) and 120 runs at random settings at
  48 and 192 kHz: 0 differing samples. The declared exception, Classic EQ at exactly 5/5/5: the
  engine returns the input bit for bit; the frozen code differs from it in 99.8 % of samples, worst
  -103.1 / -100.5 / -92.6 / -82.2 dBFS and -98.4 / -97.3 / -88.4 / -78.4 dB re the signal at 44.1 /
  48 / 96 / 192 kHz. Nothing else differs. [M] `t_null.py`, `t_small.py`
- Real-time safety: `--rt-check` 1440 runs with generated inputs (22.05 to 192 kHz, blocks 1 / 37 /
  512 / 4096, every type, all knobs automated, type and voice switches, on/off): 0 allocations.
  `--reset-repeat` 180 runs: every type passes except Classic drive, which the frozen code fails the
  same way. [M] `t_rt.py`, `t_null.py`
- EQ statics: shelves within 0.043 dB, low cut within 0.01 dB, tilt within 0.03 dB of the analog
  curves at 44.1 / 48 / 96 kHz; Graphic single bands within 0.124 dB with centres at +-12.000 dB;
  all seven at +12 give +18.4 dB (unchanged); Stack noon level averages 0.00 to +0.09 dB over 80 Hz
  to 5 kHz. The high cut is still not de-cramped (an 8 kHz cut is 23.9 dB below the analog curve at
  19.8 kHz at 44.1 kHz), which errs on the dark side. [M] `t_eqresp.py`, `t_eqgs.py`
- The 1e-3 dB neutral window: inside it the output is the input bit for bit; just outside it the gain
  is the requested one (level 0.00101 reads +0.00101 dB); a step across the edge under a 98 Hz note
  leaves -143.9 dBFS above 1 kHz; a real 0.1 dB step leaves -127.0 dBFS. Not audible, continuous to
  1e-3 dB. [M] `t_neutral.py`
- EQ knob moves, 2575 cases (every knob of every type and voice; steps, ramps, 4 ms toggling; 98 and
  41 Hz notes and 40 to 800 Hz noise; 44.1 / 48 / 192 kHz): apart from N2 and N3 the worst line
  above the cut-off is -71.6 dBFS on a step (Graphic level -12 to +12 with every band at +12, the
  note at +3 dBFS) and -69.5 dBFS on 4 ms toggling, which is the skirt of the 5 ms glide. The
  mid-frequency jump with a +15 dB Q 8 bell on the note still chirps (-26.0 dBFS line, as round 1)
  and is the same before and after the fix. [M] `t_eqzip.py`, `t_eqchirp.py`
- Three EQ blocks in series equal three times one block within 0.004 dB over a 90 dB range. [M]
  `t_misc2.py`

## CPU

`--bench`, stereo, block 64, pink noise at -18 dBFS, minimum of three calls (each the tool's minimum
of five). Machine load during the run: load average 15.5 at the start, 8.4 to 9.6 for most of it
(other agents' jobs); the frozen Classic drive read 0.212 % against round 1's 0.17 %, so treat the
figures as about 1.25 times high. An earlier attempt at load 18 to 22 read 3.5 to 6.7 % for the
Screamer and was discarded.

| | 48 kHz | 44.1 kHz | 96 kHz | 192 kHz | worst gain, 48 kHz | budget |
|---|---|---|---|---|---|---|
| Classic drive | 0.216 | 0.196 | 0.433 | 0.869 | 0.216 | 0.35 |
| Screamer | 1.734 | 1.595 | 1.633 | 1.532 | 1.737 (gain 12) | 2.0 |
| Distortion | 1.740 | 1.597 | 1.674 | 1.483 | 1.954 (gain 3) | 3.5 |
| Fuzz | 3.003 | 3.038 | 3.221 | 3.107 | 3.355 (gain 24) | 4.0 |
| Boost / Treble | 0.027 / 0.024 | 0.022 | 0.047 | 0.095 | 0.024 | 0.05 |
| Classic EQ 8/3/7 | 0.082 | | 0.163 | 0.330 | | 0.10 |
| Parametric, every section | 0.165 | | 0.336 | 0.663 | | 0.30 |
| Stack | 0.049 | | 0.099 | 0.200 | | 0.15 |
| Graphic, every band | 0.215 | | 0.442 | 0.884 | | 0.30 |
| Graphic, all bands 0 | 0.105 | | 0.216 | 0.429 | | 0.30 |

The Distortion is now well inside its budget (round 1: 3.0 to 3.4 %). Under ten seconds of ramps on
every knob: Classic EQ 0.166 % (bar 0.15 while moving), Parametric 0.201, Stack 0.059, Graphic
0.286; drive as N8. The EQ budgets hold at 48 kHz only: at 96 kHz Parametric and Graphic exceed
0.30 % [M]. `t_cpu.py`

## Not tested

- No listening; every "heard as" is reasoned from level, duration and frequency.
- No DAW, no `auval`, no Intel build. No sanitizer build at HEAD (round 1's 688 clean runs are of the
  pre-fix code).
- No recorded guitar. The stack, chord and pluck are synthetic; the Treble booster's effect on real
  material was not judged (on a 110 Hz stack with nothing above 1.3 kHz it changes +0.1 dB).
- CPU on an idle machine; N8 in particular needs repeating there.
- The Stack's analog closed form was taken from the source and evaluated in s; I did not redo the
  nodal solve round 1 made. The Fuzz latency model uses the stage formulas of the spec with my own
  nodal solve of the tone network.
- The commercial references were not re-measured; whether a Fuzz H2 of -50 dB matches a real Muff is
  not established.
- Processor matters outside the two engines (lane alignment, latency reporting on bypass, the shape
  of the 10 ms re-enable crossfade) beyond what the tables above state.
- `--rt-check` with my own WAV input (N10); the 1440 clean runs use the bench's generators with my
  automation.
- Round-1 defects outside this brief (Fuzz pre-clip response, split-lane sum) were not revisited.
