# Round 7: independent attack on the opening-ramp change and the repaired check rows (4f4374d)

Scope is one change to the gate (Tight and Smooth) and the repairs to `tools/fxbench/check_gate.py`, both in `4f4374d`.
Binary with the change is `build-verify/r7-gate/bin/tubamp_fxbench_new`, sha1 `6cbbd6ea01bd6f99836cc6167f8c6048a15cc02f`.
Binary before it is `build-verify/r7-gate/bin/tubamp_fxbench_old`, sha1 `36078be498067dcb4d6b7d145ee92140ca40ea37`.
Every signal is synthesized. The repository holds no real guitar recording, so nothing here was checked on one.
Nothing under `src/`, `tools/`, `tests/`, `docs/` or `ui/` was edited by me. Nothing was committed.

Each figure is **measured** (with the script that printed it), **inferred** (from code or arithmetic) or a **guess**.
Scripts are in `build-verify/r7-gate/`. Their output is beside them as `out_*.txt`. Load average was 4 to 11 on 12 cores.

## Verdict

**Ship the change: YES, as an improvement. It does not close round 6's finding 1.**

It is better than the rule in the tree on every pooled measure I took, the engine is the stated rule to the sample, and
no opening or closing moved. But three statements in the commit message are false as written, and the two new rows that
guard the change (D3, D4) are placed where the new rule passes and cannot see where it fails.

| What decides it | Before | After | Source |
| --- | --- | --- | --- |
| Picks that lose over 3 dB of their first 2 ms against a textbook 0.2 ms rise, Tight, 6 433 picks | 18.2 % | 5.6 % | measured, `q1_picks.py` |
| The same over the first 5 ms | 9.3 % | 3.2 % | measured, `q1_picks.py` |
| Picks made worse by more than 0.5 dB | | 333 of 6 433 (5.2 %), worst +20.7 dB in the first 2 ms | measured, `q1_picks.py` |
| Pick after a scrape 0.5 dB under the threshold, over 3 dB lost in 2 ms | 96 % | 79 % | measured, `q1_picks.py` |
| Openings on band-limited rises that take under 0.5 ms | 16 % | 33 % | measured, `q2_swells.py` |
| Engine against my reference of the new rule | | 1 039 of 1 040 cases within 0.00002 dB and 0 samples; the other is a level tie | measured, `q3_conformance.py`, `q3b_ties.py` |
| Opens and closes, old against new, per sample | | 0 differences in 1 040 cases | measured, `q3_conformance.py` |

The pick shares come from a mix I built to provoke the weakness. They are not the incidence on real playing.

## Findings, ranked by how audible

### 1. A pick still opens slowly when its level dwells in the last 1 dB, and now it is slower than before there

**What happens.** The ramp is three times the time since the level was last under threshold minus 1 dB. Anything that
holds the level in that 1 dB for 1.7 ms gives the full 5 ms smoothstep. Before, the same dwell alone gave a sixth of that.
So the weakness is gone 1 to 6 dB under the threshold and is six times stronger in the top 1 dB (inferred from
`Gate.cpp` 409 and 426, and `ramp_new > ramp_old` exactly when `sinceNear > sinceLow / 6`).

**Size** (all measured, `q1_picks.py`, Tight, threshold -40, own picks with a finite attack and own pick noise).

| Case | Picks | Lose over 3 dB of the first 2 ms, old to new | Made worse by over 0.5 dB |
| --- | --- | --- | --- |
| Scrape peak 5 dB under the threshold | 192 | 71 % to 0 % | 0 |
| Scrape peak 3 dB under | 192 | 88 % to 0 % | 0 |
| Scrape peak 1.5 dB under | 192 | 95 % to 0.5 % | 0 |
| Scrape peak 0.5 dB under | 192 | 96 % to 79 % | 89, worst +20.7 dB |
| Scrape peak 1.5 dB over (it opens the gate itself) | 192 | 2.1 % to 4.7 % | 12, worst +14.0 dB |
| Hiss floor 5 / 6 / 7 dB under (over 1 dB lost) | 48 each | 23 / 42 / 17 % to 0 / 4 / 4 % | 0 / 1 / 2 |
| Soft picks, peak 1 to 9 dB over the threshold | 285 | 39 % to 24 % | 60, worst +16.3 dB |
| Attack of 1 to 15 ms, no pick noise | 360 | 16 % to 11 % | 46, worst +15.8 dB |
| Own pick noise 12 to 30 dB under the note, 1.5 or 4 ms early | 576 | 19 % to 7 % | 54, worst +20.0 dB |
| Hammer-ons and tapped notes, attack 3 to 10 ms | 216 | 8.3 % to 6.9 % (5 ms window 1.9 % to 5.1 %) | 16, worst +19.9 dB |
| Low and high notes, B0 to E6 | 432 | 4.2 % to 1.9 % | 13, worst +16.5 dB |
| Picks over a ringing chord's tail | 288 | 0.3 % to 1.4 % | 6, worst +20.2 dB |
| Two beating open strings 0.5 to 7 dB under | 240 | 15 % to 3.8 % | 6 |
| Pick 5 to 80 ms after a hand stop, while releasing | 144 | 0 to 0 | 0 |
| Double and tremolo picking, 40 to 130 ms apart | 436 | 0 to 0 | 0 |

- Smooth is the same within 0.5 points in every family except steady floors and the beating open strings. There its
  gain is already near 0 dB and it loses almost nothing in either binary (0.8 % to 0.0 % on the open strings).
- One regression is not exotic. A loud pick (-12 dBFS) whose pick noise sits 24 dB under it crosses the threshold on
  the noise. 4 of 24 such picks, in each type, go from a 0.40 ms ramp to 1.58 ms and lose 4.1 dB of the first 2 ms, from 0.
- On the families with no built precursor (pick noise, hammer, low and high, soft, slow attack, releasing, double;
  2 449 picks) the share losing over 3 dB of the first 2 ms goes 12.9 % to 7.0 % (computed from the family table).
- The commit's sentence "a floor, scrape or tail in the band no longer fades a pick in" is true from 1.5 dB under.
  It is false in the top 1 dB and for the pick's own noise and attack.
- Whether a pick that loses 15 to 20 dB of its first 2 ms sounds dull is a guess. I cannot listen. A/B files are in
  `build-verify/r7-gate/listen/` (dry, old, new for four cases).

**A cheap repair, measured on my reference only, not built.** Take the shorter of the two timings,
`clamp(min(0.5 x sinceLow, 3 x sinceNear) + 1)`. On the same 6 433 picks it leaves 4.0 % losing over 3 dB (new 5.5 %,
old 18.1 %) and makes 0 picks worse than the old rule (new 334) (`q1b_minrule.py`). On rises it opens under 1 ms in
26 % of swells against 24 % now, and 83 % of finger attacks against 76 % (`q2c_minrule.py`). Its click cost was not measured.

### 2. Swells open faster than before, and on low strings almost always

**What happens.** The level is a 12 ms windowed peak, so it climbs in steps at each crest. A step of more than 1 dB can
jump the last-dB band and the ramp is 0.2 ms. And a waveform with one crest per period, on a note whose period is longer
than the window, lets the level dip between crests. A dip of 1 dB now restarts the count. Before it took 6 dB.

**Size** (measured, `q2_swells.py` and `q2b_lownotes.py`, Tight, band-limited material under 3 kHz, 1 712 openings).

| Material | Ramp under 1 ms, old to new | Click above 6 kHz against the signal at the opening, worst, old to new |
| --- | --- | --- |
| dB-linear swells 50 / 100 / 200 dB per second | 0 % to 4 / 4 / 9 % | -73 to -64 / -65 / -62 dB |
| 300 / 500 dB per second | 0 / 5 % to 15 / 28 % | -73 / -67 to -47 / -41 dB |
| 1 000 / 2 000 dB per second | 4 / 23 % to 43 / 64 % | -51 / -30 to -39 / -30 dB |
| Finger attacks 5 / 10 / 20 / 40 ms | 88 / 79 / 47 / 20 % to 90 / 86 / 72 / 55 % | -34 to -35 dB over all four |
| Volume-knob swells 0.1 / 0.3 / 0.8 s | 0 % to 48 / 19 / 8 % | -73 to -35 / -62 / -64 dB |
| Feedback and e-bow sines, 20 to 100 dB per second | 0 % to 0 % | unchanged, -87 dB |
| Beating chord's crest re-opening the gate | 0 % to 23 % | -93 to -49 dB |
| Tone creeping around the threshold | 0 % to 0 % | unchanged |
| Pulse-like E1 / B1 / D2, swells of 20 to 100 dB per second | 0 % to 75 / 100 / 100 % | -93 / -100 / -99 to -62 / -59 / -61 dB |
| The same on E2 (82 Hz) and A2 | 0 % to 0 % | unchanged |

- Over all 1 712 openings the ramp is shorter on 36 % and longer on 19 %. Ramps under 0.5 ms go 16 % to 33 %.
- The gain's fastest move is 0.104 per sample in both binaries (the 0.2 ms rise). What changed is how often it is used.
- The worst click over all material is unchanged at -30 dB against the signal. The old rule already gave 0.2 ms there.
- The output's largest move over 0.2 ms during the ramp is more than 6 dB over the waveform's own at threshold
  amplitude on 38 % of openings (old 32 %, textbook gate 50 %).
- **Audible?** Guess: slightly, and only on exposed clean swells on low strings and soft finger attacks. The click
  sits 35 to 60 dB under a signal that is itself at the threshold, and a 5 ms ramp was already shorter than half a
  period of those notes. A fixed 0.2 ms textbook gate does this on every opening.
- The commit's sentence "a steady climb gets the same ramp as before" holds only for climbs under 1 dB per crest on
  waveforms with a crest every half period.

### 3. The rows that guard the change cannot see findings 1 and 2

Detail in question 5. D3 has no precursor closer than 1.5 dB, no soft pick and no absolute bar. D4 uses odd harmonics
only (a crest every half period) from 82 Hz up, and excludes the swells that changed. No enforced row covers either finding.

### 4. Smaller things

- **Settle count.** Fixed. A note 0 to 3 ms after a reset differed from the same note after 2 s by 0.10 dB for 21 ms
  in the old binary and by 0.000 dB in the new (measured, `q4_settle.py`).
- **A level tie.** One case of 1 040 differs from my reference by 18 samples of open flag. The reference's level is
  within 4.6e-7 of the threshold there (float engine, double reference). The old binary does the same (`q3b_ties.py`).
- **`.audit/fx-implementation.tsv` and `docs/FX-ENGINES.md`** show as modified in `git status`. Not by me.

## The six questions

### 1. Is the weakness gone? No. It is about three times rarer and has moved.

Finding 1 has the table. Method. 282 scenarios in eleven families, 6 436 picks per type, 48 kHz, block 64. Each pick
runs through the old binary, the new binary and a textbook gate (my reference's opens and closes with a fixed 0.2 ms
rise). Loss is the energy of the pick's first 2 ms and 5 ms from its opening, against the textbook.

| Tight, all 6 433 picks the textbook plays | Old | New |
| --- | --- | --- |
| First 2 ms lose over 1 dB / over 3 dB | 21.7 % / 18.2 % | 6.5 % / 5.6 % |
| First 5 ms lose over 1 dB / over 3 dB | 15.8 % / 9.3 % | 5.2 % / 3.2 % |
| Worst loss, 2 ms / 5 ms | 28.0 / 19.7 dB | 28.2 / 19.7 dB |
| Opening ramp over 1 ms | 19.5 % | 7.1 % |

Smooth reads 18.8 / 15.5 % to 6.2 / 5.3 % (2 ms) and 13.7 / 8.0 % to 4.9 / 3.1 % (5 ms).

Floors by depth (over 1 dB lost in 2 ms, 48 picks per cell). Hiss and pink 0.3 to 4 dB under, 0 to 4 % to 0 %. The gate
is open most of the time there. 5, 6, 7 dB under, 13 to 52 % to 0 to 4 %. 60 Hz buzz 0.3 to 1 dB under, 4.2 % to 2.1 %.
Floors 9 and 12 dB under gain a little, 0 % to 0 to 6 %, from the pick's own noise (finding 1).

### 2. What did it cost? Finding 2.

### 3. Is the engine still the rule? Yes.

My reference is `ref7.c`, the round-6 `ref.c` with the opening time and the settle count rewritten from the header's
sentence and the brief's formula. The level is `r7lib.level` (scipy Butterworth, brute-force-checked trailing maximum).

| Run (measured, `q3_conformance.py 1040`) | Result |
| --- | --- |
| 1 040 random cases x 2 types. 44.1, 48, 96, 192 kHz. Blocks 1, 2, 7, 31, 64, 127, 256, 480, 512, 1000, 2048, 4096. Knobs over their full ranges with the edges. Ten materials, three aimed at the change | 2 558 openings, 596 with a rise strictly between 0.2 and 5 ms |
| Worst gain error outside openings, the tie case excluded (it reads 24.96 dB in Tight, in both binaries) | 0.00002 dB, both types, every rate and block |
| Worst linear gain difference inside openings | 0.000007 |
| Open flag against the reference, per sample | 18 samples in 1 case (the tie). No count mismatch |
| Notes inside 12 ms of a reset (65 cases) | Smooth 0.00002 dB |
| Old binary against the same reference in old-rule mode | 0.00002 dB, 0.000006 |

The open flag is read from Tight's gain with the range forced to -60 dB. That reading equals the bench's state meter
on 129 state changes in 48 cases, both binaries (`s1_state.py`).

**The comparison has power** (measured, `q3_conformance.py 390 --power=...`, baseline 1 of 1 040 flagged).

| Reference made wrong | Cases flagged of 390 |
| --- | --- |
| Ratio 2.9 instead of 3 | 84 |
| Last-dB band 0.9 dB / 1.1 dB | 55 / 54 |
| No extra sample | 52 |
| Settle counts closed samples only | 12, all in the reset material |
| The rule before the change | 159 |

### 4. Nothing else moved. Confirmed.

Measured, `q3_conformance.py 1040`, the same cases through both binaries.

- Per-sample open flag. 0 differences in 1 040 cases.
- Tight gain. Differs on 195 775 samples in 431 cases. All lie between an opening and 5 ms + hold + 1.6 x release +
  25 ms after it. An opening cut short by a close leaves a different gain to release from, hence the tail.
- Smooth gain. 38 cases differ outside that span. All 38 are the reset material, 36 with an opening inside 12 ms of the
  reset. That is the settle change. No other material differs.

### 5. The repaired rows

Audited by a subagent (`build-verify/r7-gate/audit/`, scripts and `.out` files there). I read D1, D3 and D4 in the
script myself (lines 1618 to 1687) and checked its `full_new.out`, `full_old.out`, `d4_oldnew.out` and
`mutsum_tight_only.out` against its claims. My runs `q1_picks.py` and `q2b_lownotes.py` are independent of it.

**Full run.** New binary 136 checks, 95 PASS, 0 FAIL, 41 info, 1 323 s at load 9 to 55. Old binary 92 PASS, 3 FAIL
(I5, P1, D3 with 198 of 480 over, worst +25.9 dB) (measured, `audit/full_new.out`, `audit/full_old.out`).

**Can each row fail, and where does its bar come from** (measured by mutants unless marked).

| Row | Can fail | Bar and source | Measured | Note |
| --- | --- | --- | --- | --- |
| D3 | Smooth yes, Tight weak | 1 dB worse than the same pick over a floor 15 dB under. Stated tolerance | +0.27 dB, 0 of 480 | No absolute bar. See below |
| D4 | weak | Ramp at least 3 ms. Arithmetic with 2 ms of slack | 5.00 ms in all 13 held cells | Fails only for an always-short opening. See below |
| I6 | yes | Gain 0 dB within 1e-6 after 5 ms over the threshold. Arithmetic | 0 over 20.2 M samples | Catches never-opens, stuck at -1 dB, 10 ms opening |
| P4 | yes | Engine's count equals the port's | 0/0 and 30/30 | Hold +1 and +3 ms pass it |
| G10 | yes | 3:1 law within 1 dB. Stated tolerance | -18.3 against -18.2 | 2.5:1 reads 5 dB off |
| I3 Smooth | yes | 3:1 on the most the estimate can hold. Arithmetic | 0 | Upper bound |
| I5 | yes | Release x 1.02, 0.2 ms rise x 1.01, each type's own gain | 0 | Fails the old binary |
| A5 | yes | Per-gap release line + 0.01 dB. Arithmetic | +0.00 | |
| TD1, A4, R3 | yes | Hold + release/3 + 3 = 23 ms. Arithmetic plus a 3 ms margin with no stated source | 20.0 ms | Hold +1 ms passes |
| TC1 | yes | -20 dB at the gap's end on riffs the arithmetic says must close | -36.0, 73 of 122 enforced | |
| D1 | yes | The rule's 0.2 ms rise on the same pick, -0.01 dB. Arithmetic | -0.01 against -0.01 | Tight only |
| X14 | weak | 1.01 dB per 10 ms. Arithmetic | 0.33 | Its signals never reach the limit. E1 covers it |
| G2 to G6, G17, X7 | yes | Window + hold + release/3 + 0.1 = 32.1 ms. Arithmetic | 31.7 to 31.9 | Hold +0.2 ms fails. X7 inferred |
| G7 | yes | The rule's earliest close | +1.1 ms | Hold -1 ms and 5 dB hysteresis pass |
| G8 | yes | 1.0 ms to -1 dB. Five times the arithmetic | 0.188 ms | Not repaired. A 1 ms opening passes |
| G9, X2 | yes | Release line + 0.01. Arithmetic | -46.5 against -46.1 | X2 inferred |
| G18 | inferred yes | Window + hold + 0.1 = 22.1 ms | 21.8 ms | Meters, not emulated |
| X12 | not shown | No dip under -1 dB | 0 of 144 | No mutant made a decay dip |

**The three near-ties are arithmetic, not fitted.** E1 at 1.00 against 1.01 sits on the slew limit, and a 103 dB per
second slew fails it. The G rows at 31.9 against 32.1 fail at hold +0.2 ms. R3 at 20 against 23 is the arithmetic plus
the 3 ms margin.

**D4's exclusion.** D4 enforces 13 of 24 cells and excludes 300 dB/s at 82 to 220 Hz and 1 000 and 3 000 dB/s
everywhere. The arithmetic for the exclusion is right for its waveform. It also hides a change. On D4's own generator
82 Hz at 1 000 dB/s goes 3.08 to 0.27 ms, 110 Hz at 3 000 dB/s 1.96 to 0.21 ms, 220 Hz at 1 000 dB/s 2.62 to 0.40 ms
(measured, `audit/d4_oldnew.out`). And D4's waveform is odd harmonics only, so its level has a crest every half
period, and its lowest note is 82 Hz (read, lines 1672 and 1677). A waveform with one crest per period under 83 Hz
opens in 0.6 ms at 20 to 100 dB per second, inside D4's "held" range (measured, `q2b_lownotes.py`). D4 cannot see that.

**D3's comparison.** It can hide a pick that is soft in both cases. A Tight engine whose every opening takes 5 ms passes
D3 with 0 of 480 over while the row prints "first 2 ms -24.26 dB" (measured, `audit/mutsum_tight_only.out`). D1, G9, G17
and A3 catch that engine. A ramp timed from 20 dB under and capped at 1.5 ms passes D1, D3, G9 and A3. Only P1 and D4
catch it. And D3's precursors are 3 to 7 dB under (floors) and 1.5 to 5 dB under (scrapes), with notes at -12 dBFS
(read, lines 1634 to 1651). So D3 stops exactly where the new rule stops working. My scrape 0.5 dB under leaves 79 % of
picks slow and D3 has no such cell.

**Wrong rules and the rows that catch them** (measured. a = the new script on the old binary. b = the engine replaced
by a mutated port, which equals the script's port exactly unmutated).

| Wrong rule | Caught by |
| --- | --- |
| Old binary (a) | I5, P1, D3 |
| Old timing, 0.5 x sinceLow (b) | P1, D3 |
| Every opening 0.2 ms | P1, D4, D2 |
| Every opening 5 ms | P1, D1, G8, G9, G17, A3, E1, D3 (Smooth cells only) |
| Last-dB band 0.5 dB / 2 dB | P1, D2 / P1, D3 (30 of 480) |
| Ratio 2 or 4 | P1 only |
| Longest opening 3 or 4 ms, always linear | P1 only |
| Settle counts closed samples only | P1, I5 |
| Opens 1 dB early | P1, I3, D4, D2 |
| Never opens | I6, G7, P1, D1, D2, D4, D3 (Smooth cells only) |
| Open gain stuck at -1 dB | I3, I5, I6, G8, G9, G15, G17, P1, P3, A3, D1, D4 |
| Hysteresis 5 dB | P1, I3 Smooth, G10, G13. No Tight row |
| Hold +1 ms | P1, G2 to G6, G17, A5 |

**`--rows` and D2.** `Q`, `G,Q`, empty, `g`, `G1` and `Common` exit 1 with the group list. G, X1 to X9 and common are
reachable. D2 prints "46 of 48 ... 2 never open, skipped". A D2 that skips everything fails. It can still pass with all
24 strings skipped, since one sine is enough for "at least half judged" (inferred by the subagent, not run).

**Holes that remain, ranked by what they let through.**
1. No row holds a pick after a precursor inside 1 dB of the threshold, a soft pick, or a pick with its own noise.
2. No row holds fast swells or low pulse-like notes. D2 runs one start time. Over 12 start times 1 of 552 swells
   exceeds D2's own bar on the new binary and 0 on the old (`audit/d2_oldnew.out`).
3. The opening's constants rest on P1 alone, and P1's port shares the rule.
4. D3 has no absolute bar. G8 and E1's note row still allow 1 ms against 0.19.
5. Rows crash with IndexError on a dead or stuck gate (G8, a P row, E1's note row) instead of failing.
6. The cpu rows fail under machine load (0.136 % against 0.1 % at load 30).

### 6. Verdict

At the top. Ranked by audibility. 1 the slow opening that remains and the picks made worse. 2 faster swells, worst on
low strings. 3 the guard rows. 4 the small things, all in the change's favour.

Before this is described as closing finding 1, correct the commit's three claims, add a D3 cell inside 1 dB and a soft
pick with an absolute bar, and give D4 a once-per-period waveform under 82 Hz. The shorter-of-both timing is worth a
bake-off. It removed every regression I measured on picks.

## Not checked

- Real recordings. None exist in the repository.
- Listening. Every audibility statement is a figure or a guess.
- The shorter-of-both timing in the engine, and its click cost on swells.
- Rates other than 48 kHz for the pick and swell statistics. Conformance covers four rates.
- Knob moves, type switches, stereo, numerics and CPU. Round 6 covered them and the opens and closes did not move.
- sc_hpf moves against a per-sample reference, and block sizes that change inside one render.
- The gate inside the plugin chain. The AU plugin was not installed or validated, as instructed.
- X2, X7, X12, X14 and G18 under closing mutants. Groups B and C run alone. R2's Tight figure "0 ms against 23".
- Whether D3's 1 dB bar was stated before measuring.
- The subagent's mutant runs beyond the four output files named above. I did not rerun them.
- The 30.87 Hz sine row of `q2b_lownotes.py` prints a click of 0.0 dB. That is a measuring artefact and is not used.

## Files

All under `build-verify/r7-gate/` (gitignored). Run each with `/usr/bin/python3` from that folder.

| File | What it is |
| --- | --- |
| `bin/tubamp_fxbench_new`, `bin/tubamp_fxbench_old` | the two binaries, sha1 above |
| `ref7.c`, `libref7.dylib`, `r7lib.py` | own reference (`clang -O2 -shared -o libref7.dylib ref7.c`), runner for either binary, round 6's synthesis |
| `s0_sanity.py`, `s1_state.py` | reference against both binaries on one signal, and the open-flag reading against the state meter |
| `q1_picks.py`, `out_q1.txt` | question 1 |
| `q1b_minrule.py`, `out_q1b.txt` | old, new and shorter-of-both on the same picks, reference only |
| `q2_swells.py`, `q2b_lownotes.py`, `q2c_minrule.py` and their `out_*.txt` | question 2 |
| `q3_conformance.py N [--power=...]`, `q3b_ties.py`, `out_q3.txt`, `out_q3_power.txt`, `out_q3b.txt` | questions 3 and 4 |
| `q4_settle.py`, `out_q4_settle.txt` | the settle count |
| `listen_make.py`, `listen/*.wav` | four A/B sets, dry, old, new |
| `audit/` | the check-script audit. `mutshim.py`, `mport.c`, `mut_rows.py`, `full_new.out`, `full_old.out`, `d4_oldnew.out`, `d2_oldnew.out`, `mutsum_*.out` |
| `work/` | temporary WAVs. A killed run left some behind. I could not delete them (the removal was refused). They are safe to delete |
