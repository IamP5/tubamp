# Mod block verification: chorus, phaser, tremolo (independent, adversarial)

Verifier builds: `build-verify/mod/` (Release) and `build-verify/mod/asan/` (address + undefined sanitizers).
Processor harness: `build-verify/mod/h/modh.cpp` — the real `TubampAudioProcessor`, chain = mod block(s) only, scripted
parameters and play head, raw float I/O; Python wrapper `build-verify/mod/h/vlib.py`. Every script named below is in
`build-verify/mod/h/`. "Processor" = measured through the whole plugin; "fxbench" = the engine alone.
The processor always runs a 5 Hz DC blocker after the chain. Where it matters the reference went through the same
blocker, or the blocker was undone (`vlib.undo_dcblock`, residual -74 dB).
Chorus and tremolo were measured by two sub-verifiers (scripts `ch_*.py`, `tr_*.py`); their top findings were
re-measured by the lead (`sb_spot.py`). Phaser, sync, switching, robustness and CPU were measured by the lead.
Fresh Guitar Rig 6 renders were made black-box through `auhost` with the study's stored one-pedal states (`gr6.py`).

## Verdicts

| Claim | Verdict | Numbers |
|---|---|---|
| P-a trajectory vs GR6 | VERIFIED, with a measurement caveat | Processor, itrack (self-test vs analytic truth: 0.01-0.26 % rms, a +2 % control reads 2.02 %; `pa_selftest.py`). Against the stored trajectories and against fresh GR6 renders (GR6 reproduces its stored trajectories to 0.00 %): Phase 90 block 0.72 % rms, 1.25 % max over the six rates; Small Stone off 1.90 % rms (3.52 % max); on 1.16 % rms (4.14 % max at 1.78 Hz). `pa_traj.py`, `pa_fresh.py`. Caveat: tracked on raw plugin output the same takes read 1.30 % / 2.41 % (block) and 1.39 % / 6.1 % (Color on), because the DC blocker's 32 ms tail overlaps the 512-sample impulse train. That is a tracker artefact (`pa_traj.py --raw`), not a change of the sweep. |
| P-b C++ = numpy prototype | VERIFIED | Processor vs prototype through the same DC blocker: <= -176.7 dB rms at 44.1 / 48 / 96 kHz, blocks 64 and 37, stereo and mono (the float32 floor). Controls: rate +0.01 % reads -71.6 dB, wrong colour -12.2 dB. `pb_parity.py`. |
| P-c only the stated misses | NOT VERIFIED | The stated misses are real (Stone off notch 2 +6.5 % at the bottom; Color-on 4.1 % at 1.78 Hz; depth/mix ignored: bit-identical, 0 difference). More exist, see P3, P4. Notch depth and resonance agree with GR6: block -28.86 vs -28.82 dB, peak +3.47 vs +3.53 dB; Stone off -12.04 vs -12.09, peak 1.94 vs 1.94; Stone on -6.87 vs -6.86, peak 6.83 vs 6.83. Whole response at matched LFO phase, 80 Hz-10 kHz: block 0.63 dB rms, Stone off 0.56, Stone on 0.16-0.22 (`pc_spectra.py`). Rate ends behave: block 0.05 Hz 420.8-1587.9 Hz (static 419.5-1589.6), 10 Hz 740-1107, 20 Hz synced 818-1009, period exact (`pc_ends.py`). |
| P-d stability, cleanliness | NOT VERIFIED for Small Stone with sync; rest VERIFIED | Stable and exactly linear from 0 to +24 dBFS, 30 s, 10 Hz and 20 Hz synced, noise / squares / sweep; impulse rings out in 5 ms (block) and 52 ms (Stone on) (`pd_stab.py`). Model and colour switches -62 to -69 dBFS above 2 kHz, rapid cycling -58.4; rate steps and per-block random rate at steady-state level (`pd_clicks.py`). Small Stone synced: defect 2. |
| P-e Classic phaser | VERIFIED | 252 fxbench classic-null runs (44.1/48/96 kHz, blocks 1/37/64/512, rate/depth/mix corners), 0 differing samples; `tubamp_golden --legacy-check` 168 renders, 0 differences outside the type crossfades (`pe_classic.py`). |
| CH-a detune bounded | NOT VERIFIED | Steady state holds (max, not p95; 4 carriers; 44.1/48/96 kHz): Wide 30.0/-30.6 cents down to 0.44 Hz, then 17.2 / 6.9 / 3.5 at 0.25 / 0.1 / 0.05 Hz; CE <= 29.6; Dimension <= 8.8; Tri <= 23.3. Transients break it: defects 1 and 3. |
| CH-b interpolation, mono | VERIFIED at 48 kHz | 10 kHz ripple 0.53 dB at 48 kHz, 0.74 dB at 44.1 kHz (bar 0.7), 0.04 at 96 kHz; interpolation loss -0.27 dB at 10 kHz, -1.1 dB at 15 kHz. Broadband mono loss <= 1.15 dB at mix <= 0.5, -3.0 dB at mix 1; worst third-octave band over time -4.4 to -6.9 dB at mix 0.5, -27 dB at mix 1 (Dimension, 224 Hz). 10 kHz spurs at full depth -29 dBc (exact delay: -51). |
| CH-c switches, mono, Classic | PARTLY | Classic: 252 + 210 classic-null runs, golden 93/93, 0 differences. Wet gap after a mode switch: CE 6-8 ms, Dimension 14, Classic 20, Tri 30, Wide 33; wet back after 41-67 ms; clicks <= -62.1 dBFS. Dry is NOT simply "still there": defect 6. Mono Dimension: defect 8. |
| T-a voices | VERIFIED except splatter | Smooth: depth 0.5 floor -12.03 dB, crest +1.50; depth 1 crest +3.00, floor -68 dB; edges 2.94 ms. Optical 11.02 ms fall, 43.85 ms rise, floor -20.00 dB, level-independent -60 to +24 dBFS. Bias 5.57 dB at -40 dBFS, 1.32 dB at -6. Harmonic band correlation -0.95, pink level +0.00 to +0.10 dB over depth. Splatter: -59.1 dBc at 1 kHz reproduced, but -57.7 at 440 Hz, -56.1 at 84 Hz, -55.8 at 40 Hz (bar -58), -49.1 at 20 Hz synced. |
| T-b stereo spread | VERIFIED | Smooth sine at 180 deg: 0.61 / 0.58 / 0.20 / 0.00 dB at depth 0.25 / 0.5 / 0.75 / 1; Bias, Harmonic <= 0.01 dB. Only for symmetric sine/triangle: pulse 3.0 dB, skewed sine 2.3, Optical up to 2.9. Width 0: L and R bit-identical. |
| T-c transparency, clicks, Classic | VERIFIED with caveats | Voice switches: largest gain step 0.0037 per sample, fade 41.1 ms. Classic: 252 + 1200 classic-null runs, 0 differences; golden 31 rows identical. Depth 0 is bit-exact only from a fresh reset (Harmonic -144 dB); after depth returns to 0 a residual stays: -90.8 dB Optical, -114 Bias, -118 Smooth. |
| S-a tempo sync | VERIFIED for rate and phase; NOT VERIFIED for "no click" | Period error <= 0.006 % (measurement floor) at 60 / 97.3 / 120 / 173 / 240 / 150 / 89.7 / 200 BPM and seven note values up to 20 Hz; tremolo dip on the grid +1.3 ms (its gain smoother); Stone sweep bottom +0.6 ms. Synced from ppq 0 equals free-running at the same rate bit for bit (chorus, tremolo), -157 dB (phaser), -77 dB (Optical). No play head: 120 BPM, free. Stop: LFO runs on at the synced rate. Tempo 120 to 90: no step. 40 Hz clamps to 20.0000 Hz free; 0.0021 Hz clamps to 0.01 Hz free (`sa_sync.py`). Phase jumps click: defects 2 and 3. Phase 90's sweep bottom is 27-32 ms after the beat (its fitted gate lag). Host tempo <= 1 BPM: rate falls back to 120 BPM while the phase follows the song, so the LFO sticks (tremolo gain 0.001-0.05 for seconds). |
| S-b mod_type crossfade, rapid switching | PARTLY | Crossfade linear, exactly 20.00 ms (10 / 50 / 90 % at 2.00 / 10.00 / 18.00 ms); identical to the old block outside the fade (legacy-check, 0 samples). Never non-finite (three blocks, 160 random changes per take). NOT click-free: into chorus -31 to -38 dBFS (defect 5); random switching including sync changes -7.5 dBFS (defect 2); `mod_on` is a hard switch, -23.1 dBFS (as in the old plugin). |
| X-a real-time safety, robustness | VERIFIED, NaN excepted | 108 sanitizer runs (22.05-192 kHz; blocks 1 / 7 / 37 / 64 / 512 / 4096, random sizes, blocks larger than prepared; mono and stereo; +24 dBFS, denormal-level, silence, DC; every mod parameter random or at extremes; transport and chain changes): 0 reports, 0 non-finite. 0 allocations in processBlock over six scripted takes (control counts 2). NaN / Inf, engines alone (fxbench, `xa_nan.py`): Classic chorus and Classic phaser latch for good (legacy juce::dsp); every new voicing recovers: chorus <= 10.5 ms, phaser and Harmonic one block, other tremolo voices one sample. |
| X-b CPU | VERIFIED | Table below. Silence and burst-then-silence within 0.02 % of noise. |

## Defects, most serious first

1. **Chorus: a rate increase at high depth throws the pitch by more than an octave.** Processor, 997.3 Hz sine -12 dBFS,
   `mod_type=0 mod_chorus_mode=1 mod_depth=1 mod_mix=1 mod_rate=0.05`, event `3:mod_rate=10`: spectral peak -1223 / +713
   cents, beyond 60 cents for 1.9 s (`sb_spot.py`, `ch_a_proc.py`). Tri -1367 / +777; CE +84 / -71 for 109 ms; Dimension
   +137 / -122 for 126 ms. Rate 1 to 10 at depth 1: about +-260 cents. A 1 s knob sweep 0.05 to 10: -457 / +341. Engaging
   sync (0.05 Hz to 1/32 at 150 BPM): Wide over an octave for 1.1 s. At the default depth 0.4, rate 1 to 10: -12 / +8 cents.
   The claimed 30 cents-per-second limit does not hold.
2. **Small Stone with sync on: every LFO phase jump ticks as loudly as the note.** Processor, 220 Hz sine -12 dBFS,
   `mod_type=1 mod_phaser_model=2 mod_color=1 mod_sync=9`, play head 120 BPM, locate (`2.4742:ppq=<any>`): -12.8 dBFS
   above 2 kHz, sample step 0.120 (steady 0.017). Transport start -12.9; sync 1/8 to 1/32 -13.0; Color off -21.6 to -40.6.
   Phase 90 is clean (-102 dBFS) because its gate smoothing absorbs the jump (`pd_syncjump.py`, `sb_find2.py`).
3. **Chorus with sync on: transport start, locate and loop jumps bend the pitch.** Dimension (depth 0.8, sync 1/1) +347 cents
   for 45 ms on play; CE (depth 0.7, sync 1/2) +188 cents (`ch_a_proc.py`, `ch_x_refs.py`). Tremolo: gain step 0.021 per sample.
4. **Bypass off and on replays frozen state.** Processor, 110 Hz sine -6 dBFS plus pink -20 dBFS, `mod_on=0` at 1.0 s,
   input silent from 1.2 s, `mod_on=1` at 3.0 s (`xa_reenable.py`). Burst peak: Small Stone on -1.8 dBFS, off -7.2;
   Phase 90 block -9.6, script -9.9; chorus Tri -5.7, Dimension -7.9, CE -10.8, Wide -12.7; tremolo Harmonic -19.3;
   other tremolo voices nothing. The old voicings do it too (Classic chorus -12.9, Classic phaser -10.6). Also the
   new tremolo voices restart at their dip on re-entry: gain step 0.80 in one sample (Smooth, rate 4, depth 0.6; `tr_c4.py`).
5. **A mod_type switch into chorus ticks.** Processor, 220 Hz sine -12 dBFS, phaser to chorus: -31.1 to -38.0 dBFS above
   2 kHz for Classic, Wide, Dimension, Tri (CE -46 to -49); other type pairs -70 to -74 (`sb_type.py`, `sb_spot.py`). The
   incoming chorus starts with empty lines and its wet steps in inside the 20 ms fade.
6. **A voicing switch passes the unprocessed input at unity, whatever the mix.** Chorus Wide to CE at mix 1, white noise:
   level +4.3 dB then -3.7 dB in 10 ms frames; dry rises from -120 dB to 0 dB for 45-65 ms; up to +7.5 dB (`ch_c_gap.py`,
   `sb_spot.py`). The phaser's model and colour switch uses the same switcher (40 ms through the dry input).
7. **The phaser fits hold at 48 kHz only.** GR6's own sweep does not move with sample rate (96 kHz against its 48 kHz:
   0.03-0.07 % rms; 44.1 kHz: 0.12-0.59 %). Ours does: Small Stone off at 96 kHz 3.52 % rms, top of the sweep 3510 Hz
   against 3786 Hz (-7.7 %), which misses its own 2.0 % bar; at 44.1 kHz 1.65 % / -3.0 %. Phase 90 block 0.74 % and 0.73 %
   (max -1.44 %); Small Stone on 1.13 % and 0.93 % (`pc_ours.py`, `pc_gr6_sr.py`). voicings.md documents the Phase 90 part.
8. **Mono Dimension chorus has no detune.** Wet 0.00 cents (stereo 3.46) with full amplitude nulls; only a moving comb
   is left (`ch_c_mono.py`).
9. **Phaser misses not on the stated list** (`pa_fresh.py`, `pc_spectra.py`, `pc_ours.py`, `pd_stab.py`):
   Phase 90 block notch 2 is 3.1-4.3 % low at the top of the sweep (6949 vs 7257 Hz), -4.8 % at 44.1 kHz; Small Stone
   notch 2 top +3.8 % (off), -3.0 % (on). Phase 90 is 5.4 dB quieter than GR6's model (deliberate, voicings.md:179) and
   1.2 dB brighter at 5-10 kHz (no 16.4 kHz input low-pass). Level against bypass on pink noise: Phase 90 -1.5 to -1.7 dB,
   Small Stone +0.2 to +0.6 dB. Small Stone Color on peaks +14.1 dB on a full-scale 82 Hz square (|out| 5.07, above the
   P6 bound of 4, which was only tested on noise); nothing saturates.
10. **Tremolo.** A mono instance obeys Width, whose default is 1: Smooth depth 1 floor -38.7 dB instead of -71.1
    (`sb_spot.py`, `tr_b2.py`). Splatter bar missed below about 500 Hz. Loudness against bypass at defaults: Smooth -1.82 dB,
    Optical -1.42, Bias -1.0, Harmonic +0.08, Classic +2.16. Bias with L = -R shows 6.0 dB depth instead of 1.3.
    Harmonic depth step 1 to 0 leaves -49.8 dB. At 20 Hz synced Smooth's depth falls to 43 dB and Optical's to 14.5 dB.
11. **Host tempo at or below 1 BPM** sticks the synced LFO (see S-a). Cosmetic.

## CPU (processor, 48 kHz, block 64, stereo, above the empty chain's 0.06 %)

| Voicing | noise | silence | burst then silence |
|---|---|---|---|
| chorus Classic / Wide / CE / Dimension / Tri | 0.073 / 0.062 / 0.051 / 0.041 / 0.075 % | 0.074 / 0.062 / 0.052 / 0.045 / 0.078 % | 0.077 / 0.066 / 0.056 / 0.045 / 0.078 % |
| phaser Classic / P90 script / P90 block / Stone off / Stone on | 0.192 / 0.149 / 0.193 / 0.086 / 0.138 % | 0.199 / 0.150 / 0.192 / 0.084 / 0.135 % | 0.201 / 0.151 / 0.196 / 0.078 / 0.141 % |
| tremolo Classic / Smooth / Optical / Bias / Harmonic | 0.017 / 0.044 / 0.023 / 0.041 / 0.049 % | 0.015 / 0.045 / 0.021 / 0.038 / 0.065 % | 0.011 / 0.037 / 0.026 / 0.043 / 0.051 % |

All inside the spec's budgets (`xb_cpu.py`).

## Professional-standard judgment

**Phaser.** At 44.1 and 48 kHz the block Phase 90 and both Small Stone settings are, by measurement, the Guitar Rig 6
models at their reference settings: notch 1 within 0.7-1.9 % rms over the whole cycle, the whole response within
0.2-0.6 dB rms, period exact. A player should expect "the Guitar Rig Phaser Nine / Stoned Phaser at noon", not a
measured pedal: nobody here measured hardware, the script-logo Phase 90 has no reference at all, depth is fixed,
nothing saturates, and the level is 5.4 dB below NI's. Defects 2, 4 and 7 are below a release standard.

**Chorus.** At rest it matches the measured units (Wide 7.54 cents at depth 0.5 against Choral 7.9-8.4; CE 1.63-4.27 ms
and 4.5-29 cents against Brigade 1.70-4.38 ms; CE wet band 129 Hz-4.3 kHz). Defects 1 and 3 (pitch dives on rate and
transport changes) are below that standard.

**Tremolo.** Inside the commercial envelope: edges 2.94 ms (GR6 2.4, TONEX 4.8), Optical 11 / 44 ms (GR6 12 / 34), stereo
ripple <= 0.61 dB (GR6 1.33). The unramped bypass and the mono Width behaviour fall short.

## Not tested

Listening. A real AU host (Logic): sync was driven by a scripted play head. Hardware pedals. The script-logo Phase 90
against anything. GR6 at other knob settings than the study's. State save and restore, UI. An independent notch
tracker (itrack was used after its self-test). Prototype parity of the tremolo (T2). The phaser's level bump during a
model switch was not measured, only its residual.
