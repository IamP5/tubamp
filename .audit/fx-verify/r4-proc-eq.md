# Round 4: adversarial pass on the round-three fixes, processor and EQ

Independent verifier, 2026-11-12. Scope: processor commit `2bbda8c`, EQ commit `d2234ab`, and the
limits the docs declare for them (D-52, D-53, D-58). Nothing was fixed. No source file, no tracked
file other than this report, no commit, no `auval`, no installed plugin touched. Everything I wrote
is under `build-verify/r4-proc-eq/`.

**Binaries, and what each number was measured with.**

- **[H]** processor harness `build-verify/r4-proc-eq/rel/r2_artefacts/Release/r2` (sha1 `aee4f89e17d6`),
  built by me from a frozen snapshot `build-verify/r4-proc-eq/tree/` = `git archive 6b3ee75 src`.
  `git diff 13ac659 6b3ee75 -- src` is empty. HEAD moved to `fcd74a0` during the run. Its only `src`
  change is `src/dsp/Gate.h`, which is not in the snapshot and not in scope.
- **[C]** compat harness, new build `r4-proc-eq/hb-new/.../compat_harness` (same snapshot), old build
  `build-verify/compat/hb-old/.../compat_harness` (the round-1 build of the base commit).
- **[F]** `build-verify/r4-proc-eq/bin/tubamp_fxbench` (sha1 `0a8ecc7cfa83`), a copy of the
  coordinator's 00:44 build. I did not rebuild it from the snapshot. `d2234ab` is from 23:41, so it
  contains the EQ fix (inferred from the times, not proven).
- **[G]** `r4-proc-eq/bin/tubamp_golden` (sha1 `1dd342f46ead`), same copy.
- Sanitizer builds of [H] (ASan+UBSan, TSan) were built from the same snapshot and deleted after use.

Labels: **[M]** measured by me, with the binary letter. **[I]** inferred. **[G?]** guess.
The harness source (`harness/r2.cpp`, `compat-harness/`) is the round-3 verifier's, recompiled. The
compat state files and inputs are the round-1 data. Every script named `py/p_*.py`, `py/eq_*.py`,
`compat-run/c_*.py` and `work/*.sh` is mine. Each prints the binary it used. Logs are in
`build-verify/r4-proc-eq/logs/`.

## 1. Verdict per fix

| Fix | Verdict | Deciding measurement |
|---|---|---|
| P1. A state restore is marked, the next callback resets entering or retyped blocks and snaps the faders. Toggles still fade | **HOLDS as built. Two consequences are not declared** (findings 1, 4, 6) | New prepare-first equals new restore-first, bit for bit, in 227 to 231 of 239 old states. The others are gain smoothers and the cab IR, as in the old build [M, C]. A toggle one sample after a restore fades (largest gain jump 0.93 dB against 12.00 dB snapped) [M, H]. An identical state restored during playback is a bit-exact no-op in 12 of 12 runs without a model [M, H] |
| P2. `parameterChanged` only bumps an atomic | **HOLDS** | 1.944 locks per audio-thread automation event on the latency ids against 1.990 for `eq_bass` (round 3: 2.89 against 1.99). 0 allocations, 0 locks, 0 system calls in 7.3 M blocks of message-thread and host-thread storms with restores [M, H + guard] |
| P3. Only the 20 ms timer reports latency | **HOLDS, one low finding** (finding 7) | 120 drive type switches at 44.1 to 192 kHz: the steady report equals the dry-lane delay in 120 of 120, every report from the message thread, last report at most 56.6 ms after the switch and within 22.8 ms of the audio's own move [M, H] |
| P4. `mod_type` crossfade, three bands, 120 ms below 150 Hz | **HOLDS for one switch inside the check's conditions. FAILS for a second request during the fade** (finding 2). D-53 understates the spread (finding 3) | 41.2 and 30.87 Hz notes: 0 of 1320 switches outside the bounds. A second `mod_type` request 2 to 60 ms after the first clicks at -22 to -45 dBFS above 5 kHz (steady -66) [M, H] |
| E1. Bells fed `x/Q` | **HOLDS** | Shelves, cuts, tilt, level equal the analytic bilinear response to 0.0001 dB and 0.0005 degrees at four rates. Bells within 0.27 dB of the analog prototype. Level linearity -140 dB from +12 to -200 dBFS in [M, F] |
| E2. Three 5 ms poles, gain no faster than a third of the decay | **HOLDS, cost quantified** (D-58) | Off-centre ring +0.07 dB at 0.92 x centre against +2.05 dB for my plain reference. A 100 Hz Q 8 bell reaches 90 % of a 0 to +15 dB move in 145 ms against 76 ms [M, F] |
| E3. Reversal on a cubic Hermite curve | **HOLDS** | 2208 reversals and 2112 third-type requests at 44.1 / 48 / 96 kHz: click at most -98.2 dBFS above 2 kHz (steady -99.1), level inside the two types' range by -0.11 / +0.10 dB [M, F] |

## 2. Findings, worst first

### 1. A live restore that changes an engine type is a hard cut. The same change by hand is silent. (medium-high, not declared)

The restore branch resets a retyped block and snaps its fader. A preset or state recall while a
note rings therefore cuts from the old engine's output to a reset engine in one sample.

`py/p_restore3.py`, `py/p_restore4.py` [M, H]. Chain `comp,eq,mod`, clean held chord at -12 dBFS
peak, 48 kHz / 256, worst of 12 to 20 restore instants. Steady peak above 5 kHz is -71.8 dBFS and
the steady largest sample step is -40.4 dBFS.

| Change | By hand (parameter) | By `setStateInformation` | By `loadPreset` |
|---|---|---|---|
| comp Clean to FET | click -72.3, step -41.2 | click -23.9, step -16.0 | -23.9 / -16.0 |
| EQ Parametric to Graphic | -72.3 / -39.9 | -35.2 / -28.8 | -35.2 / -28.8 |
| mod Chorus to Phaser | -72.3 / -41.2 | -29.5 / -21.9 | -29.5 / -21.9 |
| all three | -72.3 / -40.8 | -19.0 / -13.7 | -19.0 / -13.7 |
| knobs only, same types | | -68.4 / -36.7 (no click) | |

At 96 kHz / 64 the all-three click is -14.4 dBFS. A comp retype also plays 2.8 to 4.6 dB too loud
for 20 ms, because the reset compressor starts with no gain reduction. The step is as large as the
instantaneous output: -4.9 dBFS on a 0.8-peak drive signal (`py/p_drvsnap.py`) [M, H].

The signature that counts as a retype also covers chorus mode, phaser model, tremolo voice, colour
and EQ voice (`blockSignature`) [I, read]. CE to Dimension reads -29.9 dBFS [M, H].

D-52 states the mechanism and says "as the old build did". The old build had no types, so a recall
between two presets on the same chain glided. The compat set cannot see this: every old state is
pinned to Classic, so nothing is ever retyped [I]. Likelihood: every preset change during playing
between presets that differ in any type or voicing. A/B slot recall was not run; it takes the same
`applyStateVar` path [I].

### 2. A second `mod_type` request during the first one's fade hard-cuts. (medium, not declared)

`py/p_mod2.py`, `py/p_modstorm.py` [M, H]. Chain `mod`, mix 0.5, 48 kHz / 128. Two requests `gap`
apart, all 12 type triples including a switch asked back.

- Chord (steady above 5 kHz -66.1 dBFS, steady step -33.5 dBFS): gap 2 to 60 ms gives -23 to
  -54 dBFS above 5 kHz, worst -23 dBFS (Phaser to Chorus, back after 60 ms), step up to -15.1 dBFS.
  From 70 to 100 ms on it is clean (-67 dBFS).
- 110 Hz note (steady -135 dBFS): gap 2 to 120 ms gives -22 to -51 dBFS, step up to -14.0 dBFS.
  Clean from 160 to 200 ms on. The window is longer because the low-note fade is 120 ms.
- The click sits at +0.0 ms after the second request and is the same whichever type is asked for,
  so it is the first crossfade being dropped [I].
- Storms: `mod_type` every 1 to 60 ms for 1 s reads -22.1 to -28.2 dBFS above 5 kHz in 27 of 28
  runs. Every 100 or 150 ms is clean. 0 non-finite, peak never above steady.

Who meets it: anyone stepping through the three-way selector, and any automation that goes from
Chorus to Tremolo through Phaser. The EQ engine got exactly this case fixed in `d2234ab` (E30). The
mod dispatcher has no counterpart, and the smoke check makes one switch per render.

### 3. D-53's figures hold only at the check's own notes, instant and channel. (medium, doc)

The check switches once, at 0.8 s, on 55 to 440 Hz, and reads the left channel only
(`tests/ProcessorSmokeTest.cpp:6955`, `getReadPointer (0)`) [I, read]. `py/p_modswitch.py` [M, H],
same level definition, both channels, 48 kHz / 128, mixes 0.35 / 0.5 / 0.8 / 1.0, eight switch
instants across one LFO second unless noted. "Outside" is +1.5 dB above or 3 dB below by level.

| Signal | Switches | Outside by level | Past +3.5 / -12 dB | Worst dip / bump |
|---|---|---|---|---|
| 110 Hz and 55 Hz, the check's instant | 880 | 0 | 0 | -2.13 / +0.64 dB |
| 440 Hz | 3520 | 114 (3.2 %) | 0 | **-11.89** / +3.18 dB, right channel |
| 196 Hz | 3520 | 136 (3.9 %), 6 if the left channel alone is read | 0 | -4.52 / +1.27 dB |
| 329.63 Hz | 3520 | 13 (0.4 %) | 1 | -5.28 / **+3.73** dB |
| 659.26 Hz (8-period window) | 3520 | 67 (1.9 %) | 0 | -5.26 / +3.46 dB |
| 880 Hz (8-period window) | 3520 | 122 (3.5 %) | 35 (1.0 %) | -12.30 / +2.73 dB |
| 1318.51 Hz (8-period window) | 3520 | 124 (3.5 %) | 2 | -10.02 / **+3.99** dB |
| 41.2 and 30.87 Hz, 3 instants | 1320 | 0 | 0 | -0.95 / +0.96 dB |
| dense chord, decorrelated stereo | 3520 | 0 | 0 | -1.17 / +0.80 dB |
| high E major chord, four pure tones | 3520 | 0 | 0 | -0.35 / +0.32 dB |
| 110 Hz left, 146.83 Hz right, 3 instants | 660 | 2 | 0 | -6.63 / +1.32 dB |
| 110 Hz plus DC 0.1, 3 instants | 660 | 36 (5.5 %) | 1 | -12.94 dB |
| 110 Hz at -80 dBFS / at 0 dBFS | 440 / 220 | 1 / 0 | 0 | -3.01 dB |
| 55 and 110 Hz at 44.1, 96, 192 kHz, the check's instant | 1320 | 0 | 0 | -2.22 dB |

- Doc: 34 to 38 of 2640 (1.3 to 1.4 %), worst dip 10.24 dB, bump 3.20 dB. Mine: 3.2 to 3.9 % on
  held notes once the instant varies and the right channel is read, dip 11.89 dB at 440 Hz, bump
  3.99 dB at 1318 Hz. The 2 % allowance and the 3.5 dB guard are passed outside the check's set.
- The 35 guard passes at 880 Hz are all tremolo into chorus Wide at mix 0.5. That is probably
  round 3's chorus Wide start-up notch, not the crossfade [I].
- Dips are short: at 880 Hz the longest stretch more than 3 dB under is 23 ms, more than 6 dB
  under 16 ms. With a one-period window the 880 Hz dip reads -23.2 dB.
- The bass premise holds: nothing outside below 55 Hz.
- I did not rebuild the check's sub-60 Hz figure (-45.5 dBFS). Not confirmed, not contradicted.
- The check's click figure is an amplitude excess over the steady peak above 2 kHz. On material
  with partials above 2 kHz it reads a level bump as a click (-51.3 dBFS "excess" on the dense
  chord, where the peak above 2 kHz never rises 3 dB over steady). Not a click.

### 4. D-52 says a render is the same in both orders. It is not for seven gain parameters and the cab IR. (medium-low, doc; behaviour equals the old build)

- `compat-run/c_orders.py` [M, C], 239 old states, four renders each (old/new x restore-first/
  prepare-first). New prepare-first differs from new restore-first in **8 of 239** (silent first
  50 ms) and **12 of 239** (audio from sample 0, 48 kHz / 512 and 44.1 kHz / 127). The states are
  `trim_level`, `ampM_gains`, `cab_ir`, `cab_ir_cuts`, `classic_M_ir_driveon`, `rig_shaped_M_*`,
  `split_driveA_mode0..2`. Size up to 0.70 (cab IR arriving late) and 0.06 to 0.20 (gains).
- `py/p_orderbisect.py` [M, H]: 261 parameters, each alone off default. Seven make the order
  matter: `input_trim`, `output_level`, `amp_input`, `amp_output`, `mix_alevel`, `mix_blevel`,
  `mix_level`. The gain ramps from the default for 21 ms. With `output_level` restored low the
  first 21 ms are up to 0.45 to 0.50 louder than a render whose peak is 0.00.
- `py/p_restore1.py` [M, H]: 12 random states x 5 configurations: 60 of 60 differ between orders.
  My 8 hand-made states (no gain parameter moved): 40 of 40 bit-equal.
- The old build has the same dependence on the same states (`trim_level`: old 0.1669, new 0.1669;
  old-against-new identical in both orders) [M, C]. Not a regression.
- `tubamp_golden --both-orders host` and `preset`: 339 rows, 0 differences each, re-run by me
  [M, G]. The golden rows do not show it [I: they leave these parameters alone or start silent].

### 5. D-52's counts are right. Their denominators are not 84. (low, doc)

[M, C] `compat-run/matrix.py prepfirst`, `matrix_r3.py pf0`, `live.py`, classified by `summ.py`:
**18 of 239**, **64 of 478**, **22 of 84**. The doc reads "18, 64 and 22 of 84". The claim behind
them holds: 0 states where prepare-first differs old against new while restore-first is identical
and the old build is order independent (`c_orders.py`). In the 22 live restores the new render's
largest step and its peak above 5 kHz in the first 100 ms are equal to or below the old build's
(`compat-run/c_live2.py`). The difference is level in the first 100 ms (`chain_all_instances` at
48 kHz: new -19.6 dBFS RMS in 0 to 20 ms against old -11.0).

### 6. The restore mark reaches user actions next to it. (low)

`py/p_restore2.py`, `py/p_restore4.py` [M, H].

- A toggle between a restore and the next callback is snapped: gain jump 12.00 dB in one sample,
  output step 0.737 against 0.130 steady. With 512-sample blocks that window is up to 10.7 ms.
  One sample after the callback it fades (0.93 dB largest jump).
- A restore during a user fade ends the fade at once (10.46 dB jump 3 ms into an off fade).
- A restore of the same `drive_type` 2 to 15 ms after a hand switch drops the drive's own type
  crossfade: -23.5 to -28.3 dBFS above 5 kHz against -63.0 when the restore comes after it.
- A restore during a `mod_type` crossfade: 0 non-finite in 18 cases. Same type: no click
  (-67 dBFS). Other type: finding 1 (-30 to -44 dBFS).
- Two restores: B then A with no callback between is bit-identical to no restore at all.

### 7. Audio that stops inside the latency hold leaves the held figure standing. (low)

`py/p_latency.py` (d) [M, H]. Fuzz to Boost, audio stops 10 ms later: the report stays 5 (4 at
96 kHz) at +30, +80, +280 and +680 ms. The chain's latency is 0. It corrects itself once audio
runs again. A type changed after the audio stopped is reported right at +80 ms (48 kHz / 256,
96 kHz / 32) and +280 ms (44.1 kHz / 4096), which matches the 40 to 250 ms idle window.

### 8. E29 leaves out a band that misses the row's own bar, and the doc's reason for it does not hold. (low)

`py/eq_far.py` [M, F], the row's own metric (eighth-order high-pass at 1 kHz), 98 Hz at -12 dBFS,
other bands at +12 dB, 48 kHz.

| Band, 12 to -12 dB | Engine | My plain bell, two 5 ms poles | Three 5 ms poles |
|---|---|---|---|
| g800 | **-75.8 dBFS** (doc -75, bar -80) | -76.5 | **-107.6** |
| g1k6 | -83.9 (doc -84.5) | -90.7 | -121.4 |
| g400 | -70.2 | -64.9 | -95.6 |

- The doc's -75 dBFS is confirmed. "That is the band's own response" is not: a trapezoidal bell
  fed `x` with the same three poles and per-sample coefficients rings 32 dB less. The engine
  behaves like the two-pole reference here. Cause not traced. The 16-sample coefficient knots are
  my guess [G?].
- The row's "four octaves or more" also hides the note. Under a 196 Hz note g1k6 reads -75.1 dBFS
  and g800 -66.8 dBFS.
- Audibility: the ring is 79 dB under a note that leaves the EQ near +3 dBFS. I would not expect
  to hear it [G?].

### 9. Older races still fire on a restore. (low, not from these commits)

- ASan, flags `tysoOA` (restores from two threads plus audio-thread automation), 96 kHz / 32:
  one `container-overflow` in `juce::ListenerList<ValueTree::Listener>` under
  `setStateInformation`, run aborted. Three other 30 to 40 s ASan+UBSan runs: 0 reports [M, H-asan].
- TSan, 4 x 45 s: toggles, types and knobs alone 0 warnings. With restores 1 to 10 warnings, all in
  `CabSim::clearImpulseResponse` against `processChainBlock` and the ValueTree listener list.
  **None in the restore flags, `lastSignature`, the latency atomics or the faders** [M, H-tsan].
- These are round 3's "three older races". Two threads restoring at once is my harness, not a host.
- 4 allocations on the audio thread in 2 of 3 audio-thread automation runs (0 in the third), as in
  round 3. Not traced.

## 3. What held under attack

**Processor** [M, H]

- Identical restore during playback: bit-identical for 4 states x 3 configurations. With a NAM
  model it is not (difference up to 1.36, RMS 1.1 to 1.4 dB down for 50 ms): the model is swapped
  through silence. Not from these commits [I].
- Latency: storms of `drive_type` every 5 ms for 0.5 s give 0.24 to 0.44 reports per tick, never
  more than one, final figure equal to the dry-lane delay. With a 300 ms tick one correct report at
  the next tick, no flap.
- `mod_type` while bypassed, then re-enabled: no click at the switch (-72.4 dBFS above 5 kHz),
  after re-enable -67.1 to -71.7 dBFS (steady -66.1), peak under steady.
- Garbage through a full chain with two restores and a `mod_type` switch, block sizes 1, 3, 37,
  64, 1023, 4096 at 22.05 to 192 kHz: NaN, +Inf, -Inf, 3e38, 1e-40, full-scale square, silence:
  42 of 42 runs with 0 non-finite and 0 errors. Largest peak 3.80 (full-scale square).
- Restore runs at block size 1 and 2048: 0 non-finite in 400 renders (`p_restore1.py`).

**EQ** [M, F]

- Steady state, 44.1 / 48 / 96 / 192 kHz (`py/eq_steady.py`, impulse, magnitude and phase):
  shelves, cuts, tilt, level at most 0.0001 dB and 0.0005 degrees from the bilinear model. Bells
  against the analog prototype: 0.27 dB / 36.9 degrees (44.1 kHz, 8 kHz, Q 0.5, at 14 to 20 kHz),
  0.26 dB / 32.1 degrees (48 kHz), 0.010 dB / 10.5 degrees (96 kHz), 0.0005 dB / 3.0 degrees
  (192 kHz). The phase error is the matched design near Nyquist. Graphic all +12: 0.17 dB / 30
  degrees at 44.1 kHz. The Stack was not checked against its formula.
- Flat is a pass-through: 196 of 196 renders bit-equal to the input (Parametric, Graphic, Classic
  5/5/5, with host float noise on the neutral controls, and after every control was moved and
  returned), four rates, blocks 1 to 4096.
- Classic null: 80 of 120 null. The 40 others are the two declared additions and nothing else:
  flat 5/5/5 (pass-through, 5.6e-6 to 1.1e-5) and knob automation. Static non-flat settings and the
  on/off toggle: 80 of 80.
- Static renders are bit-identical at block sizes 1, 3, 7, 16, 33, 127, 512, 1000, 4096, all four
  types. `--rt-check` and `--reset-repeat` pass for all four with moves and switches.
- Moves at the extremes (`py/eq_moves.py`, 648 moves, four rates): gain moves on the 100 Hz Q 8
  bell overshoot at most +0.16 dB (+0.45 dB at 0.95 and 1.05 x centre in `eq_ref.py`; doc +0.45).
  Q moves at most +1.05 dB (Q 0.5 to 8 cut, note an octave below, 44.1 kHz; doc +0.91). Bell at
  8 kHz, high cut, treble, tilt and Graphic g6k4 at 44.1 kHz with notes up to 19 kHz: 0.00 dB
  overshoot, click at most -69.5 dBFS. Low cut and bass under 25 to 55 Hz: at most +0.51 dB.
- Frequency jumps (`py/eq_freqjump.py`, 72 cases): a 100 to 8000 Hz jump of a +15 dB Q 8 bell
  under a 100 Hz note chirps at **-12.0 dBFS** above 2 kHz. My plain reference does the same
  (-12.1). Envelope equal within 0.02 dB in all 72. It is the filter's own stored energy. The
  docs do not give this figure.
- Type switches: asked back every 3 ms (2208 runs), a third type every 6 ms (2112 runs), Stack
  voice changes inside a switch (528 runs): all finite, click at most -98.2 dBFS above 2 kHz. On
  pink noise and a 30-partial chord (1728 runs) the 10 ms level stays within +1.72 dB of the louder
  and 2.22 dB of the quieter engine in the same window with strong settings, +0.26 / -0.92 dB with
  mild ones. My "mild" storm rows in `eq_sw_storm.txt` compare against the wrong steady renders
  (a bug in my script) and are not used.
- Fast modulation: every control re-drawn every 2 ms for 2 s, noise and a 0 dBFS sine, three
  types, 44.1 / 48 / 192 kHz, 36 runs: all finite, peak at most +18.2 dBFS, and 1 s after it stops
  the output equals the static render exactly (difference 0.0).
- Garbage: 72 renders, all finite. NaN, Inf, 1e30, 3e38 and 1001 are silence. 0.5 to 1 s after a
  bad sample types 1 to 3 are within 2.4e-7 of the clean render (5.3e-6 after a 10 ms NaN burst),
  Classic within 1e-4 at 192 kHz. A sample of
  999 is processed (peak 179 to 5460), as declared.
- Level: +12, -60, -120 and -200 dBFS in, scaled against the -12 dBFS render: -131.8 to -142.4 dB
  relative at 48 and 192 kHz with a +15 dB Q 8 bell at 100 Hz. No headroom or noise-floor effect
  from the `x/Q` feed.
- Denormals: with flush-to-zero the tail is exact zero for all types. Without it types 1 to 3
  still reach exact zero on silence. Classic holds 4.8e-43.

## 4. D rows

| Row | Claim | Mine | Verdict |
|---|---|---|---|
| D-52 | restore snaps, toggles and chain edits still fade | toggle 1 sample later fades [M, H] | confirmed |
| D-52 | a render is the same in both orders; golden 0 of 339 | golden host and preset 0 of 339 [M, G]. 8 to 12 of 239 old states and 7 of 261 parameters differ [M, C, H] | **contradicted as worded** (finding 4); same as the old build |
| D-52 | restore-before-prepare matches the old build exactly | not re-run in full. `std0`: 239 states, 0 undeclared [M, C] | confirmed on that set |
| D-52 | 18, 64 and 22 of 84 | 18 of 239, 64 of 478, 22 of 84 [M, C] | counts confirmed, denominators wrong |
| D-52 | all on rows where the old build's own two orders differ | 0 exceptions [M, C] | confirmed |
| D-52 | not stated: what a retyped block sounds like on a live restore | -19.0 dBFS click against -72.3 by hand [M, H] | **missing limit** (finding 1) |
| D-53 | 34 to 38 of 2640 outside, dip 10.24 dB, bump 3.20 dB | 3.2 to 3.9 % on held notes over eight instants and both channels, dip 11.89 dB, bump 3.99 dB [M, H] | **worse than stated** outside the check's set (finding 3) |
| D-53 | guards no switch may pass (3.5 dB, 12 dB) | passed by 1 of 3520 at 329.63 Hz, 2 at 1318.51 Hz, 35 at 880 Hz (likely chorus Wide's notch) | contradicted outside 55 to 440 Hz |
| D-53 | under 60 Hz -45.5 dBFS at 96 kHz | not measured | not checked |
| D-53 | not stated: a second request during the fade | -22 dBFS click [M, H] | **missing limit** (finding 2) |
| D-58 | gain no faster than a third of the decay, 20 ms per pole at 100 Hz Q 8 +15 dB | time to 90 %: 145 ms against 76 ms (0 to +15), 135 against 59 (-15 to +15), 79 against 23 (+15 to 0) [M, F] | confirmed, about twice as slow |
| D-58 | other bells keep 5 ms | Q 2 and below, and 425 Hz and above at Q 8: +6.6 to +8.0 ms, the third pole. 100 Hz Q 4 and 200 Hz Q 8: +29 to +31 ms | confirmed; the in-between cases are not named |
| D-58 | 5 ms poles rang +2.0 dB at 0.92 x centre | my reference +2.05 dB, engine +0.07 dB [M, F] | confirmed |
| D-58 | off-centre frequency moves equal the analog reference | envelope equal to my reference within 0.02 dB in 72 cases [M, F] | confirmed |
| D-58 | g800 rings at -75 dBFS | -75.8 / -76.5 dBFS [M, F] | confirmed |
| D-58 | "that is the band's own response" | plain three-pole bell -107.6 dBFS [M, F] | **contradicted** (finding 8) |

## 5. Not checked

- The shipped AU, `auval`, Logic: which order Logic restores and prepares in, how it delivers
  automation, what it does with a latency change.
- `tubamp_smoke` was not run. The sub-60 Hz figure of D-53 was not rebuilt.
- A/B slot recall. Hosted FX slots. Mono bus beyond the harness defaults. 176.4 kHz.
- The Stack's steady-state response against the Yeh-Smith formula.
- CPU, by instruction.
- `tubamp_fxbench` rebuilt from the snapshot (I used the coordinator's 00:44 build).
- Where the 4 audio-thread allocations come from. Why the engine's far-band ring matches a
  two-pole reference.
- EQ type switches on notes other than 110 Hz for the click figure (level was checked broadband).

## 6. Reproduce

All from `build-verify/r4-proc-eq/`, with `/usr/bin/python3`.

| Finding | Command | Log |
|---|---|---|
| 1 | `py/p_restore3.py`, `py/p_restore4.py`, `py/p_drvsnap.py` | `logs/p_restore3.txt`, `p_restore4.txt` |
| 2 | `py/p_mod2.py`, `py/p_modstorm.py` | `logs/p_mod2.txt`, `p_modstorm.txt` |
| 3 | `py/p_modswitch.py repro\|instants\|low\|chord\|stereo\|dc\|quiet\|rates\|more\|high\|chord2` | `logs/p_mod_*.txt`, per-switch rows `logs/p_mod_rows_*.tsv` |
| 4 | `compat-run/c_orders.py`, `py/p_orderbisect.py`, `py/p_restore1.py` | `logs/c_orders.txt`, `p_orderbisect.txt`, `p_restore1.txt` |
| 5 | `compat-run/matrix.py prepfirst`, `matrix_r3.py pf0`, `live.py`, `summ.py <json>`, `c_live2.py` | `logs/c_*.txt`, `c_*.tsv` |
| 6 | `py/p_restore2.py`, `py/p_restore4.py` | `logs/p_restore2.txt` |
| 7 | `py/p_latency.py` | `logs/p_latency.txt` |
| 8 | `py/eq_far.py` | `logs/eq_far.txt` |
| 9 | `work/guard.sh`, `work/san.sh asan`, `work/tsan.sh` (the sanitizer builds: `./build.sh harness asan -DR2_SAN=address,undefined`, `./build.sh harness tsan -DR2_SAN=thread`) | `logs/p_guard.txt`, `p_asan.txt`, `p_tsan.txt` |
| EQ | `py/eq_steady.py`, `eq_moves.py`, `eq_ref.py`, `eq_freqjump.py`, `eq_misc.py flat\|classic\|blocks\|garbage\|scale`, `eq_switch.py back\|third\|storm\|voice\|fastmod`, `eq_switch_bb.py` | `logs/eq_*.txt` |

`eq_switch_bb.py` imports `eq_switch.py`, which runs every section on import. That is why the
fast-modulation rows are in `logs/eq_sw_bb.txt`.
