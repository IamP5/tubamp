# Effect study: what the reference plugins do and what tubamp should build

On 2026-11-11 eight agents each studied one tubamp effect against the best plugins
installed on this Mac. They read the plugins' code (disassembly and Hopper pseudo-code),
measured them through a headless Audio Unit host, and wrote a spec for tubamp. A second,
independent agent then re-checked every load-bearing claim of each report against the
binaries and re-rendered the measurements.

> Update: the engines specified from this study were implemented and verified in two rounds.
> The contract is `docs/FX-ENGINES.md`; the verification reports are in `.audit/fx-verify/`.
> The phaser figures below are superseded by `phaser/voicings.md`. Notch-1 trajectory error
> against Guitar Rig 6 is now 0.41 to 0.70 % rms (Phase 90 block), 0.12 to 0.21 % (Small Stone
> Color off) and 0.30 to 0.44 % (Small Stone Color on).

Reverb and delay are not here. Their study is `docs/REVERB-DELAY.md`, which today exists
only as an uncommitted file in the main checkout.
The amp block (a neural model player), split, mix and the external AU slots were out of
scope.

This page is the map. Each effect has `report.md` (study and recommendations),
`claims.tsv` (the claims that were checked), `verification.md` (the independent check)
and `evidence/`. Every report opens with a "Verification status" section: read it
first, because the report body below it was not rewritten.

## Results

| Effect | What tubamp ships today | Main finding | Build first | Claims that held |
| --- | --- | --- | --- | --- |
| [Phaser](phaser/report.md) | `juce::dsp::Phaser`: six stages, no feedback, sine LFO | Guitar Rig 6 and AmpliTube 5 both model the Phase 90 and the Small Stone as four equal allpass stages, and both are linear | The two voicings in [`phaser/voicings.md`](phaser/voicings.md). The Phase 90 follows Guitar Rig 6's whole notch sweep within 0.7 % rms; the Small Stone within 1.9 % rms | 16 of 20; refitted in three rounds after verification |
| [Compressor](comp/report.md) | `juce::dsp::Compressor` plus makeup | The knobs mislabel time: "5 ms / 120 ms" measures 2.2 ms and 14 ms, and bass distorts (2 % THD at 100 Hz) | A log-domain "Clean" type and a gain-reduction meter | 23 of 24 |
| [Drive](drive/report.md) | `tanh` under 2x oversampling, tone low-pass | Every reference shapes the signal before the clipper. Ours does not, and its alias floor is -21 dB | A Tube-Screamer-style type with pre-clip filtering | 16 of 21, 5 inconclusive |
| [Gate](gate/report.md) | The NAM plugin's gate, unchanged | It does not close between palm mutes, and its close delay grows with level | A "Tight" mode with hysteresis, hold and range | 18 of 20 |
| [Tremolo](tremolo/report.md) | Sine gain LFO with a dry/wet mix | The mix adds two copies of one signal, so the default is 1.6 dB deep and 2.9 dB louder | A "Smooth" voice with a real depth law, then "Optical" | 19 of 20 |
| [Chorus](chorus/report.md) | `juce::dsp::Chorus`: one voice for both channels | No stereo width, and detune runs to hundreds of cents at the knob extremes | A two-voice engine with depth in cents | 20 of 21 |
| [Cab](cab/report.md) | `juce::dsp::Convolution` plus two cut filters | The same IR is 3.0 dB louder at 96 kHz than at 48 kHz | A loader with rate-independent level | 20 of 20 |
| [EQ](eq/report.md) | Three cookbook biquads | Flat is not a pass-through on Apple silicon (-99 dB residual), and knob moves click | Knob smoothing, then a parametric type | 18 of 20 |

## Defects found in tubamp today

These are facts about our own code, separate from any proposal. "AU" means the
verifier measured the installed tubamp Audio Unit with the block in its chain. That
build is the main checkout's, not this worktree's; the DSP source for these blocks is
the same in both.

| Defect | Size | How it was established |
| --- | --- | --- |
| Compressor time constants are the knob value divided by 2π, and act before the gain law | attack 2.21 ms and release 13.67 ms at "5 ms / 120 ms"; release 6.8 ms after 3 dB of reduction | AU, plus the JUCE source compiled separately |
| Compressor distorts bass | 1.99 % THD at 100 Hz at defaults, 12.38 % at the fastest settings | AU |
| Drive adds delay that tubamp does not report | 3.14 samples, reported latency 0. A split lane with a drive would comb against the other lane (first null computed at 7.38 kHz, not measured) | AU and source for the delay; the comb is computed |
| Drive aliases | -21.4 dB for a 5 kHz tone at full gain | AU and an independent model |
| EQ at flat changes the signal | -99.5, -89.5 and -80.3 dB residual at 44.1, 96 and 192 kHz, arm64 only (fused multiply-add) | AU, plus a float model that predicts it |
| EQ knob moves click | -62.5 dBFS under a -12 dBFS note | two independent models; not measured on the plugin |
| Cab level depends on the session rate | +3.0 dB at 96 kHz against 48 kHz | AU and a model of JUCE's loader |
| Cab passes signal through JUCE's default IR after a load | -2.30 dB for roughly 70 to 80 ms, only on a first load issued after `prepare`; seen in offline renders, not observed in a real-time host | AU and model; the duration is an estimate |
| Tremolo mix is a second depth knob that also changes level | default: 1.6 dB of tremolo, +2.9 dB of level | derived from `src/dsp/FxBlocks.cpp:426-440` and reproduced |
| Gate never closes in 75 ms gaps | gap gain 0 to -2 dB | the AudioDSPTools source compiled and measured; Gateway, which ships the same gate, measures the same |
| Chorus has no width | one delay line feeds both channels | source |
| Gate gain is applied to the amp output without delay alignment | not measured | inferred from `src/PluginProcessor.cpp:1233` |

## What the references agree on

- **Nothing ships a bare textbook block.** Each reference effect has a voicing: a
  filter before the clipper, a lopsided LFO, a second chorus voice, a two-stage release.
- **Type 0 stays the old algorithm.** Every report proposes a type or mode parameter
  whose first value is today's code path bit-for-bit. The reverb and delay rewrite uses
  the same pattern.
- **Smooth after the decision, not before.** Compressors smooth gain reduction after the
  gain computer. Gates use hysteresis and a hold timer. Tremolos smooth the gain, not
  the LFO.
- **Stereo is linked or deliberately decorrelated.** Dynamics take one gain for both
  channels. Modulation gets width from a second voice at 90 or 180 degrees.
- **No lookahead, and oversampling only around nonlinear stages.**
- **The classic pedals are modelled as linear filters plus one nonlinearity.** Phase 90,
  Small Stone and CE-1 models add no noise, compander or clock artefacts.
- **Parameter changes are ramped** over about 20 ms where the vendor ramps at all.
  Solid EQ has the code and ships with it switched off.
- **Meters are standard** on every dynamics unit.

## Suggested order of work

Ranked by what a player gains against what it costs, with the owner's stated priority
first. This order is a judgment, not a measurement.

1. **Phaser: Phase 90 and Small Stone voicings.** The request that started this study.
   The spec and a numpy reference exist, and ten audition WAVs are in `phaser/audition/`.
   Against Guitar Rig 6's models at the settings the study chose, the Phase 90 voicing
   follows the whole first-notch sweep within 0.7 % rms (1.3 % at worst) at six rates.
   The Small Stone follows it within 1.9 % rms with Color off and 1.1 % with Color on.
   Two known misses remain: the Small Stone's second notch sits 6.5 % high with Color
   off, and one reading at the fastest Color-on rate is 4 % off. The reference is a
   vendor's model, not a measured pedal, and the script-logo Phase 90 has no measured
   reference at all.
2. **Compressor: Clean type and gain-reduction meter.** The largest measured defect.
   Drop or redesign the 8 ms detector hold first: its zero-distortion figure was an
   artefact of the test frequency.
3. **Drive: Tube-Screamer-style type, and report the oversampler's latency.** The pedal
   most used in front of an amp model. The proposed alias scheme reaches about -55 dB
   in the audible band, not the -72 dB first quoted.
4. **Gate: Tight mode and a sidechain high-pass.** The prototype closes in 30 to 47 ms
   depending on level, so it needs tuning to meet its own 35 ms target.
5. **Tremolo: Smooth and Optical voices.** Small code, large audible change.
6. **Chorus: two-voice engine.** Fix three design gaps first (see its Verification
   status): the cents law below 1.09 Hz, the CE voicing above 2.86 Hz, and the
   Dimension preset that fails its own acceptance band.
7. **Cab: rate-independent level and a primed IR swap.** Leave the proposed -9 dB
   normalisation rule out until it is tested on real IRs.
8. **EQ: knob smoothing, then a parametric type.**

## Decisions for the owner

1. **Which base to build on.** The reverb and delay rewrite is uncommitted in the main
   checkout and edits the same files (`FxBlocks`, `Parameters`, `PluginProcessor`). New
   effect types should land on top of it, and take the next AU version hint after the
   one it uses.
2. **One parameter scheme for the mod block.** The three mod reports each propose their
   own ids: `mod_chorus_mode`, `mod_width`, `mod_tone`; `mod_phaser_model`,
   `mod_phaser_color`; `mod_trem_voice`, `mod_trem_wave`, `mod_trem_shape`,
   `mod_trem_spread`, `mod_trem_sync`, `mod_trem_mult`. Width, spread, sync and tone
   overlap. A workable merge: keep one voicing choice per type, because a choice list
   is frozen per id, and share `mod_sync`, `mod_mult`, `mod_spread`, `mod_tone` and
   `mod_shape` across the three.
3. **Whether Classic EQ keeps its flat residual.** Keeping it preserves old renders
   bit-for-bit. Fixing it changes them by less than -80 dB.
4. **Whether to compensate the drive's delay in split lanes.** It removes a comb filter
   and changes how existing split sessions sound.
5. **Whether the in-process harness is an acceptable method.** The drive study loaded
   Guitar Rig 6 and AmpliTube 5 into its own process and called their DSP classes
   directly (`drive/evidence/tools/rigcall.cpp`). It patches nothing, but it enters
   below the plugins' own start-up path. Five drive claims rest on it alone.
6. **What to do with vendor preset snippets in the evidence.** Six small Guitar Rig
   factory-preset XML files sit under `drive/evidence/tools/gr6au/` and
   `phaser/evidence/tools/`. `tools/re/ngrc_xml.py` regenerates them from the installed
   plugin, so they need not be committed.

## Limits of this study

- **AmpliTube 5 was read, never measured.** It crashes in its constructor under the
  headless host.
- **Four UADx units were not measured** (API 2500, API Vision, Hitsville EQ, Studio D
  Chorus): they hang or raise an alert.
- **PACE-protected plugins were measured only**, never disassembled: every UADx unit,
  Soundtoys, Relab, Sonnox, Antares.
- **Hopper was used less than planned.** It analyses one binary at a time (10 minutes
  for Guitar Rig 6, 21 for AmpliTube 5), so most reading was done from symbol-guided
  disassembly. The Guitar Rig 6 pseudo-code cache is complete for the effect classes
  (2,551 procedures); the AmpliTube 5 cache stopped at 962 of 4,222 when the disk
  filled.
- **Several prototypes looked better than they are** until the verifier changed the
  test tone. The pitfalls are written down in `tools/re/README.md`.
- **Seven of the eight verifications rest on the verifier agent's own report.** The
  coordinator re-ran measurements itself only for the phaser fix rounds. The second
  round was then checked by a fresh verifier (`phaser/verification-refit.md`); the third
  was checked by the coordinator with that verifier's tracker, not by a fresh agent.
- **Two rules in `tools/re/README.md` were added after the fact.** One agent ran macOS
  `sample` on its own hung host process while it hosted a PACE-protected plugin, and one
  built the in-process harness, before either was forbidden. Nothing from the sample is
  used in a report.
- **Scratch evidence is temporary.** Working files lived under the session's scratch
  directory in `/private/tmp`. The files the decision log points at were copied to
  `.audit/fx-evidence/`; rendered WAVs were deleted to save disk and regenerate from the
  commands in each evidence file.
- **Some constants are guesses** and are tagged so: the Phase 90 feedback resistor,
  the Muff and RAT component values, several knee and hold values.

## Reproduce or extend

The kit is in `tools/re/`. `README.md` there is the working contract an agent follows:
tools, fences, method, evidence tags and deliverables. `inventory.py` lists every
installed Audio Unit with whether its code is readable. `auhost` renders a WAV through
any Audio Unit headless. `gr6state.py` builds a Guitar Rig 6 state holding one pedal.
The decision log of this run is `.audit/fx-reverse-engineering.tsv`.
