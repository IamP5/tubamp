# Reverb and Delay Engines — Design Spec

Status: implemented, 2026-11-11. Branch: `feat/reverb-delay-pro`.

The reverb and delay blocks grow from one fixed algorithm each into a family of
engines selected by a per-instance **type** parameter. The original algorithms
(`juce::dsp::Reverb` and a clean stereo delay) are gone: a session or preset written
before the types existed loads on the default types, `Digital` and `Plate`, and sounds
different from how it was saved (§4).

Sections: §1 parameter contract (frozen) · §2 reverb design · §3 delay design ·
§4 older sessions · §5 verification · §6 research basis.

## Invariants carried over (chain-v2, STEREO.md, SPLIT.md)

- APVTS ids are frozen. New parameters are APPEND-ONLY at the end of
  `createParameterLayout()`, literal id constants in `Parameters.h`, instance-indexed
  arrays (never string concatenation).
- Choice lists are frozen once shipped: append only, never reorder. The C++ enums
  (`ReverbFx::Type`, `DelayFx::Type`, `kDelaySyncBeats`) are indexed by them.
- The audio thread never allocates or locks. Every engine allocates in `prepare()`.
- Reset-on-reentry via `pendingReset` is unchanged; a pending block processes as bypassed.

## 1. Parameter contract

Instance 1 ids are shown; instances 2 and 3 use the `delay2_` / `delay3_` and
`reverb2_` / `reverb3_` prefixes, with host names "Delay 2 Type" etc.

### 1.1 Delay — seven new parameters per instance

| id | host name | kind | range / choices | default |
|---|---|---|---|---|
| `delay_type` | Delay Type | choice | `Digital`, `Analog`, `Tape`, `Lo-Fi`, `Diffuse` | 0 (`Digital`) |
| `delay_sync` | Delay Sync | choice | `Off`, `1/32`, `1/16T`, `1/16`, `1/8T`, `1/16.`, `1/8`, `1/4T`, `1/8.`, `1/4`, `1/2T`, `1/4.`, `1/2`, `1/2.`, `1/1` | 0 (`Off`) |
| `delay_lowcut` | Delay Low Cut | float, log | 20 – 1000 Hz, step 1 | 80 |
| `delay_highcut` | Delay High Cut | float, log | 1000 – 20000 Hz, step 1 | 7000 (§3.4) |
| `delay_mod` | Delay Mod | float | 0 – 1, step 0.001 | 0.2 |
| `delay_color` | Delay Color | float | 0 – 1, step 0.001 | 0 (§3.4) |
| `delay_duck` | Delay Duck | float | 0 – 1, step 0.001 | 0.25 (§3.4) |

Existing and unchanged: `delay_on`, `delay_time`, `delay_feedback`, `delay_mix`,
`delay_mode`, `delay_ratio`, `delay_width`.

- With `delay_sync` ≠ `Off` the base time is the note length at
  the host tempo (120 BPM when the host reports none), clamped to 20–2000 ms, and
  `delay_time` is ignored. `kDelaySyncBeats` in `Delay.h` holds the beat lengths in the
  same order as the choice list; a `static_assert` ties the two sizes together.
- `delay_color` is the amount of the selected type's character (§3.3).
- Defaults marked §3.4 or §2.4 are the default type's entry in a per-type table: the
  editor moves those knobs to the picked type's entry when a type is chosen.

### 1.2 Reverb — six new parameters per instance

| id | host name | kind | range / choices | default |
|---|---|---|---|---|
| `reverb_type` | Reverb Type | choice | `Room`, `Hall`, `Plate`, `Spring`, `Ambient` | 2 (`Plate`) |
| `reverb_predelay` | Reverb Pre-Delay | float | 0 – 250 ms, step 0.1 | 10 |
| `reverb_decay` | Reverb Decay | float, log | 0.2 – 20 s, step 0.01 | 1.6 (§2.4) |
| `reverb_lowcut` | Reverb Low Cut | float, log | 20 – 1000 Hz, step 1 | 120 |
| `reverb_highcut` | Reverb High Cut | float, log | 1000 – 20000 Hz, step 1 | 8000 |
| `reverb_mod` | Reverb Mod | float | 0 – 1, step 0.001 | 0.35 |

Existing and unchanged: `reverb_on`, `reverb_size`, `reverb_damping`, `reverb_mix`,
`reverb_width`. `reverb_damping` keeps its default of 0.5, which is `Plate`'s starting
point (§2.4).

### 1.3 Append order

`createParameterLayout()` appends, after everything that exists today:

1. For delay instance 1, then 2, then 3: `type`, `sync`, `lowcut`, `highcut`, `mod`,
   `color`, `duck`.
2. For reverb instance 1, then 2, then 3: `type`, `predelay`, `decay`, `lowcut`,
   `highcut`, `mod`.

39 parameters: 30 floats and 9 choices.

### 1.4 WebView relays

Appended to the relay id tables in `WebEditor.cpp` and mirrored by the TS arrays:

- Sliders (+30 → 119): `delay_lowcut`, `delay_highcut`, `delay_mod`, `delay_color`,
  `delay_duck`, then the same five for `delay2_`, then `delay3_`; then
  `reverb_predelay`, `reverb_decay`, `reverb_lowcut`, `reverb_highcut`, `reverb_mod`,
  then the same five for `reverb2_`, then `reverb3_`.
- Combos (+9 → 17): `delay_type`, `delay_sync`, `delay2_type`, `delay2_sync`,
  `delay3_type`, `delay3_sync`, `reverb_type`, `reverb2_type`, `reverb3_type`.
- Toggles: unchanged (30).

### 1.5 Engine API

`src/dsp/Delay.h` and `src/dsp/Reverb.h` (included by `FxBlocks.h`) replace the old
positional `setParameters` overloads with `DelayParams` / `ReverbParams`. The processor
fills one struct per instance per block; `DelayParams::bpm` comes from the host play
head.

## 2. Reverb design

Code: `src/dsp/Reverb.{h,cpp}`, building blocks in `src/dsp/FxPrimitives.h`.

### 2.1 Shell

```
in ──┬──────────────────────────────────────────────── dry ───────────────┐
     └─► pre-delay ─► low cut ─► high cut ─► ENGINE ─► width (M/S) ─► wet ─┴─► out
         0–250 ms     12 dB/oct, wet only
```

- **Pre-delay** moves crossfade between two integer taps (30 ms, equal-power): no pitch
  bend, no click.
- **Low/high cut** sit *ahead* of the engine, so the tank is never excited by rumble or
  fizz, and the tail keeps its own damping.
- **Mix** is constant-dry: `dry = min(1, 2 − 2m)`, `wet = min(1, 2m)^1.5`. The dry signal
  holds unity up to the half-way point, so adding reverb never pulls the guitar down, and
  the wet curve gives the bottom of the knob fine control.
- **Loudness**: every engine is trimmed to the same wet energy for the same input
  (§5). The lattice normalises its own taps (§2.2), so its level holds steady as decay
  and size move; the spring takes back half of the 3 dB per doubling that a longer ring
  would otherwise add.
- **Type changes** go through `fx::EngineSwitcher`: the effect fades out to the untouched
  input over 20 ms, the incoming engine is reset while silent, and the effect fades back
  in. Two engines never run at once. Clearing an engine is megabytes of memory, so on a
  switch it is done a bounded amount per block while the output is held on the dry
  signal (worst block ≈ 0.1 ms with three reverbs switching together at 192 kHz).
- A non-finite input sample is kept out of the delay memory, where it would otherwise
  stay until the next reset.
- Host blocks are processed in chunks of 256 through fixed scratch buffers, so no block
  size reaches the heap.
- Every modulator restarts from a fixed state on `reset()`: two renders are bit-identical.

### 2.2 The lattice — Room, Hall, Plate, Ambient

One engine (`LatticeReverb`), four voicings (`LatticeVoicing`):

```
a = L + loop    b = R + loop        the two delayed channels
c = L + loop    d = R + loop        two side channels that are never delayed

for each of 20 stages, shortest delays first:
    a, b   ← absorb(delay(a)), absorb(delay(b))      odd stages modulated
    out   += tap · (a, b)                            alternating L/R, signed
    (a, b) ← rotate 45°, sense alternating stage to stage
    (a, c), (b, d) ← rotate 45°
loop ← a, b, c, d
```

- **No recirculation inside the chain.** Each stage hands half the signal to a side
  channel that skips the delays that follow, so a path through the chain collects the
  delays of some subset of the stages — about a million distinct arrival times, none of
  them periodic. Echo density grows explosively without a single short comb, which is
  where a metallic ring comes from.
- **A very long loop.** The only feedback is around the whole chain, whose forty delays
  add up to seconds (`chainLo`…`chainHi`, set by `Size`). A loop has as many resonances
  per hertz as it has seconds of delay, so they sit far closer together than the ear can
  resolve: the tail has the statistics of noise with the modulation switched **off**.
- **A continuous build-up.** The delays are one geometric series (ratio 1.3 per stage,
  the two sides half a step apart), so the burst reaching each stage's tap is already
  longer than the gap to the next stage. Early sound and late tail are one process;
  there is no seam between an "early" and a "late" section to hide.
- **An exact decay.** Every rotation is lossless, and each delay is followed by an
  absorption filter (`fx::DecayFilter`: a broadband gain with first-order low and high
  shelves) sized to its own length, so a path loses exactly as much as the time it took,
  per band, whatever route it followed. `Decay` is the mid-band T60; the voicing sets
  the bass multiplier and `Damp` the treble ratio and its crossover.
- **Taps place the listener.** Starting the output taps at a later stage, and fading
  them in over a few stages, removes the earliest arrivals and slows the attack.
  The tap gain is normalised from the stages' weights and the decay each has already
  taken, which is what keeps the level steady across decay and size.
- **Modulation is optional.** Odd stages move, the two sides in opposite directions (so
  the average pitch stays put), driven by two sines that never align plus two
  band-limited random sources. `Mod` 0 is completely static and still smooth.
- **Size** scales every delay; the scale glides over 200 ms, so a size move bends pitch
  briefly instead of clicking.

| Voicing | Loop delay (size 0 → 1) | Taps open from | Character |
|---|---|---|---|
| Room | 0.8 → 2.4 s | stage 2, 2-stage fade | present, speaks within 4 ms |
| Hall | 2.4 → 5.5 s | stage 5, 5-stage fade | set back, slower swell, bass rings longest |
| Plate | 1.2 → 3.0 s | stage 0 | dense from the first milliseconds, bright |
| Ambient | 4.0 → 8.0 s | stage 8, 6-stage fade | long fade-in, deepest modulation |

The lattice is the third network tried; the first two were measured and replaced:

1. A 16-line Hadamard feedback delay network with series-allpass input diffusion:
   accurate decay, but 13.6–18 dB on the ringing metric with modulation off.
2. The same network with several seconds of line length and a four-stage feed-forward
   diffuser in front: 9.5–11.3 dB, but a 3–4 dB sag in the build-up between the diffuser
   burst and the first line returns, and 2.2 % of a core.
3. The lattice: 9.3–11.0 dB, a build-up that is monotonic by construction, 0.9 % of a core.

Also tried and dropped along the way: an allpass inside every line's loop (rougher, no
denser) and a Dattorro figure-eight tank for the plate (155 ms to full echo density).

### 2.3 Spring

Three springs, each a feedback loop around ~100 "stretched" first-order allpasses
`(a + z^−K)/(1 + a z^−K)` — the dispersion model of Välimäki, Parker & Abel (JAES 2010).
A spring carries high frequencies slower than low ones, so every echo arrives as a
chirp; the allpass chain's group delay grows from ≈ 2 ms at DC to ≈ 90 ms at the band
edge, and because it is inside the loop each lap smears further, as in a real tank. The
loop gain is negative (the wave inverts at the spring's end), the pickup tap sits half a
round trip in, and the output is low-passed just under the band edge (≈ 4 kHz), like a
tank's pickup. `Size` is spring length, `Decay` the loop gain (capped at 6 s), `Damp` a
low-pass in the loop, `Mod` random length wobble. Two springs go to the sides and the
third to both: wide, but mono-safe.

### 2.4 Where each type starts: decay and damping

A parameter has one default, but two reverb knobs only make sense relative to the kind
of space, so each type has its own **starting point** for them (`kReverbDecayDefault`
and `kReverbDampingDefault` in `Reverb.h`, mirrored in `ui/.../typeDefaults.ts`). The
parameter default is `Plate`'s, the default type. Picking another type in the editor
moves those two knobs to that type's entries and leaves every other knob alone; host
automation and preset recall never trigger it.

The rule for which knobs get a table: only those whose sensible range depends on the
type. Mix, width and pre-delay are the player's choices (the types already speak at
different speeds, §5.1, so a per-type pre-delay would duplicate that), and size and mod
are already scaled per type inside the engine.

**Decay.** A room that rings for 1.6 s is a hall, and an ambient wash that lasts 1.6 s
is not one.

| Type | Starts at | Where it comes from |
|---|---|---|
| Room | 0.7 s | LX480's medium room preset is labelled 0.69 s |
| Hall | 2.2 s | Lexicon 224 halls measure 2.2–2.8 s at their defaults |
| Plate | 1.6 s | Lexicon 224 plates 1.55–2.1 s; LX480 plate 1.53 s |
| Spring | 1.8 s | the published spring model's loop gain (−0.8 on a 56 ms loop) is about 1.7 s |
| Ambient | 4.5 s | Raum measures 4.3 s at its default decay |

**Damping.** `Damp` means the same thing in every type: how much faster the treble
decays than the mids, and from what frequency up. It is in-loop damping, not an EQ on
the output — the treble's decay time and its level move together, as they do in every
reference reverb measured (§6).

| Knob | Treble decay ÷ mid decay | Crossover |
|---|---|---|
| 0 | 0.97 | 14 kHz |
| 0.5 | 0.45 | 7.4 kHz |
| 1 | 0.10 | 2 kHz |

Geometric between those points. The numbers are what the absorption filters are set
to; a first-order shelf is gentle, so the 8 kHz octave measures slower than the target
(third column below).

| Type | Starts at | Measured 8 kHz ÷ 1 kHz decay | Where it comes from |
|---|---|---|---|
| Room | 0.65 | 0.45 | Lexicon 224 Room A 0.44; FutureVerb's Room sets treble decay to 0.41 × the decay time |
| Hall | 0.65 | 0.54 | Lexicon 224 halls 0.46–0.52; FutureVerb Hall 0.59 |
| Plate | 0.50 | 0.65 | Lexicon 224 plates 0.74–0.88; plate emulations (Pure Plate, Little Plate, Plate-140) 0.50–0.60 |
| Ambient | 0.55 | 0.80 | Raum at its default damping 0.80 |
| Spring | 0.50 | — | No reference measured. The tank's own 4 kHz band limit does most of the work; here `Damp` is a low-pass in each spring's loop (3.5 kHz at 0.5) |

`reverb_damping` is a v1 parameter whose default has always been 0.5, which is why the
knob's centre is calibrated to `Plate`.

## 3. Delay design

Code: `src/dsp/Delay.{h,cpp}`.

```
            ┌────────────────────── feedback × wet ◄──────────────┐
in ─► (+) ─► COLOUR (type) ─► low cut ─► high cut ─► delay line ──┴─► width ─► duck ─► wet
                                                     (Hermite read, modulated)
```

### 3.1 Shared by every type

- **Colour runs on the way into the line**, so the first repeat is coloured once and the
  n-th n times. The low and high cut (12 dB/oct) are inside the loop for the same reason:
  repeats darken cumulatively, the way they do on every reference delay measured.
- **Routing** (`delay_mode`) is unchanged: Stereo, Ping-Pong (mono sum into L, each line
  feeds the other), Dual (R time = L × ratio). Width narrows the wet signal only.
- **Mix** is constant-dry: `dry = min(1, 2 − 2m)`, `wet = min(1, 2m)`.
- **Ducking** follows the dry input (3 ms attack, 250 ms release) and scales the wet
  *output* only — the loop keeps running underneath: `gain = 1 / (1 + 40·duck²·envelope)`.
- **Tempo sync**: `delay_sync` picks a note length at the host tempo (`kDelaySyncBeats`).
- **Time changes**: the clean types crossfade between the old and new tap (40 ms,
  equal-power, no pitch artefact); Analog and Tape slew the delay itself, so the audio
  already in the line bends in pitch as the hardware does.
- **Stability**: no element in any loop has gain above 1 except through a saturator that
  bounds it, so at maximum feedback every type decays (§5).

### 3.2 Time modulation (`delay_mod`)

- Digital, Analog, Lo-Fi, Diffuse: a 0.6 Hz sine, right channel a quarter cycle behind;
  depth `4 ms × mod²` (≈ 1 cent rms at the default 0.2, ≈ 19 at full).
- Tape: wow (0.55 Hz sine + random drift) and flutter (6.3 Hz + noise), both channels
  together since one transport carries them; `40·mod²` cents of wow plus a little that
  comes with `color` (a worn transport wanders even with the knob down).

### 3.3 Character (`delay_color`)

| Type | What `color` does | Time change |
|---|---|---|
| Digital | 0 = strictly linear loop. Above: a soft limiter with unity small-signal gain, so the feedback knob keeps its meaning | crossfade |
| Analog | Bucket-brigade: 4th-order low-pass whose cutoff falls as the delay grows (`3.4 kHz × √(0.3 s / T)`, 1.5–6.5 kHz) and with `color`; soft saturation with an even-harmonic term (the compander's signature) | repitch, 120 ms lag |
| Tape | Tape saturation; head bump (+1 dB at 90 Hz, with an equal loop trim so nothing exceeds unity); two gap-loss poles sliding from 9 kHz (new tape) to 1.4 kHz (worn) | repitch, two 300 ms lags (capstan inertia) |
| Lo-Fi | Early digital hardware: sample rate 26 kHz → 7 kHz, word length 12 → 7.5 bits, weak anti-alias filters, no dither; the quantiser truncates toward zero, so tails fall into true silence rather than a limit cycle | crossfade |
| Diffuse | Four allpasses per side inside the loop: every repeat smears further; `color` is the allpass coefficient (0.2 → 0.7). Their own delay (≈ 32 / 35 ms) is taken out of the line, so a lap still lasts the time asked for | crossfade |

### 3.4 Where each type starts: color, high cut and duck

As with the reverb (§2.4), three delay knobs have a per-type **starting point**
(`kDelayColorDefault`, `kDelayHighCutDefault`, `kDelayDuckDefault` in `Delay.h`,
mirrored in `ui/.../typeDefaults.ts`): the parameter default is `Digital`'s entry, and
picking a type in the editor moves those three knobs to that type's. Time, feedback,
mix, mode, sync, ratio and width are what the player is working with and never move;
mod is already scaled per type inside the engine.

| Type | Color | High cut | Duck |
|---|---|---|---|
| Digital | 0 | 7 kHz | 0.25 |
| Analog | 0.40 | 12 kHz | 0.15 |
| Tape | 0.35 | 12 kHz | 0.15 |
| Lo-Fi | 0.40 | 7 kHz | 0.20 |
| Diffuse | 0.50 | 7 kHz | 0.40 |

**Color** is a different thing in every type (§3.3), so it cannot share a value.
Digital starts clean, as Replika XT's Modern style does (harmonics below −89 dB at its
default); Diffuse starts where Replika's Diffusion style measurably becomes a wash
(around 50 %). The Analog, Tape and Lo-Fi values are judgement, not measurement.

**High cut.** Analog and Tape darken every repeat on their own — at these starting
points 5 dB at 4 kHz and 27 dB at 8 kHz per pass for Analog, 7 and 15 dB for Tape. A
7 kHz in-loop cut on top would darken them twice, so theirs starts out of the way.

**Duck** pulls the repeats down while you play and lets them back up when you stop:
`gain = 1 / (1 + 40·duck²·envelope)`, where the envelope follows the dry input's peak
(3 ms attack, 250 ms release). Three things about it come from how Replika XT's ducker
was found to work: it listens to the **dry** input, it scales the **wet output only**
(the feedback loop keeps running underneath, so the repeats are still there when you
stop), and its release is a few hundred milliseconds. Unlike Replika's it has no
threshold: a threshold has to be set against the input level, and Replika's default one
did not react to a −12 dBFS signal at all. This one scales with how hard you play.

Replika ships with its ducker at zero. Here each type starts with some, because a
guitar delay that steps back while you play is easier to place, and how much it needs
depends on how much the type's repeats compete with the dry signal:

| Type | Duck | Repeats while playing at −12 dBFS | Why |
|---|---|---|---|
| Digital | 0.25 | −4.1 dB | Full-bandwidth repeats sit right on top of the playing |
| Analog | 0.15 | −1.7 dB | Its repeats are already dark and sit behind |
| Tape | 0.15 | −1.7 dB | Likewise |
| Lo-Fi | 0.20 | −2.8 dB | Band-limited, but gritty |
| Diffuse | 0.40 | −8.1 dB | The repeats smear into a continuous wash, which masks the most |

## 4. Older sessions

A parameter absent from restored state takes its default, so a session or preset
written before the types existed loads its delays as `Digital` and its reverbs as
`Plate`, with the newer controls (cuts, mod, color, duck, pre-delay, decay) at their
defaults. The algorithms it was made with no longer exist, so it will not sound as it
did; there is nothing to migrate it to. State that carries the type ids round-trips
exactly.

## 5. Verification

Tools:

- `tubamp_fxrender` (`tools/FxRender.cpp`) renders one reverb or delay instance offline
  with a known stimulus to a float WAV. It compiles only the two engines, so it builds in
  seconds.
- `tools/audiometrics.py` (numpy only) computes the metrics below from those renders.
- `tubamp_smoke` covers the processor side: type defaults and starting points, every type ×
  mode × layout rendering finite and decaying to silence, tempo sync landing on the note,
  and type ids round-tripping through both restore paths. 19/19 suites pass at 48, 44.1
  and 96 kHz.

Metrics, all from impulse or burst responses at 48 kHz, 100 % wet:

- **T30** — decay time from a line fit to the Schroeder curve, broadband and per octave.
- **Mixing time** — when the normalised echo density (Abel & Huang) first reaches 0.9:
  how quickly discrete echoes become a smooth tail.
- **Ringing** — peakiness of the late tail's spectrum (max over median bin power, averaged
  over third-octave bands). Noise scores ≈ 9 dB; a metallic tail scores much higher.
- **Wobble** — pitch deviation of a steady 1 kHz sine through the effect, in cents rms.

### 5.1 Reverb, each type at its own starting points

| | Decay | T30 | 500 Hz / 8 kHz T30 | Mixing time | Ringing, mod default | Ringing, mod off | Wobble | Onset after pre-delay |
|---|---|---|---|---|---|---|---|---|
| Room | 0.7 s | 0.51 s | 0.60 / 0.31 | 9 ms | 9.2 dB | 9.1 dB | ≈ 1 cent | 4 ms |
| Hall | 2.2 s | 1.89 s | 2.08 / 1.16 | 48 ms | 9.7 dB | 9.0 dB | ≈ 5 | 21 ms |
| Plate | 1.6 s | 1.46 s | 1.73 / 1.02 | 15 ms | 9.7 dB | 9.6 dB | ≈ 2 | 3 ms |
| Ambient | 4.5 s | 4.16 s | 4.56 / 3.08 | 85 ms | 9.9 dB | 9.0 dB | ≈ 8 | 68 ms |
| Spring | 1.8 s | 1.54 s | 1.69 / 0.47 | 6 ms | (chirps by design) | — | ≈ 1 | 24 ms |
| *The reverb this replaced (`juce::dsp::Reverb`, size 0.5)* | — | 1.11 s | 1.32 / 0.77 | 177 ms | 18.9 dB | 18.9 dB | 0 | 25 ms, fixed |

With modulation off the lattice stays at 9.7–11.1 dB out to a 10 s decay.

The same metrics on the reference plugins measured for this work:

- Valhalla FutureVerb, modulation off: 7.2–11.7 dB at a 3 s decay across its modes; at
  10 s its lattice modes stay at or below 10.7 dB and its allpass-cascade modes reach
  11.3–13.5 dB. Default modulation: 2–5 cents. Wet energy constant across decay.
- Lexicon 224 and LX480 programs: 12–14 dB, 1–6 cents.
- Plate emulations: 11–15 dB, 5–80 ms mixing time.
- Raum: 9.4–11 dB (with its default modulation, ≈ 11 cents). Supermassive: 8–10 dB.

Broadband T30 reads 0.8–0.9 × the decay label on Raum and LX480 as it does here: the
label is the mid-band time and treble decays faster.

Also checked:

- Decay tracks the knob from 0.2 s to 20 s (Ambient measures 19.6 s at 20 s).
- Results hold at 44.1 kHz and 96 kHz and on a mono layout.
- Wet energy for the same input is within ±0.7 dB across Room/Hall/Plate/Spring/Ambient
  at their starting points (which differ in decay); on the lattice it moves by at most
  2 dB from a 0.8 s to a 12 s decay and by about 1 dB across the size range.
- A lattice reverb instance costs about 0.9 % of one core at 48 kHz, the spring 1.5 %,
  a delay 0.1 %.
- An AddressSanitizer/UBSan build survives a fuzz of random types, parameters, resets,
  re-prepares, channel counts and block sizes (including zero-length and oversized
  blocks), and stability sweeps of every engine at its extremes.

### 5.2 Delay, each type at its own starting points (300 ms, feedback 50 %)

| | Repeat spacing | Level drop per repeat | Loss per pass at 4 kHz / 8 kHz | Wobble (mod 0.2 / 1) |
|---|---|---|---|---|
| Digital | 300.0 ms | 6.0 dB | 0.5 / 5 dB (its 7 kHz cut) | 0.7 / 19 cents rms |
| Analog | 300.2 ms | 6.1 dB | 5 / 27 dB | 0.7 / 19 |
| Tape | 300.1 ms | 7.6 dB | 7 / 15 dB | 1.5 / 19 |
| Lo-Fi | 300.1 ms | 6.1 dB | 2 / 12 dB | as Digital |
| Diffuse | smeared, lap time as asked | 6 dB in energy | 2 / 6 dB | as Digital |

Analog's bandwidth follows the delay time: with the cuts open and color 0.3 it loses
0 / 5 dB per pass at 80 ms, 4 / 24 dB at 300 ms and 16 / 41 dB at 800 ms.

At feedback 0.95 with colour at maximum and the cuts wide open, every type decays, and
Lo-Fi's undithered quantiser truncates toward zero so its tail reaches exact digital
silence instead of a limit cycle. A mid-render time change produces no sample jump above
twice the steady-state slope in any type.

## 6. Research basis

The design choices above lean on three kinds of evidence gathered for this work.

**Black-box measurement** of installed reference plugins through a headless AU harness
(impulse, burst, noise and sine stimuli; the metrics in §5): Valhalla FutureVerb (all
eight modes), UAD Lexicon 224 (all nine programs), Relab LX480, Valhalla Supermassive
and FreqEcho, NI Raum and Replika XT, UAD
Pure Plate and Capitol Chambers, Soundtoys Little Plate, Arturia Plate-140. What it
established: in-loop filtering that darkens repeats cumulatively; the Lexicon 224's
two-band decay split with a ~2-octave transition, its 1–6 cent tail modulation and the
fact that its Depth/Diffusion controls shape the early field without touching T60;
loudness rising only 1–1.7 dB per doubling of decay time on every reverb that normalises
at all; the constant-dry mix laws; BBD bandwidth tracking delay time; ducking applied to
the wet output only.

**Static analysis** of plugins that ship unencrypted code (symbol-guided disassembly,
and Hopper decompilation of FutureVerb, Supermassive and Raum), read for structure and
laws only:

- Valhalla FutureVerb — the main reference for the lattice. Its natural modes are a
  chain of ~20 delay stages with a rotation after each one that trades signal with
  undelayed side channels, closed by a single feedback around several seconds of delay;
  decay is a three-band attenuator per delay sized to that delay; "early/late" is a
  profile on the stage taps; modulation is optional. Measured with modulation off, it
  is the smoothest reverb of the set, which is what made its structure worth
  understanding. Its other modes are an allpass cascade in a loop and measure rougher
  at long decays.
- Raum: a 31-line network with an orthogonal feedback matrix, damping as one filter per
  line designed from the HF/DC decay ratio, decay-dependent output gain, modulation in
  opposite-signed pairs, engine switches through a fade gate, allpass diffusion on the
  output rather than in the loop.
- Supermassive: 16 delay lines, an orthogonal mixing butterfly whose angle is the
  density control, every line modulated at its own phase, time changes that glide.
- FreqEcho and Replika XT: filters and saturation inside the feedback loop; two delay
  styles — a moving read tap (clean, crossfaded) and a variable-speed line (repitching,
  bandwidth tied to speed); the ducker listening to the dry input.
- The Lexicon family (LX480, UAD 224, MPX Native) is PACE-protected and was not
  disassembled; only its plain symbol names, data-table shapes and preset metadata were
  read, and its behaviour was established by measurement instead. FutureVerb's keyfile
  licensing code was not read either.

**Published literature**: Jot & Chaigne (absorptive delay networks), Dattorro 1997
(plate tank, modulation against metallic ringing — implemented, measured, and set aside
in favour of the lattice), Abel & Huang (echo density), Välimäki, Parker & Abel
2010 (parametric spring reverberation), Raffel & Smith (bucket-brigade delay).

Nothing here is a port: no code or tuning table was taken from any product. The lattice
follows the structural idea found in FutureVerb, but the stage count, delay series,
rotation pattern, tap profiles, absorption filter, modulators and every constant are
this project's own, set by the measurements in §5.
