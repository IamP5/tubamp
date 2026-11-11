# Mod block verification, round 2: chorus, phaser, tremolo (independent, adversarial)

Scope: the fixes of commits `08da770` (chorus), `70377ce` + `6adbd0b` (phaser), `2b847b9` (tremolo), at HEAD `93b05cf`.
Every number below is **measured** by this round's own scripts unless tagged *inferred* or *guess*. Nothing was listened to.
No author check script (`tools/fxbench/check_*.py`, `fxcheck.py`) was run or imported.

## Method

- **Two drivers.** *Processor*: the real `TubampAudioProcessor`, chain = the mod block only, scripted parameters and play
  head (`build-verify/mod-r2/modh`, round 1's verifier harness recompiled against HEAD's objects; it adds the plugin's 5 Hz
  DC blocker). *Engine*: `build/tubamp_fxbench_artefacts/Release/tubamp_fxbench` (HEAD, built 13:38, newer than every
  engine source). Helper library `build-verify/mod-r2/r2lib.py`. All scripts are in `build-verify/mod-r2/`
  (`p2_*` phaser, lead; `ch2_*` chorus and `tr2_*` tremolo, two sub-verifiers; `x_*` lead re-measurements of the
  sub-verifiers' top findings and cross-effect checks). Run with `/usr/bin/python3` (the interpreter with scipy).
- **Guitar Rig 6 reference, fresh.** States authored here with `tools/re/gr6state.py` from a new `auhost state` default and
  the factory component documents (`gr6r2.py`); they null against the study's stored states to -92.9 dB or better
  (`p2_statecheck.py`). Black box through `auhost` only. **The Rack's noise gate is on in the default state and attenuates
  sparse impulse probes by 0.7 to 17 dB** (`p2_ssimp.py`); every reference render here uses `Rack.G=0`, after which GR6 is
  linear in level and probe spacing to 0.01 dB (peak +8.93 / +1.94 / +6.85 dB, the study's values).
- **Notch tracker (own, `p2_track.py`).** Impulse train, each impulse's windowed response zero-padded to 65536 points,
  notches = prominent local minima with parabolic refinement. Self-test against an analytic 4-stage sweep: notch 1
  0.06 % rms at 0.3 Hz, 0.20 % at 1.07 Hz, 0.69 % at 3.7 Hz, 1.84 % at 9.76 Hz (a pure time lag, common to both systems
  compared). Two comparisons: phase-binned with one fitted phase shift (`p2_gr6.py`) and snapshot by snapshot with ours
  started at GR6's LFO phase and no fitted shift (`p2_gr6snap.py`). LFO period by a second method (sine envelope,
  `p2_period.py`): GR6 equals its knob law within 0.002 %, ours exact.
- **A lesson kept from this round:** comparing sample rates through phase bins read 0.15 to 1.3 % "rate dependence" that
  was only the impulse grid (identical numbers for 44.1/88.2 and for 96/192). With impulse times identical in seconds
  at every rate and a snapshot-by-snapshot comparison it is 0.00 to 0.06 % (`p2_srtraj.py`).
- **Clicks**: peak after a 2 kHz Kaiser FIR high-pass (511 taps, 100 dB), on sines that are not sample-locked.
- Chorus and tremolo were measured by two sub-verifiers under the same rules; the lead re-measured their top findings
  with separate code (`x_recheck_ch.py`, `x_recheck_tr.py`, `x_spot.py`, `x_huge.py`, `x_classic.py`) and all agree.

## 1. Round-1 defects

Totals: **13 fixed, 5 partly fixed, 2 not fixed** (chorus 6 / 1 / 0, phaser 3 / 3 / 1, tremolo 4 / 1 / 1).

| # | Defect (round 1 number) | Status | Round-2 number, worst case | Script |
|---|---|---|---|---|
| C1 | Chorus pitch on rate / depth changes (-1223 / +713 cents, 1.9 s) | **fixed** | Per soloed voice, 8610 renders, 44.1 to 192 kHz: worst +7.5 cents above the steady peak of the settings involved (Wide, depth random every block, 192 kHz; contract 4); steps and sweeps <= +5.0. Lead, summed output, spectral peak, 3301.7 Hz: rate 0.05 -> 10 at depth 1 Wide -45 / +38 against steady +-39; Tri -28 / +21 against +-21 | `ch2_pitch.py`, `x_spot.py` |
| C2 | Chorus pitch on sync start / locate / loop (+347 cents) | **fixed** | Worst +4.5 cents above steady (Tri, engaging sync); processor CE / Dimension +4.3. Paid for with a wet dip, see N1, N7 | `ch2_sync.py` |
| C3 | Chorus replays its lines on re-enable (Tri -5.7 dBFS) | **partly** | Wide, CE, Dimension, Tri: silence (-364.8 dBFS, the DC blocker's float floor). **Classic still bursts: -5.0 dBFS (sub), -7.3 dBFS (lead)** | `ch2_misc.py`, `x_spot.py` |
| C4 | Level during a chorus mode switch (+4.3 / -3.7 dB, dry at unity) | **fixed** | Modes 1-4, 20 ordered pairs, mix 0.35 / 0.5 / 1: within +-0.6 dB of both steady envelopes in 5 ms frames, dry gain holds its law, click <= -98.5 dBFS. Pairs with Classic at mix 1 dip 2.6 dB for 10 ms | `ch2_switch.py`, `ch2_switch2.py` |
| C5 | Mono Dimension has no detune (0.00 cents) | **fixed** | 3.53 cents p95, mono = stereo left (lead); 3.49 / 3.54 and -148 dB against stereo left at 44.1 / 48 / 96 kHz (sub) | `x_spot.py`, `ch2_misc.py` |
| C6 | Chorus non-finite input (Classic latched) | **fixed** | NaN, +-Inf, runs of NaN: 0 failures in 600 runs. (A finite pair beyond +-1.7e38 still latches Classic: N14) | `ch2_nonfinite.py` |
| C7 | Tick when `mod_type` switches into chorus (-31 dBFS) | **fixed** | -66.0 dBFS in, -63.6 out | `ch2_misc.py` |
| P1 | Phaser voicings hold at 48 kHz only (Stone off 3.52 % rms at 96 kHz, top -7.7 %) | **fixed** | Frozen notches (5 sweep positions, <= 10 kHz) against 48 kHz at 44.1 / 88.2 / 96 / 192 kHz: worst 0.16 % (Stone on notch 2 at the top, 192 kHz), 0.02 % script, 0.000 % block and Stone off. Running sweep, notch 1 and 2: <= 0.06 % rms, 0.16 % worst. Against GR6 at 96 kHz Stone off is now 1.10 to 1.26 % rms. What is left is the level *between* notches: section 2, D-30 | `p2_frozen.py`, `p2_srtraj.py`, `p2_gr6.py 96000` |
| P2 | Small Stone tick on a sync phase jump (-12.8 dBFS) | **partly** | Block 64: <= -66.1 dBFS over 56 jumps x 3 probes (the authors' own probe -72.1). **Block >= 512: -31.1 dBFS on the authors' probe (220 Hz -12 dBFS, half-cycle jump from the sweep bottom), -24.7 dBFS at 329.6 Hz -6 dBFS, -23.9 dBFS at 146.9 Hz -3 dBFS; 4 to 6 of 56 jumps above -60.** Note value 1/1 -> 1/4: -31.7; transport start -47.6. Phase 90 clean (-122 dBFS), Stone off <= -73.6. Cause: N3 | `p2_syncjump3.py`, `p2_syncjump2.py`, `p2_syncevents.py` |
| P3 | Phaser pop on re-enable (Stone on -1.8 dBFS) | **fixed** | Four voicings at 44.1 / 48 / 96 kHz, blocks 37 / 64 / 512: silence (-364.8 dBFS); with the note still playing, click <= -89.4 dBFS at both edges. Classic unchanged: burst -11.3 dBFS, click -18.7 dBFS | `p2_reenable.py` |
| P4 | Second-notch position (block 3.1 to 4.3 % low at the top, Stone off +3.8 %, on -3.0 %) | **partly** | Stone on fixed: 0.41 % rms, +-1.0 % worst. Phase 90 block: 1.6 to 2.2 % rms, worst **+4.6 %** (now high, at the top). Stone off: 4.1 % rms, worst **+5.0 % (snapshot) / +6.9 % (binned)** | `p2_gr6snap.py`, `p2_gr6.py` |
| P5 | Peak on a low square with Color on (5.07, +14.1 dB) | **not fixed** | Full-scale 82.4 Hz square, rate 10 Hz: \|out\| 5.15 (+14.2 dB); 110 Hz 4.80; 0 dBFS guitar take 1.34. Nothing saturates. The new make-up also puts Phase 90 script 2.7 dB and block up to 5.0 dB (sine at the resonance) above a full-scale input | `p2_level2.py` |
| P6 | Phaser level against bypass (Phase 90 -1.5 to -1.7 dB on pink) | **partly** | Pink: script +1.21, block -0.15, Stone off +1.08, on +0.29 dB (rate independent to 0.03 dB). Guitar-like takes (mixed / low riff / high register): script +0.7 / +1.8 / -1.6; block -1.1 / -0.4 / -1.6; Stone off +0.5 / +1.3 / -0.4; **Stone on +1.6 / +2.9 / -2.2 dB**. The "within about 1 dB" claim holds on pink for two of four and on guitar for none across registers. Classic -1.65 pink, -2.3 to -3.3 guitar (unchanged) | `p2_level2.py` |
| P7 | Non-finite input latches Classic phaser | **fixed** | NaN, +-Inf, 100 NaNs: output finite and identical to the clean render one second later, all five models. (Huge finite samples: N14) | `p2_robust.py` |
| T1 | Mono tremolo obeys Width (Smooth floor -38.7 dB) | **fixed** | Width 1 against width 0, mono: 0 differing samples, four voices; Smooth depth-1 floor -71.1 dB (lead), -69.5 (sub) | `x_spot.py`, `tr2_a1_mono.py` |
| T2 | Harmonic replay on re-enable (-19.3 dBFS), 0.80 gain step on re-entry | **fixed** | Burst <= -222 dBFS (engine), -363 dBFS (processor); re-entry gain step <= 0.0037 per sample. Classic on/off still a hard switch (step 0.975, -17.3 dBFS) | `tr2_a2_reenable.py`, `x_spot.py` |
| T3 | Tremolo sync jumps click (gain step 0.021, -44 dBFS) | **partly** | Locate, loop, transport start: <= -90.6 dBFS at 0 dBFS input. **Engaging sync or changing the note value is not covered: N4** | `tr2_a3_sync.py`, `x_recheck_tr.py` |
| T4 | Pulse splatter against carrier (-55.8 dBc at 40 Hz, -49.1 at 20 Hz synced) | **not fixed** | Nothing in the DSP changed (`kGainSmoothHz = 120`, the commit only adds the jump fade); the bar was relaxed per carrier. 4 Hz: -55.8 dBc at 40 Hz, -59.1 at 1 kHz. Outside the new bar's one operating point: 10 Hz -51.8, 20 Hz synced -48.8, duty 0.1 -42.0 dBc | `tr2_a4_splatter.py` |
| T5 | Depth automated to zero leaves a residue (-49.8 to -118 dB) | **fixed** | Engine bit-exact in 800 of 800 runs, at the latest 382 ms after depth reaches 0 (Optical; others <= 350 ms). Processor equals the empty chain within 1.04 s | `tr2_a5_depth0.py` |
| T6 | Tremolo non-finite input | **fixed** | A NaN / Inf sample spoils only itself, every voice; processor 0 non-finite out | `tr2_a6_nan.py` |

Unchanged round-1 notes (measured again, `tr2_a7_loud.py`): tremolo loudness against bypass at the starting points, pink /
guitar: Smooth -1.83 / -2.02, Optical -1.44 / -1.58, Bias -0.99 / -1.11, Harmonic +0.08 / -0.01, Classic +2.16 dB; Bias
with L = -R shows 6.01 dB depth against 1.32; at 20 Hz synced Smooth's depth is 43.3 dB and Optical's 14.5 dB.

## 2. Phaser against the reference (fresh Guitar Rig 6 renders, 48 kHz; 96 kHz in brackets where it differs)

"study" knob positions are the ones the fit used; **NEW** ones (Phaser Nine Rate 0.35, 0.60; Stoned Phaser Rate 0.60, 0.90)
were never seen by the fit. Errors are ours / GR6 - 1.

| | Phase 90 block vs Phaser Nine (Color 0.4217) | Small Stone, Color off vs Stoned Phaser | Small Stone, Color on vs Stoned Phaser (0.74) |
|---|---|---|---|
| Rates measured (Hz) | 0.143, 0.247, 0.480 NEW, 1.071, 1.303 NEW, 1.930, 3.721, 9.756 | 0.158, 0.316, 0.631 NEW, 1.778, 5.012 NEW | same |
| Sweep period | exact (GR6 within 0.002 % of its knob law) | exact | exact |
| Notch 1, whole sweep, rms | 0.40 to 0.84 % (study points 0.40 to 0.75; NEW 0.76, 0.84) | 1.18 to 1.31 % | 0.81 to 0.86 % up to 0.63 Hz; 1.30 % at 1.78 Hz (tracker-limited) |
| Notch 1, worst | -1.84 % (binned), -3.0 % at 3.72 Hz without a fitted shift | -3.97 % (ours low) | -1.55 %; frames at the wrap read -7.5 % at 1.78 Hz (tracker) |
| Notch 1, sweep ends (low / high) | -0.5 to +0.3 % / -0.8 to +0.6 % | **-2.8 to -2.5 % / -2.1 to -1.9 %** | +0.0 to +0.3 % / -1.1 to -1.0 % |
| Notch 2, rms / worst | 1.6 to 2.2 % / **+4.6 %** (high at the sweep top) | **4.1 % / +5.0 to +6.9 %** | 0.41 to 0.48 % / +-1.0 % |
| Notch-1 depth below the peak | 32.3 dB both (0.0 to 0.1 dB apart) | 14.0 dB both | 14.2 to 14.6 dB both |
| Resonance peak height | +8.93 GR6, +4.99 ours: same shape, **ours 3.9 dB quieter** (GR6's x1.862 make-up, ours x1.189) | +1.94 GR6, +2.44 ours (our +0.5 dB make-up) | +6.84 / +6.83; at 1.78 Hz +6.55 / +6.62 |
| Response between notches, 80 Hz to 10 kHz, level-matched, rms / worst | **1.0 dB / +2.7 dB at 5.5 kHz with the sweep at its top** [96 kHz: 0.72 / +1.9] | 0.62 dB / +3.0 dB at 10 kHz [0.48 / +1.7] | 0.37 dB / +1.5 dB at 3.4 kHz at the sweep top [0.17 to 0.24 / +0.7 to +1.3] |
| Long-term third-octave level, ours - GR6 (pink, level-matched) | -0.6 dB below 2.5 kHz, +0.2 at 4 to 6 kHz, **+1.2 at 10 kHz, +2.2 at 12.7 kHz, +3.7 at 16 kHz** | flat within 0.4 dB above 125 Hz (0.7 dB at 80 Hz) | -0.5 to -0.3 dB up to 8 kHz, +0.5 at 16 kHz |
| Waveform null on a guitar-like take (residual re GR6's output; the dry against GR6 is -4.6 / -3.1 / 0.0 dB) | **-25.9 to -27.6 dB** | **-10.8 to -12.1 dB** (-16.6 with our dry and wet weights swapped) | **-29.2 to -31.2 dB**; -23.9 dB at 5.0 Hz |
| Pitch modulation of a steady note (p99 cents, 196 Hz, at 0.32 / 1.78 / 5.0 Hz) | not measured (the null covers it) | **ours 3 / 18 / 71, GR6 14 / 58 / 144** | not measured (the null covers it) |

Scripts: `p2_gr6.py`, `p2_gr6snap.py` (tables `p2_gr6_48000.txt`, `p2_gr6_96000.txt`, `p2_gr6snap_48000.txt`),
`p2_null.py` (`p2_null.txt`), `p2_ssswap.py`, `p2_ssphase4.py`, `p2_period.py`.

**The authors' fit numbers are reproduced.** Claimed 0.70 % (block), 1.25 % (Color off), 0.85 % (Color on) rms; measured
0.75 / 1.31 / 0.86 % at the study's points. At knob positions the fit never saw it is 0.84 / 1.31 / 0.85 %.

**Where ours differs audibly, plainly:**

1. **Small Stone with Color off does not behave like Guitar Rig's model in phase.** The magnitude matches (notch depth,
   peak, long-term spectrum within 0.4 dB), but the complex ratio of matched impulse snapshots has unit magnitude and a
   phase that runs through +-180 degrees (`p2_ssphase4.py`). Ours mixes dry 0.75 + wet 0.50; GR6 behaves as dry 0.50 +
   wet 0.75 (same magnitude; swapping our weights from our own output improves the null from -10.8 to -16.6 dB). With
   the wet dominant the output phase follows the all-pass chain all the way round, so GR6's model has 2 to 4.4 times
   our pitch wobble: on a 196 Hz note at 1.78 Hz, 58 cents p99 (18 rms) against our 18 (5.7); at 5 Hz 144 against 71. At
   noon (0.32 Hz) it is 14 against 3 cents and *inferred* inaudible; above about 1.5 Hz it is the "vibrato" part of the
   Small Stone sound and ours has less than half of it. The fit could not see this: it targeted magnitudes only.
2. **Level.** Phase 90 block is 3.9 dB below GR6's Phaser Nine (deliberate); Small Stone Color off 0.5 dB above.
3. **Top end.** GR6's models roll off (Phaser Nine -1.1 dB at 8 kHz, -2.4 at 12 kHz, -3.9 at 16 kHz relative to ours;
   Stoned Phaser -0.7 / -1.4 / -2.3). Ours is flat. *Inferred* audible only on a full-range clean signal, not after a cab.
4. **Second notch** of Phase 90 block (up to 4.6 % high at the sweep top) and of Small Stone Color off (4.1 % rms):
   together with D-30 this is the +2.7 dB / +3.0 dB worst difference between notches. *Inferred* at the edge of audibility.
5. Phase 90 block and Small Stone Color on are the reference to within a -26 to -31 dB waveform null on guitar: these two
   sound like the models. The script-logo Phase 90 still has no reference at all.

**D-30, checked** (`p2_frozen.py`, `p2_d30.py`). Frozen |H|, 48 kHz minus 192 kHz, 20 Hz to 10 kHz, where the 48 kHz
response is within 20 dB of its peak: script +1.70 dB, block +1.42, Stone off +1.06, Stone on +1.61 (the document's 1.69 /
1.40 / 1.06 / 1.61). At 44.1 kHz it is 2.06 / 1.70 / 1.29 / 1.94 dB, which D-30 does not state. The 48 kHz render is the
high one. **Where it sits is not what D-30 says** for three of the four: the worst point is at 10 kHz (script), 8.0 kHz
(block) and 7.5 kHz at sweep position +0.5 (Stone off), on the slopes; "inside the top 3 dB at 3.4 to 5.5 kHz" is true
only of Stone on (+1.54 dB at 3.37 kHz, the resonance, sweep top). Inside the top 3 dB the others differ by 0.68 dB at
5.5 kHz (script), 0.82 dB at 4.6 kHz (block), 0.84 dB at 6.3 kHz (Stone off). Null of one guitar-like performance rendered
at 48 kHz and at 192 kHz (192 kHz output resampled, control -153.6 dB): residual **-39.6 dB (Stone on), -39.9 (block),
-43.7 (script), -45.0 (Stone off)** re the output, worst 50 ms frame -30.8 dB, level difference <= 0.013 dB; by band the
residual is -22.5 dB in 3 to 6 kHz (block). On pink noise -24.0 to -31.6 dB. At 44.1 kHz 1.5 dB worse. *Inferred*: not
audible on guitar; a session bounced at another rate will not null below about -40 dB. Against GR6 our 96 kHz render is
the closer one (Stone on 0.17 to 0.24 dB rms against 0.37 at 48 kHz), which confirms that 48 kHz is the side that is off.

## 3. New findings, most audible first

| # | Finding | Number, conditions | Reproduce |
|---|---|---|---|
| N1 | **Chorus with sync on chops the wet when the host's tempo or song position is not perfectly smooth.** Every block whose phase is more than 1e-3 cycle from the engine's own triggers the 20 ms dip | Tempo in 1-BPM steps, block 512: 39 dips in 40 steps at 1/32, 32 at 1/4. Tempo ramp 100 -> 140 BPM, block 4096: 47 dips (1/32), 34 (1/4); none at block <= 1024. Song position jittered by +-2 samples per block at 1/32: 200 dips in 9.5 s, up to 41 dB deep in 2 ms frames, mean level -1.3 dB at mix 1; +-4 samples: 462 dips, -4.1 dB (a 47 Hz chop); 1/8 needs +-8 samples, 1/4 +-16. Exact song position, steady tempo, any block size, 60 s: 0 dips. Whether a real host is exact is untested | `x_recheck_ch.py`, `ch2_jitter.py`, `ch2_loop.py` |
| N2 | **Small Stone Color off has less than half the reference's pitch modulation** (section 2, item 1). Steady state, every note, rates above about 1.5 Hz | 18 against 58 cents p99 at 1.78 Hz; null against GR6 -10.8 dB | `p2_ssswap.py` |
| N3 | **Small Stone sync jumps: the glide lasts 10 ms only when the host block is at least 10 ms; shorter blocks restart it every block.** At blocks >= 512 the real 10 ms glide sweeps the Color resonance through up to five octaves and zips; at block 64 the "fix" is the restart stretching the glide | Time to within -40 dB of the new phase: 9 to 29 ms at block >= 512, 25 to 41 ms at 256, 85 to 217 ms at 64, 460 to 850 ms at 16. Tick at block >= 512: -31.1 dBFS (220 Hz -12 dBFS), -23.9 dBFS worst; block 64: <= -66 dBFS. `Phaser.cpp:551-560` recomputes `glideOffset` from the pending glide on every block that carries a sync phase and reloads `glideLeft` (*inferred* cause, consistent with every row) | `p2_syncjump2.py`, `p2_syncjump3.py` |
| N4 | **Tremolo: engaging sync or changing the note value while playing clicks at host blocks 64 to 256** (round 1's click, back by another door). The rate smoother's 20 ms glide puts every block's lock more than 1e-3 cycle off, each restart cancels the fade | Smooth -44.9 dBFS above 2 kHz (110 Hz at -12 dBFS), -37.4 at 0 dBFS; Harmonic -47.5 / -38.9; Bias -57.0. Note value 1/4 -> 1/16: -47.8. Clean at blocks 16, 512, 1024 (<= -84.8; Optical's own lamp edges aside) | `x_recheck_tr.py`, `tr2_b1b_engage.py` |
| N5 | **Tremolo voice switch passes the dry signal at unity.** When the old voice is in its dip the level swells to 0 dB for 18 to 64 ms. It never exceeds the crest of either voice | Against the old voice's trajectory: Optical -> Smooth +6.7 dB at depth 0.6, +20.0 at depth 1; Smooth -> any +14 dB at depth 0.6; Harmonic -> Smooth +2.4 / +16.2. The other engineer's "about 5 dB into Smooth" is this, and it is up to 6.7 dB at the default depth. Smooth then restarts at its own dip (-14 dB, 40 ms later). Click <= -62 dBFS | `x_recheck_tr.py`, `tr2_b5_switch.py` |
| N6 | **Phaser model / Color switch goes through the dry input at unity** (`fx::EngineSwitcher`, unchanged since round 1) | Sustained note: up to **+11.5 dB above** both steady levels (note at a notch) and **20.3 dB below** both (Phase 90 script -> Small Stone on, 196 Hz: dry and incoming wet in antiphase cancel mid-fade), outside the span by more than 1 dB for 48 to 65 ms. Broadband, ensemble of 24 switches: <= 1.0 dB above, <= 3.2 dB below (guitar), 1.9 (pink). Click <= -62.2 dBFS. The other engineer's "up to 9 dB" is confirmed and exceeded | `p2_switch.py`, `p2_switch2.py` |
| N7 | **One chorus dip is a hole at mix 1.** 21 ms long, about 90 ms after the knob stops; no click | Mix 1: -26 dB (lead, 2 ms frames, 331.7 Hz) to -38 dB (sub), 6 to 7 ms below -10 dB. Mix 0.5: -2.5 dB for 13 ms; mix 0.35: -1.4 dB. Click <= -84.6 dBFS. No dip during a 0.5 / 2 / 10 s ramp, at most one after it; Wide dips on ordinary single moves at default settings (depth 0.4 -> 0.6, rate 1 -> 2): eight hand moves gave Wide 4 to 5 dips, Tri 1 to 2, CE and Dimension 0. A loop of whole LFO cycles never dips; a fractional loop dips at every wrap | `x_recheck_ch.py`, `ch2_dip_aud.py`, `ch2_dips_auto.py`, `ch2_loop.py` |
| N8 | Phaser loudness depends on the register by up to 5 dB (P6) | Small Stone on: +2.9 dB on a low riff, -2.2 dB on a high-register take | `p2_level2.py` |
| N9 | Tremolo pulse wave ticks on every edge (by design, T4) | -43.4 dBFS above 2 kHz on an 82 / 110 Hz note at -12 dBFS; Optical lamp-on -58.8 | `tr2_a4_splatter.py` |
| N10 | Tremolo switch-on starts in Smooth's dip | -11.9 dB 20 ms after `mod_on` at depth 0.6, -22.3 dB at depth 1. The reset glide itself is within 1 dB of the steady trajectory after <= 19.9 ms (Optical 42 ms): the first note is not un-tremoloed | `tr2_b2_reset.py` |
| N11 | Bias tremolo cuts a note onset that lands on the dip | -4.7 dB for 6.5 ms at depth 0.75; a DC offset of 0.01 lowers the depth from 14.4 to 11.7 dB | `tr2_b6_bias.py` |
| N12 | Chorus slope limit lags continuous automation | Detune within 12 % of its new value <= 220 ms after the knob stops; worst lag during a ramp about 0.45 s; rapid mode switching (every 30 ms) sags 5.4 dB at mix 1 | `ch2_lag.py`, `ch2_switch2.py` |
| N13 | Classic voicings keep the old bypass behaviour | Re-enable burst: chorus -5.0 to -7.3 dBFS, phaser -11.3 dBFS; on/off click phaser -18.7, tremolo -17.3 dBFS | `p2_reenable.py`, `x_spot.py` |
| N14 | **One huge finite sample is permanent.** Not real audio, but nothing recovers it | Processor, one sample of 3e38: non-finite output for the rest of the take with Small Stone on, tremolo Smooth (depth 1) and tremolo Classic; a bypass toggle and `reset()` do not clear it. With the empty chain the output stays above +288 dBFS for more than 3 s (the DC blocker's decay); 1e37 and below stay finite. Engine: the pair +3e38, -3e38 latches Classic chorus and Classic phaser to NaN for good | `x_huge.py`, `p2_robust.py`, `ch2_nonfinite.py` |
| N15 | Tremolo Smooth at width 0.5 is over its CPU budget | 0.119 % against 0.10 % (0.191 % during a fade) | `tr2_d3_cpu.py` |
| N16 | D-30's wording of where the level difference sits is wrong for three voicings, and 44.1 kHz (up to 2.06 dB) is not stated | section 2 | `p2_frozen.py` |

Clean, attacked and held: chorus per-block sync over 60 s at 97.3 and 133.7 BPM, blocks 16 / 64 / 512 (0 spurious dips in
120 runs); chorus mono is still a chorus in every mode (level against stereo left at mix 0.35 / 0.5 / 1: Wide -0.46 /
-0.79 / -2.62 dB, CE -0.32 / -0.57 / -2.58, Tri -0.18 / -0.31 / -1.00, Dimension 0.00); chorus stereo image and mono
compatibility unchanged from round 1 (mono-sum loss at mix 0.5 / 1: Wide -0.70 / -2.46, CE -0.52 / -2.36, Dimension
-0.66 / -2.29, Tri -0.27 / -0.81 dB; L/R correlation 0.13 / 0.16 / 0.18 / 0.66 at width 1, 1.0000 at width 0); tremolo
loops, tempo ramps and 60 s of per-block sync at odd tempos (timing <= 0.02 ms after a wrap, no click); tremolo automation
zipper <= -94.8 dBc; phaser coefficient tables at 193 sweep positions, 22.05 to 384 kHz, four voicings (finite, no solver
glitch: the largest step between neighbours is 1.6 times the median); phaser `prepare()` 2.3 to 2.75 ms at every rate,
16 allocations, tables 43.5 kB per instance (`p2_prep.cpp`); phaser stable on denormal-level input and DC; 60 s of
silence then a note equals a fresh engine to -161.9 dB (Stone on); Phase 90 and Stone off sync events <= -73.6 dBFS.

## 4. Classic bit-identity (`--classic-null`, own inputs and settings)

| Effect | Runs | Differing samples |
|---|---|---|
| Phaser (lead) | 126: 44.1 / 48 / 96 kHz x blocks 1 / 37 / 512 x mono / stereo x 7 timelines (corners, ramps, color toggles, sync phase, bypass) | 0 |
| Chorus | 612 (sub) + 90 (lead) | 0 |
| Tremolo | 234 (sub, also bitwise against `--legacy`) + 90 (lead) | 0 |

Both compare against the authors' legacy adapter inside the same binary; an independent Classic reference was not built.

## 5. Real-time safety, robustness, CPU

- `--rt-check`: phaser 140 runs (22.05 to 192 kHz, blocks 1 / 37 / 512 / 4096, model and Color switches, sync jumps,
  re-enable), chorus 540, tremolo 184: 0 `operator new` in all. The tremolo sub-verifier saw 28 runs fail on a non-repeatable
  `free` count (0 to 62) while running in parallel; the lead could not reproduce it in 180 serial runs. The hook is
  process-wide, not per thread (`tools/fxbench/main.cpp:721`): *inferred* tool noise. Processor: 0 allocations in 12
  `modh --rt` runs with random voice, sync, transport and bypass events.
- `--reset-repeat`: every non-Classic run passes (phaser 112, chorus 378, tremolo 184). Every run that starts on a Classic
  model fails, and the legacy reference fails identically (phaser 5665 samples): the old code's own behaviour.
- CPU, engine, 48 kHz, block 64, stereo, % of real time: phaser Classic 0.203, Phase 90 script 0.066, block 0.089, Stone
  off 0.063, Stone on 0.128; at 192 kHz 0.27 to 0.52; switching models every 60 ms 0.116; silence and no-FTZ within
  0.002. Chorus Classic 0.084, Wide 0.084, CE 0.068, Dimension 0.058, Tri 0.092, switching <= 0.155. Tremolo Classic 0.018,
  Smooth 0.119 at width 0.5 (N15), Optical 0.028, Bias 0.053, Harmonic 0.066.

## 6. Not tested

Listening. A real host (Logic): whether its song position is sample-exact, how it wraps loops and ramps tempo, which
decides how often N1, N3 and N4 happen. Hardware pedals: "reference" means Guitar Rig 6's models at the study's settings.
The script-logo Phase 90 against anything. GR6 at other Color / Depth / Sweep settings, and at 44.1 / 88.2 / 192 kHz (48 and
96 kHz were rendered). A sanitizer build (round 1 ran 108; not repeated). State save and restore, the UI, mono-in /
stereo-out layouts, x86 denormal cost. Small Stone Color on at 1.78 and 5 Hz by notch tracking (the 52 ms response is too
long for snapshots at those rates; the waveform null covers them). Chorus and tremolo numbers are the sub-verifiers',
with the lead's separate re-measurement of C1, C3, C5, T1, T2, N1, N4, N5, N7 and N14 only.

## Judgment

**Professional standard.** In steady playing all three are: the chorus and tremolo pitch and level defects of round 1
are gone and nothing was traded for them at rest. What is below that standard is all in transitions under tempo sync and
voicing switches: the chorus's wet dip under tempo automation (N1), the tremolo's click on engaging sync at ordinary
buffer sizes (N4), the Small Stone's zip on a large sync jump at large buffers (N3), and the dry-at-unity switchers of
the phaser and tremolo (N5, N6).

**The two phaser voicings.** Phase 90 block and Small Stone Color on are Guitar Rig 6's models to a -26 to -31 dB null on
guitar and 0.4 to 0.9 % rms on the first notch, at knob positions the fit never saw as well, and now at every sample
rate. Small Stone Color off matches the model's tone but not its motion (N2); Phase 90 script remains unreferenced.
