# Stoned Phaser with Color on: structure read from the binary

What `NI::DSP::StonedPhaser` (Guitar Rig 6) does when Color is on, read from the arm64
disassembly. Structure, ranges and roles only: no coefficient set from here is used in the
prototype or the spec. Our constants are fitted by `fit/fit.py` to `fit/targets.tsv`.

```bash
export RE_STATE=<scratch>/re RE=<repo>/tools/re
GR="/Library/Audio/Plug-Ins/Components/Guitar Rig 6.component/Contents/MacOS/Guitar Rig 6"
python3 $RE/disasm.py "$GR" --addr 0x697eac     # also 0x69830c 0x691b4c 0x611e48 0x611350
```

All five are in the Hopper cache (`hopper.py "$GR" 0x697eac ...`), but its pseudo-code prints most
of the NEON and double arithmetic as raw `asm`; it was used for control flow, and the arithmetic
below is read from `disasm.py`.
The rack component that was measured is `RigPhaserBT`; the earlier study found the same calls
and pool constants there (`decompiled_notes.md` section 2).

## Answers

| Question | Answer | Where |
| --- | --- | --- |
| Swept stages | four first-order allpasses, realised as N = 2 second-order sections | 0x697edc (N at +0x5c), loop 0x698600-0x698674 |
| One break frequency or staggered? | one: every section gets the same `g`, Color on and off | 0x6985b4-0x6985e0 computes one `g` per channel, reused in every section |
| Feedback taken from | the last section's output (the wet signal, before the mix) | 0x6983a4-0x6983ac stores it to lanes 2,3 of +0x230 |
| Feedback injected | at the input of the first stage, summed with the shaped dry input | 0x6985a0-0x6985a8 sums the four biquad lanes in pairs |
| Gain and sign | `+c` delay-free (built into the sections) and `−c` through a filter and one sample of delay; `c = Color × strength × (±1 Invert)`, clamped to ±0.85, so 0.74 at the measured settings | 0x611ed0-0x611ef8, gains vector +0x2b0 written at 0x611f48 |
| "Color biquads" | one second-order network with two numerators on a shared denominator: one in the signal path (wet input only), one in the feedback path | 0x6980b0-0x6982e4 |
| Dry/wet | dry = 1 − w, wet = w; at Wet 0.5, w = min(1, 0.5 + 0.5·\|c\|): 0.5 off, 0.87 on. The dry is the unfiltered input | 0x611f4c-0x611f88; mix at 0x6983b0-0x6983b8 |
| Sweep range | two fixed limit pairs selected by Color > 0.5 (lower and narrower with Color on) | 0x611ea8-0x611ec8, stored at +0x38/+0x3c |
| LFO shape | the same code for both states | 0x6983fc-0x698558 has no Color branch |
| Rate | unchanged by Color: the Color case never writes the phase increment (+0x198) | 0x611e80-0x611fa4; measured period error 0.00 % |

## Notes by procedure

**`setParameter` 0x611e48** (jump table at 0x611e74). Color case 0x611e80: the value is kept if
≤ 0.5, else 1.0; the sweep limits flip at 0.5; `c` is recomputed with the stored Invert sign
(+0x48) and strength (+0x14); then a gains vector `[gA, gA, −c, −c]` with
`gA = 1.5·(1 − 0.9·|c|)` (0x611f14-0x611f48), the wet share (0x611f4c), the Wet knob target
(0x611f5c-0x611f84), and a call to `calculateFilterCoeffs`. The Invert (0x611fec) and Strength
(0x6120c4) cases repeat the same tail.

**`onSetSampleRate` 0x611350.** Stores fs at +0x34 and π/fs in lanes 0,1 of +0x220 (the
argument of the tangent); control block = round(0.03·fs / block size). No oversampling inside
the class. The measured responses nevertheless fit a 96 kHz model of this topology to 0.01 dB
rms and a 48 kHz one only to 0.05-0.25 dB, with notch 2 at the sweep top 4 % higher than a
48 kHz model can place it [measured: fit/targets.tsv static rows]: the host framework runs the
component at twice the session rate [inferred].

**`processControlRate` 0x691b4c.** Ramps four parameters (+0x80 += +0x90, wet share in lane 2).
When `c` has changed (flag +0x58) it redoes the gains and wet share (0x691cd8-0x691d58) and
tail-calls `calculateFilterCoeffs` (0x691d68).

**`calculateFilterCoeffs` 0x697eac**, two parts.
- 0x697ed4-0x6980ac, the loop. `r = |c|^(1/2N)` (pow at 0x697ef0), a per-section scale
  `(1 − c)^(1/N)` (0x697f14), roots at angles `(i + ½[c<0])·π/N` (sincos at 0x697f60 and
  0x697ff4), each pair mapped by `(p − 1)/(p + 1)` into three numbers per section, stride
  0x50 from +0x2c0. This is `P⁴/(1 − c·P⁴)` factored exactly: feedback around all four
  stages with no delay.
- 0x6980b0-0x6982e4, the Color network. Element values depend on `m = min(|c|/0.7, 1)`; the
  denominator's two real roots are computed (fsqrt at 0x69816c) and a root slower than
  2π·50 rad/s is moved to 2π·50 (branch at 0x698188). Bilinear transform with K = 2·fs, no
  prewarp (0x698204). Lanes 0,1 get a second-order numerator (input branch), lanes 2,3 a
  first-order numerator whose DC gain equals the denominator's (feedback branch: unity at DC).
  Stored as a four-lane transposed direct form II: b0 +0x240, b1 +0x260, a1 +0x270, a2 +0x290,
  b2 +0x2a0.

**`processAudio` 0x69830c**, per sample.

```
lanes   = [xL, xR, yL[n-1], yR[n-1]]                     ; +0x230
bq      = Biquad4(lanes)                                 ; 0x698560-0x698598
u       = gA*bq[0..1] + (-c)*bq[2..3]                    ; 0x69859c-0x6985a8
g       = x + x^3/3 + 2x^5/15,  x = sweep * pi/fs        ; 0x6985b4-0x6985e0
y       = Section_N( ... Section_1(u; g) )               ; 0x698600-0x698674, one fdiv each
lanes[2..3] = y ;  out = (1 - w)*x + w*y                 ; 0x6983a4-0x6983c8
```

So `y = gA·BA·P⁴·x / (1 − c·P⁴·(1 − z⁻¹·BB))`. BB is unity at DC, so the effective feedback
`c·(1 − z⁻¹·BB)` is a second-order-shaped highpass: zero at DC, full strength well above the
network's poles. BA shapes only the wet input. With Color off `c = 0`: no feedback, BB unused,
and BA reduces to a first-order highpass near 44 Hz with gain 1.5 on the wet path (this is
why Guitar Rig's Color-off notch ratio is 5.4 rather than the ideal 5.83; not changed here).

## Checks

- Topology. This structure, transcribed in scratch for analysis only, reproduces the four
  earlier static captures (`gr6_snapshots.tsv`) to 0.18-0.41 dB rms from 100 Hz to 8 kHz with
  only the break frequency free.
- Rate dependence. In the factored-section form the tracked sweep narrows at fast rates
  (notch-1 top −1.6 % from 0.16 to 1.78 Hz, as measured on Guitar Rig 6: −1.6 %); the TPT
  chain of the prototype, same loop and same break-frequency trajectory, does not narrow
  (0.0 %). The difference is how the two realisations carry their state while `g` moves, not
  the static structure.
