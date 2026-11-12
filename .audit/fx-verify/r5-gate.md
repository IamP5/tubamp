# Round 5: independent attack on the gate's closing rule, version 5.1

Scope: `src/dsp/Gate.h`, `src/dsp/Gate.cpp` at 8a1081a, types Tight and Smooth at their defaults
(hold 10 ms, release 30 / 80 ms, range -90, sidechain high-pass 75 Hz). No source file was changed.

Binary: `build-verify/r5-gate/bin/tubamp_fxbench`, a copy of
`build/tubamp_fxbench_artefacts/Release/tubamp_fxbench` (built 04:47, after 8a1081a).
sha1 `0e2753fe35ed7d60f0df46ed014948865a0731c5`. Every script prints it.

Every figure below is **measured by me on that binary** unless it carries the word *inferred* or *guess*.
"Plain gate" is my own reference in `v5lib.py`: open at the threshold, close after the level has been
6 dB under it for the hold, dB-linear release, on the same level reading as the engine.
"v3" is the binary `scratchpad/gate5/fxb_v3` (sha1 `db98e54d0bc5d05814d22b56e4f69d5d52bc48c3`). Its name
says version 3. I did not check which commit it was built from (inferred).

Run any script with `/usr/bin/python3 build-verify/r5-gate/<name>.py` from the worktree root.

## Verdict

**No.** A working guitarist or mix engineer would not accept Tight at its defaults, and would not accept
Smooth either, on two kinds of material:

1. Lines of notes that die away on their own (clean arpeggios, bass lines, staccato parts with a high
   threshold). A note picked 195 to 265 ms after the previous one faded under the threshold comes out
   1 to 85 dB too quiet for its whole length (finding 1).
2. Fast palm-muted riffs. After a few short gaps the gate stops closing gaps of 60 to 100 ms and the
   floor is heard between every chug for the rest of the riff (finding 2).

Both are new in version 5.1. The v3 binary shows neither on the same signals.

Tight alone has a third fault: chords with a second in them chatter when they are played softly or held
at a steady level near the threshold (finding 3).

What holds up: single notes never chatter (480 cases), chords ringing out from a loud start chatter far
less than under a plain gate, no note of 30 000 chugs is eaten, parameter moves are clean, no clicks.

## Method and its checks

- My level reading (`gate_level`) agrees with the engine's threshold to 0.01 dB on 997.3 Hz and 82.41 Hz
  tones (bisection on the threshold, `t0_sanity.py`). My plain gate lets go 24.94 ms after a hard stop,
  Tight 24.96 ms.
- Test tones are not sample-locked (997.3 Hz, random partial phases). Plucked strings are additive, band
  limited, with faster decay for higher partials and slight inharmonicity (`v5lib.pluck`).
- Two of my own first measurements were wrong and are not used. The first sag test stepped the level by
  9 dB in 20 ms, which is a stop by the rule, and it read the gain at the file's end. `t1_deadline_return.py`
  is the corrected version (slopes of 40 dB/s, window ends before the file does).
- No real guitar or bass take exists in the repo. The WAVs under `docs/research/fx` are phaser audition
  files. Everything here is synthesized.

## Findings, ranked by how audible and how likely

### 1. A note that returns inside the deadline's fade stays attenuated for as long as it sounds (Tight and Smooth)

**What happens.** The deadline pulls the gain down over the last 80 ms before `hold` + 250 ms while the gate
is still open. If the level comes back over the open level inside that window, the gate never closed, so
nothing opens it again. The gain stays where the fade had reached, until the level next falls under the
threshold. The meters report state 2 (open).

**One command.**

```
build-verify/r5-gate/bin/tubamp_fxbench gate gen:sine:220:-30 out.wav mode=1 threshold=-40 \
  threshold@1.0=-28 threshold@1.23=-40 hold=10 release=30 range=-90 sc_hpf=75 \
  --sr=48000 --block=64 --seconds=12 --trace=tr.wav --meters=m.tsv
```

A 220 Hz tone 10 dB over the threshold. The threshold visits -28 for 230 ms. The gain is -36.1 dB from
1.23 s to the end of the file at 12 s. Meter row at 6 s: state 2, gain reduction 36.1 dB, detector -30.0 dBFS.

**On notes** (`t2_stuck_gain.py`). A2 pluck at -12 dBFS decaying at 50 dB/s through threshold -40, then a D3
pluck at -12 dBFS (28 dB over the threshold) picked D ms after the first note last read over the open level.

| D ms | 185 | 195 | 205 | 215 | 225 | 235 | 245 | 255 | 265 | 300 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Tight, gain of the second note, dB | 0.0 | -0.9 | -4.5 | -10.9 | -20.2 | -32.3 | -47.1 | -64.8 | -85.3 | 0.0 |
| Smooth | 0.0 | -0.6 | -1.3 | -1.9 | -2.5 | -3.1 | -3.7 | -4.3 | -5.0 | 0.0 |
| Plain gate | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| v3 (`t12_v3_compare.py`) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |

The gain is the same 20, 100 and 500 ms into the second note. It lasts 960 to 967 ms, which is all the time
that note is 6 dB or more over the threshold. Same at 44.1 kHz block 64 and block 1 (-32.1 dB at D 235),
96 kHz block 1024 and 192 kHz block 4096 (-30.3 dB), 48 kHz block 37 (-32.3 dB). The window follows the hold
(hold 0, 50, 250 give -32.2, -32.3, -32.3 dB at D 225, 275, 475). Release 5 and 160 change nothing. Release 300
moves the window (-25.7 dB at D 400). Smooth goes deeper after a faster first decay (worst -15.1 dB in the riffs below).

**How often** (`t3_stuck_riffs.py`). 90 seeded lines of naturally decaying notes (guitar, drop C, 5-string
bass, 60 to 140 bpm, 12 ms timing jitter, thresholds -45, -35, -28). Of 1001 notes that are 10 dB over the
threshold, Tight plays 35 (3.5 %) more than 3 dB down and 32 (3.2 %) more than 10 dB down. Smooth 24 (2.4 %) and
15 (1.5 %). Plain gate 0. Hand-stopped chugs are not hit (0 of about 30 000 notes, `t7_rhythm.py`), and deep slow
beats are not hit (0 of 58 crests, `t13_beat_ratchet.py`). The share depends on my lines (the tempo against the decay). The mechanism does not.

**Cause** (inferred from the source, and the measured depths follow -90 u^2). `Gate.cpp` lines 668 to 680 apply
`tight.gr = jmin (tight.gr, deadline)` while `open` is true. `startOpening` runs only on a closed to open change
(lines 553 to 567) and `advanceOpening` returns at once when the phase is 1 (line 874). Nothing raises the gain
again when the level returns before the deadline closes the gate.

**Why the rows miss it.** R6 and TB1 measure how the gain falls after the level leaves. No row reads the gain
after the level comes back inside `hold` + 170 to 250 ms. D-60's sentence "a chord's troughs up to about hold +
190 ms keep the gain over -6 dB" is true, and the gain then stays there.

### 2. Palm-mute gaps latch open: one ridden gap teaches the gate to ride the next, longer one (Tight and Smooth)

**What happens.** A gap the gate rode open is remembered and lengthens the wait after later stops by 1.25 times
its length. Each gap ridden that way is remembered in turn. Gaps can grow by a quarter per step up to `hold` + 150 ms
and none of them closes. The memory fades only 40 ms per second, and every ridden gap renews it.

**Constructed** (`t8b_gap_latch.py`, `t8c_gap_latch_rates.py`). E2 chugs, hand-stopped, hiss at -70 dBFS,
threshold -40. Five gaps of 20, 28, 38, 52, 70 ms, then 30 equal gaps.

| The 30 gaps | Alone, median gap gain | After the growing gaps | Plain gate | v3 |
| --- | --- | --- | --- | --- |
| 60 ms | -64.8 dB | 30 of 30 at 0.0 dB | -71.4 dB | 0 of 30 open |
| 80 ms | -79.8 dB | 30 of 30 at 0.0 dB | -90.0 dB | 0 of 30 open |
| 100 ms, E2 | mute | 1 of 30 open | -90.0 dB | not run |
| 100 ms, C2 | 0 of 30 open | 30 of 30 open | 0 open | not run |
| 100 ms, A2 over 60 Hz hum, thr -45 | 0 of 30 open | 30 of 30 open | 0 open | not run |
| 100 ms, E2, hold 30 | 0 of 30 open | 30 of 30 open | 0 open | not run |

With 80 ms gaps the floor is heard for 2.4 s of the riff's 4.4 s. Same 30 of 30 at 44.1 kHz block 64, 48 kHz
block 1, 96 kHz block 2048, 192 kHz block 4096. Smooth: 30 of 30, median 0 dB.

**Not constructed** (`t7_rhythm.py`, output kept in `build-verify/r5-gate/out_t7.txt`). 864 seeded lines with 10
or more gaps of 35 ms and longer (eighths, sixteenths, triplets, gallops at 120 to 240 bpm, notes C2, E2, B0, A2,
floors hiss, 50 and 60 Hz hum, buzz, thresholds 3 to 30 dB over the floor, 6 ms jitter). In 46 lines (5.3 %)
Tight leaves 80 % or more of the gaps over -12 dB where the plain gate closes at least half. They cluster in
gallops at 180 bpm (17 lines), sixteenths at 160 (13) and 200 bpm (7). Two of them:

- Sixteenths at 160 bpm on C2 over 60 Hz hum, threshold 30 dB over the floor. 41 of 41 gaps at 0.0 dB. The
  level sits 30 dB under the threshold for 23 to 35 ms in each. Plain gate -14 to -41 dB, 8 gaps open.
- Chugs at 170 bpm on A2 over buzz, threshold 6 dB over the floor. 27 of 28 gaps at 0.0 dB, including gaps of
  78 to 81 ms with the level under the close level for 48 to 62 ms. Plain gate -77 to -90 dB there.

In fairness, the engine beats the plain gate near the floor. With the threshold 3 dB over the floor it leaves 3061 of
5472 gaps open, the plain gate 5206. At 10 and 20 dB over it is worse (1645 against 1418, 1235 against 840).

**Cause** (inferred). `learnDips` (lines 802 to 819) stores any dip of 12.5 ms or more the gate stayed open through.
`extension()` (line 768) adds `min (150 ms, 1.25 x memory + 1 ms)` to the hold. `kDipForget` takes back 40 ms per second.
The header's "only dips the gate rode open are remembered" does not stop a ridden gap from being one.
This contradicts the comment on `kGateHoldDefault` ("every 40 and 50 ms palm-mute gap still closes").

### 3. Tight chatters on chords with a second when they start near the threshold

**Held chords** (`t6_chords.py`, `t6b_chord_detail.py`). 30 chords the rows do not use, steady level, threshold
from 3 dB over to 20 dB under the chord's median reading. Tight has gain dips (under -6 dB, recovering) in 36 of 360
cases, the plain gate in 15. Tight has more dips than the plain gate in 29 cases (541 dips against 146). Smooth: 0 of 360
(plain 14).

- E2+F#2 (beat 10.1 Hz), threshold 2 dB under the median: 33 dips in 4 s, median 10.6 ms long, median depth -22.9 dB,
  deepest -49.9 dB. Plain gate 0. v3 0.
- A2+Bb2 (6.5 Hz), same threshold: 25 dips, 30.4 ms, -60.7 dB. The gain is under -20 dB 13 % of the time. Plain gate 0.
- Of the 38 pairs one or two semitones apart from B0 to E3, 21 chatter more than the plain gate at some threshold
  1 to 5 dB under the median. E2+F2, which the rows use, is one of them (19 extra dips, v3 0).
- Same counts at 44.1, 48, 96, 192 kHz and blocks 1 to 4096. Hold 20 clears E2+F#2, hold 40 clears A2+Bb2.

**Softly plucked chords** (`t6d_soft_chords.py`). 28 chords, peak 1 to 10 dB over the threshold, decays of 5 and
10 dB/s. Tight dips in 57 of 392 cases (plain 45) and has more dips than the plain gate in 47 (104 dips against 60).
Worst: D2+E2 five dips to -78 dB, plain 0. Smooth 0 of 392 (plain 43).

**Where it does not happen.** Chords that ring out from a loud start. Tight dips in 3 of 150 cases (plain 21) in
`t6_chords.py` and in 10 of 304 plucked seconds (plain 123) in `t6c_ringing_seconds.py`. Smooth 0 in both. There the
engine is much better than a plain gate.

**Cause** (inferred). The in-band stop (lines 573 to 590) fires on the first beat trough of a chord whose crest is
only a few dB over the threshold. The close forgets the beat (lines 651 to 654) and a trough the gate closed in is
not learned, so the next trough closes it again.

### 4. Slow decays lose the hysteresis, and the release knob does nothing on them (D-60, declared in part)

- `t14_knobs_smooth.py`. On a note decaying at 20 dB/s Tight's fall from -1 to -60 dB takes 56.9 ms at release 5, 15,
  30, 60, 100 and 160 ms. It changes only above 167 ms (68.3 ms at 200, 170.7 ms at 500). On a hand stop the knob works
  as labelled (4.9, 14.8, 29.5, 59.0, 98.3, 157.3 ms). D-60 does not say that the knob is dead on natural decays.
- `t11_declared.py`. Tight lets go when the note is 1.0 to 14.1 dB under the threshold. The plain gate lets go at 4.0 to 7.0 dB.
- `t5_single_notes.py`. 240 single plucks. No dip in any. With a 6 dB/s decay Tight plays 1061 ms where the plain gate
  plays 1602 ms (E2, threshold 6 dB under the peak). In 17 cases Tight plays under half of what the plain gate plays.
- `t4_wobble.py`. A held note decaying at 2 to 8 dB/s with a level wobble of 1 to 5 dB at 0.5 to 5 Hz. Tight is muted for
  more than 100 ms while the level is over the threshold in 24 of 72 cases (plain 0). The plain gate passes sound a median
  494 ms longer, at most 2833 ms. Smooth: 0 of 72.
- `t1_deadline_return.py`. A tone that sags to 3 dB under the threshold for 279 ms and returns to 3 dB over it stays muted
  for the remaining 3 s (Tight). Plain gate and Smooth 0 dB throughout. This is D-63 with the note as its own floor.

### 5. A soft note with a hard attack is cut to a tick (Tight, constructed)

`t15_attack_spike.py`. A note whose attack stands 10 dB over its body (10 ms time constant), body 1 to 5 dB under
the threshold. Tight plays 52 to 55 ms on A2, the plain gate 238 to 739 ms, Smooth 779 to 1264 ms. On E1 it starts at a
6 dB attack (53 ms against 438 ms). My additive plucks have 0.4 to 5.8 dB of attack over the reading 50 ms later and are
not cut. Whether real picks or slaps reach 6 to 10 dB is a guess. I had no recording to measure.

### 6. Stereo input is read as the mean of its channels

`t10_moves_clicks.py`. Each channel at -34 dBFS, threshold -40. Equal channels: open. Left only: closed (the reading is
6 dB lower). Left = -right: never opens. I did not check whether the processor ever feeds the gate two channels.

### What passed

- Parameter moves on a held note (threshold steps and ramps, hold 0 to 250, release 5 to 500, range, sidechain filter
  20 to 300 Hz, Tight to Smooth, to Classic and back, bypass): deepest gain 0.0 dB, biggest sample step 0.000. A re-prepare
  cuts 7.9 ms, as a reset must.
- DC of -25 dBFS does not open the gate with the filter on. Notes at +12, +40 and -96 dBFS gate normally. Output finite.
- Splatter on band-limited notes: -66 dB re the note at chord reopenings, -27.8 dB re the local level at a pick's opening.
- Plucked pairs a few cents apart ringing out (`t4_beating.py`, 84 cases per type): as many open/close changes as the plain gate (168 against 168), nothing muted over the threshold.
- Noise floors (`t9a_noise.py`): white, pink, hum plus hiss, median reading -8 to +3 dB re the threshold, 30 s: at most
  2 open/close changes. A floor that breathes 3 dB at 0.15 Hz: 11 (plain 9). Crackle: 122 (plain 112).
- Smooth on a steady floor holds its gain within 3 dB (`t14_knobs_smooth.py`). Its depth is about twice the distance under
  the close level: -13 dB with hiss 10 dB under the threshold, -33 dB at 20 dB, -83 dB at 45 dB. It never reaches the range
  on a floor nearer than that. This is the expander law, by design, and it is why Smooth leaves 4813 of 5472 gaps over -12 dB
  with the threshold 10 dB over the floor.

## Declared limits

| Row | The doc's figure | Mine | Result |
| --- | --- | --- | --- |
| D-60 | Gain over -6 dB at most 200.6 ms after the level was last over the open level. Over -60 dB at most `hold` + 235.3 ms. | 200.6 ms on all 20 plucked notes. 233.5 to 245.3 ms for -60 dB. At the range at 259.9 ms. Smooth 243 to 1972 ms for -6 dB (`t11_declared.py`). | Confirmed. Not declared: finding 1, the dead release knob, and the mute of a note wobbling over the threshold (finding 4). |
| D-61 | Gaps of 40 to 52 ms end at -18 to -23 dB. X13: 30 ms -4.2, 40 ms -23.4, 50 ms -43.4 dB. | Steady trains, gap from the start of a 6 ms stop: 30 ms -8.0, 36 ms -20.1, 40 ms -28.1 (worst -27.2), 46 ms -40.0, 52 ms -52.1 dB on E2. C2 and A2 within 2.2 dB. | Confirmed for steady trains. Contradicted after a ridden gap: 60 and 80 ms gaps end at 0.0 dB (finding 2). |
| D-62 | 4 cells of 72, all at gap 0.1 s. Deepest dip -14.5 dB, longest 12 ms, at most 2 per case. | 50 cells of 840 at gaps 0.06, 0.08, 0.10, 0.12 s (15, 18, 8, 9). 0 at 0.15, 0.25, 0.6 s. Median dip 15.1 ms, longest 78.2 ms, median depth -31.3 dB, 46 of 76 dips under -20 dB, up to 3 per case (`t9b_g3b.py`, `out_t9b.txt`). | The limit is real and wider than stated. Depth and length contradicted on my signals. |
| D-63 | A note less than 6 dB over a floor inside the band does not reopen after a deadline close. | After a 4 dB/s decay is closed, a soft note 1 or 2 dB over the threshold is muted 0.15 s later, +3 dB plays at -5 dB, +1 dB is still muted at 0.4 s, all play from 0.8 s. Plain gate passes all. With a hiss floor 1 to 12 dB under the threshold a +1 dB note opened every time. | Confirmed. The doc names a floor. The previous note's own tail does the same (finding 4). |
| D-64 | Floor audible 346 ms in one of 76 cases. `hold` + 235.3 ms from the last crossing. | From the note's own reading passing under the hiss's: median 280 ms, max 538 ms, 23 of 80 cases over 346 ms (max by t60 0.8 / 1.5 / 3 / 6 s: 251 / 355 / 397 / 538 ms). From the last crossing of the open level, to -20 dB: max 217.7 ms (`t11b_d64.py`). | The 346 ms figure is contradicted for slow decays. The bound from the last crossing is confirmed. A plain gate never closes in these cases. |

## The bars that sit on the measured value

- **R6, 200.6 against 202, and TB1, 235.3 against 236.** Both are the arithmetic of the deadline's constants.
  10 + 170 + 80 x sqrt (6 / 90) = 200.66 ms and 170 + 80 x sqrt (60 / 90) = 235.32 ms. My 20 plucked notes read 200.6 ms
  every time. The bars are principled as a restatement of the constant. They cannot fail on any material, and they say
  nothing about what follows when the level returns in that window, which is where finding 1 lives.
- **X4 long, 2 against 2.** Principled. Two changes are one opening and one close. It holds on pink noise and on hum
  plus hiss, at offsets down to -8 dB (worst 2).
- **B2 g3b, 4 against 4.** Fitted. The count is pinned to the row's own grid. Neighbouring gaps and chords give
  50 of 840, a rate of 6 % against the row's 5.6 %, with deeper and longer dips than D-62 states.

## Not checked

- No real recording, and no listening. All figures come from gain traces.
- Deferred application (`deferred`, `lag`, `delay`), Classic and `--classic-null`, `--rt-check`, `--reset-repeat`.
- The plugin, `auval`, the UI and its meters. `check_gate.py` was not rerun.
- Most sweeps ran at 48 kHz, block 512. Findings 1, 2 and 3 were repeated at 44.1, 96 and 192 kHz and blocks 1 to 4096.
- Bass with the sidechain filter off or at 40 Hz, ranges other than -90 on notes, thresholds outside -60 to -24.
- Which commit `fxb_v3` was built from.
- Whether real pick attacks reach the 6 to 10 dB of finding 5.

## Files

- Scripts and helper: `build-verify/r5-gate/t0_sanity.py` to `t15_attack_spike.py`, `v5lib.py`.
- Kept outputs: `build-verify/r5-gate/out_t7.txt`, `build-verify/r5-gate/out_t9b.txt`.
