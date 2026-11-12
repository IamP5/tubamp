# Round 6: independent attack on the plain hysteresis gate (Tight and Smooth) at 34dbcb3

Verifier's own reference, own signals, own measures. Engine under test is the bench binary built from `34dbcb3`,
copied to `build-verify/r6-gate/bin/tubamp_fxbench`, sha1 `36078be498067dcb4d6b7d145ee92140ca40ea37`.
Every signal is synthesized. The repository holds no real guitar recording, so nothing here was checked on one.
Nothing under `src/`, `tools/`, `tests/`, `docs/` or `ui/` was edited. Nothing was committed.

Each figure is labelled **measured** (with the script that printed it), **inferred** (from the code, with lines) or
**guess**. Scripts live in `build-verify/r6-gate/`. Their printed output is kept beside them as `out_*.txt`.

## Verdict

| Type, at its defaults | Accept as a plain gate? | What decides it |
| --- | --- | --- |
| Tight (hold 10, release 30) | **YES** | The engine is the header's rule to 0.0000 dB and to the sample on 2 400 cases (measured). No stuck state, no history, no click outside an opening. One behaviour is worse than a textbook plain gate (finding 1) and it softens a pick, it does not drop or mute one. |
| Smooth (hold 40, release 80) | **YES** | Same state machine, same agreement. Closed, it follows its 3:1 law within 0.2 dB on steady floors and moves at most 1 dB per 10 ms (measured). It is a gate with a soft floor, not an expander for soft playing (finding 4). Hold 40 is defensible but its recorded reason is wrong on deep floors (finding 3). |

The YES is for the DSP as a plain gate. It does not cover the two mirrors that disagree with the code at this commit
(finding 2) or the check script's holes (section "The check script").

Residual risk I cannot remove. Finding 1's incidence on real picks depends on how much noise precedes a real pick
inside the 6 dB band. I could only construct that. Three files to listen to are in `build-verify/r6-gate/listen/`.

## Findings, ranked by how audible

### 1. A pick opens with the slow rise when anything sat in the band before it (Tight and Smooth)

**What happens.** The rise lasts half the time since the level was last under the close level, from 0.2 ms up to a
5 ms smoothstep. The rule means this for swells. It also fires when a steady floor, a scrape or a ringing tail has
lifted the level into the band (threshold - 6 dB up to the threshold) before a hard pick. The pick then fades in
over about 4 ms instead of 0.2 ms. A textbook plain gate has one attack time and does not do this. My reference of
the header reproduces it, so it is the rule as written, not an implementation slip (inferred, `Gate.cpp` 405 and 415).

**Size on one pick** (measured, `q2c_low_listen.py`). A2 at -12 dBFS, threshold -40, after 10 ms of a scrape at -43 dBFS.

| Energy against dry, dB, over the first | 2 ms | 5 ms | 10 ms | 20 ms | 50 ms | rise to -1 dB |
| --- | --- | --- | --- | --- | --- | --- |
| Engine, Tight | -21.2 | -12.9 | -2.1 | -1.0 | -0.5 | 3.53 ms |
| Fixed 0.2 ms rise (textbook) | -0.0 | -0.0 | -0.0 | -0.0 | -0.0 | 0.19 ms |
| Fixed 1 ms rise (computed) | -3.6 | -3.2 | -1.0 | -0.5 | -0.3 | 0.90 ms |

The attack peak inside the first 3 ms is down 26 dB in the worst cases below. Whether that is heard as a dull pick
is a guess. I cannot listen. The A/B files are `listen/soft_attack_dry.wav`, `soft_attack_engine_tight.wav` and
`soft_attack_fixed_0p2ms_rise.wav`.

**How often** (measured, `q2b_attack.py`, 24 picked notes x 4 seeds per cell, threshold -40, notes at -12 dBFS).

| Tight, steady hiss, median reading under the threshold | 3 dB | 5 dB | 6 dB | 7 dB | 8 dB | 9 dB and more |
| --- | --- | --- | --- | --- | --- | --- |
| Picks that open with a rise over 1 ms, of 96 | 4 % | 26 % | 40 % | 8 % | 1 % | 0 % |
| Gate open, share of the time (the known cost) | 100 % | 94 % | 67 % | 59 % | 58 % | 57 % |

- Hum plus hiss gives 4, 3, 38, 2, 0, 0 %. Worst loss of the first 2 ms is -29.3 dB (textbook -0.6 dB). First 5 ms -16 to -19.5 dB.
- Smooth on the same floors loses at most 2.7 dB. Its gain is already near 0 dB when the floor is in the band.
- A scrape whose peak is inside the band, 5 ms before the pick. 81 to 100 % of picks open slowly, first 2 ms -19 dB.
  With 10 to 40 ms of lead, 42 to 100 %, -27 to -29 dB. Smooth is the same here (-25 to -27 dB). A scrape under the
  band (-50 dBFS) changes nothing.
- Picks over a ringing close-interval chord. 4 of 576 notes (Tight), worst -30.8 dB.
- Across my played material at the defaults (`q2_playing.py`, 960 cases, 11 563 notes reading over the threshold per
  type). Tight plays 90 notes (0.8 %) with the first 2 ms more than 3 dB under the textbook gate. By threshold over the
  floor reading, +4 dB 1.9 %, +8 dB 0.8 %, +14 dB 0.4 %, +24 dB 0. Smooth 8 notes.

**Why the rows miss it.** D1, X15 and A3 use floors under the close level (audit subagent, read from the script).
P1 compares the engine with a port that shares the rule.

**What would remove it** (not built). Count the climb only while the level is rising, or cap it by the time since the
level left its last steady reading. The swell rows would still pass if the swell itself is what is timed.

### 2. Two mirrors of the gate disagree with the code at this commit

- `ui/src/features/panel/typeDefaults.ts` at `34dbcb3` has `hold: [6, 10, 10]`. `Gate.h` says Smooth's default is 40
  and the header says the editor applies the per-type value when the user picks a type. So at the merged commit a
  user who picks Smooth gets hold 10. The working tree carries an uncommitted edit to `[6, 10, 40]`, not mine
  (measured, `git diff`).
- `docs/FX-ENGINES.md` §4.1.5 (line 744) and the type table (line 610) still describe closing rule version 5.1 with its
  deadline, dip memory and reopening test. None of that exists in the code (measured, read).

Neither changes a sample. Both change what a user or the next engineer believes they have.

### 3. Hold 40 for Smooth: sound for sustain, but its recorded reason is false on deep floors

The trail (`.audit/fx-implementation.tsv`, 11:33:46Z) gives the reason. Smooth "never closes palm-mute gaps at any hold",
so a longer hold "costs it almost nothing". That holds only while the floor reads less than about 16 dB under the
threshold, where the expander's own target is shallower than -20 dB.

Measured, `q4c_gapdepth.py`. E2 chugs, 70 ms notes, 6 ms hand stop, threshold -40, 46 gaps per cell. Median gain at
the end of the gap in dB, hold 10 / 20 / 40.

| Floor reading under the threshold | 60 ms gap | 80 ms gap | 100 ms gap | Expander target |
| --- | --- | --- | --- | --- |
| 8 dB | -3.6 / -3.8 / 0.0 | -4.1 / -4.0 / -4.3 | -4.0 / -4.0 / -4.1 | -4 |
| 12 dB | -11.4 / -11.9 / -0.8 | -12.0 / -12.1 / -12.3 | -11.9 / -12.0 / -12.0 | -12 |
| 22 dB | -23.3 / -15.8 / -0.8 | -31.9 / -30.8 / -15.8 | -31.8 / -32.0 / -30.8 | -32 |
| 30 dB | -23.3 / -15.8 / -0.8 | -38.3 / -30.8 / -15.8 | -47.7 / -45.8 / -30.8 | -48 |

With the floor 22 dB or more under the threshold, hold 10 ends 46 of 46 gaps of 60 ms under -20 dB. Hold 40 ends none.

What hold 40 buys (measured, `q4b_hold.py`, 312 cases of close-interval pairs and chords with deep beats ringing through
the threshold, t60 3 and 8 s, threshold 3 dB over to 16 dB under their first reading).

| | Cases that re-open | Re-openings | Dips (under -3 dB, back over -1 dB) | Deepest dip |
| --- | --- | --- | --- | --- |
| Tight, hold 10 | 17 | 19 | 19 | mute |
| Smooth, hold 10 | 17 | 19 | 59 | -9.8 dB |
| Smooth, hold 20 | 2 | 2 | 51 | -9.0 dB |
| Smooth, hold 40 | 0 | 0 | 49 | -5.7 dB |
| Smooth, hold 80 | 0 | 0 | 13 | -5.0 dB |

**Verdict on the default.** Hold 40 is sound for what Smooth is for. It removes every re-opening on my beating
material and halves the deepest dip. What it removes was small to begin with (dips under 10 dB, re-opening jumps of
0.5 dB median, 3.2 dB worst in the listed cases). Its cost is real once the floor is 16 dB or more under the
threshold. On my material hold 20 keeps 15 of the 17 cases clean and still closes 80 ms gaps. I would not block on 40.
The trail's sentence should be corrected.

### 4. Smooth is a gate with a soft floor, not an expander for soft playing

Measured, `q4_smooth.py` part D. After 1.5 s closed over a floor 30 dB under the threshold, an A2 whose peak reads
1 dB under the threshold plays at -45.9 dB after 20 ms, -37.9 dB after 100 ms and -17.9 dB after 300 ms. Its first
300 ms are 31.7 dB under dry. The 3:1 law would pass it at 0 dB. The same note 1 dB over the threshold plays in full
from the first 0.2 ms. The cause is the 1 dB per 10 ms limit and the 200 ms floor estimate, which the header states.

This is not worse than a plain gate. Tight mutes those notes. It matters only if Smooth is described as an expander
that follows playing dynamics. It also means that after a soft close the gain falls at 100 dB per second, not at the
release rate. A note that dies through the close level and is then stopped by the hand takes 96 to 166 ms to reach
-20 dB (part E). That is floor noise fading, at -75 dBFS and under in my case.

### 5. Low tunings: what the 75 Hz filter and the 12 ms window cost

Measured, `q2b_attack.py` (last table) and `q2d_pulse.py`.

- My string model reads 3.4 dB low at B0 (30.87 Hz), 1.8 dB at E1 and F#1, 2.2 to 2.6 dB from A1 to E2, 0.6 dB at A2,
  against the unfiltered peak. A decaying note lets go at most 0.07 s earlier than with the filter off (of 1.1 s).
  A darker bass reads lower. A pure 30.87 Hz sine reads 30.8 dB low through a 4th-order 75 Hz filter (inferred).
- A pulse-like B0 (harmonics in phase, crest gap of one period, 32.4 ms) flutters at Tight's defaults while it dies
  through the band. 3 re-openings over 0.12 s, down to -19 dB. With the filter off, 10 over 0.36 s. Hold 25 gives 0.
  Smooth gives 0. From E1 (41.2 Hz) up, 0 in every case. The engine equals my reference here, so this is the plain
  rule with a 12 ms window and a 10 ms hold, not a fault. It is the size of that cost for a 5-string bass.

### 6. Smaller things

- **Settle counter** (measured, `q4_smooth.py` part F). After a reset, Smooth takes the level unsteadied for one
  window. The counter runs only while the gate is closed (`Gate.cpp` 445 to 448). If a note starts inside 12 ms of
  the reset, the remainder is spent at the first close. Difference to the same note after silence is 0.64 to 0.89 dB
  for 9 to 81 ms, no gain step. Cosmetic, but it is history the header says does not exist.
- **Opening is one sample longer than "half the climb"** (measured, `q1_conformance.py`). `0.5 x sinceLow + 1`.
  Linear gain differs from my reading of the header by up to 0.090 inside a rise at 44.1 kHz. With the extra sample in
  my reference the difference is 0.00001.
- **A threshold dropped under a sounding note opens in 0.2 ms** (measured, `q3a_knobs.py`, step 0.104 per sample at
  48 kHz). Any gate with a 0.2 ms attack does. A ramped threshold through a beating chord re-opens several times.
- **Stereo** (measured, `q3b_switch.py`). The reading is the mean. Left only reads 6 dB lower (open 0.24 s against
  0.40 s). Left = -right never opens. Both channels always get the same gain. Unchanged from round 5.
- **A dropout in the floor** (100 ms of NaN, read as silence) leaves Smooth's gain off its steady value for 1.35 s
  while the estimate recovers (measured, `q3b_switch.py`). It is floor noise fading back in.

## The seven questions

### 1. Is it a correct plain gate? Yes.

My reference is `ref.c` (state machine, written from the header) plus `r6lib.level` (scipy Butterworth and a
brute-force-checked trailing maximum). Smooth's floor estimate is not in the header. I took it from the constant's
comment in `Gate.cpp` 48 to 50, so Smooth's closed gain is a port and not independent. Its independent checks are in
question 4.

| Run (all measured) | Cases | Worst gain error outside openings | Opens and closes off by more than 0 samples |
| --- | --- | --- | --- |
| `q1_conformance.py 1440`. Random knobs over the full ranges, 44.1 / 48 / 96 / 192 kHz, blocks 1, 2, 7, 31, 64, 127, 256, 480, 512, 1000, 2048, 4096, seven kinds of material scaled to the threshold | 1 440 x 2 types, 3 653 openings | 0.0000 dB Tight, 0.0000 dB Smooth | 0 of 1 306 cases, 0 count mismatches |
| The same at 22.05 / 88.2 / 176.4 / 384 kHz (`R6_RATES=...`) | 192 x 2 | 0.0000 dB | 0 |
| `q2_playing.py`. 15 kinds of playing x 4 floors x 4 thresholds, defaults | 960 | 0.0000 dB | not counted |
| `q5_numerics.py`. 240 s at 48 and 44.1 kHz, 60 s at 192 kHz | 8 | 0.00001 dB | not counted |
| `q3a_knobs.py`. Steps and per-block ramps of threshold, hold, release, range | 32 with a reference | 0.0001 dB | not counted |

- Knob edges are in the grid. Threshold -100 and 0, hold 0 and 250, release 5 and 500, range -90, -89.9 and 0,
  sc_hpf 20, 20.5, 21 and 300.
- The comparison covers every sample where either gain is over -89.9 dB.
- The comparison has power. With my reference at 6.1 dB hysteresis, 50 of 217 cases move an event by more than 2
  samples and the worst error is 180 dB (`q1_conformance.py 240 --power`).
- Release slope at 192 kHz with release 500 is 12.00000 dB per 100 ms. Mute arrives 0.77157 s after the last sample
  (rule 0.77158) (`q2d_pulse.py`).
- Changing block sizes inside one render were not exercised. The bench takes one size. Only the short last block
  changes. `PeakGate::advance` is per sample and reads no block length (inferred, lines 389 to 461).

### 2. Is it ever worse than a plain reference? In one way, finding 1.

- Notes reading over the threshold that did not open inside 20 ms. 0 of 11 563 per type (`q2_playing.py`).
- Gaps left open, extra flips, late closes, history. None. The engine's events are my reference's events to the
  sample in 3 653 openings, on chugs, bursts, wobbles, floors and swells.
- Sticking. After any switch storm, knob move or extreme input the gain equals a fresh run (question 3).
- Against a textbook gate with a fixed 0.2 ms rise on the same level, the only difference is the opening time.
  Tight spends 1 311 ms under -6 dB while the level reads over the threshold, the textbook gate 452 ms, summed over
  480 cases (2.7 ms against 0.9 ms per case).

The accepted costs, sized on my material. Tight re-opens in 17 of 312 beating-chord cases (19 times, to mute)
(`q4b_hold.py`). On decaying single notes and ordinary chords, 0 of 144 (`q4_smooth.py` part B). Palm-mute gaps at
Tight's defaults end at -22 dB (40 ms), -62 dB (60 ms), mute from 80 ms (`q4_smooth.py` part C). A hiss floor 5 or
6 dB under the threshold holds the gate open 94 or 67 % of the time (`q2b_attack.py`).

### 3. Transitions. Clean.

All measured, `q3a_knobs.py` and `q3b_switch.py`, 48 kHz.

- **Knob moves on a held chord and on a closed floor**, each knob, steps and ramps, both types. 44 rows. Gain 2.9 s and
  5.9 s in equals the reference in every row with one. The biggest step outside an opening is the range moving
  0 to -90 on a closed floor, 0.0213 in its first sample, then a 10 ms one-pole in dB. sc_hpf moves have no
  per-sample reference (the filter's state moves with it). They show no step and end on the right gain.
- **Type switches**, all six ordered pairs, there and back, on a held chord, a floor and a riff. Gain afterwards
  equals the run that was always in that type (0.000 dB). Crossfade step 0.00007 per sample between mute and -24 dB.
- **Switch storm**, a type change every 7 ms for 1 s. 100 % of samples identical to the plain run from 0.2 s after it.
- **Re-prepare mid-note.** Gain 1 to 0 in one sample and back inside 4.7 ms. A reset must cut. The processor resets
  the gate when it is switched on and fades its bypass itself (`PluginProcessor.cpp` 1687 to 1702, read, not run).
- **Silence then signal.** 30 s of zeros, then a pluck. Tight's trace is identical to the pluck alone. Smooth differs
  by the settle counter (finding 6).
- **Hot and quiet.** Peaks from +125 dBFS (clamped at 1e6) to -118 dBFS, threshold moved with them. 0.0000 dB.
- **Non-finite.** NaN, +Inf, -Inf, 100 ms of NaN, 1e30. The gain is finite in every case. The output is non-finite
  only on the input's own bad samples. The state is back on the signal afterwards.
- **Deferred curve** (the bench's stand-in for the processor's path). Output equals the direct output delayed by N,
  bit for bit, N from 0 to 65 535, blocks 64 and 512.

### 4. Smooth. Sane, with finding 4 as its character.

Measured, `q4_smooth.py`.

- **Steady floors, 20 s, threshold 10, 20, 30 dB over the reading.** Median gain is within 0.2 dB of
  2 x (reading - close level). The 5 to 95 % spread is 1.0 dB on hiss and pink, 0.2 dB on buzz, 1.3 dB on crackle.
  Fastest closed move 19 to 88 dB per second (limit 100). No opening.
- **A floor that breathes 6 dB at 0.3 Hz** comes out with an 11 dB spread. That is the 3:1 law.
- **Pumping on riffs.** Gaps over 100 ms fall to the target and each chug opens from there in 0.2 ms. Gaps under
  about 60 ms are not touched at hold 40.
- **Opening from a partly closed state.** A linear rise from the gain it had. Covered by the conformance runs.
- **Recovery.** Slow by design, 1 dB per 10 ms in both directions once closed (finding 4).
- **Hold 40 against 10.** Finding 3.

### 5. Numerics and cost. Clean.

Measured, `q5_numerics.py` and `q2d_pulse.py`. Load average was 6.4 to 9.5 on 12 cores throughout, so the timings
carry that noise. Each is the minimum of 3 runs of the bench's own minimum of 5.

- **Drift.** 240 s renders agree with the reference to 0.00001 dB, the same in the last 30 s.
- **Denormals.** With and without flush-to-zero on silence, an impulse then silence, a sine at -140 dBFS and noise at
  -150 dBFS. 20.5 to 24.0 ns against 20.5 to 24.2 ns per frame. No slowdown.
- **`--reset-repeat` and `--rt-check`** exit 0 for both types at blocks 64 and 37 with knob events.
- **CPU.** Tight 19.3, Smooth 19.6, Classic 18.9 ns per stereo frame at 48 kHz block 64, about 0.09 % of real time.
  0.38 % at 192 kHz. The ratio to Classic (1.02 to 1.09, 1.31 at block 1) says little. Every detector runs in every
  type (`Gate.cpp` 749 to 751), so the engine in Classic already pays for Tight and Smooth. Against the legacy gate
  in one batch, 11.1 ns legacy, 27.1 ns engine in Classic, 29.9 ns engine in Tight. The engine costs about 2.5 times
  the old gate and is still a tenth of a percent of a core.

### 6. The check script

Audited by a subagent (`build-verify/r6-gate/audit/`). It ran every row group except `common_checks` against the
pinned binary. 108 rows, 0 FAIL. I confirmed by my own reading or run the items marked (mine).

**P1 is strong.** 22 mutants of the port were tried. All 22 fail P1 (`audit/p1_mut.py`, `p1_mut.out`, table read by
me). Examples. Hysteresis 5 or 7 dB fails 66 or 64 of 68 cases. Hold +1 ms 60. Window 11 or 13 ms 60. Release 5 % slow
60, worst 4.0 dB. Smooth slope 2.5:1 61. Smooth time constant cap 0.1 s 48. Thin spots. A count that does not restart
in the band is caught by only 8 to 13 cases. A close one sample early fails 3 cases with Tight at 0.053 dB against the
0.05 dB limit.

**What P1 cannot see.**
- Anything the port shares with the engine. Finding 1 is the case.
- Range smoothing. The port has none and every case holds the range. Only one row covers it.
- sc_hpf other than 75. Hold other than 0, 10, 50, so Smooth's default 40 is absent. Stereo. Knob moves, type
  switches, a mid-stream reset. My runs cover these.
- Smooth's opens and closes. Events are read from Tight's gain only. Smooth is covered by the gain comparison.

**Rows that cannot fail or prove less than their name.**
- **P4 gap latch** compares the engine with itself, `r[0] <= r[1]` (mine, lines 1999 to 2016). A gate that closes no
  gap scores 30 against 30 and passes.
- **G10** no longer tests the expander. Tight with hold 40 and release 80 gives the same -13.70 dB as Smooth.
- **I3 (Smooth)** uses a bound of 1.5 x (peak - threshold) + 12 dB where the rule gives 2 x. A 2.5:1 slope passes.
  The bound is 0 dB within 8 dB of the threshold.
- **I5** reads both slopes from Tight's trace even in Smooth cases.
- **Every I row** is an upper bound. A gate that never opens passes all four.
- **A5** enforces the median only. Its printed worst gap is 0.0 dB in four of six cells.
- **TD1** allows the stop bound plus 150 ms. A 148 ms hang passes.
- **G4 (HPF 75), G5, G6, G7, G8, G9, X2, G18** have bars 12 ms to 5 times looser than the rule's arithmetic.

**Bars on a measured value.** D1, -1.33 dB against a bar of -1.5. A4, 51.5 ms against 60. X14, 0.33 against 0.5 where
the rule allows 1.0. **TC1 is fitted to its seeds.** In-range values (60 ms notes, 50 ms gaps, floor 40 dB under,
threshold +8) give -0.6 dB at the gap's end, 0 of 6 gaps closed. The 78 enforced draws miss that corner. X13, R2, R3
and I4 follow from the rule. I4's label says "outside the band" but it enforces floors down to 4 dB under the threshold.

**Info rows.** On 75 signals from the info rows the engine equals the port, so they hide nothing the engine does
beyond the rule. They do hide the rule's own cost from the pass count. B1, 83 of 382 cases dip to mute for up to
145 ms, 7 with a re-opening click of up to +6.6 dB. C beats, 46 of 72. TA1, 50 of 256 Tight cases chatter. I4 near
the floor, 3 of 60 stops never close. Enforced before this commit and info now are X10, X11, X12, B1, B2, C, R5, TA1
and TA2. X12 is clean (0 of 144) and could stay enforced.

**Other defects.** `--rows=Q` runs 0 checks and exits 0 (mine, run). Under any `--rows` the G and X1 to X9 rows and
the common checks cannot be selected. "G1" names two rows. D2 skips silently when the gate never opens (46 of 48
cells judged). `python3 -I`, as the docstring says, fails under `/usr/bin/python3` here. Seeds are all fixed.

### 7. Verdict

Stated at the top. Ranked by audibility. 1 slow opening after an in-band precursor. 2 the stale UI default and doc.
3 the hold 40 reason. 4 Smooth's slow recovery under the threshold. 5 low-tuning costs. 6 the settle counter and the
other small things.

## What the earlier rounds' classes look like now

| Round 5 finding | Now |
| --- | --- |
| 1 stuck gain after a return inside the deadline | Gone. Gain is 0.0 dB after every threshold visit (`q3a_knobs.py`). Nothing lowers the gain while open (3 653 openings). |
| 2 palm-mute gap latch | Gone. No memory. Engine events equal a memoryless reference on 1 216 chug openings. |
| 3 chord chatter beyond a plain gate | Equal to a plain gate by construction. 17 of 312 cases on my beating material. |
| 4 dead release knob, lost hysteresis | Gone. Release 5 to 500 matches the reference on decays. |
| 5 soft note with a hard attack cut to a tick | Plain behaviour, equal to the reference. 360 such notes read over the threshold, none unopened. 6 open slowly (finding 1). The rest lose what the textbook gate loses (worst -4.5 dB in the first 2 ms). |
| 6 stereo mean | Unchanged. |

## Not checked

- Real recordings. None exist in the repository.
- Listening. Every audibility statement is a figure or a guess.
- Block sizes that change inside one render, beyond the short last block.
- The gate inside the plugin chain. `tubamp_fxrender` renders only reverb and delay. The earlier `dynproc` harness is
  built from an older tree and I did not rebuild it. Checked instead. The bench's deferred path, bit-exact.
- Blocks larger than the prepared maximum (ungated by design), meters, and the Classic type beyond switches.
- sc_hpf moves against a per-sample reference.
- The audit's `common_checks` rows were classified by reading, not run.
- The AU plugin was not installed or validated, as instructed.
- CPU on a quiet machine.

## Files

All under `build-verify/r6-gate/` (gitignored). Run each with `/usr/bin/python3` from that folder.

| File | What it is |
| --- | --- |
| `bin/tubamp_fxbench` | the binary under test, sha1 above |
| `ref.c`, `libref.dylib`, `r6lib.py` | own reference (`clang -O2 -shared -o libref.dylib ref.c`), runner, synthesis, measures |
| `s0_sanity.py` | window alignment, event timing of the bench |
| `q1_conformance.py N [--power]`, `out_q1.txt`, `out_q1_rates.txt` | question 1 |
| `q2_playing.py`, `q2b_attack.py`, `q2c_low_listen.py`, `q2d_pulse.py` and their `out_*.txt` | question 2, findings 1 and 5 |
| `q3a_knobs.py`, `q3b_switch.py` and their `out_*.txt` | question 3 |
| `q4_smooth.py`, `q4b_hold.py`, `q4c_gapdepth.py` and their `out_*.txt` | question 4, findings 3 and 4 |
| `q5_numerics.py`, `out_q5.txt` | question 5 |
| `listen/soft_attack_*.wav` | finding 1, three files, +12 dB of gain |
| `audit/` | the check-script audit. `p1_mut.py --jobs=5`, `run_rows.py`, `i_rows.py`, `info_vs_port.py`, probes and outputs |
