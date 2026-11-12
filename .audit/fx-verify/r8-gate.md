# Round 8: independent attack on the opening ramp with the run-away finish (360b499)

Scope is the opening ramp of the gate (Tight and Smooth) as it stands in `360b499`, and the check rows that guard it.
Three binaries, copied into `build-verify/r8-gate/bin/` before any run:

| Name | What | sha1 |
| --- | --- | --- |
| A | the build round 6 accepted (original timing) | `36078be498067dcb4d6b7d145ee92140ca40ea37` |
| B | the withdrawn last-dB timing (round 7) | `6cbbd6ea01bd6f99836cc6167f8c6048a15cc02f` |
| C | the current rule | `e2a345413db6b4e01dce622db05395c51f85cf44` |

Every signal is synthesized. The repository holds no real guitar recording, so nothing here was checked on one.
Nothing under `src/`, `tools/`, `tests/`, `docs/` or `ui/` was edited by me. Nothing was committed.
The wrong-rule binaries of the row audit were built from an exported copy of HEAD under `build-verify/r8-gate/mut/`.

Each figure is **measured** (with the script that printed it), **inferred** (from code or arithmetic) or a **guess**.
Scripts are in `build-verify/r8-gate/`. Their output is beside them as `out_*.txt`. Load average was 5 to 38 on 12
cores (other jobs ran the whole time), so no timing here means anything and none is reported.

## Verdict

**Ship C: YES.** Round 6's finding 1 is **reduced, not closed**.

C is the stated rule to the sample. Its gain is never under A's, so no pick loses more than in A: 0 of 4 595 picks
and 0 samples in 1 728 random renders. It fixes the hard pick that finding 1 was about. 76 % of them lost over 1 dB of
their first 2 ms in A, and 2.9 % do in C.

Two things in the commit's account are wrong, and neither is a reason to hold it.

- The remaining cost is stated too narrowly. It is not picks under 6 dB over. It reaches to about 8 to 12 dB over for
  a pick with a transient and about 15 dB for one without. Those picks are exactly as slow as in A.
- "Nothing gets worse" is true of the attack's energy and false of splatter. Where the finish runs on a note with no
  transient of its own, C makes a gate click that A did not. It is about the size of the click A already makes on the
  same attack from silence, and on a picked note it hides inside the pick.

| What decides it | A | B | C | Source |
| --- | --- | --- | --- | --- |
| Engine against my reference of the rule, 1 728 cases x Tight and Smooth, 4 rates, blocks 1 to 4096 | | | 0 flagged, worst 0.000021 linear in a ramp, 0 events off by a sample | measured, `q1_conformance.py` |
| Picks where C is worse than A by over 0.01 dB (first 2 ms, first 5 ms) | | | 0 of 4 595 | measured, `q2_picks.py` |
| Picks where B is worse than A | | 553 (12.0 %) | | measured, `q2_picks.py` |
| Hard picks after a precursor (attack under 0.5 ms, 12 dB or more over, 556) losing over 1 dB of their first 2 ms | 76.3 % | 9.9 % | 2.9 % | measured, `q2_picks.py` |
| All picks (4 595) losing over 1 dB of their first 2 ms | 62.2 % | 15.8 % | 36.9 % | measured, `q2_picks.py` |
| All picks losing over 3 dB of their first 2 ms | 48.7 % | 11.9 % | 25.7 % | measured, `q2_picks.py` |
| Picks 6 to 9 dB over (829, by the reading's actual peak) losing over 1 dB | 78.9 % | 20.9 % | 59.0 % | measured, `q2_picks.py` |
| Click on band-limited attacks where the finish runs (1 733 openings), dB re the note's peak: median, worst | -103.2, -42.3 | | -46.3, -27.7 | measured, `q3_cost.py` |
| Click A itself makes at its own 0.2 ms openings on the same material (431): median, worst | -49.2, -30.8 | | | measured, `q3_cost.py` |
| Swells up to 300 dB/s, with and without tremolo, whose ramp C changes | | | 0 of 2 283 | measured, `q3c_swells.py` |
| Chord re-openings where the finish runs, crest up to 15 dB over | | | 0 of 657 and 0 of 160 | measured, `q3b_beat.py`, `q3e_tables.py` |
| Opens and closes, A against C, per sample | | | 0 differences in 1 728 cases | measured, `q1_conformance.py` |

The pick and click shares come from material I built to provoke the weakness and the finish. They are not the
incidence on real playing.

## Findings, ranked by how audible

### 1. Picks that take longer than about half a millisecond to run 6 dB over still fade in, exactly as in A

**What happens.** The finish starts only when the reading is 6 dB over the threshold. Until then the ramp runs at the
rate the band gave it. A pick that reaches 6 dB over within 0.2 ms of the opening loses nothing. One that needs 1 ms
or more is nearly as slow as in A. (Measured, `q2_picks.py`, picks whose ramp is over 0.2 ms, Tight.)

| Time from the opening to 6 dB over | Picks | Lose over 1 dB of the first 2 ms, A / C | Lose over 3 dB, A / C |
| --- | --- | --- | --- |
| under 0.2 ms | 1 141 | 76.1 % / 0.4 % | 59.9 % / 0.0 % |
| 0.2 to 0.5 ms | 355 | 74.4 % / 13.2 % | 59.4 % / 1.4 % |
| 0.5 to 1 ms | 267 | 71.2 % / 43.8 % | 56.6 % / 5.2 % |
| 1 to 2 ms | 244 | 70.5 % / 67.6 % | 48.4 % / 35.2 % |
| 2 to 5 ms | 292 | 72.6 % / 71.6 % | 56.2 % / 55.8 % |
| over 5 ms or never | 1 404 | 82.2 % / 82.2 % | 65.0 % / 65.0 % |

**The commit's statement of the cost is too narrow.** It says picks that peak less than 6 dB over still open slowly.
Measured by how far the pick's own reading peaks over the threshold (4 595 picks, about 306 per level):

| Pick peak over the threshold | +1 to +3 | +4 | +5 | +6 | +7 | +8 | +9 | +10 | +12 | +15 | +20 | +25 | +30 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Lose over 1 dB of the first 2 ms, A | 74.6 to 75.1 % | 69.8 % | 65.9 % | 62.9 % | 62.0 % | 61.8 % | 60.7 % | 56.7 % | 58.7 % | 54.4 % | 53.1 % | 51.3 % | 51.8 % |
| The same, C | 74.3 to 74.9 % | 67.5 % | 61.6 % | 54.7 % | 44.2 % | 31.7 % | 28.9 % | 21.2 % | 13.8 % | 4.9 % | 1.3 % | 0.7 % | 0.0 % |
| Lose over 3 dB, C | 53.8 to 60.3 % | 52.5 % | 44.3 % | 37.5 % | 22.7 % | 15.4 % | 14.6 % | 13.0 % | 7.9 % | 3.6 % | 0.7 % | 0.3 % | 0.0 % |

So the cost reaches well past 6 dB. A pick has to peak about 15 dB over before fewer than 5 % still lose over 1 dB.
It depends on the pick's own transient (measured, `q2b_tables.py`, share losing over 1 dB in C):

| Pick peak over the threshold | +5 | +6 | +7 | +8 | +9 | +10 | +12 | +15 | +20 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Karplus-Strong string (noise-burst first period), 1 595 picks | 50 % | 42 % | 27 % | 14 % | 9 % | 8 % | 1 % | 1 % | 0 % |
| sum of partials with an attack envelope, 3 000 picks | 67 % | 63 % | 52 % | 42 % | 40 % | 28 % | 21 % | 7 % | 2 % |

With a pick-like transient the cost ends near 8 dB over for a 3 dB loss (3 % at +7, 0 to 1 % from +8) and near 12 dB
over for a 1 dB loss. Without one it runs to about 15 dB over.
The reason is that a pick peaking 8 dB over reaches 6 dB over only near its peak. The share of slow-ramp picks that
get to 6 dB over within 0.5 ms of the opening is 16 % at +6, 47 % at +8, 55 % at +10, 66 % at +12, 81 % at +15 and
92 % at +20 (measured, `q2b_tables.py`).

**Low, dull notes stay slow even when loud.** Of the 556 hard picks, 16 still lose over 1 dB in C. All 16 come from
my sum-of-partials generator at 41 to 330 Hz (14 of them at 41 to 110 Hz), none from the Karplus-Strong generator
whose first period is a noise burst. (Measured, `q2b_tables.py`.) Their waveform takes a median 2.4 ms to climb from the threshold to 6 dB over.
The worst loses 16.2 dB of its first 2 ms (A 19.4). Whether a real low string has enough transient to run away in
time I could not check without a recording (guess: a picked one does, a thumbed one may not).

**How audible.** Guess: this is the most audible thing left, and it is no worse than what round 6 accepted. A soft
note 1 to 8 dB over a threshold that sits within 6 dB of the floor fades in over up to 5 ms. That softens a soft
note's front. It does not gate or chop it.

**Where the threshold makes the finish impossible.** At a threshold of -6 dBFS or higher the run-away level is at or
over full scale, so a signal inside full scale never triggers the finish and C equals A (inferred, `Gate.cpp` 347).

### 2. New in C: a gate click on dull attacks that follow something in the band

**What happens.** On an attack that climbs 6 dB inside the ramp, the finish moves the gain the rest of the way in at
most 0.2 ms, at a moment when the signal is already twice the threshold and at a waveform peak (the reading only
rises when the waveform sets a new maximum). A had a 5 ms smoothstep there and no splatter at all.

Material band-limited to 3 kHz, so everything over 8 kHz in the output is the gate's. Click = output peak over
8 kHz from 0.5 ms before to 6 ms after the opening, relative to the input's peak in the 10 ms after it. Tight,
48 kHz, 40 renders per family. Smooth's totals are within 1 dB of Tight's (4 249 openings, 1 809 with the finish,
median -46.6, worst -27.7). (Measured, `q3_cost.py`.)

| Family | Openings | Finish ran | Click median A / C / textbook | Click worst A / C / textbook | C over -60 dB and over A by 3 dB |
| --- | --- | --- | --- | --- | --- |
| finger attack from silence, 3 to 15 ms | 415 | 95 | -66.1 / -54.6 / -47.8 | -36.3 / -30.9 / -30.7 | 18.1 % |
| finger attack over a floor in the band | 405 | 373 | -105.8 / -49.9 / -48.9 | -56.1 / -29.6 / -30.7 | 83.5 % |
| hammer-on over a resonance in the band | 414 | 337 | -103.9 / -46.0 / -44.2 | -82.8 / -27.7 / -30.7 | 78.5 % |
| dB-linear swell, 600 to 20 000 dB/s | 519 | 102 | -80.4 / -67.2 / -45.7 | -31.7 / -29.9 / -30.8 | 16.6 % |
| swell that speeds up after the threshold | 395 | 355 | -106.0 / -45.6 / -47.7 | -36.6 / -28.4 / -30.8 | 84.3 % |
| stepped swell | 545 | 175 | -87.9 / -73.5 / -47.0 | -32.3 / -29.3 / -30.8 | 28.3 % |
| volume swell with sine tremolo | 188 | 1 | -102.2 / -102.2 / -37.3 | -90.1 / -49.8 / -30.7 | 0.5 % |
| volume swell with chopped tremolo | 200 | 17 | -81.5 / -79.1 / -37.3 | -33.0 / -29.3 / -30.7 | 8.0 % |
| low notes, 41 to 110 Hz, 5 to 30 ms attack | 417 | 102 | -80.8 / -60.6 / -44.3 | -30.8 / -28.1 / -30.7 | 22.3 % |
| hot input, threshold -70, 40 to 60 dB over | 430 | 172 | -103.3 / -85.2 / -66.3 | -43.2 / -31.9 / -35.2 | 9.1 % |
| Smooth from a part-closed gain (Smooth only) | 415 | 89 | -65.5 / -57.0 / -51.4 | -34.7 / -29.9 / -31.9 | 17.3 % |
| all Tight families | 3 993 | 1 733 | -97.2 / -58.9 / -46.4 | -30.8 / -27.7 / -30.7 | 36.7 % |

Over all 3 993 Tight openings C is worse than A by over 3 dB in 42.5 % and by over 10 dB in 42.0 %. The worst
difference is 92.8 dB, which only says that A sat at its numeric floor of -126 dB. C is better than A in none, and
where the finish did not run C equals A in every opening (0 differ by 0.01 dB).

**One click, sample by sample** (measured, `q3f_trace.py`, the loudest of the hammer-on family, render 26). The gate opens with a
5 ms ramp. The reading sits 5.45 dB over for 3 ms. At 3.17 ms a waveform crest takes it to 6.3 dB over, and the
gain goes from 0.69 to 1.00 in four samples (0.08 ms) while the input is at 2.1 times the threshold. A's gain is at
0.70 to 0.76 over the same samples. Click: A -107.9, C -27.7, textbook -38.1 dB re the note.

**Sized against what the gate already does.** (Measured, `q3_cost.py`.)

- A's own 0.2 ms openings on the same material (431): median -49.2, 95th percentile -38.1, worst -30.8 dB.
- C's finish openings (1 733): median -46.3, 95th percentile -32.7, worst -27.7 dB. 18.9 % are over A's 95th
  percentile and 1.8 % are over A's worst.
- Against the textbook 0.2 ms rise on the same opening, C's finish click is a median 1.3 dB louder, over 3 dB louder in
  43.5 %, over 10 dB louder in 14.7 %, and 15.5 dB louder at the 95th percentile. The textbook moves its gain when the
  signal is at the threshold. C moves it when the signal is 6 dB or more over and still climbing.
- Openings over -40 dB re the note: C 508 (12.7 %), A 54 (1.4 %), textbook 811. Over -30 dB: C 19, A 0, textbook 0.
- Absolute: C's finish clicks are a median 27.9 dB under the threshold, the loudest 6.1 dB under it. The textbook's
  clicks over all openings are a median 31.0 dB under, the loudest 9.9 dB under.
- Fastest gain move, in units of the textbook's 0.2 ms slope: A 1.00, C 1.49. Largest change of slope from one
  sample to the next: A 1.00 (the corner of a linear rise), C 1.44 (the switch).

**Does it stand out of the note's own top end?** Notes that are dull but not brick-walled: a string through a tone
control (2nd-order low-pass at 800 Hz, 1.5, 3 or 6 kHz), 8 to 25 dB over, after a hum or a sympathetic string in the
band. Over 8 kHz around the opening, output through C against the dry note's own (measured, `q3d_dull.py`, 1 474
openings where the finish ran):

| Note | Finish openings | C over the dry note's own top end by over 3 dB | by over 10 dB | median |
| --- | --- | --- | --- | --- |
| Karplus-Strong string (its first period is a noise burst, as a pick is) | 753 | 2.8 % | 0.3 % | +0.0 dB |
| sum of partials with an attack envelope (no transient of its own) | 721 | 73.5 % | 62.1 % | +17.1 dB |
| tone control at 800 Hz, both generators | 362 | 54.7 % | 49.7 % | +8.8 dB |
| tone control at 6 kHz, both generators | 368 | 14.9 % | 9.5 % | +0.0 dB |

So the click hides inside a picked note's own transient, even with the tone rolled down to 800 Hz, and stands clear
of a note that has none.

**How audible.** Guess. On a picked guitar it is masked by the pick. On a note with no transient of its own (a
hammer-on, a thumbed or volume-swelled note, a bass with the tone off) that climbs 6 dB inside the ramp over a floor
sitting in the band, it may be heard as a faint tick where A had a clean 5 ms fade. The tick is 28 to 55 dB under a
note that is itself arriving, lasts 0.2 ms or less, and is 3 to 5 dB louder than the click A makes when the same
attack comes from silence, which round 6 accepted. Listening files for a human are in `build-verify/r8-gate/listen/`
(`click_*_A.wav`, `_C.wav`, `_textbook.wav`, each trio raised by one gain to peak at -3 dBFS; `listen_make.py`).

**How often the finish runs at all.** In round 6's line, chord and wobble materials 9 of 534 openings. In my six
kinds built for it, 2 353 of 6 486 (measured, `q1_conformance.py`, `q3e_tables.py`). At 96 kHz the click figures repeat: 443 finish
openings in seven families, median -45.0, worst -27.2 dB; A's own 0.2 ms openings median -44.7, worst -32.8
(measured, `R8_SR=96000 q3_cost.py 12 ...`, `out_q3_96k.txt`).

**What does not happen.**

- A swell keeps its slow rise. Plain dB-linear swells and swells with a 6 Hz tremolo 6 or 12 dB deep, six notes from
  41 to 1000 Hz, three waveforms, eight phases: up to 300 dB/s C changes 0 of 2 283 openings. At 600 dB/s it shortens
  2 to 4 % (41 and 82 Hz only), at 1 000 dB/s 7 to 10 %, at 2 000 to 5 000 dB/s 19 to 25 % (measured,
  `q3c_swells.py`). The reason is arithmetic (inferred): the ramp is half the time the level took to cross the 6 dB
  band, so a level that keeps its pace or slows cannot gain another 6 dB before the ramp ends. Only a rise that
  speeds up after a dwell in the band triggers the finish, or a low note whose reading steps by over 6 dB.
- A chord's re-opening does not click. Row D5's own construction with the crest raised to 6.5, 8, 10, 12 and 15 dB
  and five more pairs: 657 re-openings, the finish ran in 0, C equals A, worst -65.8 dB (measured, `q3b_beat.py`).
  My own harmonic-rich pairs and 3 to 6 note chords ringing down through the threshold: 160 re-openings after the
  first 0.2 s, the finish ran in 0 (measured, `q3_cost.py 300 beat,beat_hold0,chord_decay,chord_decay_hold0`, then
  `q3e_tables.py`). The reason is the same arithmetic: the level climbs out of a beat trough ever more slowly.
- A hard chop tremolo does trigger it: 17 of 200 openings, worst -29.3 dB against A's -33.0.

**The same mechanism on a pop.** A crackle pop (0.3 ms decay) 6 to 20 dB over the threshold, over a hum or hiss that
sits 1 to 5 dB under it with the gate shut: A passes the pop a median 35.5 dB down, because its 5 ms ramp has barely
started, and C passes it 4.9 dB down, 30.5 dB louder (measured, `q3g_pops.py`, 51 pops). C treats it as either build
treats a pop from silence (inferred: a 0.2 ms linear rise under a 0.3 ms decay peaks about 5 dB down). A gate is not
a declicker and the floor then holds the gate open in both builds, so I count this as a side effect, not a defect.

**A cheaper finish, not built.** The same reference with the finish at other rates (inferred, `q6_whatif.py`; the
0.2 ms column is C, since the reference equals C):

| Finish rate | 0.2 ms (C) | 0.35 ms | 0.5 ms | 1 ms | none (A) |
| --- | --- | --- | --- | --- | --- |
| Click on 807 finish openings: median / 95th pct / worst, dB re the note | -44.9 / -32.6 / -27.8 | -50.6 / -38.2 / -33.6 | -54.3 / -41.7 / -37.0 | -62.3 / -48.9 / -42.3 | -103.8 / -82.8 / -42.3 |
| Share of those over -40 dB | 28.5 % | 8.9 % | 2.2 % | 0 % | 0 % |
| Hard picks (239) losing over 1 dB / over 3 dB of their first 2 ms | 3.3 % / 1.7 % | 4.6 % / 1.7 % | 13.8 % / 1.7 % | 54.0 % / 9.6 % | 61.9 % / 47.3 % |
| All picks (1 529) losing over 3 dB | 26.0 % | 26.4 % | 26.8 % | 30.5 % | 48.1 % |

A finish at 0.35 ms would put the click back at the level of A's own from-silence openings for about one point of
hard picks. That is the author's call, and not a condition of this verdict.

### 3. No row guards finding 2, and D3 does not show finding 1

See the row audit. D5 stops at a crest of 5 dB, by design, and reads only the first millisecond after the opening,
while the finish can come up to 5 ms after it. I5 bounds the slope, not the click. D3 judges 192 of its 720 cells and
prints nothing about the loss in the other 528.

### 4. Seen in passing, not part of this change

The range smoother stalls short of its target by 0.0018 dB at 48 kHz and 0.0073 dB at 192 kHz, in A and C alike. It is
a float32 state with a float32 coefficient, and the snap at 0.001 dB is never reached at these rates (inferred from
`Gate.cpp` 496 to 504; measured as the worst gain difference from my double-precision reference outside openings,
which doubles with the rate and is the same for A and C, `q1_conformance.py`). Inaudible.

## Q1. Is C the stated rule?

**Yes.** (Measured, `q1_conformance.py 1728`, output `out_q1.txt`.)

My reference is `ref8.c`. It is round 7's `ref7.c` with the run-away finish added from the header's sentence: while a
ramp runs, a sample whose level is at or over threshold + 6 dB advances the phase by 1 / (0.2 ms) instead of the
ramp's own step, with the curve blend left as the opening set it. It was not taken from the numpy port.

- 1 728 cases, each Tight and Smooth, rates 44.1, 48, 96 and 192 kHz, blocks 1, 2, 7, 31, 64, 127, 256, 480, 512,
  1000, 2048 and 4096, all five knobs drawn over their full ranges.
- Six kinds of my own material aimed at the finish (picks over a floor that swings across the close level, fast
  swells, stepped swells, beats, scrape then pick, hot input) and six of round 6's and 7's.
- 576 cases carry knob moves timed into slow openings: a threshold step of 1 to 8 dB up or down part-way through a
  ramp, back 20 to 60 ms later, with a hold, release or range step at the same instant, or a threshold ramp over the
  whole render.
- 7 735 openings. 6 212 ramp for longer than 0.2 ms. The finish shortened 2 444 of them. The threshold moved while a
  ramp ran in 898, and the finish shortened 254 of those.
- Worst gain error inside an opening: 0.000021 linear, Tight and Smooth. Outside openings: 0.0073 dB (finding 4).
- Open and close events: 0 samples differ between the engine's open flag and the reference's, in every case.
- 0 cases flagged. 0 non-finite outputs.
- A against the same reference with the finish off and the old settle count: the same error figures.

**The comparison can fail.** The same script with the reference made wrong on purpose, 288 cases each
(measured, `q1_conformance.py 288 --power=...`, output `out_q1_power.txt`):

| Reference made wrong | Cases flagged of 288 | Worst error inside a ramp, Tight / Smooth (linear) |
| --- | --- | --- |
| run-away level 5.5 dB | 105 | 0.975 / 0.935 |
| run-away level 6.5 dB | 100 | 0.995 / 0.995 |
| run-away level 3 dB | 152 | 0.995 / 0.995 |
| run-away level 12 dB | 132 | 0.995 / 0.995 |
| no finish | 136 | 0.995 / 0.995 |
| finish on Tight only | 122 | 0.000 / 0.995 |
| finish also drops the curve blend to linear | 133 | 0.096 / 0.096 |
| finish only from the sample after the opening sample | 45 | 0.163 / 0.163 |
| ramp cap 2 ms | 168 | 0.677 / 0.677 |
| ramp = 0.5 x sinceLow with no + 1 sample | 37 | 0.086 / 0.086 |
| ramp = 0.6 x sinceLow + 1 | 122 | 0.160 / 0.160 |

The unaltered reference flags 0 of 1 728. A run-away level wrong by half a dB is flagged in over a third of the cases,
and a missing finish in 136 of 288.

## Q2. Picks

(Measured, `q2_picks.py 4620`, output `out_q2.txt`.) One pick per render from a closed gate, 48 kHz, threshold -40,
Tight and Smooth at their defaults.

- Picks: a sum-of-partials pluck with an exponential-approach attack of 0.05 to 5 ms and an optional broadband snap,
  or a Karplus-Strong string. Notes 41.2, 82.4, 110, 196, 330, 659 and 1319 Hz. Peak reading 1, 2, ... 10, 12, 15,
  20, 25 or 30 dB over the threshold.
- Precursors, each reading 0.5 to 5.5 dB under the threshold for 1.5 to 150 ms before the pick: none, hiss, 50 or
  60 Hz buzz, a scrape, a sympathetic string, a finger squeak, a hand thump, a ghost note, crackle, the pick's own
  noise, a beating pair.
- Loss = energy of the pick's first 2 ms (and 5 ms) through a textbook gate, same opens and closes with a fixed
  0.2 ms linear rise, over the same through the engine. The window starts at the opening, or at the pick's onset when
  a precursor opened the gate up to 6 ms early.
- 4 595 picks judged of 4 620. A precursor had already opened the gate in 23. Two never opened.
- 3 703 picks had a ramp over 0.2 ms. The finish shortened 1 972 of them.

| Tight, 4 595 picks | A | B | C |
| --- | --- | --- | --- |
| Lose over 1 dB of the first 2 ms | 62.2 % | 15.8 % | 36.9 % |
| Lose over 3 dB of the first 2 ms | 48.7 % | 11.9 % | 25.7 % |
| Lose over 1 dB of the first 5 ms | 46.7 % | 11.6 % | 23.9 % |
| Lose over 3 dB of the first 5 ms | 28.0 % | 7.0 % | 12.5 % |

- **C worse than A: 0 picks**, first 2 ms and first 5 ms, at a 0.01 dB margin. The lowest C minus A gain at any
  sample of any render is 0. The claim holds.
- B worse than A: 553 picks (12.0 %), by a median 3.8 dB, worst 19.1 dB. On those picks C's median loss is 0.33 dB.
- C is worse than B on 55.3 % of picks. B fixed more of the weakness and broke other picks. C fixes less and breaks none.
- C better than A by over 1 dB: 1 361 picks.
- Hard picks after a precursor (attack under 0.5 ms, 12 dB or more over, 556): median loss A 5.48 dB, C 0.01 dB.
- Smooth: the same shares to 0.1 point. Its gain is still near the range after a precursor this short.
- At 96 kHz, a quarter of the set (1 153 picks, `R8_SR=96000 q2_picks.py 1155`, `out_q2_96k.txt`): C worse than A 0;
  all picks losing over 1 dB A 63.3 %, B 15.6 %, C 37.3 %; hard picks A 77.1 %, B 10.7 %, C 5.0 %. The same picture.
- By note, C's share losing over 1 dB runs from 29.8 % (1 319 Hz) to 43.4 % (110 Hz). By attack it runs from 31.3 %
  (under 0.3 ms) to 49.0 % (2.5 to 5 ms). With no precursor it is 13.3 % (slow attacks dwell in the band on their own). With one it runs from 26.4 % (thump) to 49.0 % (scrape).

The sizing by pick peak and by time to 6 dB over is in finding 1.

## Q3. What the finish costs

In finding 2. Scripts `q3_cost.py 40` (`out_q3.txt`), `q3_cost.py 300 beat,...` (`out_q3_chords.txt`), `q3b_beat.py`
(`out_q3b.txt`), `q3c_swells.py` (`out_q3c.txt`).

One correction I made on the way: my first chord run showed finish clicks in the beat families. They were the
render's own 50 ms fade-in, an onset and not a re-opening. Counting only re-openings after the first 0.2 s gives 0.

## Q4. Nothing else moved

**Confirmed, with the one difference the brief names.** (Measured, `q1_conformance.py 1728`.)

- Per-sample open flag, A against C: 0 samples differ in 1 728 cases, knob moves included.
- Tight gain, A against C: differs on 545 776 samples, every one inside an opening ramp (opening - 1 sample to
  opening + 5 ms + 4 samples). 0 outside.
- Smooth gain: differs outside opening ramps in 95 cases. In all 95 the gate was open inside the first 12 ms after
  the reset. In the other 1 633 cases: 0 samples outside ramps. That is the settle window now counting every sample.
  In those 95 cases C's Smooth gain is up to 0.69 (linear) under A's and the difference can last over a second.
- Lowest C minus A gain at any sample, cases not open inside 12 ms of the reset: -6e-8 (float rounding).
- No ramp was cut short by a close, in 7 735 openings. Inferred as well: the reading is a 12 ms windowed peak, so it
  stays over the open level for 12 ms after an opening, and no ramp is longer than 5 ms.
- The source agrees (inferred, `git diff 34dbcb3 360b499 -- src/dsp/Gate.cpp`): the diff is the step choice, the
  run-away level, the fast step and the place where the settle count is decremented.

## Q5. The guard rows

**How the rows were attacked.** I exported HEAD with `git archive` into `build-verify/r8-gate/mut/tree`, built the
gate-only bench there, and rebuilt it once per wrong rule with one string replaced in that copy's `Gate.cpp`
(`mut/make_mutants.py`). The unmutated rebuild equals C sample for sample on eight renders, and every wrong rule
differs from C (measured, `q5_mutants_live.py`, `out_q5_live.txt`). The repository's own `check_gate.py`, unmodified,
then ran against each binary through `$FXBENCH` (`q5_rows.py`, tables in `rows/`). The full acceptance against the
unmutated rebuild: 138 checks, 0 FAIL (measured, `rows/full_base.txt`).

MATRIX

**Row by row.**

| Row | Can it fail | Where the bar comes from | What it does not see |
| --- | --- | --- | --- |
| D3 pick after a precursor | Yes. D3_FAILS | The rule's arithmetic: the gain is at 0 dB within 0.2 ms + 2 samples of the reading running 6 dB over, so the first 2 ms hold at least the dry energy from there on, -0.01 dB for float rounding. | It is a conformance bar. It says the finish is there, not that picks keep their front. See below. |
| D4 swell ramp | Yes. D4_FAILS | The rule's 5 ms, less 2 ms of stated tolerance. The condition (reading over the close level for the 10 ms before the opening) is the rule's own premise for 5 ms. | The 2 ms tolerance is about fifty times the rounding it is stated for, so any cap from 3 ms up passes, and there is no upper bound in this row (I6 has one). 9 of 36 cells are outside the condition and printed as info, 82 to 440 Hz at 1 000 and 3 000 dB/s among them. |
| D5 chord re-opening | Yes. D5_FAILS | A stated tolerance, -60 dB, about 30 dB under the textbook's figure. Not derived and not the measured value: C measures -84.5 dB, 24.5 dB inside it. | It never runs the finish (crest 0.5 to 5 dB) and reads only the first millisecond after the opening. That hides nothing for chords: with the crest raised to 15 dB the finish still ran in 0 of 657 re-openings (measured, `q3b_beat.py`). It says nothing about finding 2. |
| I5 no click | Yes. I5_FAILS | The rule's arithmetic: 1.5 x the 0.2 ms slope is the steepest slope of a smoothstep advanced at the 0.2 ms rate, +1 %. C reaches 1.49 (measured, `q3_cost.py`), so the bar is tight. | It bounds the gain's slope. The click of finding 2 is the slope times a signal that is by then twice the threshold, and no row reads that. |
| G8 open | Yes, for a slower base ramp. G8_FAILS | The rule's arithmetic: -1 dB of a 0.2 ms linear rise, + 2 samples. C measures 0.167 ms against 0.220. | A burst from silence. The band dwell is a few samples, so the ramp is 0.2 ms with or without the finish. |
| X14 Smooth floor | Not by any opening rule. X14_FAILS | The rule's 100 dB/s slew over 10 ms and over one sample, +1 %. C measures 0.32 and 0.64 of the bars. | A closed-gate row. The gate does not open in it. |
| R3 glides, volume down | Not by any opening rule. R3_FAILS | The rule's arithmetic: hold + release / 3 + 2 samples = 20.04 ms. C measures 19.98 ms, three samples inside. | A closing row. |
| info D3 soft picks | Never enforced. | Prints the share of picks 1 to 5 dB over that lose over 3 dB: 12 to 20 %. | It stops at +5 dB, where the commit's stated cost stops. The cost goes on to about +12 dB (finding 1). |
| P1 equals the plain rule (not in the brief's list) | Yes. P1_FAILS | A stated tolerance, 0.05 dB, against the numpy port on 25 materials, five of them picks after precursors. | This is the row that pins the finish. If the port and the engine carried the same wrong number it would pass, which is why Q1 uses a reference written from the header. |
| D2 opening click (not in the brief's list) | Yes. D2_FAILS | A stated tolerance, -50 dB over 3 kHz on quadratic swells of 150 and 600 ms. C measures -72.6 dB. | Its swells never dwell in the band and then jump, so the finish never runs in it. |

**D3's filter.** D3 judges 192 of its 720 cells, the ones whose reading runs 6 dB over within 0.5 ms of reaching the
threshold. I recomputed all 720 with the row's own helpers (measured, `q5_d3.py`, `out_q5_d3.txt`, `out_q5_d3_A.txt`).

- The filter hides no failure of the row's bar. In C the 528 unjudged cells meet the same floor (worst shortfall
  -0.00 dB). For 460 of them the floor is empty, because the reading is never 6 dB over inside the first 2 ms.
- The row can fail. Against A, 70 of the 192 judged cells fall short of the floor, by up to 21.8 dB.
- The filter does hide the outcome. Loss of the first 2 ms against the textbook gate, share of cells over 3 dB:

| D3's own cells | Cells | Judged | Lose over 3 dB, A | Lose over 3 dB, C |
| --- | --- | --- | --- | --- |
| picks at -12 dBFS (28 dB over) | 240 | 172 | 35 % | 8 % |
| picks 10 dB over | 240 | 20 | 28 % | 26 % |
| picks 6 dB over | 240 | 0 | 11 % | 11 % |
| judged cells | 192 | 192 | 36 % | 0 % |
| unjudged cells | 528 | 0 | 20 % | 20 % |

  The row's title names the +10 and +6 dB picks. It judges 20 of the 480, and on the other 460 C does what A did.
  Its printed line gives the textbook loss for the judged cells only (worst 1.52 dB). E1 is judged in 10 of 180 cells.
  These shares use the row's window, which starts at the pick's onset and so can hold only the first fraction of a
  millisecond after a late opening. My own picks, with the window at the opening, are in finding 1.

**What is missing.** A row on a dull attack after a dwell in the band (finding 2): none of D2, D5, I5 or X16 has
that shape. X16 covers the opening from silence and prints -30.8 dB at 82 Hz by its own measure
(`rows/full_base.txt`). By mine, A's worst opening from silence is -30.8 dB and C's finish reaches -27.7 dB
(measured, `q3_cost.py`).

## Not checked

- Any real guitar or bass recording. There is none in the repository.
- Listening. Every audibility statement is a guess. The files in `listen/` are for a human.
- Picks and clicks at rates other than 48 and 96 kHz, and at thresholds other than -40 and -70. Conformance at four rates and over the whole threshold range is measured, so the rule is the same there.
- Oversized blocks. Two spot checks only on the paths around the ramp: stereo input through the deferred path with
  curve delays of 0, 37 and 2 048 samples equals the mono trace exactly on one render (measured, `s2_deferred.py`),
  and one Tight to Smooth switch in the block where a finish starts equals Tight before it, Smooth from 0.23 ms after
  it, and stays between the two (measured, `s3_modeswitch.py`).
- Classic mode.
- The AU plugin. Not installed, not validated.
- The row groups A, B, C, E, F, T and the common checks against the wrong-rule binaries. The full acceptance was run
  once, against the unmutated rebuild.
- Whether binary A was built from `34dbcb3`. Its behaviour matches my model of that source.
- CPU cost of the extra compare per sample.

## Scripts

All in `build-verify/r8-gate/`, run with `/usr/bin/python3`.

| Script | What it prints |
| --- | --- |
| `ref8.c`, `r8lib.py` | own reference and runner |
| `s0_sanity.py`, `s2_deferred.py`, `s3_modeswitch.py` | A, B and C against their models on one render; the deferred path; a type switch in a ramp |
| `q1_conformance.py N [--power=...]` | Q1 and Q4 |
| `q2_picks.py N`, `q2b_tables.py` | Q2, finding 1 |
| `q3_cost.py K [families]`, `q3b_beat.py`, `q3c_swells.py`, `q3d_dull.py`, `q3e_tables.py`, `q3f_trace.py`, `q3g_pops.py` | Q3, finding 2 |
| `q5_d3.py`, `q5_rows.py`, `q5_table.py`, `q5_mutants_live.py`, `mut/make_mutants.py` | Q5 |
| `q6_whatif.py` | the finish at other rates, reference only |
| `listen_make.py` | listening files |
