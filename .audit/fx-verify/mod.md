# Mod block verification (chorus, phaser, tremolo) — INTERIM, written as the run proceeds

Verifier build: `build-verify/mod/` (Release) and `build-verify/mod/asan/` (address+undefined sanitizers).
Processor harness: `build-verify/mod/h/modh.cpp` (real `TubampAudioProcessor`, chain = mod block(s) only, scripted
parameters and play head, raw float I/O), Python wrapper `build-verify/mod/h/vlib.py`. Every script named below is in
`build-verify/mod/h/`. "Processor" = measured through the whole plugin; "fxbench" = the engine alone.
The processor always runs a 5 Hz DC blocker after the chain; where it matters it is undone exactly-enough
(`vlib.undo_dcblock`, residual -74 dB) or the reference is passed through the same blocker.

## Verdicts so far

| Claim | Verdict | Number |
|---|---|---|
| P-a trajectory vs GR6 | VERIFIED (with a measurement caveat) | Processor, itrack, vs stored AND vs fresh GR6 renders (identical: GR6 reproduces its stored trajectories to 0.00 %): Phase 90 block 0.72 % rms / 1.25 % max over six rates; Small Stone off 1.90 % rms (3.52 % max); on 1.16 % rms (4.14 % max at 1.78 Hz). Raw processor output without undoing the DC blocker reads 1.30 % / 2.41 % (block) and 1.39 % / 6.1 % (Color on): the blocker's 32 ms tail overlaps the 512-sample impulse train; it is a tracker artefact, not a sweep change. |
| P-b C++ = numpy prototype | VERIFIED | Processor vs prototype (through the same DC blocker): <= -176.7 dB rms at 44.1/48/96 kHz, blocks 64 and 37, stereo and mono (float32 I/O floor). Control: rate +0.01 % reads -71.6 dB, wrong colour -12.2 dB. |
| P-c only the stated misses | NOT VERIFIED — more misses exist (most are in voicings.md, none in the stated list) | see defects P1-P4 |
| P-d stability / clicks | PARTLY | Stable and linear to +24 dBFS, 30 s, 10 Hz and 20 Hz synced (no runaway; Stone Color on peaks +14.1 dB on an 82 Hz square). Model/colour switches -62..-69 dBFS residual (>2 kHz), rapid cycling -58.4. Rate automation clean. Small Stone + sync: transport start / locate / loop jump tick, up to -38.4 dBFS (defect P5). Phase 90 clean (-109 dBFS). |

(remaining rows are added below as they are measured)
