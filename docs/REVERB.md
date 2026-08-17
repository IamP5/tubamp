# Reverb Engine — Design Spec

Status: approved design, 2026-08-06. Amended same day by user directive: Freeverb
("Legacy v1") erased, Reverse added as mode 5 (§3.12), the first draft's Stages 1–2
merged into Stage 1 and later stages renumbered. **Stages 0 and 1 are implemented as of
2026-08-06, Stages 2 (Spring) and 3 (Shimmer) as of 2026-08-07** — all six modes are now
voiced, frozen and live, with only Stage 4 (Freeze) outstanding. See §7's "Shipped" blocks
for what landed, what it measures, and the dated amendments the implementation forced.
Branch: TBD (follows `feat/stereo-chain`).
Research basis: Valhalla/Costello published topology + tuning disclosures, Strymon
BigSky/NightSky manuals, the canonical literature (Dattorro 1997, Jot/Chaigne, Dahl-Jot
2000, Gardner 1992, Gerzon 1971/72, Abel & Huang 2006, Dal Santo/Prawda/Schlecht/
Välimäki 2025), and licence-audited open implementations (zita-rev1 via Faust's MIT
section, Costello's `reverbsc`, Airwindows, CloudSeedCore). Full URL list in §10.
Codebase reads: `src/dsp/FxBlocks.cpp:573-620`, `src/Parameters.cpp:111-114,202-245`,
`src/PluginProcessor.cpp:306-310,1440-1452,1808-1810,2172-2200`, `src/dsp/ChainOrder.h`,
`src/WebEditor.cpp:102-104`, `build/_deps/juce-src/.../juce_Reverb.h`.

## Scope

Replace the reverb block's DSP with a purpose-built engine. The old `juce::dsp::Reverb`
(Freeverb) is erased outright — no "Legacy v1" mode, no two-engine dispatcher, no
migration shim (user directive, 2026-08-06). Legacy sessions and presets re-render
through Room via absent-means-default (§5.3) and their sound changes; that is accepted
and intentional. Reverse reverb is now IN scope as the zero-latency windowed mode of
§3.12.

Out of scope, deliberately: convolution reverb, a true time-reversed capture (a window
of latency by definition — §3.12 is the zero-latency idiom instead), nesting
reverb into placeable sub-blocks (`ChainOrder.h` has 3 spare BlockIds and reverb stays
one tile), per-lane reverb sends, and any oversampling or lookahead anywhere.

## Invariants that must not regress

- **Latency stays exactly 0.** Pre-delay is a *musical* delay and must never enter
  `computeChainLatency` / `computeWantedLatency`. No lookahead in the ducker, no FFT,
  no oversampling. A future contributor adding a phase-vocoder shimmer breaks this.
- Chain order is STATE. APVTS ids frozen; new params APPEND-ONLY at the end of
  `createParameterLayout()`, literal id constants in `Parameters.h`, instance-indexed
  arrays. No new `chain::BlockId`.
- Audio thread never allocates, never locks, never blocks. Every buffer sized in
  `prepare()`. `ScopedNoDenormals` already covers `processBlock`.
- `pendingReset` remains the ONLY path by which a block's state is cleared on the audio
  thread, and `kMaxResetsPerCallback = 2` remains the bound on that work.
- `renderChain`'s contract (`tests/ProcessorSmokeTest.cpp:528-540`) holds: two renders
  of the same order after `prepareToPlay` are bit-identical. `ReverbEngine::prepare()`
  therefore ALWAYS clears.
- Absent `reverb_algo` resolves to **Room** on BOTH restore paths, via the
  absent-means-default sweeps of §5.3. The first draft's bit-identical-default
  invariant is retired by dated user decision (2026-08-06): legacy content changes
  sound, by design.

---

## 1. Why the current reverb falls short

`ReverbFx` (`src/dsp/FxBlocks.cpp:573-620`) wraps `juce::dsp::Reverb`, which is
Jezar's Freeverb (2000) verbatim: 8 parallel lowpass-feedback comb filters into 4 series
"allpass" filters per channel. Five defects, each structural rather than a tuning miss:

**1.1 Echo density never grows.** The four diffusers sit OUTSIDE the feedback loop, so
the comb bank's echo rate is `Σ fs/N_i` ≈ 260 echoes/s and stays there forever.
Schroeder's target is ~1000/s; Rubak & Johansen's listening threshold for "statistically
indistinguishable from a real tail" is ~4000/s. With `g = 0.5` the diffusers' own decay
times are 126/100/77/51 ms, so past ~350 ms the output is the bare periodic comb train.
This is the single biggest audible gap and no parameter can close it.

**1.2 One feedback coefficient for eight combs ⇒ 44.9% T60 spread.** `f = roomSize*0.28
+ 0.7` is shared, and `T60 = N·T·ln(0.001)/ln(f)` is proportional to `N`, so the spread
is pinned at `1617/1116 = 1.449` at every setting. At `roomSize = 0.5` the eight combs
decay in 1.00 … 1.45 s. The last ~30% of every tail is the single 1617-sample comb
ringing alone at a 36.7 ms period — a 27.3 Hz flutter with a harmonic comb spectrum.

**1.3 The "allpass" filters are not allpass.** JUCE's `AllPassFilter::process` is
`buffer[i] = input + 0.5*buffered; return buffered - input`, i.e.
`H(z) = (-1 + 1.5 z⁻ᴺ)/(1 - 0.5 z⁻ᴺ)` — a feedback comb in series with a feedforward
comb, 4.44 dB of magnitude ripple per stage, 17.75 dB worst case across the cascade.
(It is only truly allpass at `g = (√5-1)/2 = 0.618`.)

**1.4 Delay lengths are not mutually prime and drift with sample rate.** 7 of the 8
comb tunings share a factor of 3; only 1277 is coprime to the rest. `setSize()` uses
integer truncation (`(int) sampleRate * tuning / 44100`), so the ratio set — and hence
the character — differs at 44.1 / 48 / 88.2 / 96 kHz. At 88.2 kHz all 28 length pairs
share a factor.

**1.5 Modal density is 0.250 modes/Hz — 5.2× short at its own maximum.** `Σ N_i =
11024` at 44.1 kHz. Schroeder/Logan needs `M ≥ 0.15·T60·fs`; at `roomSize = 1`
(T60 ≈ 8.6–12.5 s) that is 57,219. zita-rev1 is 1.460 modes/Hz; Costello's `reverbsc`
is 0.547.

**1.6 Stereo is a 23-sample offset.** The core is mono (`processStereo` sums L+R first);
"stereo" is `stereoSpread = 23` samples added to every right-channel line. Two highly
correlated signals offset by 0.522 ms comb on mono-sum, notches at 959 / 2877 / 4795 Hz.
`width` is a wet M/S matrix on top of that, so it cannot fix the correlation.

**1.7 Two live bugs in the wrapper.** `ReverbFx::prepare()` calls `reverb.reset()`, and
`juce::CombFilter::setSize()` calls `clear()` on any size change — so every host
re-prepare (Logic does this on transport and I/O buffer changes) truncates the tail.
And `freezeMode` has been hardwired to `0.0f` since day one, so the parameter JUCE
exposes has never been reachable.

---

## 2. How the reference plugins actually do it

A tight summary of what is *documented*, with the design consequence. Sources in §10.

- **Valhalla has no single engine.** Costello: "Each of the Modes in a Valhalla plugin
  is a unique algorithm… the Mode switch is used to select the *render function*."
  22 modes in VintageVerb. His internal names — "BigFDN32", "AllpassLoop8" — reveal
  exactly two structural families: FDNs and allpass loops. *We do not copy this;* §9.
- **Jot per-line damping is the highest-value single technique.** Design the per-delay
  filter so every line decays at the same rate at every frequency:
  `|H_i(ω)| = 10^(-3·m_i/(T(ω)·fs))`. Costello grafted this onto a nested-allpass
  reverb for VVV 4.0.0's Chamber1979 and reported it fixed both the metallic ring and
  the inability to reach short decays.
- **Damping is exposed as decay-time MULTIPLIERS with crossovers, not filter cutoffs.**
  ValhallaRoom: Bass Mult ×0.25–×4.0, Bass Xover 300–1500 Hz, High Mult (≤0.5× "for a
  more natural decay"), High Xover 3–8 kHz. BigSky calls the same thing "Low End" and
  "Tone". This is Jot's parameterisation and it is the most copyable UX decision here.
- **HF decay has a physical ceiling.** Costello: "the maximum RT60 at 10 kHz will never
  exceed around 1.25 seconds for **any** physical space." FutureVerb's Neutral color
  hard-codes it.
- **Modulation is many shallow random modulators, not one deep LFO.** ValhallaRoom /
  VVV / FutureVerb: "several dozen random LFOs… all of which are randomized and have
  different base frequencies", rates centred on the Mod Rate knob, useful band
  0.25–1.0 Hz. Dattorro reaches the same conclusion: "the diffusion burden becomes more
  distributed. Hence the required rate and depth of modulation are lessened for each
  diffuser."
- **Two scaling laws are stated explicitly and are not optional.** (a) "Shorter Size
  settings may also result in deeper modulation for the same decay setting, so be sure
  to retune this" — a fixed sample depth is a larger fractional detune on a short line.
  (b) "For long decays, you may wish to back off on the modulation depth… A modulation
  depth that works for short decays may sound seasick for long decays."
- **Modulate delays, never allpass coefficients.** Costello, Spin Semi 2006: modulating
  the coefficient "can result in a sound much like banging a metal pot full of water."
  And never modulate SHORT allpasses: "a sound similar to water sloshing around in a
  metal pan."
- **Early reflections should contain no allpasses.** ValhallaRoom's Early section "has
  no allpass delays in it, which allows it to avoid the coloration caused by short
  allpasses" — he frames it as "Early Energy", not ray-traced geometry. Griesinger's
  perceptual rule for the envelope: energy flat to ~150 ms, *then* exponential.
- **Diffusion must be a user control** because pulse-like sources ring short allpasses;
  Costello names male vocals, and a palm-muted guitar through distortion is more
  pulse-like still.
- **Stereo comes from tap selection out of ONE tank.** Dattorro's plate takes 7 signed
  taps per channel from the tank delays, with each channel drawing its same-line taps
  from the *opposite* half of the figure-eight. Gardner's alternative — perturb every
  delay by ≤2% for a second, decorrelated instance — is the multichannel version.
- **Interpolation choice is audible and compounding.** Costello's rule of thumb: a 30 s
  decay over ~0.1 s loops means ~300 traversals, so "any little quirk in the feedback
  path, like interpolation noise… will be increased on each pass." Linear interpolation
  is a fractional-delay lowpass whose cutoff moves with the fractional part — inside a
  near-lossless loop that is time-varying, unaccounted damping. Both Dattorro reference
  ports and `reverbsc` use 4-point cubic.
- **The colourless threshold is measurable.** Dal Santo/Prawda/Schlecht/Välimäki 2025:
  "more than 6000 modes are needed for an IR to be perceived as rather colorless", with
  logarithmically distributed coprime delays; optimized N = 4/6/8 FDNs match much larger
  ones in MUSHRA. 16 lines buys nothing measurable.
- **Spring is a different machine, not a preset.** Välimäki/Parker/Abel's parametric
  model is a long cascade of stretched allpass sections inside a feedback loop around a
  randomly-modulated delay. Gamper/Parker/Välimäki's Leem KA-1210 calibration: chirp
  transition ~4.2 kHz, `a1 ≈ 0.63`, `Td ≈ 56 ms`, ≈ −5.4 dB per pulse, and capping the
  cascade at 100 sections (from 318) was perceptually indistinguishable.
- **Shimmer is a routing switch, not a topology.** BigSky MX exposes Input (pre-core,
  non-regenerative) / Regen (in-loop) / Both around ONE shifter. Pitch shifting is
  itself a stability mechanism (it translates energy away from any resonance), but an
  octave-up shifter doubles the spectrum every pass, so an in-loop lowpass after the
  shifter is mandatory.
- **The color axis is the product.** VVV's 1970s color is a genuinely downsampled render
  path (8 kHz internal bandwidth) with quantized, noisy modulation — Costello later
  corrected himself to say the modulation *source*'s noise matters more than the tone
  filter. The Chaotic modes add tape pre-emphasis → nonlinearity → de-emphasis and he
  reports this *increases* clarity by reducing ring-out. None of this is transparency.

---

## 3. Architecture

One engine — an 8-line absorbent-allpass FDN in the Jot / Dahl-Jot lineage — with
per-mode constant tables, plus two bolt-on stages (Spring dispersion, Shimmer shifter)
and one alternate back end (the Reverse window, §3.12) for the places the shared core
genuinely cannot stretch.

New files: `src/dsp/ReverbEngine.{h,cpp}` (self-contained, `juce_dsp` only, must compile
under `TUBAMP_HEADLESS=1`; the recursive glob at `CMakeLists.txt:145-155` picks it up
with no CMake edit), later `src/dsp/SpringDispersion.h` and `src/dsp/PitchShifter.h`.
`ReverbFx` becomes a thin wrapper over the single `ReverbEngine`; the
`juce::dsp::Reverb` member is deleted, not parked behind an algo index.

### 3.1 Block diagram

```
                     ┌──── dry ──────────────────────────────────────────────┐
 in L,R ──┬──────────┘                                                       │
          │                                                                  │
          ├─► duck sidechain: |x| → env follower (20 ms atk / 1200 ms rel) ───┼─► wetGain
          │   (feed-forward only — no lookahead, latency stays 0)            │
          │                                                                  │
          ├─► inject:  a = 0.70·L + 0.30·R   (even lines)                     │
          │            b = 0.70·R + 0.30·L   (odd  lines)   [L==R ⇒ a==b]     │
          │                                                                  │
          ├─► DC block   one-pole HP 18 Hz                                    │
          ├─► send HP    one-pole, reverb_lowcut   20 .. 800 Hz  (def 20=off) │
          ├─► send LP    one-pole, reverb_highcut  1.2k .. 20k   (def 20k=off)│
          │                                                                  │
          ├─► PREDELAY   0 .. 250 ms  (fractional slewed read < 5 ms jump,    │
          │                            equal-power crossfade ≥ 5 ms jump)     │
          │                                                                  │
          ├─►[Spring only] DISPERSION (§3.9) — REPLACES the input diffusers   │
          │                                                                  │
          ├─► INPUT DIFFUSERS ──────┐   4 series 2-multiply Schroeder allpass │
          │     4.8 / 3.6 / 12.7 / 9.3 ms × sizeScale                         │
          │     g = inDiffG[k] × (0.15 + 0.85·reverb_diffusion)               │
          │     NEVER modulated (short allpasses slosh)                       │
          │                         │                                         │
          ├─► EARLY ENERGY (§3.7) ──┼──────────────────► erL, erR             │
          │        │                │   ◄── AMENDED 2026-08-06: the Early tap │
          │        └── × erToTank ──┤       line is fed the DIFFUSED signal   │
          │                         │       (see §3.7)                        │
          │                         ▼                                         │
          ├─►[Reverse only] REVERSE WINDOW (§3.12) — REPLACES the tank and    │
          │                 idles the Early Energy section                    │
          └──────────────────► TANK (§3.2) ────────────► tankL, tankR         │
                                                                             │
   wet = tank + 0.60·erLevel·er            ← constant-tail law, NOT a xfade   │
     → output DC block (one-pole HP 18 Hz)                                    │
     → [Plate/Hall] frequency-dependent L/R spread (§3.8)                     │
     → reverb_tilt  (±6 dB shelving pair, pivot 800 Hz)                       │
     → M/S width (reverb_width, wet only)                                     │
     → hard limiter (safety, −1 dBFS knee)                                    │
     → mix: out = dry·(1−mix) + wet·mix ◄─────────────────────────────────────┘
```

The mix law is `dryLevel = 1 − wetLevel`, preserved verbatim from today — not equal
power, which would alter the balance of every existing preset.

### 3.2 The tank

Per line `i ∈ [0,8)`, per sample:

```
r_i = cubicRead(line[i], writePos − lineSamples_i − mod_i(t))   // 4-pt Hermite
a_i = allpass_i(r_i)          // modulated, length apSamples_i, fixed coeff apG_i
s_i = jotDamp_i(a_i)          // §3.4: low shelf + HF one-pole + mid gain
w_i = softClip(s_i) + 1e-20   // §3.5 C¹ clip + zita staynormal
```

then the mixer and write-back:

```
y = Â · w                     // Â = normalized 8×8 Hadamard, every entry ±1/√8
line[i] ← y_i + inject_i
```

`Â` is 12 in-place butterflies — pairs (0,1)(2,3)(4,5)(6,7), then (0,2)(1,3)(4,6)(5,7),
then (0,4)(1,5)(2,6)(3,7) — 24 add/sub, followed by 8 multiplies by `1/√8`. **The
normalisation lives in the matrix, not in `g_i`.** `g_i` is pure Jot decay; the input
scale is its own per-mode constant `k_in` (default 0.35) so injection level and feedback
gain are independently tunable. Injection sign pattern `+ + − − + − + −` (zrev2's), even
lines fed `a`, odd fed `b`.

> **AMENDED 2026-08-07 — the injection is DECAY-NORMALISED: `k_in` is scaled per block
> so line level, and with it wet level and the in-loop clip onset, stop tracking
> `reverb_decay`.** For white input the steady-state line RMS is `k_in/√(1 − g²)` — the
> law Spring's wet trim (`kSpringOutGain`, §3.9) is already derived from — and `g` runs ~0.86 at Room's
> 0.2 s floor to the 0.9999 clamp of §6.6 at Hall's 10 s ceiling, i.e. over 20 dB of
> internal level swing driven by a knob that sets a TIME. Two consequences, one audible
> and one structural: the wet output level rides Decay (against the constant-tail law
> `kErToOutput` already holds for Early Energy in §3.7), and the distance to §3.5's
> ABSOLUTE 0.8 clip threshold rides it too, so the onset of in-loop distortion is a
> hidden function of Decay — and it compounds, because a long tail also recirculates it
> hundreds of times. §8's single-point nonlinearity row cannot see either, because it
> measures at one decay setting.
>
> So the injection carries a per-block scale:
>
> ```
> gRef        = 10^(−3·D_min / 1.5)          // per mode, at sizeScale 1.0, built in configure()
> injectGain  = clamp(√(1 − g_max²) / √(1 − gRef²), 0.25, 2.0)
> inject_i    = injectGain · k_in · sign_i · (a or b)
> ```
>
> `D_min` is the mode's MINIMUM nominal loop delay (line + its allpass; in Spring, spring
> + the dispersion's DC group delay) because `g_max` is the max over lines — the shortest
> loop carries the largest `g` — so the two conventions match and the scale is exactly
> 1.0 at the 1.5 s reference, the same pivot §3.6's `t60clamp` uses. `g_max` is the
> PER-PASS gain the RMS law is stated on: in the tank that is `g_mid`, and in Spring it is
> `g_mid` *before* the `/loopLoss` compensation of §3.9, since that division hands back
> what the in-loop filters take out again on the same pass. Spring is scaled by the same
> ramp as the tank, and must be: `kSpringOutGain` was derived assuming both back ends sit
> at matched `g`, so a tank-only scale would break that parity everywhere except 1.5 s.
> Reverse needs no change and gets none — nothing recirculates and §3.12's tap
> normalisation is already window-invariant. Two recorded consequences: (a) matching line
> RMS cannot match the CREST factor with it — a 0.2 s tank sums fewer overlapping copies,
> so at equal RMS its peaks sit nearer the 0.8 knee, and the measured in-loop distortion
> moves from the long end of the knob to the short end (Room, −6 dBFS pink at 44.1 kHz:
> −129 → −47 dB residual-to-signal across 0.2 → 2.5 s before, −53 → −74 dB after) rather
> than vanishing; the [0.25, 2.0] clamp is what bounds that end. (b) Spring's Dwell drive (§3.9) saw a
> decay-dependent input level through the loop before this and now does not, so its grit
> stops thickening as Decay rises — arguably the point, but it is a character change.
> **Preset-audible**, see §5.5. Gated by §8's two decay-normalisation rows.

Why Hadamard and not Householder `I − (2/N)uuᵀ`: at N=8, Householder's diagonal is 0.75
against off-diagonals of −0.25, so it is 3× self-weighted and drifts toward decoupled
parallel combs as N grows (JOS). Hadamard is fully mixing, so echo density builds
fastest. The 8-multiply difference is noise next to the cubic interpolators.

**There is no `density` blend.** The original design proposed
`y = (1−d)·P + d·H` with a shift permutation `P`. That is a strict contraction: the
smallest singular value of the blend is 0.600 at `d = 0.8` and 0.200 (−14.0 dB) at
`d = 0.4`, so per-eigenmode loop gain would spread by 14 dB while `g_i` is solved
assuming a lossless matrix — a worse version of the Freeverb comb spread this whole
design exists to remove. The one mode that wanted it (Ambience) is cut (§4). Prepare
asserts `cond(A) < 1.001`. *[If a Density control is ever wanted, interpolate along the
unitary group — `A(d) = P·expm(d·logm(Pᵀ·Â))` precomputed at prepare into an 8×8 table,
64 mults/sample — never a linear blend of two unitaries.]*

> **AMENDED 2026-08-07 — at nonzero `reverb_mod` the mixer is `R(t)·Â`, not `Â`:
> §3.6's Layer C applies four 2×2 Givens rotations on pairs (0,1)(2,3)(4,5)(6,7)
> between the butterflies and the write-back.** `R(t)` is exactly orthogonal at every
> sample for any angle — each pair uses `(c,s) = (1,θ)·rsqrt(1+θ²)`, which satisfies
> `c²+s²=1` by construction — so `cond` stays 1, the prepare assert is untouched (it
> checks `Â`, which is still the static butterfly), and every stability and Jot-solve
> argument stated against a lossless mixer carries over unchanged. At `reverb_mod = 0`
> the layer is skipped outright and the mixer is bit-identically `Â`. The Density
> bracket above is the STATIC member of this family and remains unbuilt; Layer C is
> the time-varying one — rotations being how one moves on the unitary group without
> ever leaving it. Depth, rates and rationale live in §3.6.

### 3.3 Delay lengths and sample-rate scaling

**The 48 kHz sample counts are authoritative; the millisecond column is derived from
them by division.** At `prepare()`:

```
lineSamples[i] = tankSamples48[i] * (fs / 48000) * sizeScale       // float, fractional
apSamples[i]   = lineSamples[i] * apRatio_i                        // §3.6
```

Reads are fractional and modulated, so integer primality at the host rate is neither
achievable nor operative. The property that matters is that the *ratios* admit no
small-integer coincidence — the "low-order dependencies" the tiny-colorless-FDN paper
names — which is scale-invariant and is verified by the flutter autocorrelation gate at
44.1 / 48 / 96 kHz (§8), not asserted.

**The only prepare-time hard constraint is `Σ lineSamples ≥ 6000`**, satisfied by
construction at each mode's `sizeMin` (see §4) so the Size knob is never clamped.

**Schroeder/Logan modal density (`M ≥ 0.15·T60·fs`) is enforced as a per-mode DECAY
CEILING, not a Size clamp.** It is a design-time constant derived from
`Σ D_i` (delay + allpass) at `sizeScale = 1.0`, and it caps `reverb_decay`'s range for
that mode. At small Size with long Decay the reverb falls below the criterion and sounds
thinner and more metallic — which is exactly what real reverbs do, and what Costello
documents ("a smaller Size setting will sound more metallic for longer decays"). Three
lenses converged here: clamping Size silently kills a knob, and enforcing the criterion
per-block would make Size a function of Decay and glide the tail whenever Decay moves.

`maxDelaySeconds = 0.36` per line — Hall's longest line at `sizeMax = 1.5` is 344.6 ms.
Buffers are **exact length with a 4-sample mirrored guard region** at the tail, wrapped
by `if (pos >= size) pos -= size`; NOT power-of-two. Power-of-two rings would round
131,072 out of 76,800 at 192 kHz and inflate the footprint ~60% (§6).

### 3.4 Jot 3-band damping — the zita-rev1 solve

This is what kills the 44.9% T60 spread. For line `i` with total loop delay `D_i`
seconds (line + its allpass), and target times

```
T60_mid  = reverb_decay                                    (clamped to the mode ceiling)
T60_low  = reverb_decay · reverb_bassmult                  (×0.25 .. ×4)
T60_high = min(reverb_decay · (1.25 − reverb_damping), hfMaxT60)
```

the per-line filter is zita-rev1's `Filt1`, which carries the mid-band decay as a scalar
and the other two bands as *ratios*:

```
g_mid = 10^(−3·D_i / T60_mid)
g_lo  = 10^(−3·D_i / T60_low ) / g_mid − 1        // low-shelf excess
r_hi  = 10^(−3·D_i / T60_high) / g_mid            // < 1
w_lo  = 2π·f1 / fs                                // f1 = bassXoverHz
chi   = 2π·f_h / fs                               // f_h = hfXoverHz
t     = (1 − r_hi²) / (2·r_hi²·chi)
w_hi  = (√(1 + 4t) − 1) / (2t)

per sample:  s_lo += w_lo·(x − s_lo);  x += g_lo·s_lo;
             s_hi += w_hi·(x − s_hi);  x  = g_mid·s_hi
```

**This replaces the formula in the first draft**, which took only `g_mid` and the
crossover and had no input for the target HF gain at all — so `reverb_damping` and
`hfMaxT60` could not reach it, and its behaviour inverted with decay time (at
`decay = 30 s` it offered at most −0.85 dB of HF cut; at `decay = 0.5 s` it offered
heavy damping). With the ratio form there is no degeneracy as `g_mid → 1`, so the
`g_m ≤ 1 − 1e-4` clamp is dropped.

**Interpolator compensation.** 4-point Catmull-Rom is not transparent: at `frac = 0.5`
it is −0.031 dB at 4.8 kHz, −0.457 dB at 9.6 kHz and −2.16 dB at 14.4 kHz, and the loss
moves with `frac`, so it IS time-varying under modulation. Room at T60 = 2 s makes ~50
passes: 22.8 dB of unintended extra loss at 9.6 kHz, a −27.5% T60 error. Therefore
`r_hi` is solved against a *compensated* target:

```
r_hi = 10^(−3·D_i / T60_high) / (g_mid · |Hi(f_h)|_mean)     clamped ≤ 1 − 1e-4
```

where `|Hi(f)|_mean` is the interpolator's magnitude averaged over a uniform `frac`
distribution, tabulated at prepare per mode. The Stage-0 probe measures it. *[Inferred:
the uniform-frac average is a good enough model of the random walk's phase distribution.
If the top-octave error still exceeds the gate, switch the tank reads to 6-point Lagrange
(−0.06 dB at 9.6 kHz) — ~10 more mults/line/sample, trivial here.]*

> **AMENDED 2026-08-07 — `|Hi(f_h)|_mean` is tabulated over §5.1 axis 4's kernel blend,
> not as one scalar.** The era knob fades the line read from cubic toward linear
> (§5.1), and the blended read is itself a 4-tap FIR on the cubic's tap frame, so the
> same mean-magnitude integral applies to it exactly. `configure()` tabulates 17 blend
> steps `c ∈ {0, 1/16, …, 1}` at `f_h`; `updateBlockRamps()` lerps the table at the
> live `colorInterp` and the `r_hi` solve divides by that — so §3.4 keeps owning the
> per-band decay at every colour, which §8's colour-1 per-octave row gates. Entry 0 is
> the pre-amendment scalar to the bit. One caveat, recorded: in a frequency-flat
> configuration (`T60_high = T60_mid`) `r_hi` sits at its `1 − 1e-4` clamp and no HF
> solve — compensated or not — has headroom to act; the compensation is live wherever
> `reverb_damping` or `hfMaxT60` makes `r_hi < 1`, which includes the 0.5 default.

`hfMaxT60` is Costello's physical ceiling and is a hard per-mode cap.

> **AMENDED 2026-08-07 — tonal correction: per-band tail LEVEL is decoupled from
> per-band decay TIME.** In an FDN the tail's integrated energy per band is proportional
> to that band's T60 — steady-state wet power is `∫h_band²` by convolution — so the solve
> above makes `reverb_bassmult = 4` sit ~+6 dB hotter in the low band and
> `reverb_damping = 1` ~4–5 dB duller in the high band *in level*, on top of the
> decay-time change the knob asks for (Jot; Prawda–Schlecht–Välimäki DAFx 2020 pair
> "tonal correction filters" with the absorption filters for exactly this reason). That
> coupling is why the two knobs read as boomy or dull away from their centres. Two
> first-order wet-bus shelves in `outputBus()` — after the output DC block, before the
> spread, in the tilt's shelf idiom, at the mode's own `bassXoverHz`/`hfXoverHz` corners
> — hand the level back: amplitude ∝ √T60, so
>
> ```
> gCorrLow  = (T60_mid / T60_low)^(0.5·tonalCorrect)
> gCorrHigh = (T60_highRef / T60_high)^(0.5·tonalCorrect)
> T60_highRef = max(0.05, min(0.75·decay, hfMaxT60))          // T60_high at damping = 0.5
> ```
>
> both clamped to [0.35, 2.8], solved per block where the Filt1 targets already are. The
> HF gain is solved against the NEUTRAL-KNOB reference — `T60_high` at damping's 0.5
> default — not against `T60_mid`: the `hfMaxT60` air-absorption ceiling then cancels
> between numerator and denominator, so the default preset stays neutral, the correction
> can never fight the ceiling with a boost, and the clamp is a safety net rather than the
> operating point. `tonalCorrect` is a per-mode amount (§4.1); Spring and Reverse take 0
> and SKIP the filters rather than run them at unity — a one-pole shelf at gain 1 still
> computes `state + (x − state)`, which is not bit-transparent in float, and §3.12 gate 6
> asserts damping/bassmult bit-inert in Reverse. Two recorded caveats: (a) the ER sum is
> already on the wet bus, so the shelves rescale Early Energy too, whose level does not
> follow the T60 energy law — accepted because the ref-solve keeps the correction small
> near the defaults (the alternative, a tank-only correction ahead of the ER sum, would
> move the filters inside `processTank`); (b) the first tank pass reaches the output taps
> before the Filt1 stages have acted, so that early chunk follows neither knob and is
> rescaled all the same — the share it carries is what Hall's reduced amount in §4.1 is
> tuned against. Gated by §8's per-octave tail-LEVEL row.

### 3.5 In-loop allpass and soft clip

Each line carries ONE modulated 2-multiply Schroeder allpass **inside** the feedback
path — the thing Freeverb structurally cannot do — so echo density grows with time.

**`apG` is capped at 0.60 for every mode.** A Schroeder allpass of coefficient `g` has
group delay ranging from `m(1+g)/(1−g)` at DC to `m(1−g)/(1+g)` at its notches. At
`g = 0.72` that is a 6.1:1 range, and since we fold the allpass delay into the scalar
`D_i` (zita's approximation, not strict Dahl-Jot), `D_i` is only the frequency-*averaged*
loop delay. The averaging period is `fs/m` ≈ 55 Hz for a Plate-length allpass, so below
~150 Hz individual modes see loop delays varying 6:1 and therefore T60s varying 6:1 —
the classic "allpass in the loop booms in the bass" artefact, which `reverb_bassmult`
cannot fix because it is a per-mode spread, not a band tilt. At `g = 0.60` the range
drops to 4:1. Plate's metallic edge comes from `apRatio` and `inDiffG` instead. Gate:
the per-octave T60 assertion extends down to the 63 Hz and 125 Hz bands (§8).

*[If Plate ever genuinely needs higher `apG`, implement the strict Dahl-Jot absorbent
allpass — attenuation and lowpass inside the allpass's own feedback rather than folding
its delay into `D_i` — which makes decay frequency-flat by construction.]*

The in-loop saturator must be C¹ continuous. The first draft's `x/(1+0.3|x|) above 0.8`
jumps by 0.155 at its own threshold — a step generator inside a feedback loop. Use:

```
softClip(x) = x                                            for |x| ≤ t
            = sign(x)·( t + (|x|−t) / (1 + k·(|x|−t)) )    for |x| >  t
```

with `t = 0.8` and `k = inSat` (driven by `reverb_color`, §5). Value and slope both
match at `t`. It is **bypassed during Freeze** (§3.10), or "loop gain exactly 1.0" is a
lie for any tail above the threshold.

> **AMENDED 2026-08-07 — the tank clip is evaluated through first-order ADAA on its
> RESIDUAL, not pointwise.** `softClip` runs on every line every sample inside the
> recirculating loop, and §2 forbids oversampling anywhere: every harmonic it generates
> above Nyquist folds into the band, re-enters the loop and re-aliases — Costello's
> ~300-traversal rule, the same compounding §3.4 solves for the interpolator. ADAA is
> the one aliasing reduction available under a hard no-oversampling, zero-latency
> constraint, and the clip's antiderivative is closed form. The FORM matters: naive
> ADAA1 of the full function reads `(x + x_prev)/2` below the knee — a 2-tap averager
> with a Nyquist null imposed on the whole loop signal even where the clip never
> engages, i.e. unbudgeted HF damping on top of the solved Filt1 law and a lie
> everywhere this document says the linear region is exactly neutral (colour 0,
> Freeze's "clip bypassed" recipe). So the ADAA runs on the residual
> `r(x) = softClip(x) − x`, identically 0 below the knee, and the output is
> `y = x + (F_r(x) − F_r(x_prev)) / (x − x_prev)` with
>
> ```
> F_r(u) = u/k − log1p(k·u)/k² − u²/2,   u = |x| − t     (even; 0 below the knee)
> ```
>
> When both samples sit inside the knee the path early-outs to EXACT identity — the
> linear region is bit-identical to the pointwise clip and costs nothing. The divided
> difference falls back to `r(midpoint)` under `|Δx| ≤ 1e-3` — 1e-3 and not
> machine-small, because a near-machine divisor amplifies the float rounding of the
> antiderivative difference into broadband error; `F_r = O(u³)` at the knee keeps both
> branches tiny at the crossover, and softClip's C¹ knee is inherited by `F_r'` so the
> ADAA form stays C¹. One float of state per line (`clipPrev`), cleared in
> `resetState()`; renders stay bit-identical run to run. ADAA1's half-sample smoothing
> applies to the residual only — reported latency stays exactly 0 (§2's first
> invariant), and half a sample inside a 1381-sample loop is not a reportable quantity.
> The two single-pass saturators stay pointwise, deliberately: Spring's is Dwell (§3.9),
> an input-side `tanh` ahead of the dispersion that never recirculates, and Reverse's
> colour clip (§3.12) feeds a pure FIR tap table — their aliasing is rendered once, not
> compounded, which is the whole case for ADAA. §3.2's per-line schematic
> `w_i = softClip(s_i)` is realised as `softClipAdaa(s_i)` since this amendment. Gated
> by §8's clip-aliasing row: before → after on the folded product comb, −83.0 → −91.0 dB
> re the fundamentals, and −87.2 → −102.4 dB for folds landing below fs/4.

### 3.6 Modulation

Two layers, both scaled by the single `reverb_mod` knob.

**Layer A — per-line random walk on the tank reads** (Costello's `reverbsc` scheme).
Every `fs/rate_i` samples, draw a new target within `±depth_i` from a fixed-seed LCG and
glide the read pointer linearly to it. Constant Doppler within a segment, stepping at
boundaries — a random staircase of detunes rather than a coherent warble.

```
rate_i  = modRate  · (0.6  + 0.8·r_i)       // r_i, r'_i from a seeded LCG, fixed at prepare
depth_i = modDepth · (0.85 + 0.3·r'_i)
```

**Layer B — quadrature sine on the in-loop allpass delays**, phases `2πk/8`, half Layer
A's rate, 40% of its depth. This is the distributed-shallow-modulation conclusion both
Dattorro and Costello (Hall1984) reach. Layer B is **disabled per line whenever
`apSamples_i < 15 ms`** — modulating allpasses of the same length class as the input
diffusers is exactly what the design refuses to do to the diffusers. In practice that
means Layer B is off for Room entirely, partly on for Plate, on for Hall. The allpass's
own fractional read uses the same 4-point Hermite. Layer B inherits **both** depth laws.

**Both scaling laws apply to both layers:**

```
depth_eff = depth_base · sizeScale · clamp(1.5 / T60_mid, 0.25, 1.0)
```

`× sizeScale` because a fixed sample depth is a larger fractional detune on a short line.
`× clamp(1.5/T60)` because detuning compounds per loop pass — this IS the Chamber1979
"seasick modulation" fix.

**Two-stage `reverb_mod` law** (Strymon Cloud's, documented): 0 → 0.6 raises depth 0 → 1
at fixed rate; 0.6 → 1.0 holds depth and raises the rate by ×1 → ×4.571 on the mode's own
base rate. One knob, two axes.

> **AMENDED 2026-08-06 — the published 0.35 → 1.6 Hz range is applied as a RATIO, not as
> absolute endpoints.** As absolute values it contradicts §4.1, whose per-mode `modRate`
> is 0.60 / 0.90 / 0.30 Hz: Plate's 0.90 Hz would jump *down* to 0.35 Hz at the knob's
> midpoint and the law would be discontinuous at 0.6. The ratio `1.6/0.35 = 4.571` keeps
> both the published span and §4.1's per-mode voicing, and is continuous at the knee.

**Negative-delay guard in both places** — this exact bug shipped in ValhallaRoom 1.0.8.
Clamp `depth ≤ (minLineSamples − 4)/2` at parameter-set time, AND clamp the read index
at read time.

**Why not `juce::dsp::DelayLine`:** confirmed from source (`juce_DelayLine.h:318-320`)
that `delay`, `delayInt`, `delayFrac` and `alpha` are scalar members while only
`readPos`/`writePos`/`v` are per-channel — an 8-line FDN would need 8 objects and 8
`AudioBuffer` allocations, with integer `% totalSize` per sample per line. Its Thiran
mode is additionally unsafe to modulate: the allpass state `v[channel]` is never reset
when the delay changes, so per-sample modulation means a transient every sample.

> **AMENDED 2026-08-07 — Layer C: a time-varying Givens layer inside the mixing
> matrix, `reverb_mod`'s third layer and the only one that moves no delay.** A static
> FDN rings the same discrete mode set on every excitation — the "metallic on
> sustained material" signature (Schlecht & Habets, JASA 2015) — and the standard fix,
> deeper delay modulation, is precisely what tonal material cannot tolerate. In this
> engine the delay layers are thinnest exactly where the metal is worst, by
> construction: Layer B's 15 ms gate turns it off for Room entirely, and the
> negative-delay guards scale Layer A's depth to the shortest line, so modulation
> thins out at small Size. Layer C is immune to both mechanisms, because it modulates
> the MIXING, not the delays: four 2×2 rotations on pairs (0,1)(2,3)(4,5)(6,7)
> between the butterflies and the write-back (see §3.2's amendment of the same date),
> each pair at angle
>
> ```
> θ_p(t) = rotMax · depthScale · sin(2π · mul_p · rateHz · t),   mul = {0.079, 0.113, 0.173, 0.281}
> (c,s)  = (1, θ)·rsqrt(1 + θ²)        // c²+s²=1 for ANY θ: exactly orthogonal, no table
> ```
>
> driven by one quadrature rotator per pair (Layer B's scheme: increments per block,
> advanced per sample, renormalised per block). `rotMax` is per mode (§4.1): Room
> takes the deepest rotation — it is the mode with neither a full Layer A nor any
> Layer B — Plate and Hall take less, and Spring/Reverse never reach the tank mixer.
>
> **This evades, and does not reopen, the recorded Room decision above.** "Layer B is
> off for Room entirely" was reasoned from modulating sub-15 ms allpasses — the same
> coloration the design refuses to inflict on the input diffusers — and that
> reasoning stands unamended: Layer C touches no allpass, moves no read pointer,
> interpolates nothing and detunes nothing. It gets Room the missing liveliness by
> varying WHICH mix of lines each line hears rather than WHEN it hears it. For the
> same reason §3.6's depth laws deliberately do NOT apply: no `× sizeScale` (a
> rotation has no fractional-detune to scale) and no `× clamp(1.5/T60)` (nothing
> compounds per pass — the layer is norm-preserving at any angle at any rate, which
> is also why there is no negative-delay guard to inherit). Only `depthScale` rides
> along, so one knob drives all three layers and `reverb_mod = 0` skips the layer
> outright — the mixer is bit-identically the static `Â`, skipped rather than run at
> angle zero for the shimmer write-back ternary's reason (a `-0.0f` through an
> angle-zero rotation can come back `+0.0f`).
>
> **The pair rates sit an octave-and-a-bit under the delay layers', deliberately
> (slow-and-deep).** The rotation's residual contribution to the wet's instantaneous
> frequency is the rate of arg-response wobble, which at fixed depth scales with the
> angular RATE — measured on §8's pitch-stability row, Room's ungated notch-transit
> statistic read 19.7 cents with these multipliers at ×4 (Layer-B territory) and the
> gated pitch statistics sit at the delay-layer baseline at the shipped values
> (Room level-weighted rms 1.445 → 1.489 cents; worst loud-frame hop 3.815 → 3.103,
> i.e. DOWN). Depth buys the mode redistribution; rate is what the pitch row taxes —
> the opposite trade from a delay modulator, where depth and rate both feed Doppler.
> The four multipliers are incommensurate with each other and with Layer A's
> `(0.6 + 0.8r)` and Layer B's 0.5, so the mixing never settles into a repeating
> pattern the tail could ring along with.
>
> **One placement invariant is load-bearing:** the Shimmer Regen tap (§3.11) reads
> `w[0]` BEFORE the rotation on pair (0,1), so the row the tap sees is still
> `sum(w)/√8` — the reference §3.11's gain bound is stated against — and a Shimmer
> render at `reverb_shimmer = 0` stays bit-identical to Hall whatever the rotation
> does. Gated by §8's pitch-stability row.

### 3.7 Early Energy

Costello's ValhallaRoom finding: the Early section has NO allpasses, which is why its
Diffusion can be maxed without ringing. For a guitar — a pulse train through distortion,
more pulse-like than the male vocals he names as the problem case — this matters more.

- One mono tapped delay line, **24 taps**, spaced so density grows quadratically
  (`t_k` spacing ∝ 1/t) from 3 ms to `erWindowMs`, non-arithmetic, seeded.
- Gains `a_k ∝ t_k^(−erEnvExp)`, alternating sign, ±10% seeded jitter. `erEnvExp` is
  0.5 for Hall (Griesinger: flat to ~150 ms, *then* exponential) and 1.0–1.2 elsewhere.
- **Per-tap equal-power pan** from the same seeded LCG — this, not a matrix, is where
  the width comes from (Moorer's approach).
- Then **two short series allpasses at `g = 0.5`** (6.9 / 11.3 ms × sizeScale) after the
  tap sum. The ringing Costello warns about comes from HIGH-`g` allpasses on a pulse
  train; `g = 0.5` at those lengths is inaudible and it is what lifts echo density from
  ~190/s (sparser than the Freeverb we are replacing) to the ~1000/s the gate requires.

The first draft proposed a 4×4 Hadamard on group sums and called it "energy spreading".
It is not: the Hadamard plus a fixed 2×2 rotation contributes exactly four scalar
weights, so the result is still a 24-tap FIR of the input with gains `c_{g(k)}·a_k` — a
rank-4 rescaling that does not increase tap count, decorrelate, or raise density. Dropped.

ER feeds the output at `0.60·reverb_erlevel` **and** the tank input at `erToTank`
(Gardner: driving the IIR from the FIR output raises tank-input density for free). The
tank gain is FIXED, so turning Early up never starves the tail.

**AMENDED 2026-08-06 — the tap line is fed from the diffuser output, not from the
pre-delay output.** Both routings were built and measured (Room, 44.1 / 48 / 96 kHz):

| ER tap line fed from | ER-only density @ 60 ms | full IR reaches 0.95 | tail minimum |
|---|---|---|---|
| the diffuser output (shipped) | 0.963 / 0.944 / 0.890 | 66.9 / 64 / 74 ms | 0.933 / 0.948 / 0.931 |
| the pre-delay output (as drawn) | 0.635 / 0.599 / 0.624 | 74.9 / 100 / 77 ms | 0.907 / 0.880 / 0.887 |

The pre-delay feed misses §8's ER-isolation row at every rate and takes Room's headline
echo-density rows down with it — the one defect §1.1 names as the reason this engine
exists. What the *tank* sees is identical either way, because the diffuser cascade and
the ER FIR are both LTI and therefore commute:
`D(rail) + erToTank·ER(D(rail)) = D(rail + erToTank·ER(rail))`. The whole difference is
whether the DIRECT wet ER carries the diffusers, and the density numbers above say it
should. Costello's rationale — no allpasses in the Early section, so pulse-like sources
do not ring it — is kept as a *measured* claim rather than a structural one: the probe
now runs §8's spectral-flatness row on the isolated ER path and gates what the diffusers
ADD to it as `reverb_diffusion` sweeps 0 → 1, where they reach `g = 0.78`. Worst peak
above the local median at 44.1 kHz, diffusion 0 → 1: Room +7.58 → +9.26 dB, Plate
+7.50 → +10.58 dB, Hall +5.21 → +5.49 dB. The absolute figure is the 24-tap FIR's own
comb — Plate's ER window is 18 ms — which is why the *increase* is the gated quantity,
bounded at 4 dB against a shipped worst case of +3.08 dB (Plate, 44.1 kHz). For scale,
the full IR's spectral flatness, which §8 gates at 10 dB, reads +4.09 dB for Plate.

### 3.8 Stereo and decorrelation

**One shared tank, two decorrelated output tap sets** (Dattorro's method, not two tanks —
reading a delay buffer at a second offset is free, the samples are already there).
Offsets are fixed fractions of each line's length, so they track Size automatically.

| | line | frac | sign | | line | frac | sign |
|---|---|---|---|---|---|---|---|
| L | 1 | 0.081 | + | R | 0 | 0.107 | + |
| L | 3 | 0.293 | + | R | 2 | 0.331 | + |
| L | 5 | 0.577 | − | R | 4 | 0.519 | − |
| L | 7 | 0.771 | + | R | 6 | 0.742 | + |
| L | 0 | 0.412 | − | R | 1 | 0.386 | − |
| L | 2 | 0.634 | − | R | 3 | 0.691 | − |
| L | 4 | 0.913 | − | R | 5 | 0.874 | − |

Each tap scaled 0.35. L draws its four "own" taps from odd lines and R from even —
the deliberate cross-asymmetry that makes the outputs genuinely uncorrelated rather than
delayed copies. Combined with the `+ + − − + − + −` injection pattern and the 0.70/0.30
crossfeed, decorrelation starts at sample 0.

**Frequency-dependent L/R spread (Plate, Hall).** Costello on EMT140 plates: "for high
frequencies, the pickups may only be a millisecond apart; for low frequencies, there can
be a difference of 15 or more milliseconds." Implemented as **two differently-tuned
cascades of three 2nd-order allpasses, one per channel** — ~7 ms group delay at 100 Hz
on L and ~13 ms on R, both falling to ~1 ms above 4 kHz. A one-sided chain (the first
draft) would pull the image left on every LF transient, because L's bass would always
arrive 12 ms early; splitting it costs the same ~9 multiplies and makes neither channel
"the delayed one". Off for Room, Spring and Reverse.

**Mono-sum safety.** Because the two tap sets are different *delays*, not a fixed
offset, L+R loses ~3 dB smoothly with no periodic notches. Gated in §8.

**Mono host (`channels == 1`).** Input = ch0 (the crossfeed collapses to identity);
output = the **L tap set only**, not `0.5·(L+R)`, which would reintroduce exactly the
comb the two-tap-set design exists to avoid. `width` is ignored. Cheaper and correct.

### 3.9 Spring dispersion (Spring mode only)

Replaces the input diffusers; only 3 tank lines active.

```
Dwell:   pre-emphasis (+6 dB shelf @ 2 kHz) → tanh(drive·x) → de-emphasis
         (harmonics generated BEFORE dispersion get chirped too — why real tanks
          sound the way they do)

per spring s ∈ {0,1,2}:
  A_M(z) = Π over M=100 of  [a1 + A_fd(z)·z^−K1] / [1 + a1·A_fd(z)·z^−K1]
           A_fd(z) = (a2 + z⁻¹)/(1 + a2·z⁻¹),   K1 = round(K) − 1,
           a2 solved from frac(K)
  a1 = 0.63
  K  = fs / (2 · chirpFcHz),  chirpFcHz = 4200          ← NOT a fixed integer
  → tank line s,  Td_s = {51.3, 56.1, 61.7} ms
  loop: one-pole LP 2.8 kHz + one-pole HP 90 Hz  (the real circuit's rumble/feedback
        high-pass — the first half of BigSky's Low End range)
  loop gain tuned to ≈ −5.4 dB per pulse
```

`K` **must** be derived from `fs`. Fixing `K = 5` (the first draft) puts the chirp knee
near `fs/2K`: 4.8 kHz at 48 kHz, but 4.4 kHz at 44.1 and 9.6 kHz at 96 — the mode's
entire identity would change with the host rate, in a design whose stated rule is that
nothing is stored in samples. At 48 kHz the corrected `K = 5.71`; re-fit `a1` against
the KA-1210 data at the corrected `K` during Stage 2.

Capping `M` at 100 (from the unconstrained 318) was shown perceptually indistinguishable
— 300 first-order allpasses total, ~300 multiplies/sample.

### 3.10 Freeze

The full recipe, not JUCE's half:

- Loop gain exactly 1.0 (`g_mid = 1` on every line).
- **Damping filters flattened** — at unity gain, an in-loop LP still removes HF every
  pass, and the tail dulls to a bass drone over ~15 s.
- **Modulation off and delays snapped to integer reads** — fractional interpolation is
  itself lossy, so a modulated frozen tail decays anyway.
- **Soft clipper bypassed** — otherwise any tail above `t` keeps decaying toward it.
- Input crossfaded to zero over 50 ms (an instant mute leaves a step in the loop).
- **One-shot DC removal**: during that 50 ms crossfade, subtract each line buffer's
  running mean, so the frozen state starts DC-free.

**No 25 Hz HP goes into the loop.** The first draft justified one by "DC accumulates
without bound", but the input is muted at freeze, so nothing new enters, and with a
unitary matrix at unity gain the already-injected DC is bounded (marginally stable), not
divergent. Meanwhile a 25 Hz one-pole HP is `|H| = 0.970` at 100 Hz — 3% loss per pass,
~25 passes/s for Room, so 100 Hz is gone in ~5 s and the frozen tail becomes a *thin
treble drone*, the mirror image of the failure it was meant to prevent, and it would
fail the Stage-4 spectral-centroid gate. If a residual leak is ever wanted, use 2 Hz
(`|H| = 0.9998` at 100 Hz, 0.5 dB over 30 s).

### 3.11 Shimmer

One granular shifter with a 3-way routing switch, plus mandatory in-loop damping.

- **4 overlapping grains** (not 2), Hann crossfade, randomized grain start. A naive
  1–2 window shifter combs badly on a dense tail; Costello's own shifter "uses
  randomization to avoid the comb filtering artifacts".
- `reverb_shimmer_interval`: `{−1 oct, +P5, +1 oct, +oct+5th}`, FROZEN, default `+1 oct`.
- Routing: **Input and Both are feed-forward sends that never re-enter the regen path.**
  Only Regen carries the hazard.
- In-loop one-pole LP forced ≤ 4 kHz **after** the shifter, plus a **hard limiter** (not
  a soft clip) on the shifter output.
- **Gain constraint, enforced in code:** `shimmerGain ≤ α·(1 − g)` with `α = 0.5`,
  enforced **per block against the live loop gain** — `g` is a function of `reverb_decay`,
  which is a live parameter, so freezing it at prepare would mean freezing it at the
  largest `g` the decay range allows and running that worst-case bound at every decay
  setting. Per-block is strictly stronger (the bound holds at the `g` actually in force,
  not just at the ceiling) and costs one `max()` in a loop that already solves every `g`.
  Applied as a **scale** on the knob, `shimmerGain = knob·α·(1 − g)`, not a clip of it,
  so the knob stays live and monotonic at long decays (§5.2). The first draft's
  `g·|H_damp(2ω)| < 0.97`
  is a single-hop test with no shimmer-level term at all: energy at 100/200/400/800/1600
  Hz all lands in the 2–4 kHz band within a few passes, so that band accumulates five
  source octaves even when every individual hop is under 0.97.

### 3.12 Reverse

The Lexicon 224 nonlinear/reverse lineage, not a time-reversed capture. Recording a
window and playing it backwards is a window of latency *by definition* and breaks the
first invariant; what the hardware actually does — and what every zero-latency pedal
implementation since has done — is a **diffused tapped-delay FIR whose tap gains RISE
with tap time**. Each input sample spawns a crescendo of echoes that stops dead at the
window's end. There is no recirculation: Reverse replaces the tank (and idles the Early
Energy section — the window IS the multitap), while reusing the front end verbatim: DC
block, send HP/LP, pre-delay, and the 4 input diffusers under the same
`g = inDiffG[k] × (0.15 + 0.85·reverb_diffusion)` law. Diffusion matters more here than
in any tank mode: an under-diffused rising tap train reads as discrete slapbacks, not a
swell.

**Window.** `W = min(reverb_decay, 1.5 s)` — the mode's decay *ceiling* is 1.5 s
through the same ceiling mechanism as §3.3, and the parameter's own 0.2 s floor bounds
the window from below, so `W ∈ [0.2, 1.5] s` (inside the sensible 0.15–1.5 s band).
The UI shows the clamped value and the knob is relabeled **WINDOW** (§5.2). Longer
would demand a proportionally longer buffer for an effect whose illusion — a tail
sucked backwards into the note — collapses past ~1.5 s; shorter than ~0.15 s is a
flam, not a reverse.

**The window line.** One mono line, written with `0.5·(a+b)` from the diffuser output
(the even/odd `a`/`b` split is a tank concept), 1.5 s + 4-sample mirrored guard,
allocated unconditionally at `prepare()` — a mode switch must never allocate (§6.2).

**Taps.** 64 — enough that the densest region approaches the ER section's smoothed
density, with the two density allpasses below closing the rest of the gap. Positions
are fixed fractions of `W` (they scale with the window; nothing is stored in samples),
spaced by §3.7's density law *time-mirrored* — cumulative tap count ∝ `(t_k + 0.45·W)²`,
i.e. spacing ∝ `1/(t_k + 0.45·W)`, so echo density grows exactly where the envelope
grows — non-arithmetic, seeded, ±jitter from the fixed-seed LCG. Gains are the
time-mirror of an exponential decay with `T60 = 1.75·W`:

> **AMENDED 2026-08-06 — the regulariser is a fraction of `W`, not a literal 10 ms, and
> the mirror direction is the sentence's, not the formula's.** The first draft wrote the
> spacing as ∝ `1/(W − t_k + 10 ms)`, which puts the SPARSEST taps at `t = W` — a 180 ms
> hole at the loudest point of the swell, contradicting the same sentence's "density
> grows where the envelope grows" and measuring −49.7 dB on gate 1a. Flipped to match the
> sentence but keeping the literal 10 ms, the hole moves to `t = 0` and the envelope
> collapses 49.7 dB below its running maximum at 0.171 s. Both are the same defect: 10 ms
> against a 1.5 s window is a 151:1 spacing ratio across only 64 taps, while §3.7's ER law
> — which this one mirrors — spans a bounded 14:1 to 26:1 because it runs from a real 3 ms
> floor. `0.45·W` gives the mirror the same bounded shape at every window length (the last
> gap stays 3.2× tighter than the first) and is the largest fraction at which gate 1b's
> 40 dB fall still holds (45.6 dB at 0.15·W, 41.3 dB at 0.45·W, 37.1 dB — failing — at
> 0.60·W, worst case over 44.1 / 48 / 96 kHz). It also keeps the positions fixed fractions
> of `W`, which an absolute 10 ms does not.

```
a_k = 10^(−3·(W − t_k) / (1.75·W))       // −34.3 dB at t = 0, rising to 0 dB at t = W
```

with alternating sign, ±10% seeded gain jitter, and the whole set normalised so
`Σ a_k²` is window-invariant (equal wet loudness at every `W`). *[Inferred: the
`1.75·W` mirror constant (≈ 34 dB rise) is a listening decision against the 224
lineage; tune within 28–40 dB at Stage 1. The gates below are deliberately
rise-agnostic.]* Per-tap **equal-power pan** from the seeded LCG — §3.7's law; this,
not a matrix, is the stereo image.

**Density allpasses.** Two series `g = 0.5` allpasses per channel, cross-tuned —
L: 6.9 / 11.3 ms, R: 7.9 / 10.3 ms, × sizeScale — the same length-and-`g` class §3.7
argues is inaudible on its own. Being true allpasses they add ring, not comb: the
mono-sum gate applies unchanged.

**Interpolation.** Tap reads are integer, or 2-point **linear** when modulated. §3.4's
cubic mandate exists because interpolation loss compounds per loop pass; a single-pass
FIR traverses each read exactly once, so linear is transparent here and 64 cubic reads
would triple the mode's cost for nothing.

**Modulation.** `reverb_mod` keeps its two-stage law and drives **8 shared Layer-A
random walkers**; tap `k` adds walker `k mod 8`'s offset to its read position. 64
independent walkers would be 8× the RNG work for no audible gain, and the `mod 8`
assignment decorrelates *adjacent* taps, which is where coherent motion would be
heard. `depth_eff = depth_base · (W / 1.5)`: §3.6's `1.5/T60` compounding clamp is
inapplicable — nothing recirculates — but a fixed sample depth over-detunes a short
window exactly as it over-detunes a short line, hence the same shape of scaling. Base
depth 0.30 ms, base rate 0.50 Hz. Both §3.6 negative-delay guards apply.

**Parameter map — nothing silently inert (§5.2):**

| param | in Reverse |
|---|---|
| `reverb_decay` | The window `W`; relabeled **WINDOW**; retunes via the deferred path below. |
| `reverb_size` | Unchanged meaning: sizeScale on the diffusers and density allpasses (0.60 … 1.40). |
| `reverb_diffusion`, `reverb_predelay`, `reverb_lowcut`, `reverb_highcut` | Unchanged (shared front end). |
| `reverb_mod` | Unchanged law; walker-modulated tap reads (above). |
| `reverb_color` | Axes 2 and 3 of §5.1 as written (pre-window saturation, quantised/correlated walkers); axis 1's in-loop LP becomes a one-pole LP on the window-line input, same 18 k → 6 kHz law. |
| `reverb_mix`, `reverb_width`, `reverb_tilt`, `reverb_duck` | Unchanged (wet-bus, mode-agnostic). |
| `reverb_damping`, `reverb_bassmult`, `reverb_erlevel` | **Inert, greyed in the UI.** No recirculating decay to shape, no separate ER bus. Greyed is §5.2's rule; asserted bit-inert by gate 6 below. |
| `reverb_freeze` (Stage 4) | Inert, greyed — nothing recirculates to hold. Recorded here so Stage 4 does not invent one. |

**Stereo.** Per-tap pan decorrelates L and R from the first tap; the cross-tuned
allpass pairs decorrelate the ring; `plateSpread` is off; `width` is the usual
wet-only M/S. Mono host (§3.8): input = ch0, taps sum at unity pan, L allpass pair
only.

**Window retune — deferred, never a live memset.** A `W` change moves all 64 tap
positions and re-normalises the gain set; done live that is 64 simultaneously swept
combs plus a loudness step. So a `reverb_decay` edit in Reverse latches §6.2's
deferred reconfigure verbatim: 60 ms wet fade-out → `reconfigurePending` →
`pendingReset` → `resetStep()` (tap table rebuilt on the first step; the window line
cleared in tank-line-sized chunks, §6.3) → 60 ms fade-in, with `snap` (§6.4)
suppressing any glide on the first block after. A knob drag latches the newest value
and rebuilds once at fade end — one fade cycle per gesture, not one per delta.
Entering or leaving Reverse rides the same path, as any mode change does.

**Cost.** Memory: the 1.5 s mono line + guard — 281 KB @48 kHz, 1.13 MB @192 kHz
(§6.7). CPU: 64 taps × (linear read + 2 pan MACs) + 4 density allpasses + the shared
front end ≈ 330 ops/sample — the same class as the tank + ER path it displaces, far
under Spring (§6.8).

**Verification gates** (Stage 1, in `runReverbTests` + `tubamp_verbprobe`, run at
44.1 / 48 / 96 kHz):

1. **Rising envelope** — the 20 ms-Hann RMS envelope of the wet IR's stereo power
   `√((L²+R²)/2)` rises to `W`: **no point within 20 dB of the peak more than 2 dB below
   its running maximum (gate 1a), and no point at all more than 8 dB below it (gate
   1a′)**; then it falls ≥ 40 dB within 150 ms (gate 1b). The 11.3 ms `g = 0.5` allpass
   reaches −60 dB in ≈ 113 ms, so the stop is measurably hard.
   > **AMENDED 2026-08-06 — the first draft's single 1 dB bound on the L channel is
   > unreachable at this section's own 64 taps, and the tap count is the constant §6.8's
   > cost budget depends on.** 64 taps over `W = 1.5 s` is a 23.4 ms mean gap, wider than
   > the 20 ms analysis window, so *any* 64-tap layout ripples. Modelled independently of
   > the engine (this tap table, the ±10 % gain jitter, alternating signs, per-tap pan,
   > the two density allpasses), the worst dip on the stereo power is −1.53 dB for a
   > perfectly uniform layout and −8.58 dB for the density law; reaching 1 dB needs
   > 256–512 taps, 4–8× the count and the same multiple on the mode's cost. The measure
   > moves from L alone to the stereo power for the same reason gate 2 already integrates
   > both channels: this section pans every tap by an independent equal-power draw, so one
   > channel's envelope carries that draw's variance (−8.9 dB against −3.3 dB on the same
   > render). Both halves still discriminate: the literal mirror direction rejected above
   > measures −15.5 dB on gate 1a, and both literal-10 ms regularisers measure −78 and
   > −83 dB on gate 1a′. Engine, 44.1 / 48 / 96 kHz: −1.25 / −1.34 / −1.37 dB (1a) and
   > −3.34 / −3.56 / −3.38 dB (1a′).
2. **Energy centroid** — `Σ t·h²(t) / Σ h²(t)` of the wet IR lands in the last 40% of
   `[predelay, predelay + W]`.
3. **Hard stop** — wet output ≤ −80 dBFS for every `t > predelay + W + 18.2 ms + 150 ms`.
4. **Window tracking** — the measured stop point follows `predelay + W + 18.2 ms` within
   5% of `W` at `reverb_decay` = 0.4 / 0.9 / 1.5 s, and is pinned at 1.5 s for any larger
   decay.
   > **AMENDED 2026-08-06 for gates 3 and 4 — the expected stop time includes the density
   > allpasses' group delay, exactly as it already includes the pre-delay.** The two
   > series `g = 0.5` allpasses sit AFTER the tap sum, and a Schroeder allpass of delay
   > `m` has a mean group delay of exactly `m`; the cross-tuned pairs are chosen so both
   > channels sum to the same 18.2 ms (L 6.9 + 11.3, R 7.9 + 10.3), scaled by `sizeScale`.
   > It is a fixed path delay, not window-tracking error, and not counting it was the
   > whole of both misses: gate 3 read −76.4 / −77.3 / −75.5 dBFS and gate 4 read
   > +5.8 / +5.7 / +5.2 % at `W = 0.4 s` before the correction, and −84 … −87 dBFS and
   > +0.1 … +2.0 % after it. The −80 dBFS and 5 % thresholds are untouched.
5. **Mono sum** — no notch deeper than 6 dB (§8's gate; the per-channel chains are
   allpass).
6. **Asserted inertness** — damping, bassmult and erlevel at min vs max render
   bit-identically in Reverse.

---

## 4. Mode set

**Six entries, every one a different machine.** `reverbAlgoChoices` is FROZEN and
**declared in full at Stage 1**, so `AudioParameterChoice`'s normalisation divisor
(`index / (numChoices − 1)`) never changes and no recorded automation lane is ever
repointed. Unshipped modes fall back at the DSP level to their nearest shipped relative
and are **greyed out in the UI combo** — a visibly disabled menu entry is not the
"silently does something else" failure that argues against declaring modes early.

```
0  Room    (default)          3  Spring
1  Plate                      4  Shimmer
2  Hall                       5  Reverse
```

Spring shipped at Stage 2 and Shimmer at Stage 3, so from this pass on **every entry is
live** and no fallback path remains: the greying and the Spring→Plate / Shimmer→Hall DSP
fallbacks are gone, including `ParamMenu`'s `unavailable` / `unavailableHint` props, which
had no caller left (2026-08-07). The rule above stays documented because it is what let
the choice list be declared in full at Stage 1 — a future frozen list that ships in stages
re-adds the disabled-entry rendering rather than shipping a menu entry that lies.

There is no Legacy entry. Freeverb is deleted (user directive, 2026-08-06); Room at
index 0 is both the fresh-tile default and the resolution target for every legacy
preset and session (§5.3). Room, Plate, Hall and Reverse all shipped in Stage 1, so only
Spring and Shimmer were ever greyed, and both are now shipped too.

**Ambience and Bloom are cut** from the mode list and ship as factory presets on Room and
Hall. Ambience was "Room × 0.6 tank, decay clamped, Early weighted high" — a preset, not
a machine — and it was the only consumer of the `density` blend that §3.2 removes. Bloom
was Hall plus one feedback gain, and its swelling envelope is reachable with a volume
pedal or the ducker's release. A menu of eight entries backed by three machines A/Bs as
cosmetic; a menu of six backed by six does not.

### 4.1 Per-mode tuning tables

Sample counts at 48 kHz are authoritative; ms = samples/48.

**Room** — Σ = 15,990 samples = 333.1 ms ⇒ 0.333 modes/Hz.

| line | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| samples @48k | 1381 | 1511 | 1723 | 1877 | 2099 | 2269 | 2447 | 2683 |
| ms | 28.77 | 31.48 | 35.90 | 39.10 | 43.73 | 47.27 | 50.98 | 55.90 |

**Plate** — Σ = 26,264 = 547.2 ms ⇒ 0.547 modes/Hz.

| line | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| samples @48k | 2309 | 2539 | 2819 | 3037 | 3433 | 3691 | 4027 | 4409 |
| ms | 48.10 | 52.90 | 58.73 | 63.27 | 71.52 | 76.90 | 83.90 | 91.85 |

**Hall** — Σ = 67,270 = 1401.5 ms ⇒ 1.401 modes/Hz (5.6× Freeverb).

| line | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|---|
| samples @48k | 6007 | 6619 | 7309 | 8069 | 8719 | 9421 | 10099 | 11027 |
| ms | 125.15 | 137.90 | 152.27 | 168.10 | 181.65 | 196.27 | 210.40 | 229.73 |

All 24 values verified prime. Primality is a design-time property of the 48 kHz table,
not a runtime invariant (§3.3).

| field | Room | Plate | Hall | Spring |
|---|---|---|---|---|
| `apRatio` base | 0.22 | 0.30 | 0.16 | — |
| `apG` | 0.58 | 0.60 | 0.55 | 0.0 |
| `inDiffG[4]` | .75/.75/.625/.625 | .78/.78/.70/.70 | .70/.70/.60/.60 | — (dispersion) |
| `erWindowMs` | 42 | 18 | 78 | — |
| `erEnvExp` | 1.0 | 0.3 | 0.5 | — |
| `erToTank` | 0.60 | 0.90 | 0.75 | 0 |
| `modDepth` / `modRate` | 0.35 ms / 0.60 Hz | 0.40 / 0.90 | 0.55 / 0.30 | — |
| Layer B | off (ap < 15 ms) | per-line | on | off |
| `rotMax` (Layer C) | 0.30 | 0.15 | 0.10 | — |
| `bassXoverHz` | 450 | 500 | 400 | 250 |
| `hfXoverHz` | 4500 | 5500 | 3500 | 2800 |
| `hfMaxT60` | 1.00 s | 1.25 s | 1.25 s | 0.35 s |
| `plateSpread` | off | on | on | off |
| `sizeMin … sizeMax` | 0.40 … 1.40 | 0.35 … 1.20 | 0.50 … 1.50 | 0.50 … 1.50 |
| Σ at `sizeMin` | 6396 ✓ | 9192 ✓ | 33,635 ✓ | n/a |
| **decay ceiling** | **2.5 s** | **4.5 s** | **10.0 s** | **4.0 s** |
| `inSat` base | 0.15 | 0.20 | 0.10 | 0.50 (Dwell) |
| `k_in` | 0.35 | 0.35 | 0.30 | 0.40 |
| `tonalCorrect` | 0.60 | 1.00 | 0.75 | 0 |
| `colorLpBottomHz` | 6000 | 7000 | 9000 | 6000 |
| `colorInSatTop` | 0.60 | 0.60 | 0.35 | 0.60 |
| `colorInterpTop` | 0.90 | 0.60 | 0.25 | 0 |

> **AMENDED 2026-08-07.** The `tonalCorrect` row is §3.4's tail-level correction amount
> (0 for Spring above, and for Reverse, which skips the filters — see §3.4's amendment
> of the same date). Plate takes the full amplitude ∝ √T60 correction; Room keeps some
> of the classic boom/dull coupling at 0.6 as character. Hall's 0.75 is measured, not
> styled: its first tank pass reaches the output taps before the Filt1 stages have acted
> (lines are 10–19 % of a 2 s decay) and carries roughly twice the low-octave energy of
> the late tail, so the full-strength shelves overcorrect its steady-state band balance —
> +3.6 dB at `bassmult ×0.25` against §8's 3 dB tail-level gate; 0.75 brings both knob
> extremes inside it (worst 2.63/2.76/2.78 dB at 44.1/48/96 kHz).

> **AMENDED 2026-08-07.** The three `color*` rows are §5.1's era columns (see that
> section's amendment of the same date, which supersedes its 2026-08-06 note pinning the
> knob-top endpoints as global constants). One knob, three machine ages: Room darkest /
> grittiest / coarsest interpolator, Plate the 1980s middle, Hall nearly modern with a
> reduced `inSat` top. Shimmer shares Hall's row; Spring and Reverse keep the
> pre-amendment endpoints with `colorInterpTop = 0` — structural, not tuning (a spring's
> era is its dispersion; Reverse's window line is single-pass, where §3.12 already
> declares linear transparent). Reverse has no column in the table above; its `kModes`
> row reads {6000, 0.60, 0} for its window-line LP and pre-window clip exactly as
> before this amendment.

> **AMENDED 2026-08-07.** The `rotMax` row is §3.6 Layer C's depth — the largest
> tangent the Givens layer's angles reach at full `reverb_mod`. The ordering is the
> inverse of the delay layers': Room, the mode with no Layer B and the thinnest Layer
> A, takes the deepest rotation because matrix modulation is the only liveliness lever
> it has left; Plate and Hall already run both delay layers and take less. Shimmer
> shares Hall's row as always, and Spring/Reverse never reach the tank mixer, so their
> `—` is structural rather than tuning. Depth was set on §8's pitch-stability row: at
> 0.30 Room's gated pitch statistics sit at the delay-layer baseline (the layer's
> whole claim), while the ungated notch-transit statistic honestly records the
> spectral motion bought (6.5 → 18.3 cents on frames the moving dip has pulled toward
> the response floor).

`apRatio` is **per line**: `apRatio_i = base × (0.90 + 0.20·r_i)` from the fixed-seed
LCG, chosen so no ratio falls within 1% of a fraction with denominator < 12. A single
shared ratio (Room's 0.22 ≈ 2/9) would make every 9th allpass echo coincide with every
2nd line circulation on all eight lines simultaneously — precisely the low-order
dependency §3.3 claims to avoid, and invisible to a flutter test that only autocorrelates
at tank-line lags.

Decay ceilings derive from `Σ D_i / (0.15·fs)` at `sizeScale = 1.0` including the
allpasses: Room 2.71 s, Plate 4.74 s, Hall 10.84 s, rounded down.

Reverse takes none of the tank fields — no `apRatio`, `apG`, damping bands or `k_in`;
its own constants (window law, tap table, rise, density allpasses) live in §3.12. From
this table it borrows only Hall's `inDiffG` (.70/.70/.60/.60), a `sizeMin … sizeMax`
of 0.60 … 1.40 (scaling the diffusers and density allpasses), and a **window ceiling
of 1.5 s** standing in for the decay ceiling.

---

## 5. Parameters

Existing, **unchanged** id / range / default: `reverb_on`, `reverb_size`,
`reverb_damping`, `reverb_mix`, `reverb_width` (and the `reverb2_*` / `reverb3_*`
mirrors) — all now driving the new engine (size → sizeScale within the mode's
min..max, damping → the §3.4 HF law, mix/width laws verbatim). Every new parameter
below exists ×3, appended at the end of `createParameterLayout()`. The first twelve
rows are the Stage-1 batch, in exactly this order, at `kVersionHint3`; the two shimmer
ids (Stage 3, `kVersionHint4`) and `reverb_freeze` (Stage 4, `kVersionHint5`) follow —
one hint per shipped batch. Stage 2 (Spring) adds no parameters at all.

| id | type / range | default | notes |
|---|---|---|---|
| `reverb_algo` | choice, FROZEN 6 (§4) | 0 (Room) | The compatibility keystone: absent → default → Room (§5.3). |
| `reverb_decay` | log 0.2 … 30 s | 2.0 | Mid-band T60, true and measured. Clamped to the mode ceiling; the UI shows the clamped value. Also feeds the mod depth law. In Reverse it is the window length, relabeled **WINDOW** (§3.12). |
| `reverb_predelay` | 0 … 250 ms | 0 | After the send filters, before the diffusers (Dattorro's order). 40–100 ms is the documented guitar window. **Not latency.** |
| `reverb_diffusion` | 0 … 1 | 0.7 | `g = inDiffG[k]×(0.15+0.85·d)`. Must be exposed, not hardcoded. In Spring it is Dwell drive (§5.2). |
| `reverb_lowcut` | log 20 … 800 Hz | 20 (off) | Send HP. The single highest-value tone control for distorted guitar. |
| `reverb_highcut` | log 1.2k … 20k Hz | 20000 (off) | Send LP. Stops the reverb amplifying cab fizz. |
| `reverb_mod` | 0 … 1 | 0.35 | Two-stage law (§3.6). |
| `reverb_bassmult` | ×0.25 … ×4.0 | 1.0 | Jot low-band decay MULTIPLIER, not an EQ. Inert (greyed) in Reverse (§3.12). |
| `reverb_erlevel` | 0 … 1 | 0.5 | Early Energy level. Tank gain is fixed; this never starves the tail. Inert (greyed) in Reverse (§3.12). |
| `reverb_color` | 0 … 1 | 0.0 | §5.1. |
| `reverb_tilt` | −1 … +1 | 0.0 (inert) | Wet-bus shelving pair, pivot 800 Hz, ±6 dB. |
| `reverb_duck` | 0 … 1 | 0 (off) | Wet ducking sidechained from the DRY send. 20 ms atk / 1200 ms rel. Feed-forward only. |
| `reverb_shimmer` | 0 … 1 | 0 (off) | Shifted-voice level, separate gain from decay, bounded by §3.11. **Stage 3**, `kVersionHint4`. |
| `reverb_shimmer_interval` | choice, FROZEN 4 | 2 (+1 oct) | Inert outside Shimmer. **Stage 3**, `kVersionHint4`. |
| `reverb_freeze` | bool | false | §3.10. **Stage 4**, `kVersionHint5`. |

### 5.1 `reverb_color` — the character axis

Without this the plugin lands at *excellent transparent reverb*, not VintageVerb tier:
every objective gate in §8 is a transparency metric, and a reverb can pass all of them
and sound like a convolution of white noise. The reference products sell on deliberate
colour. `reverb_color` simultaneously drives:

1. **In-loop bandwidth ceiling** 18 kHz → 6 kHz (a one-pole LP in the loop, on top of
   the Jot HF band), **faded in by the knob** so that `color = 0` is exactly neutral.
   > **AMENDED 2026-08-06.** Run unconditionally at 18 kHz the filter is not neutral at
   > the knob's zero: it sits INSIDE the loop, so its loss compounds once per circulation
   > — Room makes ~20 passes/s and an 18 kHz one-pole is −2.16 dB at 16 kHz when the host
   > runs at 96 kHz, i.e. −43 dB/s of damping stacked on the §3.4 solve that exists to own
   > the per-band decay. The probe measured Room's 16 kHz octave decaying in 0.607 s
   > against its 1.000 s Jot target (−39.3 %, outside §8's ±35 % band) at `color = 0`.
   > Mixing the filter in by the knob restores §3.4's authority at the default and leaves
   > both documented endpoints (18 kHz at 0, 6 kHz at 1) exactly as written.
2. **`inSat` drive** 0.05 → 0.6 (the C¹ clipper of §3.5). Costello reports in-loop
   saturation *reduces* metallic ring-out.
3. **Modulation grit**: quantise the Layer-A random-walk target to 1/32-sample steps and
   correlate the per-line LCG streams (`ρ = 0 → 0.5`). This is the 224XL's quantized
   modulation "halo", and Costello's own 2013 correction says the modulation *source*'s
   noise, not the tone filter, is what makes the old boxes sound old.
4. **Interpolator degradation** (AMENDED 2026-08-07): fade the tank's *line read* — the
   Layer-A-modulated read — from the 4-point Hermite toward the 2-point linear kernel,
   `x ← x_cubic + colorInterp·(x_linear − x_cubic)`, up to a per-mode `colorInterpTop`.
   This is the third of the era ingredients Costello attributes the 224's signature to
   in *Algorithmic Reverbs, Distortion, and Noise*: a coarse fractional-delay
   interpolator is a moving LP whose loss and "halo" sit inside the loop and compound
   ~300× over a long tail — the one degradation a post-EQ genuinely cannot fake. It
   does NOT break §3.4's authority, because the blended read *is* a 4-tap FIR over the
   cubic's own tap frame (`h = (1−c)·h_cubic + c·{0, 1−t, t, 0}`), so
   `|Hi(f_h)|_mean` extends to it **exactly**: the engine tabulates the blended-kernel
   magnitude over `c ∈ [0, 1]` at configure and the `r_hi` solve divides by the value
   at the live blend — the same compensation discipline whose absence produced axis 1's
   recorded −39.3 % incident. `color = 0` takes a branch, not a `+ 0`, so it reads the
   cubic bit-for-bit.
   > **Why the line read only, not the in-loop allpass read too.** The loop crosses two
   > interpolators per pass, but `r_hi` divides by `|Hi|` once — the allpass read's
   > cubic droop has always ridden inside the scalar-`D_i` approximation of §3.5 and
   > the ±35 % HF band of §8. Degrading only the line read keeps the axis's delta at
   > *one kernel swap per pass*, which the single division compensates exactly;
   > degrading both would need the blended/cubic magnitude ratio SQUARED — a second
   > approximation stacked on the allpass recursion's already-approximate scalar
   > treatment, i.e. the −39.3 % failure class re-invited from the other side. The
   > character lost is minor: the line read is the longer delay and the one the walk
   > modulates, so it carries the halo either way.

> **AMENDED 2026-08-07 — the knob's endpoints are per-mode era columns now, which
> supersedes this section's 2026-08-06 note pinning "both documented endpoints (18 kHz
> at 0, 6 kHz at 1) exactly as written".** The 18 kHz `color = 0` corner stays global
> and pinned — colour 0 remains the same "exactly neutral" statement in every mode —
> but the knob-top endpoints become §4.1 columns so one knob reads as a different
> machine age per mode: `colorLpBottomHz` / `colorInSatTop` / `colorInterpTop` are
> Room {6 kHz, 0.60, 0.90} (the 1970s box: darkest, grittiest, coarsest interpolator),
> Plate {7 kHz, 0.60, 0.60} (the 1980s), Hall {9 kHz, 0.35, 0.25} (nearly modern —
> its `inSat` top drops from the global 0.60 so the cleanest machine stays clean under
> drive). Shimmer shares Hall's row as always; Spring and Reverse keep the
> pre-amendment constants {6 kHz, 0.60} and `colorInterpTop = 0` — Spring's era lives
> in its dispersion, not a delay interpolator, and Reverse's window line is a
> single-pass FIR where §3.12 already declares linear transparent, so there is nothing
> for the axis to compound.

Default 0 (neutral), so no existing preset gains colour. The §8 gates become
colour-conditional: at `color = 0` they apply as written; above 0.5 the spectral-peak and
flutter gates are inverted for Plate — if it does not show peaks, the knob is not working.
At `color = 1` the per-octave T60 row additionally runs against era-adjusted targets and
gates fatally (§8's 2026-08-07 colour-1 amendment).

### 5.2 Mode-dependent parameter meaning (Spring, Reverse)

Spring repurposes `reverb_diffusion` → Dwell drive and `reverb_size` → spring length.
Reverse relabels `reverb_decay` → **WINDOW** and greys out `reverb_damping`,
`reverb_bassmult` and `reverb_erlevel` (§3.12), which have no meaning without a
recirculating tail. This is genuine reuse of a scarce id budget, but it must be
**visible**: `knobSpecs.ts` is extended to key on `(kind, algo)` — built in Stage 1
for Reverse, reused by Spring — so the labels render **DWELL** and **TANK** under
Spring and **WINDOW** under Reverse, and inert knobs render disabled, never
live-but-dead. Precedent note: `delay_ratio` is *inert* in the wrong mode; it does not
mean something else. A silent semantic override is worse than a relabel, and a
silently inert knob is worse than a greyed one.

> **AMENDED 2026-08-08 — reverb panel UX overhaul.** The panel's shape changed;
> the per-mode semantics above did not. Deltas:
> - The `reverb_algo` dropdown is now a six-segment **mode bar** (kit
>   `Segmented`); the frozen choice order is unchanged.
> - The scrolling secondary grid is now **three captioned, non-scrolling groups
>   of four** in signal-flow order, with short labels (full names live in the
>   hover strip and the control tooltips): SPACE — PRE, DIFF, EARLY, WIDTH ·
>   TONE — LO CUT, HI CUT, TILT, COLOR · TAIL — DAMP, BASS, MOD, DUCK.
> - The SHIMMER dial is now **always mounted as the fourth primary dial and
>   greyed outside Shimmer** (this amends the earlier "renders conditionally"
>   rule): an empty fourth cell in five of six modes teaches nothing, while a
>   greyed dial whose hover strip says "switch the mode to bring this to life"
>   teaches the mode set. The INTERVAL combo stays conditional (mode row only).
> - An 18px hover **info strip** explains whatever control the pointer is on,
>   with a derived range chunk; greyed controls report "inert here".
> - **Deliberately absent, do not add:** per-mode default patches and any MIX
>   LOCK — a mode change writes `reverb_algo` and nothing else, ever.
>
> **TODO — pre-release, not blocking the merge.** Both items were declared out
> of scope for the UX overhaul itself and are tracked here so they are not lost:
> 1. **AU WebView fit re-verify.** The 1000×600 fit check (all six modes: no
>    scrollbar in the reverb body, no clipped label or readout,
>    `.rvBody` scrollHeight === clientHeight) has only been run in Chrome
>    against the mock bridge. Re-run it in the real AU WebView, where Safari 14
>    text metrics — not Chrome's — decide the 182px budget.
> 2. **Deuteranopia pass on the six mode accents.** The per-mode body palette
>    (room cyan · plate `#a5f3fc` · hall `#38bdf8` · spring `#e3a857` ·
>    shimmer `#f0abfc` · reverse `#94a3b8`) has not been run through a
>    red-green colour-blindness simulation. The mode bar's selected pill and
>    the segment labels carry the mode identity textually, so this is a polish
>    check, not a correctness one — but hall/plate/room sit in one family and
>    should be confirmed distinguishable.

### 5.3 Backward compatibility — exact mechanism

There is no legacy engine to route to. The keystone is **absent-means-default**:
`reverb_algo` defaults to 0 = Room, so a state that never mentions it must resolve to
Room on BOTH restore paths.

- **Preset path.** `applyStateVar` (`PluginProcessor.cpp:2183-2192`) already runs an
  absent-means-default sweep over `getParameters()`. A preset without `reverb_algo` is
  explicitly set to its default → 0 → Room. ✅ verified in source.
- **Session path.** `setStateInformation` → `apvts.replaceState` has **no** such sweep.
  JUCE's `replaceState` fires `valueTreeRedirected` → `updateParameterConnectionsToChildTrees`,
  which for a parameter with no child in the incoming tree creates a fresh child and
  flushes the parameter's *current* value, not its default. On a freshly-instantiated
  plugin that is the default (fine) — but on a **live** instance (Logic's "Load
  Setting…", AU user-preset recall, some hosts' A/B) it leaves `reverb_algo` at whatever
  the user last selected, and an old session then renders through that mode — not Room.
  **Required Stage-1 work: mirror `applyStateVar`'s absent-means-default sweep into
  `setStateInformation` before `apvts.replaceState`.** ~20 lines, and it removes the
  "two paths, two mechanisms, same answer" hand-wave entirely.

**Legacy content changes sound — by design.** A session or preset saved against
Freeverb now renders through Room at its stored size/damping/mix/width values. This is
a deliberate, dated user decision (2026-08-06), traded against carrying Freeverb's
structural defects (§1) behind a "Legacy" index forever. No migration shim exists, so
there is nothing to deprecate later.

**Version hints.** JUCE's AU wrapper sorts the parameter list by identifier hash then
stable-sorts by version hint, so adding params at `kVersionHint2` would interleave them
among the shipped v2 block and shuffle Logic's automation menu. Add
`constexpr int kVersionHint3 = 3;` for Stage 1's twelve-parameter batch,
`kVersionHint4` for Shimmer (Stage 3), `kVersionHint5` for Freeze (Stage 4) — one new
hint per shipped batch, never reusing a released hint. Stage 2 (Spring) ships no
parameters, so it consumes no hint.

**Relay counts** (`WebEditor.cpp:102-104` + the TS mirrors, in the same append order):

| stage | sliders | toggles | combos |
|---|---|---|---|
| today | 89 | 30 | 8 |
| Stage 1 (+11×3 sliders, +1×3 combos) | 122 | 30 | 11 |
| Stage 3 Shimmer (+1×3 sliders, +1×3 combos) | 125 | 30 | 14 |
| Stage 4 Freeze (+1×3 toggles) | 125 | 33 | 14 |

### 5.4 Default-mode product risk

Dissolved by the erasure. With Room as `reverb_algo`'s default, a fresh Reverb tile, a
legacy preset and a legacy session all land on the same mode through the same
mechanism, and the first draft's add-block write of `reverb_algo = Room` is deleted as
redundant — a UI-side default duplicating a parameter default is a second mechanism
waiting to disagree.

For the record, factory presets are still not a lever:
`PresetManager::createFactoryPresetsIfMissing()`
returns immediately if the presets directory contains any `*.json`
(`src/library/PresetManager.cpp:169`), so every existing user is unreachable by that
route. If factory presets are ever to be revised, they need a `factoryVersion` marker
and a re-emit rule for unmodified factory-named presets — out of scope here.

### 5.5 Preset-audible changes after the parameter set froze (2026-08-07)

> **AMENDED 2026-08-07 — §3.2's decay-normalised injection changes the wet balance of
> every existing preset whose `reverb_decay` is not 1.5 s.** Recorded here rather than
> only at §3.2 because it is the first change to alter shipped presets *without* touching
> a parameter, and the two rules that govern that case pull in opposite directions.
>
> - **What moves.** Wet level at fixed `reverb_mix`, by `√(1 − g²)` relative to the 1.5 s
>   reference: measured in the 500 Hz octave at 44.1 kHz, Room −20 dBFS pink read
>   −39.4 dB at 0.2 s and −32.7 dB at 2.5 s before the change and −34.4 / −35.0 dB after,
>   so short-decay presets get *louder* by ~5 dB and long-decay presets *quieter* by ~2 dB
>   (Hall at its 10 s ceiling, 6.1 dB quieter). `reverb_mix` is untouched and still
>   means what it meant; it is the wet bus behind it that no longer rides Decay.
> - **Why it is allowed.** The bit-identical-default invariant is retired by dated user
>   decision (2026-08-06, see *Invariants*), and no recorded decision ever said wet level
>   must track Decay — the engine's own constant-tail law for Early Energy (§3.7's
>   `kErToOutput`) says the opposite for the section that already had a choice.
> - **Version hints.** No hint is consumed and none may be. §5.3's rule is one new hint
>   per shipped batch of PARAMETERS, and this change adds none: `gRef` and `injectGain`
>   are internal, derived from `reverb_decay` and the mode tables. Spending
>   `kVersionHint5` here would reserve Freeze's hint for a change with no parameter behind
>   it and re-sort Logic's automation menu for nothing. The append-only rule is therefore
>   satisfied by this dated entry, which is the record a later reader needs when a preset
>   made before today mixes differently after it — there is no id to read that from.

---

## 6. Realtime hygiene, CPU and memory

### 6.1 `ReverbFx` shape

```cpp
class ReverbFx {
public:
    struct Params {
        int   algo = 0;              // 0 = Room (§4)
        float size01 = 0.5f, damping01 = 0.5f, mix01 = 0.25f, width01 = 1.0f;
        float decayS = 2.0f, predelayMs = 0.0f, diffusion01 = 0.7f;
        float lowCutHz = 20.0f, highCutHz = 20000.0f;
        float mod01 = 0.35f, bassMult = 1.0f, erLevel01 = 0.5f;
        float color01 = 0.0f, tilt = 0.0f, duck01 = 0.0f;
        bool  snap = false;          // §6.4
        // freeze / shimmer01 / shimmerInterval: AMENDED 2026-08-06, see below.
    };
    void prepare (const juce::dsp::ProcessSpec&);
    void reset();                    // full clear
    bool resetStep();                // §6.3; returns true when done
    void setParameters (const Params&);
    bool wantsReset() const noexcept; // §6.2
    void process (juce::dsp::AudioBlock<float>);
private:
    ReverbEngine engine;             // all six algo indices; Spring→Plate and
                                     // Shimmer→Hall inside the engine until
                                     // Stages 2 and 3 ship (§4)
};
```

The 4-float setter becomes a struct; a 17-argument call would be unreadable.

> **AMENDED 2026-08-06 — `freeze`, `shimmer01` and `shimmerInterval` are NOT plumbed.**
> The first draft carried them unread "so the struct shape is final on day one"; the
> dated user directive that moved Freeze and Shimmer out of Stage 1 also removed
> their parameters from that pass, and a field with no parameter behind it is a field
> nothing can set. Stages 3 and 4 add each field in the same change as its parameter,
> its DSP and its gate. The struct is internal — `ReverbFx` is its only caller — so the
> blast radius of adding one is a recompile.

Update the
file-top block contract comment in `FxBlocks.h:7-9` so the next reader does not read the
struct as a violation of it.

### 6.2 Mode change must not memset on the audio thread

`reverb_algo` arrives through `setParameters` inside `processChainBlock`
(`PluginProcessor.cpp:1445`) — i.e. on the audio thread, every block. A synchronous
"reconfigure + clear" there is a multi-megabyte `memset` inline in `process()`, entirely
outside the `kMaxResetsPerCallback = 2` budget the codebase built for exactly this
(`PluginProcessor.cpp:24-27, 1060-1071`). Instead:

1. On an algo change, `ReverbFx` starts a 60 ms equal-power **wet fade-out** on the
   current engine.
2. At fade end it latches `reconfigurePending`; `wantsReset()` goes true.
3. `processChainBlock` sees it and sets `pendingReset[id]`. The existing guard
   (`if (isOn(...) && !pendingReset[id])`) already bypasses the block while pending, so
   the transition is silence, not a click.
4. The drain loop calls `resetBlockInstance(id)` → `ReverbFx::resetStep()` under the
   existing 2-per-callback cap. It reconfigures tables on the first step.
5. First `process()` after: 60 ms fade-in.

No dual engine: that would double the footprint purely to smooth a transition users read
as a scene change anyway.

The same deferred path serves Reverse's window retune (§3.12): in Reverse a
`reverb_decay` edit latches a reconfigure instead of gliding, because moving 64 tap
positions live is 64 simultaneously swept combs plus a gain-renormalisation step. The
rebuild uses the value current at fade end, so a knob drag costs one fade cycle, not
one per delta.

### 6.3 `reset()` and `resetStep()`

`resetStep()` is **mandatory in Stage 1**, not a contingency. It clears one delay line's
buffer per call (≈276 KB at 192 kHz ≈ 17 µs) and returns `true` when the last one is
done; `resetBlockInstance` calls it once per drain and re-arms `pendingReset[id]` while
it returns `false`. That keeps the worst case comparable to today's `DelayFx` clear
(`DelayLine<float> { 96000*2 }` ≈ 1.5 MB, `FxBlocks.h:150`). The Reverse window line
(1.13 MB at 192 kHz) is cleared in tank-line-sized chunks — four steps at full length —
so no single step exceeds that bound.

### 6.4 `prepare()` always clears; `snap` is an explicit flag

**`ReverbEngine::prepare()` ALWAYS clears**, matching every other block in
`prepareToPlay` (`PluginProcessor.cpp:797-807`) and — decisively — `renderChain`'s
documented contract that two renders of the same order are bit-identical
(`tests/ProcessorSmokeTest.cpp:528-540`). A tail-preserving re-prepare would make the
existing `expectIdentical` renderChain tests, `runChainOrderTests` and
`runSplitPathTests` all non-deterministic. Surviving Logic's transport-time re-prepares with the tail intact
is a separate, chain-wide change, not a reverb-local exception.

The first draft's "snap Size when `pendingReset` is set" is unimplementable: while
`pendingReset[id]` is true, `setParameters` is never called, so the engine cannot observe
the flag — and a preset recall that leaves the order unchanged sets no `pendingReset` at
all. Replace with an explicit `Params::snap`, set by the processor for one block after:
`resetBlockInstance` for this instance (`PluginProcessor.cpp:1808-1810`), the end of
`applyStateVar`, the end of `setStateInformation`, and the first block after `prepare()`.

### 6.5 Parameter smoothing

- Gains (mix, width, erLevel, tilt, duck, wet) — `SmoothedValue`, 20 ms.
- `g_mid_i`, shelf/LP coefficients — recomputed once per block, linearly ramped across
  the block (8 + ~24 ramps).
- **Size** — slew-limited to **250%/second** (full range in ~400 ms), yielding ~60 cents
  of Doppler on a 100 ms line — a clearly audible, musical tape-reel glide that tracks
  the hand. The first draft's 12%/s bounded Doppler to ~21 cents, which is inaudible as
  an effect while costing 1.5 s of rubber-banding. `snap` overrides (§6.4).
- **Pre-delay** — fractional read with a slew-limited pointer for jumps < 5 ms;
  equal-power crossfade for jumps ≥ 5 ms. Equal-power on a small jump would give a +3 dB
  bump and a swept comb, because the two reads are highly correlated there.

### 6.6 Denormals, DC, stability

`ScopedNoDenormals` already wraps `processBlock`; belt-and-braces, add zita's `+1e-20`
per line (8 adds) for hosts that reset MXCSR mid-callback. DC blockers on the injection
and on the wet output only — never inside the loop outside Freeze, where they perturb
the decay. Stability guards: `Â` is exactly unitary in exact arithmetic, so keep an
explicit scalar loop gain ≤ 0.9999 against float drift, clamp `r_hi ≤ 1 − 1e-4`, and
clamp `reverb_decay` to the mode ceiling.

### 6.7 Memory

Exact-length buffers (§3.3), 32-bit float, per instance:

| buffer | @48 kHz | @192 kHz |
|---|---|---|
| 8 tank lines × 0.36 s | 553 KB | 2.21 MB |
| 8 in-loop allpasses × 0.12 s | 184 KB | 737 KB |
| pre-delay 0.25 s | 48 KB | 192 KB |
| ER multitap 0.09 s | 17 KB | 69 KB |
| 4 input diffusers | 12 KB | 49 KB |
| spring dispersion | 6 KB | 6 KB |
| Reverse window line 1.5 s (mono) + guard | 281 KB | 1.13 MB |
| **total** | **≈ 1.10 MB** | **≈ 4.41 MB** |

Three instances: 3.3 MB @48 kHz, 13.2 MB @192 kHz. The Reverse line is allocated
unconditionally at `prepare()` — a mode switch must never allocate (§6.2) — and the
deleted Freeverb hands back its ~0.1 MB, which no longer appears in this table. (CloudSeed statically allocates ~67 MB
for one stereo instance — that is why it is rejected, §9.)

### 6.8 CPU

~230 ops/sample/instance for the tank + ER + filters; Spring adds ~300 (the 100-section
cascade × 3 springs). Reverse replaces the tank + ER cost with ≈ 330 (64 taps × linear
read + equal-power pan, 4 density allpasses, the shared front end) — the same class as
what it displaces, and single-pass, so no interpolation-compounding concern (§3.12).
Three simultaneous Spring instances at 192 kHz is the worst case
and is the one number that must be measured before Stage 2 ships. For scale, NAM
inference is tens of thousands of ops/sample. **No undersampling scheme** — the CPU it
would save is not there, and it would buy a resampler that must be re-prepared on rate
change, a bug class this project has already hit.

### 6.9 Lanes

`ReverbFx::process` keeps `channels = jmin(2, block.getNumChannels())` and the early-out
at 0. The engine runs identically on the host buffer (serial or lane A) and on
`laneScratch` (lane B): it is stateful, but `parseOrder` dedups ids
(`ChainOrder.h:343-357`) so a single `ReverbFx` can never serve two segments in one
block, and nothing caches which buffer it saw. `split_on == false` still runs lane B, so
a reverb parked there keeps ringing — unchanged.

---

## 7. Staged implementation

Stages are split **by mode, not by feature**, so a mode's voicing freezes when that mode
ships. A feature-split (core in one stage, Early Energy and modulation in the next)
would change the sound of every preset saved in between under the same `reverb_algo`
index. Stages 0 and 1 shipped 2026-08-06; Stage 1 merged the first draft's Stages 1 and 2
(minus Freeze) and added Reverse, so Room, Plate, Hall and Reverse froze together.
The remainder renumbers to **Stage 2 Spring, Stage 3 Shimmer, Stage 4 Freeze** (user
directive, 2026-08-07: Spring and Shimmer ship before Freeze). **Stages 2 and 3 shipped
2026-08-07**, so all six modes are now voiced and frozen and nothing is greyed in the
algo menu; only Stage 4 (Freeze) remains.

### Stage 0 — the ruler, before any engine code

**Goal:** you cannot tune tank tables, `apRatio`, `apG`, `inDiffG` or the Jot solve
without measuring them, and the baseline the upgrade is judged against is Freeverb.

**Work:** `tools/VerbProbe.cpp` + a `tubamp_verbprobe` console target (model it on
`tools/FxProbe.cpp` and the existing `tubamp_smoke` CMake block). It renders an IR WAV
plus a metrics report and returns non-zero on any failure. Validate it immediately
against a **probe-local `juce::dsp::Reverb` instance** — the deleted Freeverb survives
only inside `tubamp_verbprobe`, as the measurement baseline — which also produces the
before/after record.

**Files:** `tools/VerbProbe.cpp`, `CMakeLists.txt`, `docs/REVERB.md`.

**Verification:** the probe reproduces Freeverb's known defects — 0.250 modes/Hz, T60
spread ≈ 44.9% across the comb bank, echo density plateauing well below 1.0, a
mono-sum notch near 959 Hz.

### Stage 1 — engine core + Room, Plate, Hall, Reverse

Merges the first draft's Stages 1 and 2 (user directive: one parameter batch, one
voicing freeze) and adds Reverse; Freeze moves out to Stage 4.

**Goal:** four fully-voiced, shippable modes that close the audible gap, with the
legacy resolution story (§5.3) proven on both restore paths.

**Work:** `ReverbEngine` — 8 lines in one flat vector with exact-length buffers + 4-sample
guard, 4-point Hermite reads, one modulated allpass per line, per-line zita `Filt1` with
interpolator-compensated `r_hi`, normalized Hadamard butterfly, two decorrelated output
tap sets, C¹ soft clip + staynormal, Layer-A random walk with both depth laws and both
negative-delay guards, per-line `apRatio` jitter; Layer-B quadrature modulation with the
`< 15 ms` per-line gate; the two-sided frequency-dependent L/R spread (§3.8); the colour
axis (§5.1). Input chain: DC block, send HP/LP, pre-delay with the dual crossfade law,
4 input diffusers. Early Energy (24 taps + 2 short `g=0.5` allpasses + per-tap pan).
ModeSpec tables for Room, Plate and Hall. The Reverse back end (§3.12): 1.5 s mono
window line, 64-tap rising table with seeded pan and jitter, cross-tuned per-channel
density allpasses, walker-modulated linear tap reads, deferred window retune. `ReverbFx`
single-engine wrapper with the deferred mode change (§6.2), `resetStep()` (§6.3, with
the chunked Reverse-line clear), `snap` (§6.4). `setStateInformation`
absent-means-default sweep (§5.3). Params: `reverb_algo` (all 6 choices declared,
Spring and Shimmer greyed), `reverb_decay`, `reverb_predelay`, `reverb_diffusion`,
`reverb_lowcut`, `reverb_highcut`, `reverb_mod`, `reverb_bassmult`, `reverb_erlevel`,
`reverb_color`, `reverb_tilt`, `reverb_duck` ×3, at `kVersionHint3`, in that order.
New `ReverbBody.tsx` modelled on `DelayBody.tsx`, keyed on **kind** so
`reverb2`/`reverb3` reach their own ids; `knobSpecs.ts` keyed on `(kind, algo)` for the
WINDOW relabel and the greyed damping/bassmult/erlevel knobs in Reverse (§5.2).
Secondary knobs go in a new `ScrollingKnobGrid`
built on `ParamSlider`/`KnobRow` and the existing relay hooks — `FxParamGrid` is bound to
`FxSlotState.params`/`bridge.fxSetParam` and is **not** reusable for tubamp's own slider
ids; only its `.fxGrid` CSS is. **Do not raise `--dock-h`** (note: `--min-app-h: 600px`
at `tokens.css:184` and `kMinEditorHeight` at `WebEditor.cpp:119` are two independent
literals that happen to agree; `--dock-h: 248px` does not feed either).

**Files:** `src/dsp/ReverbEngine.{h,cpp}`, `src/dsp/FxBlocks.{h,cpp}`,
`src/Parameters.{h,cpp}`, `src/PluginProcessor.{h,cpp}`, `src/WebEditor.cpp`,
`tests/ProcessorSmokeTest.cpp`, `ui/src/bridge/{types,paramMeta}.ts`,
`ui/src/features/panel/{knobSpecs.ts,ReverbBody.tsx,index.tsx,panel.module.css}`,
`docs/REACT-UI.md`, `docs/REVERB.md`.

**Verification** (`runReverbTests`, split so nothing passes vacuously):
1. **Legacy resolution, both paths** — a preset AND a live-instance session restore
   (Logic "Load Setting…" shape) that never mention `reverb_algo`, applied over a
   non-default live value, both leave `reverb_algo == 0` (Room). This replaces the
   first draft's `expectIdentical` Legacy regression guard, which died with the engine
   it guarded.
2. **Modes audible** — pairwise `firstDifference >= 0` across algo 0/1/2/5, everything
   else identical. Fallbacks exact: algo 3 renders bit-identically to algo 1, and
   algo 4 to algo 2, until their stages ship.
3. **Knobs audible** — per shipped mode, each live param at min vs max →
   `firstDifference >= 0`. (Use a reverb-only order; exclude `amp`, the one block the
   file already documents as not resetting bit-exactly.) The documented-inert
   combinations — damping, bassmult, erlevel under Reverse — must instead be
   bit-identical min vs max (§3.12 gate 6): inertness is asserted, not assumed.
4. Finite and non-silent on stereo AND mono hosts (`withMonoHost`,
   `processMonoSineFinite`); `reverb_on` off is a bit-exact pass-through; re-entry after
   removal does not replay the old tail.
5. `sizeScale` is strictly monotonic in `size01` with no flat regions, at every
   (mode × decay) corner — the Size-goes-dead guard.
6. Measured T60 at 44100/48000/96000 agrees within 10% (Freeverb fails this).
7. `tubamp_verbprobe` gates (§8) pass for Room, Plate and Hall at `color = 0`; the
   inverted colour gates at `color = 1`; the ducking gate (with `duck = 1`, wet RMS
   during a sustained note ≥ 6 dB below **the same render at `duck = 0`**); the
   two-sided-spread gate (LF image centroid stays within ±0.1 of centre on an 80 Hz
   transient, which a one-sided chain fails); the ≥ 60 Hz per-octave T60 bands (§8)
   that catch the allpass group-delay spread.
   > **AMENDED 2026-08-06.** (a) *Ducking.* The first draft compared the wet during the
   > note against the wet 800 ms after note-off, which cannot pass with §3.1's own
   > 1200 ms release: 800 ms after note-off the sidechain envelope has fallen only to
   > 51 % of its steady value, so the ducker gives back ~4.6 dB while the tail it is
   > measured on has decayed 28.5 dB over the same 800 ms at Room's 2 s default. That
   > comparison measures the tail, not the ducker. Against the same render at `duck = 0`
   > the quantity is well posed, and the 6 dB threshold is unchanged: the engine measures
   > 16.6 dB, with the release separately asserted to be giving gain back (11.5 dB
   > residual at +800 ms). The literal figure is still reported. (b) *Spread.* The
   > transient is ONE Hann-windowed cycle of 80 Hz, and the balance is integrated over
   > 1.5 s. Two cycles is narrow enough (~40 Hz) that the measurement samples §3.8's two
   > output tap sets at exactly 80 Hz rather than the LF image — it reads +0.204 on Hall
   > and even reverses the sign of Plate's arrival skew against a band-limited impulse.
   > Inside 50 ms the same objection applies in time: only three or four Hall taps have
   > fired, so an early-window balance measures which taps landed. Engine: Plate −0.068,
   > Hall +0.077. A second bound is added on what a one-sided cascade actually breaks —
   > the LF arrival skew between channels, ≤ 6 ms against the ~13 ms a one-sided chain
   > would produce (engine: −3.0 ms Plate, −2.7 ms Hall). Both live in `tubamp_verbprobe`,
   > not in `runReverbTests`: the ducker is fed from the engine's own dry send and the
   > spread cascades live on its wet bus, so neither needs the processor.
8. The six Reverse gates of §3.12, at 44.1/48/96 kHz.

Plus `cmake --build build`, `tubamp_smoke`, `auval -v aufx Tamp Tuba`,
`npm run typecheck && npm run build`. **Subjective gate:** the same DI through the same
NAM model into this engine and into a reference plugin at matched decay, A/B'd by a
human, before the stage is called done.

#### Shipped — Stages 0 and 1, 2026-08-06

`tools/VerbProbe.cpp` + `tubamp_verbprobe` (Stage 0, with the probe-local Freeverb
baseline), `src/dsp/ReverbEngine.{h,cpp}` (the whole of §3 bar Spring, Freeze and
Shimmer), `ReverbFx` as a single-engine wrapper with the deferred mode change, the
chunked `resetStep()` and `snap`, the twelve-parameter `kVersionHint3` batch ×3, the
`setStateInformation` absent-means-default sweep, `ReverbBody` + `ScrollingKnobGrid` +
`(kind, algo)`-keyed `knobSpecs`, and `runReverbTests` in the smoke test. Room, Plate,
Hall and Reverse are voiced and frozen; Spring and Shimmer are declared, greyed, and
fall back Spring→Plate and Shimmer→Hall (superseded 2026-08-07 — see the shipped blocks
under Stages 2 and 3; no fallback and no greyed entry remains).

Every §8 gate and every §3.12 gate passes at 44.1 / 48 / 96 kHz — `tubamp_verbprobe`
exits 0 — and `tubamp_smoke` passes. Headline measurements at `color = 0`,
`reverb_decay = 2 s`, 48 kHz:

| | echo density (0.95 by / tail min) | broadband T60 vs 2.00 s asked | flutter | mono-sum notch |
|---|---|---|---|---|
| Room | 64.0 ms / 0.949 | 1.735 s, R² 0.9990 | 0.000 | −5.18 dB |
| Plate | 24.0 ms / 0.960 | 1.855 s, R² 0.9994 | 0.000 | −2.72 dB |
| Hall | 51.0 ms / 0.927 | 1.673 s, R² 0.9967 | 0.000 | −3.22 dB |
| Reverse | n/a (finite window) | n/a | n/a | −4.33 dB |

Broadband T60 runs short of the asked-for 2.00 s because it is one number over a decay
the §3.4 solve deliberately makes frequency-dependent — the per-octave row is the gated
statistic and it holds ±15 % of its Jot target from 63 Hz to 4 kHz.

Reverse's own gates at 48 kHz: envelope dip −1.37 dB over the audible swell and
−3.56 dB over the whole swell, 49.7 dB fall within 150 ms, energy centroid 1.382 s in a
1.5 s window, hard stop −86.9 dBFS, window tracking +0.1 … +1.9 % across
0.4 / 0.9 / 1.5 / 3.0 s, and damping/bassmult/erlevel bit-inert. Ducking measures
16.6 dB at `duck = 1`; the two-sided spread measures −0.067 (Plate) and +0.077 (Hall).
Per-octave T60 holds ±15 % from 63 Hz to 4 kHz with R² > 0.98, and the 8 kHz+ octaves
stay inside their documented ±35 %.

Amendments made while shipping, each dated 2026-08-06 and recorded at its own section:
§3.1/§3.7 (the Early tap line is fed from the diffuser output), §3.6 (mod rate as a
ratio on the per-mode base), §3.12 (the tap regulariser is a fraction of `W`; gate 1a
splits and moves to the stereo power; gates 3 and 4 count the density allpasses' group
delay), §5.1 (the colour LP is faded in by the knob), §6.1 (freeze/shimmer fields
deferred to their stages), §7 gate 7 (ducking measured against `duck = 0`; the spread
transient is one cycle), §8 (12 kHz density analysis band). No threshold was loosened
except §3.12 gate 1a, whose 1 dB is unreachable at this section's own 64 taps — the
reasoning and the independent model are in §3.12.

Still open from §7's list: `auval -v aufx Tamp Tuba` and the subjective A/B, both of
which need a human at the machine.

### Stage 2 — Spring

**Work:** `src/dsp/SpringDispersion.h` — 100 stretched first-order allpass sections per
spring, `a1 = 0.63`, `K = fs/(2·4200)`, `K1 = round(K)−1`, `a2` from `frac(K)`; 3 springs
at `Td = 51.3/56.1/61.7 ms` into 3 active tank lines; Dwell saturator ahead of dispersion;
in-loop LP 2.8 kHz + HP 90 Hz; loop gain ≈ −5.4 dB/pulse. Re-fit `a1` at the corrected
`K`. The DWELL/TANK relabel rides the `(kind, algo)` keying Stage 1 built for Reverse
(§5.2). Ungrey.

**Files:** `src/dsp/SpringDispersion.h`, `src/dsp/ReverbEngine.cpp`, `src/Parameters.h`,
`tests/ProcessorSmokeTest.cpp`, `ui/src/features/panel/knobSpecs.ts`,
`ui/src/bridge/paramMeta.ts`, `docs/REVERB.md`.

**Verification:** the two chirp assertions must measure the *same* phenomenon, not
contradict each other (the first draft's did). The cascade's group delay depends on
`θ = K·ω` alone — `M·K·(1−a1)/(1+a1)` at DC (130 samples = **2.70 ms**, rate-invariant),
`M·K·(1+a1)/(1−a1)` at the knee (**~52 ms**), and back to the DC value at the wrap
`fs/K` = 8.4 kHz, which is exactly why §3.9 buries the repeats under a 2.8 kHz one-pole.
So 6–10 kHz straddles the wrap and is the band that measures the *undispersed* spacing,
and the band that arrives late is the **knee**, not LF. Measure pulse spacing in a
**6–10 kHz band only** and assert within 5% of `Td + M·K·(1−a1)/(1+a1)`; measure the
**knee band (3.4–5 kHz)** separately; assert the **difference** between knee and HF
spacing is ≥ 15 ms (signed, knee later). Measure LF (100–400 Hz) and report it, but do
**not** gate LF-vs-HF: by the arithmetic above the two agree to within a millisecond by
construction, so that gate is unsatisfiable — the first draft asserted it, and paired it
with a `K1`-basis "roughly 37 ms" that is really the *knee* delay. Assert ≈ −5.4 dB/pulse
over the first 10 dB. Run at 44.1/48/96 kHz. Measure CPU for 3 simultaneous Spring
instances at 192 kHz.

#### Shipped — Stage 2, 2026-08-07

`src/dsp/SpringDispersion.h` (M = 100 stretched allpass sections, `a1 = 0.63`,
`K = fs/(2·4200)` re-derived at every rate, `K1 = round(K)−1`, `a2` from the leftover
fraction), the `modeSpring` back end in `ReverbEngine.cpp` — 3 springs at
`Td = 51.3/56.1/61.7 ms`, Dwell saturator ahead of the dispersion, in-loop LP 2.8 kHz and
HP 90 Hz, one loop gain in place of the Jot solve — the DWELL/TANK relabel on Stage 1's
`(kind, algo)` keying, and the five Spring gates plus the CPU pass in `tubamp_verbprobe`.
No new parameters, as planned. Spring is ungreyed and its DSP fallback to Plate is gone.

At `color = 0`, 44.1 / 48 / 96 kHz:

| | 44.1 kHz | 48 kHz | 96 kHz |
|---|---|---|---|
| `K` / `K1` | 5.2500 / 4 | 5.7143 / 5 | 11.4286 / 10 |
| DC group delay `M·K·(1−a1)/(1+a1)` | 119.2 smp = **2.702 ms** | 129.7 smp = **2.702 ms** | 259.4 smp = **2.702 ms** |
| 6–10 kHz spacing vs `Td +` that (5 % gate) | −0.2 / +0.1 / +0.3 % | +0.5 / +0.3 / +0.5 % | +0.5 / +0.3 / +0.2 % |
| chirp spread, knee band − 6–10 kHz (≥ 15 ms) | **+47.89 ms** | **+49.25 ms** | **+49.00 ms** |
| loop loss per pulse (−5.4 ± 0.5 dB) | 5.78 dB | 5.63 dB | 5.62 dB |
| deepest mono-sum notch (≤ 6 dB) | −3.50 dB | −1.68 dB | −1.67 dB |

The DC group delay is **rate-invariant in milliseconds** — 2.702 ms at all three rates —
which is the whole point of deriving `K` from `fs`: the chirp knee stays at 4200 Hz and
the wrap at 8400 Hz everywhere.

**CPU (§6.8, reported not gated):** 3 Spring instances at 192 kHz, 512-sample blocks —
0.5746 ms per block for all three (0.1915 ms each) against a 2.667 ms real-time budget,
**21.5 % of one core**. Spring is the engine's most expensive mode by a wide margin
(300 allpass sections per block on top of the tank) and it still leaves 4× headroom at a
rate no host asks for in practice.

**Amendment, 2026-08-07:** §7 Stage 2's verification block above was rewritten. The first
draft asserted the ≥ 15 ms spread between the **LF** and 6–10 kHz bands and quoted a
`K1`-basis "roughly 37 ms" of LF group delay. Both are wrong against the §3.9 cascade the
same section specifies: the group delay depends on `θ = K·ω` alone, so it returns to its
DC value at the wrap `fs/K` = 8.4 kHz, and LF and 6–10 kHz therefore agree to within a
millisecond **by construction** (measured +0.50 / +0.25 / +0.00 ms) — an LF-vs-HF spread
gate is unsatisfiable, and the 37 ms figure is really the knee's. No threshold moved: the
15 ms is asserted between the knee band (3.4–5 kHz) and 6–10 kHz, where it measures ≈ 49 ms.

### Stage 3 — Shimmer

**Work:** `src/dsp/PitchShifter.h` — 4 overlapping grains, Hann crossfade, randomized
grain start; routing switch Input / Regen / Both around ONE instance, with Input and Both
as feed-forward sends; in-loop LP ≤ 4 kHz after the shifter plus a hard limiter;
`shimmerGain ≤ 0.5·(1−g)` enforced per block against the live `g` (§3.11 — equivalently,
at prepare against the largest `g` the decay range allows, but without pinning the knob
to that worst case at every decay). `reverb_shimmer` ×3 (sliders 122 → 125)
and `reverb_shimmer_interval` ×3 (combos 11 → 14) at `kVersionHint4`. Ungrey.

**Files:** `src/dsp/PitchShifter.h`, `src/dsp/ReverbEngine.cpp`,
`src/Parameters.{h,cpp}`, `src/PluginProcessor.{h,cpp}`, `src/WebEditor.cpp`,
`tests/ProcessorSmokeTest.cpp`, `ui/src/bridge/*`,
`ui/src/features/panel/knobSpecs.ts`, `docs/REVERB.md`.

**Verification:** the stability test is the point. Sweep `reverb_shimmer` 0→1 against
`reverb_decay` 0.2→ceiling on a 2D grid × 4 intervals, feeding 10 s of pink noise then
60 s of silence. Assert (a) peak absolute sample never exceeds 1.5, (b) the tail
monotonically decays at every grid point, and (c) **the 2–4 kHz band energy converges** —
successive 10 s windows within 1 dB — which a peak-only test would miss, because the
runaway is many-to-one (five source octaves land in that band). Spectral: energy above
8 kHz decays at least as fast as mid-band. Pitch: a 220 Hz sine produces a 440 Hz partial
within 5 cents at `+1 oct`.

#### Shipped — Stage 3, 2026-08-07

`src/dsp/PitchShifter.h` (4 overlapping grains, Hann crossfade, randomized grain start,
no allocation after `prepare`), the Shimmer path in `ReverbEngine.cpp` — Regen tap on row
0 of the normalised Hadamard, injection signs that sum to zero, in-loop LP ≤ 4 kHz after
the shifter and a hard limiter on its output — `reverb_shimmer` ×3 and
`reverb_shimmer_interval` ×3 at `kVersionHint4` (sliders 122 → 125, combos 11 → 14),
the conditional SHIMMER knob + interval combo in `ReverbBody`, and the §3.11 stability
grid in `tubamp_verbprobe`. Shimmer is ungreyed and its DSP fallback to Hall is gone.
**Input and Both are not built** — §5 declares no routing id, and §6.1's rule is that a
field with no parameter behind it is a field nothing can set; Regen is the routing that
carries the hazard and the one the mode exists for.

Stability grid (§7's, run at 44.1 kHz): `reverb_shimmer {0, 0.5, 1}` × `reverb_decay
{0.2, 5.1, 10.0} s` × 4 intervals = 36 points, each 10 s of pink noise at 0.25 RMS
followed by 60 s of silence, nothing trimmed. Worst case over the whole grid:

| gate | bound | worst measured | where |
|---|---|---|---|
| peak absolute sample | ≤ 1.5 | **0.5161** | shimmer 1.0, decay 10.0 s, +1 oct |
| tail never rises (1 s windows) | ≤ 0 dB | **−4.90 dB** | shimmer 1.0, decay 10.0 s, −1 oct |
| 2–4 kHz convergence (10 s windows) | ≤ +1 dB | **+0.10 dB** | shimmer 0.5, decay 0.2 s, +P5 |
| above 8 kHz decays ≥ as fast as 2–4 kHz | — | 34.9 vs 10.2 dB/s at the worst point | shimmer 1.0, decay 10.0 s, +1 oct |

Pitch: the shifter alone turns 220 Hz into 439.810 Hz (**−0.75 cents**); inside the tank
the 440 Hz partial reads −2.74 / −0.43 / +1.08 cents at 0.5 / 1.0 / 2.0 s of decay
(**−0.70 cents** mean), and that partial is 89–108 dB above the same bin at
`reverb_shimmer = 0`, so it is the shifter's and not the tank's.

The decay ceiling matters here: 10.0 s is Shimmer's, so the worst rows above are at the
largest loop gain the mode can reach, which is where §3.11's bound is tightest.

**Amendment, 2026-08-07:** §3.11 and Stage 3's work item above said the gain constraint is
"solved at prepare from the mode's loop gain". It is enforced **per block against the live
`g`** instead — `g` is a function of `reverb_decay`, a live parameter, so a prepare-time
solve would have to freeze `g` at the mode's ceiling and run that worst-case bound at
every decay setting (0.054 instead of 0.498 at Hall's floor — a knob that does almost
nothing over the lower half of its range). Per block is strictly stronger, not weaker: the
bound holds at the `g` actually in force at every decay rather than only at the ceiling,
`gMidMax` tracks the largest `g` over the lines, and both `shimmerGain` and the line gains
are linear ramps over the same block so the inequality survives the interpolation exactly.

### Stage 4 — Freeze

**Work:** the full recipe of §3.10 — unity loop gain, flattened damping, mod off with
integer-snapped reads, soft clip bypassed, 50 ms input crossfade, one-shot DC removal,
**no in-loop HP**. `reverb_freeze` ×3 (toggles 30 → 33) at `kVersionHint5`; a Toggle in
`ReverbBody`'s centred section, the `MixBody.tsx` pattern.

**Files:** `src/dsp/ReverbEngine.{h,cpp}`, `src/dsp/FxBlocks.{h,cpp}`,
`src/Parameters.{h,cpp}`, `src/PluginProcessor.{h,cpp}`, `src/WebEditor.cpp`,
`tests/ProcessorSmokeTest.cpp`, `ui/src/bridge/*`,
`ui/src/features/panel/ReverbBody.tsx`, `docs/REVERB.md`.

**Verification:** RMS after 5 s of silent input within ±1 dB of RMS at 1 s; **spectral
centroid drifts < 15% over 30 s** (proves the damping filters really were flattened —
and would have caught the 25 Hz in-loop HP the first draft specified); mean of the frozen
output within 1e-4 of zero over 60 s.

---

## 8. Quality verification methodology

`tubamp_verbprobe` renders a wet-only IR and reports; non-zero exit on any failure.

| metric | method | gate |
|---|---|---|
| **Normalized echo density** | Abel & Huang: `η(t) = (1/0.3173)·Σ_w 1{|h(τ)| > σ}`, 20 ms Hanning window | reaches 0.95 by 80 ms (Room/Plate), stays ≥ 0.9 through the tail |
| **ER density in isolation** | same, tank muted | ≥ 0.9 at 60 ms |
| **Per-octave T60** | Schroeder backward integration, bands 63 Hz … 16 kHz | ±15% of `reverb_decay` with R² > 0.98 for bands ≤ 4 kHz; **±35% for 8 kHz and above, documented as interpolator-limited** (§3.4) |
| **Per-octave T60 at `color = 1`** (2026-08-07) | same bands, damping 0.5, `erLevel = 0`, against era-adjusted targets: per line `rate_i(f) = 60/T60_jot(f) + (L_lp(f) + ΔL_interp(f))/d_i`, five frequencies per octave, folded through the same Schroeder fit | same ±15% / ±35% bands — §5.1's axes 1+4 move the decay only by their *declared* per-pass losses, i.e. §3.4 keeps its authority at the knob's far end |
| **Per-octave tail level** (2026-08-07) | integrated band energy vs the 1 kHz octave, `bassmult × damping` grid {0.25, 1, 4} × {0, 0.5, 1} at `erLevel = 0` | 63–250 Hz and 4–8 kHz octaves move ≤ 3 dB from their neutral-knob (bassMult 1, damping 0.5) values — §3.4's tonal correction did its job |
| **Wet level vs Decay** (2026-08-07) | sustained −20 dBFS pink noise, 500 Hz-octave RMS after 0.4·T60, `erLevel = 0`, at decay {0.2, 0.5, 1.5 s, mode ceiling} | flat within ±1.5 dB across each mode's full decay range — §3.2's decay normalisation did its job |
| **In-loop distortion vs Decay** (2026-08-07) | residual-to-signal of a −6 dBFS pink render against the same render 20 dB down and scaled back up (everything but §3.5's clipper cancels) | no growth with Decay: the mode ceiling reads ≤ the 1.5 s reference + 3 dB, and the worst point of the sweep stays under 1 % |
| **In-loop clip aliasing** (2026-08-07) | two tones on one exact-bin comb (9·k0 / 11·k0 ≈ 0.99 / 1.21 kHz) at +6 dBFS summed, Plate at `color = 1`, `mod = 0`, decay ceiling; energy on the FOLDED product comb vs the fundamentals, periodic-Hann 2¹⁷ FFT, pinned 48 kHz | folded products ≤ −88 dB re the fundamentals, and folds landing below fs/4 ≤ −97 dB — §3.5's residual ADAA did its job |
| **Rotation pitch stability** (2026-08-07) | 440 Hz sine at −12 dBFS, sustained through Room and Hall at `mod = 1`; heterodyne F0 tracking (100 ms boxcar, 10 ms phase-slope hops), pinned 48 kHz | level-weighted rms and worst at/above-median-level hop stay inside the delay-layer baseline + margin: Room ≤ 1.75 / 4.50 cents, Hall ≤ 0.50 / 1.10 — §3.6's Layer C added no measurable F0 deviation |
| **Flutter** | autocorrelation of the trend-removed dB envelope | no peak > 0.2 at any `lineSamples_i`, at `lineSamples_i·(1+apRatio_i)`, or at `lineSamples_i·apRatio_i·k` for k ∈ 1..12 |
| **Spectral flatness** | smoothed IR magnitude vs local median | no peak > 10 dB above (at `color = 0`); **inverted at `color > 0.5`** |
| **Mono sum** | \|L+R\| vs \|L\| | no notch deeper than 6 dB |
| **Rate independence** | all of the above at 44.1 / 48 / 96 kHz | flutter and notch frequencies must not move with rate |
| **Nonlinearity** | probe at −6 dBFS pink noise, not just an impulse | T60 within 15% of the impulse-measured T60 |
| **Jot solve unit test** | measured per-pass gain per line at 250 Hz / 1 kHz / 8 kHz | within 0.5% of `10^(−3·D_i/T60_band)` |
| **Matrix unitarity** | prepare-time assert | `cond(Â) < 1.001` |

**AMENDED 2026-08-06 — the two echo-density rows are measured over a 12 kHz analysis
band.** `η(t)` is a sample-domain statistic, so it is only comparable across host rates
if every rate is measured over the same audio band; unbanded, the Plate build-up read
27 / 28 / 93 ms to reach 0.95 at 44.1 / 48 / 96 kHz. Band-limiting fixes almost all of
it, and resampling identifies the residual as signal rather than estimator: at a 16 kHz
band the ER row read 0.944 / 0.944 / 0.890, and the 48 kHz render *resampled* to 96 kHz
still read 0.941 while the 96 kHz render resampled to 48 kHz still read 0.889. What
differs is the top octave — the tank and pre-delay reads are 4-point Hermite, whose loss
inside a 16 kHz band shrinks as the host rate rises (§3.4), so the higher rate renders a
genuinely sharper early field and a sample-domain density statistic reads sharper as
sparser. Below 12 kHz that term is gone and the same row reads 0.985 / 0.964 / 0.967.
This is the boundary §8's own per-octave row already draws (±35 % for 8 kHz and above
against ±15 % below) applied to the density rows. No threshold moved.

**AMENDED 2026-08-07 — the per-octave tail-LEVEL row is new, beside the T60 row.**
Energy ∝ T60 means a Schroeder slope cannot see a level offset: before §3.4's tonal
correction, `bassmult ×4` measured +4.5..+5.2 dB of low-octave steady-state level on
every tank mode against the 3 dB bound while every then-existing row passed. The row
renders at `erLevel = 0` — the direct ER follows neither knob and the correction shelves
rescale it too (§3.4's caveat) — and integrates whole-IR band energy, which by
convolution is exactly the steady-state wet power sustained material hears. A
late-field-only statistic was tried and rejected: a Schroeder-fit intercept explodes on
the crossover-straddling 4 kHz octave and on the sub-200 ms T60s the damping extreme
reaches, and a fixed tail-start truncation re-introduces the decay-TIME dependence (a
shorter T60 has decayed further by any fixed instant) that the correction rightly
leaves alone. The integral statistic under-reads movement where the first tank pass —
knob-independent, pre-Filt1 — carries a large share of a band's energy, which is what
Hall's reduced `tonalCorrect` (§4.1) is tuned against. Applies to Room, Plate and Hall
(and thereby Shimmer, which shares Hall's row); Spring and Reverse take
`tonalCorrect = 0` and are asserted bit-identical elsewhere instead.

**AMENDED 2026-08-07 — the two decay-normalisation rows are new, and neither is a
flatness bound on a THD number.** *Level:* it is measured in the 500 Hz octave, not
broadband. The RMS law of §3.2 is a per-band statement, and the HF band's T60 is capped
by `hfMaxT60`, so a broadband statistic averages the band that follows the knob with one
that stops following it — 1.4 dB of Room's movement hides there. 500 Hz is in the mid
band of every tank mode and within a quarter-octave of Spring's passband centre at the
default damping, i.e. the band whose T60 `reverb_decay` actually pins in all four. The
row runs once, at the first rate asked for: a level law is rate-independent by
construction, since `g` and `gRef` both move with `fs` through `D`. At 44.1 kHz, before
the change it read 6.8 / 7.7 / 5.7 / 6.2 dB of movement on Room / Plate / Hall / Spring
against the ±1.5 dB bound; after, 0.7 / 0.7 / 1.7 / 1.4 dB. *Distortion:* measured by
subtraction rather than as a harmonic ratio, and gated directionally. A sustained sine
is the wrong excitation — a tone builds coherently in the tank (~1/(1 − g)), which no
RMS-law normalisation can flatten, and it reads FFT leakage at every decay where the
clipper is identity; noise builds by the incoherent 1/√(1 − g²) law the correction is
stated on and is what material through a NAM block looks like. Everything in the engine
except §3.5's clipper is linear — time-varying, but identically so in both renders, since
the walk is seeded and the parameters match — so a hot render minus a scaled-up quiet one
leaves the clipper's own contribution. The gate is directional because matching line RMS
cannot match crest factor with it (§3.2's recorded consequence (a)): what the change
removes is distortion GROWING with Decay, where the tail also recirculates it hundreds of
times. Room read −61.9 dB at 1.5 s against −47.0 dB at its 2.5 s ceiling before, and
−71.0 against −73.9 after; Plate −88.3 against −36.1 before, −89.0 against −89.6 after.
The companion 1 % bound on the worst point of the sweep is not a free pass in either
direction: the pre-change build broke it at Plate's ceiling (−36.1 dB), and it is what
holds the short end after, where the injection is boosted up to the +6 dB clamp.

**AMENDED 2026-08-07 — the pitch-stability row is new, and its statistics are
amplitude-aware on purpose.** The row is §3.6 Layer C's discriminator: liveliness
bought with deeper delay modulation (the standard fix for static-FDN metallicity)
moves it, liveliness bought on the unitary group must not. *Estimator:* the
instantaneous frequency of a narrowband signal is unbounded at envelope zeros —
`d/dt arg z` spins freely as `|z| → 0` — so a raw worst-hop statistic measures the
estimator, not the tone: any modulation that sweeps the 440 Hz response through a
deep dip printed hundreds of phantom cents there (174.5 measured on an early draft of
the row). The two gated statistics are keyed to what PITCH modulation, as opposed to
spectral motion, must do. The level-weighted rms is the deviation of the tone as
heard — a frame counts in proportion to how loud the tone is in it. The worst hop at
or above the MEDIAN level exploits an asymmetry: Doppler detunes every frame, loud
ones included, while a response dip transiting 440 Hz slews the phase only in frames
it has already pulled below median by definition — so the statistic passes a moving
notch (a phaser, which is the liveliness) and fails a coherent detune (a chorus,
which is the seasick failure mode). The worst hop down to −12 dB of median is
reported ungated as the notch-transit figure. *Measured, delay layers alone → with
the shipped Layer C:* Room rms 1.445 → 1.489 cents and worst loud-frame hop
3.815 → 3.103 (down — the rotation decorrelates the dip pattern the delay layers
made); Hall 0.386 → 0.391 and 0.827 → 0.836. Against the same bounds, the rotation
run at Layer-B-territory rates (multipliers ×4) reads 19.7 cents on the ungated
statistic — the measurement behind §3.6's slow-and-deep rate choice. *Pinned at
48 kHz* like the clip-aliasing row: depths and rates are solved in seconds, so the
statistic is a property of the modulation law, not the host rate, and Room + Hall
bound the new layer's deepest setting and the baseline's deepest delay modulation
respectively. Two estimator repairs rode along, both places where a gate was
differencing readings below its method's own resolution and the rotation perturbed
the residue. The Shimmer grid's 2–4 kHz convergence check now honours the −120 dBFS
staynormal floor its own section header already documented (as a skip, so a genuine
runaway climbing out of the floor is still caught at the first pair whose earlier
window is back above it) — at `decay = 0.2 s` every 10 s window past the first is
§6.6's ~−390 dBFS residue, whose numerical wobble the rotation modulates and which
is not a convergence claim about signal. And the decay-normalisation distortion
gate's 1.5 s reference is floored at −78 dB before differencing: the subtraction
method bottoms out at the engine's recirculated float-rounding residue (~−83 dB
broadband, the same figure the clip-aliasing row's estimator is built around), and
Layer C moved Room's 48 kHz reference −79.8 → −83.5 dB *while lowering the ceiling
reading too* — an improvement at every decay point that the unfloored difference
read as +3.2 dB of growth. The floor sits 5 dB above the measured residue and
30+ dB under that gate's recorded failure exemplars (−47 / −36 dB), so what it
polices is untouched.

**AMENDED 2026-08-07 — the colour-1 per-octave T60 row is new, and it gates at
`color = 1`, fatally, with targets that CARRY the voicing.** §5.1's axes 1 and 4 are
in-loop, so at the knob's far end they shorten per-band decay on purpose; the row's
targets fold the colour LP's exact per-pass loss and the blended kernel's droop delta
(credited with the `r_hi` re-solve's compensation at `f_h` for bands at/above the
crossover — below a one-pole's corner its passband gain pins near 1, so the re-solve
cannot reach those bands) into per-line decay rates, sample the Jot law at five
frequencies per octave (Hall's 4 kHz octave straddles its 3.5 kHz crossover, and the
longer-ringing side dominates a late fit), and run the same Schroeder estimate on the
synthetic eight-line envelope. It runs at damping 0.5, not the flat row's 0.25: with
`T60_high = T60_mid` the `r_hi` solve clamps at `1 − 1e-4` and the compensation is
structurally inert (§3.4's caveat of this date). Fatal for the clip-aliasing row's
carve-out reason: the targets internalise the voicing, so a miss is §3.4 losing the
decay — a defect at every colour. Measured margins at the shipped voicing: every band
inside its bound at 44.1/48/96 kHz, worst ±15 %-band reading +13.1 % (Plate 125 Hz,
low-frequency modal variance also present in the colour-0 row) and worst ±35 %-band
−17.9 % (Hall 16 kHz at 96 kHz). Two honesty notes, both measured. (a) The row is a
REGRESSION bound, not a differential detector: against the pre-axis-4 build its worst
movement was Hall's 4 kHz (−10.1 % → −5.1 %) — the voicing deltas at these loop
lengths sit inside the tolerance the estimator itself needs, so the row cannot "fail
before / pass after"; what it pins down is that the era axis can never silently grow
beyond its declared per-pass losses. (b) With the line-read-only blend (§5.1's
rationale), the compensation's own T60 footprint in the gated bands is 2–6 % — the
colour LP dominates at `color = 1` — so the re-solve is held by this row as a bound
rather than exhibited by it.

**AMENDED 2026-08-07 — the clip-aliasing row is new, and it gates at `color = 1`,
fatally.** The exit contract's "any gate failure at color = 0" drew the line where the
colour VOICING inverts the spectral rows; this row uses `color` only as DRIVE —
`inSat` at its 0.60 maximum (§5.1 axis 2) — and aliasing is a defect at every colour,
so it counts toward the exit code. *Separability by construction:* the two tones sit at
9 and 11 times one exact-bin base `k0` (~110 Hz), so every harmonic and
intermodulation product `|m·f1 + n·f2|` lands on the multiples-of-`k0` comb, while a
fold about Nyquist lands `N mod k0` bins OFF it (`k0` is nudged if a rate ever puts
that inside the exclusion width). `mod = 0` keeps every delay static, so §3.5's clipper
is the only source of off-comb energy above the engine's own float floor. *Estimator
care, each item bought by a measured failure:* periodic Hann, not symmetric — the
symmetric form's skirt sums to ~−90 dB off-comb on its own; warm-up of two T60s — the
onset transient is non-stationary and still read −62 dB of broadband smear at a 3 s
warm-up against a 4.5 s T60; a ±30 Hz skirt around each fundamental is ignored — the
tank modes adjacent to a driven tone carry recirculated float noise at ~−88 dB; and the
aliased sum reads the fold comb only (±1 bin), because the full off-comb floor
integrates to ~−83 dB over 64k bins and would swallow the effect. *Drive:* +6 dBFS
summed at the decay ceiling is deliberate — a tone builds coherently as `1/(1 − g)`,
past §3.2's incoherent normalisation, and at 0 dBFS the knee is barely touched (−114 dB
of products); at the chosen drive the wet peak is 0.644, safely under §3.1's limiter.
*Pinned at 48 kHz:* at 44.1 kHz the same tones land near enough to a Plate resonance
that the wet crosses the limiter (peak 0.994) and its single-pass distortion floods the
fold comb; rate independence is a claim about the linear metrics, not about where
aliases fold. *Before → after:* −83.0 → −91.0 dB on the whole fold comb, −87.2 →
−102.4 dB below fs/4. The near-Nyquist folds move only ~6 dB — they come from products
just above Nyquist, which a first-order antiderivative's sinc envelope can only
attenuate a few dB — so the ≥10 dB the change was sized against is gated where ADAA1
can deliver it, below fs/4, and the whole-comb bound holds the rest. Spring appears
nowhere in this row: it never reaches §3.5's clip (its saturation is Dwell, §3.9,
input-side and single-pass), the same reason Dwell and Reverse's window-line clip stay
pointwise under §3.5's amendment.

Reverse is a finite window, not a tail: the echo-density, T60, flutter and
spectral-flatness rows do not apply to it. Its gates are the six of §3.12, plus the
mono-sum and rate-independence rows of this table, which apply as written.

The spectral-flatness row is additionally run on the **Early Energy path in isolation**
with `reverb_diffusion` maxed, which is what keeps §3.7's no-allpass rationale a measured
claim after that section's 2026-08-06 amendment.

The nonlinearity gate exists because every other metric is measured from an impulse,
where levels sit far below the clipper's threshold and the clipper is identity — while
real material through a NAM amp block will drive it.

Run the whole suite against the probe-local Freeverb instance (Stage 0) as the
published before/after.

---

## 9. Open questions and rejected alternatives

### Open

- **6-point Lagrange for the tank reads.** If the compensated `r_hi` still leaves the
  8 kHz+ octaves outside their ±35% band, switch. ~10 more mults/line/sample. Decide
  with Stage-1 probe data, not by ear.
- **Strict Dahl-Jot absorbent allpass.** Needed only if Plate at `apG = 0.60` still shows
  bass-band T60 spread beyond the 63/125 Hz gate. Would remove the `apRatio`-into-`D_i`
  approximation entirely.
- **Spring at 3 instances at 192 kHz.** ~900 multiplies/sample of allpass cascade. Small
  in absolute terms but the only mode materially above the others, and the one most
  likely to be used 2–3× in a chain. Measure before Stage 2.

### Rejected

- **N hard-wired render functions per mode** (Costello's actual approach). Correct for a
  company shipping 22 modes with a dedicated DSP author. Here it multiplies the surface
  that must be kept denormal-safe, allocation-free and correct across three instances and
  two lane geometries. One core + ModeSpec, with Spring and Shimmer as bolt-ons where the
  core genuinely cannot stretch.
- **Dattorro figure-8 as the core.** Superb per line of code and fully public, but it is
  one fixed geometry — Gardner published three separate delay tables precisely because a
  single nested-allpass structure cannot cover small-room through hall. Its two-tap-set
  stereo IS adopted (§3.8); its topology is not.
- **CloudSeed-style parallel comb loops.** No mixing matrix at all — the lines never
  exchange signal — so there is no modal room signature, only smear. And ~67 MB
  statically allocated per stereo instance.
- **16 lines.** Dal Santo et al. showed optimized N = 4/6/8 match much larger FDNs in
  MUSHRA; the binding constraint is `Σ M_i ≥ 6000`, which 8 lines clear easily. Doubling
  doubles memory and reset cost for no measured gain.
- **Householder feedback matrix.** §3.2.
- **A `density` matrix blend.** §3.2 — it is a strict contraction with 14 dB of singular
  value spread, which is a worse version of the defect this design exists to remove.
- **Ambience and Bloom as modes.** §4 — presets, not machines.
- **Hybrid ER convolution.** Duplicates the cab's IR-loading path and a fixed ER IR
  cannot sweep; guitarists expect to move Size and Decay live.
- **Fixed internal rate with Airwindows-style decimation.** §6.8.
- **Dual-engine crossfade on mode change.** §6.2.
- **Decomposing reverb into placeable sub-blocks.** `ChainOrder.h` uses 28 of 31 BlockIds
  and the 5-bit packing assert fires at 31 (`ChainOrder.h:489`). Would be a packing-format
  change, and would break the contiguous-pair arithmetic `instanceOf`/`instanceId` depend
  on. Reverb stays one tile.
- **Keeping Freeverb as a "Legacy v1" mode** — the first draft's compatibility
  keystone. Overruled by dated user directive (2026-08-06): a two-engine dispatcher
  doubles the surface that must stay denormal-safe, allocation-free and lane-correct,
  and freezes §1's defects into the product forever to protect content the user chose
  to re-voice. Superseded by absent-means-default → Room on both paths (§5.3); the
  deleted engine survives only inside `tubamp_verbprobe` as the Stage-0 baseline.
- **A true time-reversed capture for Reverse.** Record-then-play-backwards is a window
  of latency by definition — the first invariant. §3.12's rising tapped FIR is what
  the hardware lineage actually does, and it is zero-latency.
- **A versioned state key to resolve legacy states.** Unnecessary once both
  serialization paths run the same absent-means-default sweep (§5.3) — it would be a
  second mechanism waiting to disagree with the first.
- **`juce::dsp::DelayLine`.** §3.6.
- **Linear interpolation on the tank reads.** §3.4 — and Costello's 300-traversal rule
  means any per-pass artefact is multiplied ~300× at long decays.
  > **AMENDED 2026-08-07 — the rejection stands as written, for the UNCONDITIONAL
  > case; §5.1 axis 4 sidesteps it rather than overturning it.** What was rejected is
  > linear as *the* tank interpolator: silent, always-on, uncompensated compounding
  > loss. Axis 4 fades a cubic↔linear blend in BY THE KNOB — `color = 0` takes a
  > branch and reads the cubic bit-for-bit (the same mechanism that saved axis 1's LP
  > from this list) — and the 300-traversal compounding is not an accident there but
  > the product: it is re-solved into `r_hi` through the blended kernel's exact
  > `|Hi(f_h)|_mean`, gated by §8's colour-1 per-octave row. Degradation chosen and
  > compensated is character; degradation imposed and unaccounted stays rejected.
- **Modulating allpass coefficients rather than delays.** Documented as a mistake.
- **Equal-power wet/dry mix law.** Better in the abstract; would alter every existing
  preset's balance.
- **GPL sources.** freeverb3 (GPLv2) and Dragonfly (GPL-3.0) are read-and-taint hazards;
  Faust's `vital_rev` and `kb_rom_rev1` are GPL-3.0; Signalsmith's
  `reverb-example-code` has **no licence file at all**. Every constant in this document
  comes from a published paper, a manufacturer's own documentation, or an MIT/STK-4.3
  source (Faust's `jos` section, `reverbsc`, Airwindows, el-visio/dattorro-verb). Keep a
  written note of which source each constant came from.

---

## 10. Sources

**Valhalla / Sean Costello**
- https://valhalladsp.com/2011/01/21/reverbs-diffusion-allpass-delays-and-metallic-artifacts/
- https://valhalladsp.com/2023/12/13/valhallavintageverb-4-0-0-new-reverb-modes-chamber1979-and-hall1984/
- https://valhalladsp.com/2023/02/10/valhallavintageverb-the-modes/
- https://valhalladsp.com/2011/05/18/valhallaroom-the-late-controls/
- https://valhalladsp.com/2011/05/04/valhallaroom-early-reflections-versus-early-energy/
- https://valhalladsp.com/2011/07/07/algorithmic-reverbs-distortion-and-noise/
- https://valhalladsp.com/2009/08/02/more-general-reverb-tips/
- https://valhalladsp.com/2010/11/30/valhallashimmer-tips-and-tricks-diffusion/
- https://valhalladsp.com/2015/11/08/the-physics-and-psychophysics-of-plates/
- https://valhalladsp.com/2022/10/21/the-more-you-know-valhalla-modes-algorithms/
- https://valhalladsp.com/2020/05/06/valhallasupermassive-the-controls/
- https://valhalladsp.com/2025/10/03/valhallafutureverb-the-color-modes/
- https://valhalladsp.com/2021/09/22/getting-started-with-reverb-design-part-2-the-foundations/
- https://www.aes-media.org/sections/pnw/ppt/costello/AES2015ReverbPresentation.pdf
- http://www.spinsemi.com/forum/viewtopic.php?t=3 (Costello & Keith Barr thread)
- http://www.spinsemi.com/knowledge_base/effects.html (Barr's allpass-loop article)
- https://www.kvraudio.com/forum/viewtopic.php?t=331150 (the 1.0.8 negative-delay bug)
- https://www.kvraudio.com/forum/viewtopic.php?t=624427 (Size automation artefacts)

**Strymon**
- https://www.strymon.net/manuals/BigSky_UserManual_RevD.pdf
- https://www.strymon.net/manuals/BigSky_MX_UserManual_RevB.pdf
- https://www.strymon.net/manuals/Nightsky_UserManual_RevF.pdf
- https://www.strymon.net/manuals/blueSky_UserManual.pdf
- https://www.strymon.net/flint-reverb-summary-paper-three-classic-reverb-types/
- https://www.strymon.net/dsp-anyway/

**Literature**
- https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf (Dattorro, JAES 45(9) 1997)
- https://ccrma.stanford.edu/~lukedahl/pdfs/Dahl,Jot-AbsorbentAllPassReverbs-Dafx2000.pdf
- https://www.ee.columbia.edu/~dpwe/papers/Gardner92-virtroom.pdf
- https://ccrma.stanford.edu/~jos/pasp/Artificial_Reverberation.html (and the
  `FDN_Reverberation`, `Householder_Feedback_Matrix`, `Choice_Delay_Lengths`,
  `Achieving_Desired_Reverberation_Times`, `Zita_Rev1_Delay_Line_Filters`,
  `Delay_Line_Interpolation`, `Freeverb` sub-pages)
- https://ccrma.stanford.edu/courses/318/mini-courses/rooms/mus318_Abel_Lecture/echo%20density.pdf
- https://eprints.whiterose.ac.uk/id/eprint/226786/1/s13636-025-00401-w.pdf
  (Dal Santo, Prawda, Schlecht, Välimäki — tiny colorless FDNs, M ≥ 6000)
- https://www.dafx.de/paper-archive/2011/Papers/39_e.pdf (Gamper/Parker/Välimäki spring
  calibration — the KA-1210 numbers)
- https://research.aalto.fi/en/publications/parametric-spring-reverberation-effect
- https://aes2.org/publications/elibrary-page/?id=13788 (Abel, Berners, Costello & Smith,
  spring waveguide scattering)
- https://tre.ucsd.edu/wordpress/wp-content/uploads/2018/10/reverbtopo.pdf (Erbe)
- https://www.soundonsound.com/people/david-griesinger-lexicon-creating-reverb-algorithms-surround-sound
- https://citeseerx.ist.psu.edu/document?doi=e69cb5643d81264ed96201e31cef7a7d375408bb&repid=rep1&type=pdf
  (Rocchesso & Smith, circulant FDNs)

**Implementations**
- https://github.com/csound/csound/blob/develop/Opcodes/reverbsc.c
- https://github.com/grame-cncm/faustlibraries/blob/master/reverbs.lib — `zita_rev_fdn`
  and `dattorro_rev`; `vital_rev;
  `kb_rom_rev1`
- https://github.com/el-visio/dattorro-verb
- https://github.com/airwindows/airwindows
- https://github.com/GhostNoteAudio/CloudSeedCore
- https://github.com/michaelwillis/dragonfly-reverb
- https://github.com/Signalsmith-Audio/reverb-example-code
