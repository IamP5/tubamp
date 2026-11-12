# Round 4, chorus and phaser: adversarial check of the round-three fixes

Scope: chorus `0fde1ee`, phaser `006f743`, the limits D-54 to D-57 of `docs/FX-ENGINES.md` §8, the Phase 90 and
Small Stone tone fits. Nothing was fixed and no source file was changed. Nothing was listened to.

## Method and labels

- **[M]** measured by me with `build-verify/r4-mod/bin/tubamp_fxbench` (sha1 `0a8ecc7cfa`), copied from `build/` at the
  start. Its `Chorus.cpp.o` is from 22:36:37 and its `Phaser.cpp.o` from 00:44:24, after the two fix commits' sources.
  `git diff 006f743 HEAD -- src/dsp/Chorus* src/dsp/Phaser* tools/fxbench/fx_chorus.cpp fx_phaser.cpp main.cpp` is
  empty. HEAD moved `13ac659` → `6b3ee75` → `8a1081a` during the run (audit and gate only). `build/` was rebuilt meanwhile
  (its bench now has another hash); only the copy was used.
- **[Mpre]** measured by me with `bin/tubamp_fxbench_r3pre_asan` (sha1 `2f976c7521`), the round-3 verifier's ASan build of
  11-11 19:45, before both fix commits. Used only for before / after on the same script.
- **[A]** the author's own function from `check_phaser.py`, run by me on my copy, on the row's grid (reproduction) or on
  another grid (stated).
- **[I]** inferred. **[G]** guess.
- All scripts are in `build-verify/r4-mod/`, run with `/usr/bin/python3 <script>`. Each prints the binary and HEAD.
  `r4lib.py` is my own helper (envelope by complex demodulation, checked on a synthetic AM/FM tone: 20.05 cents for 20.0,
  levels exact; a 17 dB dip of 3.5 ms reads 17.0 dB at 2479 Hz, 13.0 dB at 368 Hz, 3.0 dB at 110 Hz, so short dips on low
  notes are under-read, never over-read).
- Two reference traps cost me false leads and are fixed in the figures below. A chorus mode's steady minimum must be taken
  over 60 s, not 5 to 9 s (Wide and Tri have two LFOs that beat). A phaser dip is judged against both voicings **at the same
  instant** (the outgoing one unswitched, the incoming one started fresh at the request), not only against their steady
  span.

## Verdict per fix

| Fix | Verdict | Measurement |
|---|---|---|
| Chorus: each voice chooses its path, the CE channel hole | **HOLDS** | CE, loop wraps, 26 notes 82 to 1500 Hz, 4 loop settings, mix 0.35 / 0.5 / 1: 0 of 6864 channel-jumps more than 3 dB under the 60 s steady minimum for 5 ms [M `ch_loop3.py`]. Against the stricter 0.5th-percentile reference at mix 1: 0 of 2288 at 48 kHz, 0 at 44.1 kHz decorrelated stereo, 0 at 96 kHz mono; before the fix 156 of 2288 (6.8 %), 72 deeper than 10 dB [M, Mpre `ch_loop2.py`] |
| Chorus: mode switches fade whole outputs per channel in two bands and wait | **HOLDS at mix 0.35, mostly at 0.5, PARTLY at mix 1** | 20 ordered pairs, 9 notes, 4 times, each channel, against the 60 s steady minima, two settings (0.8 Hz depth 0.5 at 48 kHz; 3 Hz depth 0.8 at 44.1 kHz, decorrelated stereo): mix 0.35 2 of 2880 channel-switches (5.3 dB, 9 ms); mix 0.5 13 of 2880 (deepest 15.2 dB for 15 ms, Dimension → Tri, 1318.5 Hz); mix 1 91 of 2880 (3.2 %), 41 deeper than 10 dB, deepest 40.1 dB for 6 ms (Classic → CE, 440.3 Hz) and 34.6 dB for 8 ms without Classic (CE → Dimension, 82.4 Hz), rise up to +3.49 dB [M `ch_switch2.py`]. Chords and a bass note, 20 ms frames: 11 of 1920 dip more than 3 dB, worst 5.7 dB, all at mix 1 [M `ch_chord.py`] |
| Chorus: glide up to 80 ms, crossfades at least 250 ms apart while a knob moves | **HOLDS** | 14 rate / depth moves, every mode, per tap from the trace: gliding at most +4.1 cents over the settings' own detune (CE; the others +4.0); both taps during a crossfade at most +4.0. Crossfade starts while the knob moves: 250 ms or more apart (the closer one, 159 to 172 ms, is the one a stopped knob starts). Pink noise, 10 ms frames, gain re the input against the steady range of the settings on the path: at most 0.70 dB under and 0.50 dB over (Tri, mix 1) [M `ch_auto.py`, `ch_auto_look.py`] |
| Chorus: a spurious jump train makes the LFO free-run | **HOLDS** | Song position off by ±200 samples per block: 1 to 8 jumps in 8 s, 0 crossfades, delay swing 100 % of steady (round 3: 7499 jumps, swing 28 %). A real loop of 185 ms counts 32 to 33 jumps in 6 s and the swing falls to 62 to 84 % [M `ch_sync.py`] |
| Chorus: sync lock (not changed, re-attacked) | holds | Ramps 60 → 200 BPM in 2 s, 200 → 60 in 0.5 s, 100 → 140 in 20 s at 1/16, 1 BPM steps, ±16 samples, blocks 64 and 4096, four modes: 0 jumps; tap detune at most +3.9 cents over the end tempo's own, except the fast downward ramp (+11.5, the start tempo's own is larger) [M `ch_sync.py`] |
| Phaser: switch waits for correlation, two-band correction capped at +1.4 dB, 69 to 390 ms | **HOLDS at LFO rates up to 1.3 Hz and on a guitar take at every rate. FAILS on held notes from 2 Hz up, where it is worse than before the fix** | Timing 68.5 to 389.6 ms over 14 400 switches [M]. Details in finding 1 |
| Phaser: second-order sync lock, integral bounded at ±10 % | **HOLDS inside ±10 %** | Host 0.5 to 10 % off its reported tempo, 1/4, 1/16 and 4/1, blocks 16 to 4096, 44.1 to 192 kHz: 0 glides, within 0.005 cycle after at most 1.07 s, 0.002 cycle standing, overshoot at most 0.004 cycle, rate ripple after lock at most 0.07 %. Outside ±10 % see finding 4 [M `ph_sync.py`] |

## Findings, most audible first (ranking is [G])

### 1. Phaser: on a held note, a voicing switch at 2 Hz and faster still dips, more often than before the fix

- The author's P19 measure and bars, other notes and rates (172.3, 273.1, 433.0, 520.6, 752.8, 992.6, 2494.9 Hz; 20 ordered
  pairs; 4 LFO phases; 560 switches per rate) [A `ph_p19_grid.py`]:

  | LFO rate | over the 3 dB bar | over 6 dB | worst dip | worst rise (bar 1.5) |
  |---|---|---|---|---|
  | 0.7 Hz | 2 (0.4 %) | 2 | 7.21 dB (Classic → block, 520.6 Hz) | +1.01 dB |
  | 2.0 Hz | 30 (5.4 %) | 14 | 13.37 dB (Stone on → Stone off, 433 Hz) | +1.30 dB |
  | 4.0 Hz | 139 (24.8 %) | 60 | 18.41 dB (Stone on → Classic, 433 Hz) | +1.27 dB |
  | 8.0 Hz | 175 (31.2 %) | 97 | 18.15 dB (Stone on → Classic, 172.3 Hz) | +1.45 dB |

- My own measure, 40 notes 82.4 to 3000 Hz, rates 0.3 / 1.3 / 4 Hz, 6 switch times, 14 400 switches. Output under the
  lower of the two voicings at the same instant by more than 3 dB (where that voicing is within 40 dB of the input):
  17 (0.4 %) at 0.3 Hz, 86 (1.8 %) at 1.3 Hz, **1008 (21.0 %) at 4 Hz**. Longest stretch more than 6 dB under: 22.8 ms.
  Of the 596 dips over 6 dB at 4 Hz, 562 have neither voicing in a notch: the two cancel in mid-fade. A plain linear
  crossfade over the engine's own window dips more than 3 dB in 2036 switches against the engine's 1111, so the bridge
  helps but does not close it [M `ph_switch.py`, `ph_switch_an.py`, data `ph_switch_full.json`].
- Before and after on the same 384 switches (8 notes, 12 pairs without Classic, 4 times), dips more than 6 dB under both
  voicings' steady span: at 1.3 Hz **14 → 1** (worst 28.2 → 8.6 dB). At 4 Hz **14 → 38** (3.6 % → 9.9 %), longest
  4.8 → 11.5 ms. Off the note the worst peak fell from −19.4 to −25.6 dBFS at 4 Hz and from −18.8 to −38.8 at 1.3 Hz
  [M, Mpre `ph_switch.py … 8 4.0,1.3 4`].
- On the guitar-like take the switch holds at every rate: 0 of 80 over the bar at 0.7 / 2 / 4 / 8 Hz, worst dip 2.66 dB,
  worst rise +0.86 dB [A `ph_p19_take.py`]. Pink noise with a switch request every 30 / 100 / 450 ms stays inside the five
  voicings' own span (−5.21 to +2.24 dB against −6.08 to +2.27) [M].
- Why [I]: the fade is 40 to 80 ms and the wait is built on a 2.5 ms correlation estimate. At 4 Hz the two sweeps turn
  against each other by a large part of a cycle inside one fade, so a moment that is good at the start is antiphase in
  the middle. The old switch was over in 70 ms.
- Reproduce: `/usr/bin/python3 ph_p19_grid.py` (the table above, about 8 minutes), `/usr/bin/python3 ph_switch_an.py`
  (reads `ph_switch_full.json`; every case with its pair, note, rate and request time `t0` is in that file).

### 2. Chorus Tri: a song-position jump leaves holes deeper and longer than D-54 says

- 48 kHz, depth 0.8, 120 BPM, 26 notes, loops of 2.63 / 1.37 / 0.81 / 3.5 beats, each channel, against the mode's own
  minimum over 60 s (same settings, no loop). A hole is more than 3 dB under that minimum for at least 5 ms.
  The same detector on the steady render finds 0 [M `ch_loop3.py`, log `ch_loop3_now.log`]:

  | mix | channel-jumps with a hole | deeper than 10 dB | deepest | longest |
  |---|---|---|---|---|
  | 0.35 | 161 of 2288 (7.0 %), 12 of 26 notes | 53 | 19.7 dB for 20 ms (1500 Hz) | 48 ms (4.7 dB, 952 Hz) |
  | 0.5 | 122 of 2288 (5.3 %), 13 notes | 43 | 33.5 dB for 5 ms (758.7 Hz) | 17 ms (7.2 dB, 306 Hz) |
  | 1.0 | 27 of 2288 (1.2 %), 3 notes | 12 | 53 ms, right channel at −44 to −47 dBFS where its steady minimum over 90 s is −26.0 dBFS (481.7 Hz) | same |

- Before the fix, with the 0.5th-percentile reference on both builds: mix 0.35 109 → 171 holes, mix 0.5 128 → 136,
  mix 1 28 → 29 (the 53 ms hole at 481.7 Hz is in both) [M, Mpre `ch_loop2.py`]. So Tri is not improved and at
  mix 0.35 is somewhat worse. 44.1 kHz decorrelated stereo and 96 kHz mono show the same picture (mix 0.5: 221 and 45
  holes, deepest 39 and 50 dB) [M, 0.5th-percentile reference].
- Chords and a bass note (E chord, A power chord, E1, a high triad), 20 ms frames: 0 of 2976 channel-jumps more than
  3 dB under the steady minimum, worst 0.6 dB, every mode [M `ch_chord.py`]. The holes are a single-note effect.
- Reproduce the long one: `tubamp_fxbench chorus gen:sine:481.7:-12 o.wav mode=4 mix=1 depth=0.8 host_bpm=120 host_beats=1 host_loop_len=1.37 host_loop_end=2 --seconds=10`,
  right channel 50 to 110 ms after 2.370 s (`ch_loop_look.py` prints it).

### 3. Chorus mode switches at mix 1: pairs without Classic dip as deep, and the rise passes the stated bound

- Against the 60 s steady minima, rate 0.8 Hz, depth 0.5, 48 kHz [M `ch_switch2.py`, log `ch_switch2_a.log`]: with Classic
  26 of 576 channel-switches (4.5 %), 9 deeper than 10 dB, deepest 27.0 dB for 7 ms (Dimension → Classic, 1318.5 Hz),
  rise +1.85 dB. Modes 1 to 4 only: 17 of 864 (2.0 %), 10 deeper than 10 dB, deepest 34.6 dB for 8 ms (CE → Dimension,
  82.4 Hz), **rise +2.35 dB** (CE → Dimension, 1318.5 Hz). Loudest click above max(2 kHz, 4 f0): −75.5 dBFS.
- The same at 3 Hz, depth 0.8, 44.1 kHz, decorrelated stereo [log `ch_switch2_b.log`]: with Classic 32 of 576 (5.6 %),
  17 deeper than 10 dB, deepest 40.1 dB for 6 ms and **+3.49 dB** over both modes' maxima (Classic → CE, 440.3 Hz, the
  pair D-55 names). Modes 1 to 4 at mix 1: 16 of 864, deepest 23.7 dB for 5 ms (Wide → CE, 82.4 Hz). At mix 0.5, modes
  1 to 4: 12 of 864, deepest 15.2 dB for 15 ms (Dimension → Tri, 1318.5 Hz). At mix 0.35: 0 of 1440.
  `ch_switch_look.py` prints the envelope of single cases.

### 4. Phaser sync lock outside its stated bounds

- Host more than 10 % off its reported tempo [M `ph_sync.py drift`, log `ph_sync_drift.log`]: 12 % leaves 0.007 cycle
  (1/4) and 0.026 cycle (1/16) standing. At 15 % a 1/4 note slips through every phase with no glide and the LFO rate swings
  24 to 33 %; a 1/16 note glides on nearly every block (1287 glides in 16 s at block 512). 25 % at 1/16: 1499 glides.
- A synced rate above the 20 Hz clamp (300 BPM at 1/32): 116 glides in 10 s at block 4096, slipping with a 48 % rate
  swing at block 64. This is round 3's item 4, second half, still open. 160 BPM at 1/32 (21.3 Hz) now locks.
- Position jitter of ±64 samples at 1/16, block 256: 138 glides in 12 s. ±16 samples: 0.
- Tick on a loop wrap, Small Stone Color on, 587.3 Hz, 4 Hz, block 64: **−60.4 dBFS** above 2 kHz (uninterrupted −80.7),
  0.4 dB inside P15's −60 bar; P15 reports −81.8 as its worst. Every other case −67 dBFS or lower; Phase 90 −135 or lower
  [M `ph_sync2.py`].
- Inside the bounds the correction is not a wobble [I from M]: the error is pulled in one-sidedly (overshoot at most
  0.004 cycle at constant drift, 0.042 cycle when the drift flips from +5 % to −5 % at 1/16), and the rate ripple after lock
  is at most 0.07 %. During pull-in from a 10 % error the phase is up to 0.031 cycle (1/4) or 0.12 cycle (1/16) off for
  0.8 to 1.1 s. A tempo step 120 → 240 BPM leaves 0.032 cycle for about 0.9 s, and at blocks 512 and 4096 costs one glide.

### 5. Smaller points

- Phaser sync events with my own song position (loop wraps, stop and start, two locates 12 ms apart, a locate every 100 ms):
  exactly one glide per jump, 0.0000 cycle off 0.3 s later, every voicing, blocks 64 and 1024 [M `ph_sync2.py`]. A
  synced start at beat 0.3 and sync switched on mid-song: 0 and 1 glide, 0.0000 cycle. A voicing switch while synced
  stays locked.
- Switching into Wide at 440.3 Hz, mix 0.35, the level falls to −40 dBFS 150 to 260 ms later for about 85 ms. This is
  Wide's own comb when its two LFOs are aligned, which a fresh start always is: the steady mode reaches −41.3 dBFS by
  itself within 90 s, and the pre-fix build gives the identical hole (8 of 16 switch times, 85 ms, 17.3 dB) [M, Mpre
  `ch_switch_pre.py`]. Not a switch defect. It does mean every switch into Wide starts at the mode's deepest comb.
- Classic chorus after `reset()` equals the legacy render again: `--classic-null` with `reset@2.5=1` and with three resets,
  0 of 64 runs differ (round 3: 18 of 612) [M `rb.py`].

## The declared limits

| Row | The document says | My figure | Verdict |
|---|---|---|---|
| D-54 | Tri: 6 of 56 jumps under −3 dB, worst −12.1 dB. Wide, CE, Dimension: worst −2.82 dB, 0 of 56 | Tri: 5.3 to 7.0 % of channel-jumps at mix 0.35 / 0.5, 1.2 % at mix 1, but deepest 19.7 dB for 20 ms, 33.5 dB for 5 ms, and a 53 ms hole at mix 1; 43 to 53 holes deeper than 10 dB per 2288. Wide and CE: 0 of 13 728 against the 60 s minimum. Dimension: 0 of 6864 (0.5th-percentile reference) [M] | Count **confirmed**. Depth **contradicted** (worse than stated). The other three modes **confirmed** |
| D-55 | 5 of 60 outside, worst −20.3 dB (Classic → CE, mix 1); the rest −4.4 and −3.04 dB; none over +2.0 dB or above −60 dBFS | Two settings, 2880 channel-switches per mix. Mix 0.35: 2, at most 5.3 dB for 9 ms. Mix 0.5: 13, deepest 15.2 dB for 15 ms (Dimension → Tri). Mix 1: 91 (3.2 %), deepest 40.1 dB for 6 ms (Classic → CE) and 34.6 dB for 8 ms without Classic; rise +3.49 dB (Classic → CE) and +2.35 dB (CE → Dimension); clicks −75.5 dBFS at 0.8 Hz, at 3 Hz −57.1 dBFS where the modes' own read −57.7 [M] | Mix 0.35 **confirmed**. At mix 1 the depth (40 against 20.3 dB), the "involving Classic" attribution and the +2.0 dB bound are **contradicted**; mix 0.5 has one pair deeper than the row's −4.4 dB. Click bound **confirmed** (the modes' own) |
| D-56 | A glide bends at most 4 cents beyond the settings' own detune; crossfades at most once per 250 ms while a knob moves | +4.0 cents (CE +4.1); 250 ms or more [M] | **Confirmed**. The cost figures (+315 / +43 / +6 cents and the ramp figures, read on the summed voice) were not re-measured |
| D-57 | 4 of 1200 switches dip more than 6 dB, worst 17.3 dB for 3.5 ms into the script Phase 90 at 2479 Hz; attributed to that voicing's own notch, not verified. P19 passes at 3.00 dB | Same span measure: 10 of 4800 (0.21 %) at 0.3 Hz, 21 of 4800 (0.44 %) at 1.3 Hz, 13 of those 21 into the script voicing; at 2494.9 Hz 17.3 and 18.2 dB for 2.9 ms [M]. **The attribution is right** (below). At 4 Hz 326 of 4800 (6.8 %), up to 13 ms. P19 on its own grid: **2.9971 dB** [A `ph_p19_repro.py`]; on other notes it fails from 0.7 Hz (7.21 dB) | Figures **confirmed for 0.3 and 1.3 Hz**. The row and P19 say nothing about faster rates, where the limit is far worse (finding 1). **The P19 margin is not real** |

**D-57 attribution, checked** [M `ph_d57.py`, `ph_switch_an.py`]. The incoming voicing is reset at the request and its
sweep starts at the bottom; after the switch the output equals a fresh engine fed from the request block within −284 dB,
so the target's own output during the fade is known exactly. At 1.3 Hz the script voicing's second notch crosses
2479 Hz 62 ms after the request, inside the 30 to 70 ms fade. At the deepest point the target alone reads −46 dBFS
(34 dB under the input), the source alone −19 dBFS, the output −58 to −59 dBFS. A plain crossfade of the two alone
renders dips 11.5 to 12.5 dB at the same place. Over the 21 span dips at 1.3 Hz the target alone has a median of
−43.4 dBFS at the dip. So the dip is the target's notch arriving while the source's remainder is still in the mix: 12 to
13 dB under the notch itself, for under 3 ms. It is not the antiphase blip. The "off the note" half of the row (−52.1
dBFS worst, 9 of 720 above −60) I could not reproduce with my measure (peak outside 0.6 to 1.4 f0): 567 of 4800 above
−60 dBFS at 1.3 Hz, loudest −39.7 dBFS at 82.4 Hz where a plain crossfade reads −41.6. By the author's measure the excess
over an ideal fade stays inside +0.5 dB on my grid too (1 of 2240 at +0.5) [A].

## The tones at this build

| | Stated | Mine [M `ph_tone.py`, `ph_tone_dbg.py`, log `ph_tone.log`] |
|---|---|---|
| Engine against `phaser_ref.py` `render()`, four voicings, 44.1 / 48 / 96 kHz, block 37 | ≤ −80 dB | −151.9 dB, largest difference 3.0e-8 |
| Phase 90 block, notch-1 trajectory against Guitar Rig 6 (`trajectories.tsv`), six rates | 0.41 to 0.70 % rms, max 1.18 % | 0.41 to 0.70 % rms, worst −1.21 % |
| Small Stone Color off, three rates | 0.12 to 0.21 % rms, max 0.53 % | 0.13 to 0.24 % rms, worst −0.86 % |
| Small Stone Color on, three rates | 0.30 to 0.44 % rms, max 1.21 % | 0.32 to 0.45 % rms (1 to 2 bins of 150 dropped at two rates, where my tracker took notch 2) |
| Waveform null, Phase 90 block, 1.071 / 3.72 Hz | −27 dB (−26.7 / −27.1) | −26.71 / −27.09 dB |
| Waveform null, Small Stone off, 0.316 / 1.778 Hz | −37 dB (−40.8 / −37.3) | −40.84 / −37.25 dB |
| Waveform null, Small Stone on | −32 dB (−36.0 / −31.7) | −36.03 / −31.70 dB |

- My tracker is an impulse-snapshot tracker written for this run, with the same P and M as each Guitar Rig take. On the
  engine frozen at the sweep ends it returns 419.42 and 1589.92 Hz for the block voicing (spec table 419.4 and 1589.9).
- The null is a complex check: one least-squares gain, the LFO start and an integer latency searched (16 or 17 samples
  found). The dry take alone nulls at −5.0 to −0.0 dB. The fitted gain times our make-up is 1.0010 to 1.0016 for both Small
  Stone voicings, so their dry and wet weights and level law match Guitar Rig to 0.01 dB. For the block voicing the gain
  is +3.02 dB (Guitar Rig 6 is 3.0 dB louder than our made-up output; one scalar, the shape nulls to −27 dB).
- The Guitar Rig side of the trajectory figures is `trajectories.tsv`, the authors' tracking of renders that are not in
  the repository. Only our side is independent there. The nulls use the Guitar Rig renders themselves.
- The script Phase 90 has no measured reference; only its parity with the prototype was checked.
- **The steady tones are untouched by the round-three fix** [M]: parity −151.9 dB, fits and nulls equal to the stated ones.

## Both engines: null, bad input, block sizes [M `rb.py chorus`, `rb.py phaser`; logs `rb_chorus.log`, `rb_phaser.log` (the phaser log stops before the bad-input rows: `rb.py phaser bad_input blocks` prints them)]

- `--classic-null`, 44.1 / 48 / 96 / 192 kHz × blocks 1 / 37 / 512 / 4096 × mono / stereo: chorus 0 of 224 differ over
  seven key sets (automation, on / off, resets, width and tone); phaser 0 of 160 over five. The sets with `host_bpm`
  differ only because the adapter gives the legacy reference the plain `rate` key: with `rate` set to the tempo's rate
  they null. Phaser Classic with `reset_on_enable=1` differs from the legacy by design.
- `--reset-repeat`: every failure is Classic (chorus 40, phaser 35), and the legacy reference fails the same way.
  Modes 1 to 4 and voicings 1 to 2, switching every 50 ms included: 0 failures. `--rt-check`: 0 of 325.
- NaN, ±Inf, a NaN burst, ±3e38, 2e6, 1e30: 0 non-finite outputs, and 0.5 s later the output is bit-equal to the render
  with zeros in place of the bad samples (blocks 1 / 64 / 512, every mode and voicing). Denormal tail: at most 3e-37.
  Silence: exactly 0. Full-scale squares: chorus +8.3 dBFS (Tri, mix 1), phaser +12.5 dBFS (Small Stone Color on).
- Block sizes 1, 2, 3, 7, 16, 37, 64, 127, 1000, 4096 against 512, static settings: bit-equal for both engines.

## What I did not check

- Nothing through the processor or the dispatcher. The adapters' simulated hosts stand in for it. The round-3 processor
  harness sources are gone and I did not rebuild one.
- Sample-rate change with re-prepare: the bench cannot re-prepare in one run. Only fresh runs at each rate.
- CPU, by instruction. Listening.
- The author's chorus rows CH30, CH30t, CH33 were not re-run; my grids replace them. D-56's summed-voice cost figures.
- Phaser switches at other sample rates than 48 kHz and with decorrelated stereo. Chorus switches at 96 kHz.
- The strict (60 s) Tri figures on the pre-fix build and at 44.1 kHz: I stopped those runs unfinished (the ASan build
  is slow and the machine was loaded). The before / after for Tri above uses the 0.5th-percentile reference on both
  builds. Rerun with `R4BIN=$PWD/bin/tubamp_fxbench_r3pre_asan /usr/bin/python3 ch_loop3.py 48000 dual 0.8 120 24 4`.
- Whether any dip is audible. Durations are given so that can be judged: the phaser dips are 2 to 23 ms, the Tri holes
  5 to 53 ms.

## Corrections to my own first readings (kept so nobody repeats them)

- Chorus "holes" for Wide and for CE at mix 0.5 against a 9 s reference were the modes' own combs; against 60 s they are 0.
- A phaser `--classic-null` failure with `host_bpm` and Classic `--reset-repeat` failures are the adapter and the frozen
  code, not the engines.
- My first off-note measure used a linear crossfade as the ideal; the author's equal-power ideal is the fair one.
