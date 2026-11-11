# Round 2: the plugin processor after the fix round (678c9c8, HEAD 93b05cf)

Independent, adversarial check, 2026-11-11. Nothing tracked was changed; no git command that
changes state was run; the installed Audio Unit and the user's `~/Library/Application
Support/tubamp` were not touched (24 files before and after, none newer than my first log;
every run used `CFFIXED_USER_HOME` under `build-verify/processor-r2/`). arm64, Release, Apple
clang. Every statement is labelled **[M]** measured (script named), **[I]** inferred from
reading, or **[G]** guess. The engineer's smoke test and golden tool were not used as evidence.

## 1. Verdict

- Old sessions: **4 155 old-vs-new renders of my own runs (3 585 static, 192 with automation,
  378 reverb / delay / cab), 0 unexplained differences**; round 1's automation (114) and
  live-restore (84) sets re-run with the same result. Every difference is a declared exception,
  plus one undeclared but benign one (section 3.3).
- Round-1 processor defects: **7 fixed, 2 partly, 0 not fixed** (section 2).
- The fix broke nothing I could make fail in old sessions. New paths have six small issues
  (section 6, items 1 to 3 and 5 to 7). The pre-existing thread races are all still there.

## 2. Round-1 processor defects

| # | Defect (round 1) | Status | Number now, worst case | Script |
|---|---|---|---|---|
| 1 | One NaN/Inf input sample latched the DC blocker (and three engines) | **Fixed** [M] | 286 renders, 11 layouts (empty, amp with and without model, deferred gate, all blocks new types, all blocks Classic, split Copy / L-R / X-Over, model in a lane, everything off), 8 bad patterns (single NaN L or R, +Inf both, -Inf, first sample, 300 mixed, 5 bursts, a whole block), 44.1 / 48 / 96 kHz, blocks 1 / 64 / 256 / 512, stereo and mono: 0 non-finite output samples, every render bit-identical to the same input with those samples zeroed | `py/t_nan.py` |
| 2 | A block switched back on replayed stale state (-7.2 dBFS) and popped | **Fixed** for every non-Classic type [M]; Classic unchanged, as declared | 57 cases x 3 rates x 2 signals. Stale tail after re-enable over silence: <= **-201.8 dBFS** (round 1: -7.2). Largest sample step at the switch: <= **0.5 dB** over the steady renders. >5 kHz band at the switch: gate, comp, EQ, chorus, phaser <= **-98.3 dBFS** (EQ Parametric); drive Boost at 36 dB gain -73.0 dBFS; tremolo Optical **-68.1 dBFS** (34.8 dB over its steady render, see 6.7). Classic: still -8.1 dBFS pop (EQ), +6.0 dBFS stale burst (hot drive), bit-identical to the old build (section 3.2) | `py/t_toggle.py` |
| 3 | `drive_on` / `drive_type` blanked the other split lane for 3 to 5 samples (step 0.1531) | **Fixed** [M], with a residual | Longest all-zero run 0; largest step 0.0028 against the sine's own 0.0033 (100 Hz, 0.25) at 44.1 / 48 / 96 kHz. A non-Classic `drive_on` no longer moves anything. Residual: the 10 ms tap slide is a comb in the waiting lane: -4.8 dB at fs/10 for a 3-sample change, a full notch (-50 to -70 dB, below -6 dB for 5 ms per slide) at fs/10 for a 5-sample change. Two changes inside 10 ms step: see 6.3 | `py/t_lane.py` |
| 4 | Drive latency counted while the drive waited for its re-entry reset | **Fixed** [M] | Classic drive re-entering with gate and comp: latency log 3 (message thread) -> 0 (audio thread, the waiting callback) -> 3 (audio thread, 64 samples later); other lane's largest step 0.1041 against 0.1040 steady. Cost: two more audio-thread latency reports per such chain edit | `py/t_pending.py` |
| 5 | First instance on a fresh install came up with 23 parameters changed | **Fixed** [M] | Empty home: 0 -> 4 preset files; 0 of 261 parameters differ from an instance started with the folder present, 0 away from their defaults, chain and latency equal, 0.5 s of audio bit-identical. Same with an existing empty presets folder and with a read-only one. The four files written have the old build's values for all 166 old ids and all 19 type ids at 0 | `py/t_state.py`, `compat-run/factory.py` |
| 6 | A `mod_type` switch into chorus ticked (-31 to -38 dBFS) | **Fixed** [M] | 110 ordered voicing pairs x 3 rates x 2 knob sets (660 renders), 220 Hz sine at -12 dBFS: into any chorus <= **-74.2 dBFS** in the >5 kHz band; worst of all pairs -69.9 dBFS (into tremolo Optical). 0 non-finite. Level during the fade: 6.5 | `py/t_mod.py type` |
| 7 | Defaults not mirrored from the engine headers (`gate_sc_hpf` 90 against 75) | **Fixed** [M/I] | Parameter table: `gate_sc_hpf` default 75; `Parameters.cpp` reads `kGateScHpfDefault`, `kGateReleaseDefault`, `kGateHoldDefault`, `kCompKneeDefault` and the `*Params {}` defaults directly | `tubamp_golden --params`, reading |
| 8 | Host tempo at or under 1 BPM stuck the synced LFO | **Partly** [M] | 1 BPM and 0.5 BPM: 2.000 Hz (free at 120 BPM) for tremolo Smooth and Bias. A host that reports **0 or a negative tempo** while playing with a song position that does not advance still holds the LFO: 0.075 Hz measured (expected 2.0), because a tempo <= 0 never reaches `transport.bpm` and so the `<= 1` rule does not see it. No position, no ppq, no tempo, no play head, stopped, ppq = NaN: all 2.000 Hz | `py/t_tempo2.py` |
| 9 | "Reported latency equals measured for every drive type, slot and lane" | **Partly** [M]: true for the rule, not for the measured delay of a running drive | Section 4. Bypassed non-Classic drive: a pure delay of exactly the reported figure, bit-exact, 216 of 216. Running drive: measured delay exceeds the report by a constant 2 to 184 us (the circuit's own filters), which at 192 kHz is up to 35 samples (Fuzz: reported 2, measured 37.2) | `py/t_latency.py` |

Not claimed fixed and checked anyway: round-1 item 10 (gate meter on a block larger than
prepared): Classic still shows state 2 (open) on silence; Tight is correct (state 0, 60 dB) [M].
Round-1 item 7 (gate curve clamp): now 65 535 samples, the lane ring's size [I]; not run, the
headless build cannot host a plugin with that much latency.

## 3. Old sessions

### 3.1 Static renders: the round-1 harness against the current build [M]

`compat-run/matrix.py` (round 1's script and its 239 states saved by the old binary, harness
rebuilt from HEAD in `hb-new/`; old binary = `build-verify/compat/hb-old`, built by round 1 from
`221a3cb` [I]; `git diff bf81728 221a3cb -- src` is empty [M]).

| Run | Conditions | Renders | Unexplained |
|---|---|---|---|
| base | 44.1 kHz / 127, 48 kHz / 512, 96 kHz / 64 | 717 | 0 |
| varbs | 44.1 and 96 kHz, random block sizes | 478 | 0 |
| prepfirst, mono | prepare before restore; mono bus | 478 | 0 |
| playhead | host playing at 97 BPM | 478 | 0 |
| preset, presetph | the same states through `PresetManager::loadPreset` | 717 | 0 |
| extra (mine) | 192 kHz / 256, 22.05 kHz / 128, 88.2 kHz / 512 | 717 | 0 |
| total | | **3 585** | **0** |

By state (all 12 round-1 renders): 176 bit-identical; 23 bit-identical audio with latency +3 per
active Classic drive; 29 flat-EQ (9 of them also +3 / +6 / +9); 11 split with a drive in a lane.
The same split as round 1.

Also re-run against the current build, same results as round 1: `legacy.py` (oldest-format
blobs), `live.py` (84 restores while audio runs: 80 identical, 4 flat-EQ), `readback.py` (239
blobs + 239 presets: old ids equal, pins all Classic, other new ids at default), `ontop.py`
(956 of 956), `classicignores.py` (239 of 239), `forward.py` (new state into the old build: ok),
`pathdep.py` (the 63-state blob/preset path dependence is the old build's too).

### 3.2 What the round-1 harness does not cover [M]

`compat-run/r2_auto.py`: six more old states (all 15 FX instances; classic chain with model;
split without a drive; split with a model in a lane; split with a Classic drive; deferred gate),
each through host state and through a saved preset, at 48 kHz / 512, 44.1 kHz / 127, 96 kHz / 64,
random blocks, a playing host, and block size 1.

| Test | Renders | Result |
|---|---|---|
| every block off and on in turn; all at once; five blocks toggled every 30 ms for 1.5 s | 36 | bit-identical |
| five chain reorders; each block removed and re-added | 24 | bit-identical |
| classic chain with model: each block off and on (amp, amp EQ, cab included); reorders | 24 | bit-identical |
| deferred gate: gate and amp toggles, gate moved behind the amp and back | 12 | bit-identical |
| split without a drive: static, lane-block toggles, `split_on`, `mix_on`, six reorders across the split, mixer and split-mode moves | 48 | bit-identical |
| split, Classic drive in a lane: static and toggles | 24 | differ: exception b |
| split, model in lane A: static | 12 | bit-identical |
| split, model in lane A: `amp_on` off and on | 12 | 6 identical (48 kHz, no resampler), 6 differ (44.1 and 96 kHz): section 3.3 |
| total | **192** (160 + 32 at block size 1) | 162 identical, 24 exception b, 6 lane slide |

Round 1's own automation set (`auto.py`, 114 renders): 92 identical, the 22 others are EQ knob
ramps, `mod_type`, split with drive, and blocks added after load, as in round 1.

Old-session CPU (`compat-run/r2_cpu.py`, same old state, both builds, minimum of 7 runs):
-2 % to +5 %; worst 4.48 -> 4.69 % of real time (gate, two drives, two EQs, two mods, 96 kHz / 512).

### 3.3 Exceptions, measured size

| Exception | Size |
|---|---|
| a. Classic EQ at exactly neutral is a pass-through | New equals the old render with those EQs off, bit for bit, in every such render. One flat amp EQ: -98.7 dB (44.1 kHz), -97.8 (48), -88.7 (96), -89.6 (88.2), **-79.9 dB (192 kHz)**, -107.6 (22.05) relative. Worst state: -74.0 dBFS peak (44.1), -74.9 (48), -65.4 (96), **-57.0 dBFS / -69.5 dB relative at 192 kHz**. "About -99 dB" holds at 44.1 and 48 kHz for one EQ only |
| b. Classic drive reports 3 samples | +3 per active Classic drive at every rate from 22.05 to 192 kHz (570 renders +3, 45 +6, 30 +9 of 3 585); audio bit-identical in series. Split: the other lane is the old lane delayed by exactly 3 samples, bit for bit; the old 6 kHz comb notch (-39.7 dB) in the summed response is gone (`split.py`). A session that automates Classic `drive_on` now reports 3 -> 0 -> 3 from the audio thread where the old build reported a constant 0 |
| c. `mod_type` crossfade | max old-new 0.095 (-20.4 dBFS) at 48 kHz, 0.103 at 44.1, 0.087 at 96; exactly 20.0 ms ahead of the DC blocker (last non-decay sample +20.01 to +20.02 ms). Into Classic chorus at depth 0.4: +31.0 ms (11 ms hold + 20 ms), max 2.4e-3 (-52.6 dBFS). Then the DC blocker's free decay: 4.5e-4 at +100 ms |
| d. Classic EQ knob ramps | one knob step: 0.42 (-7.5 dBFS) in the first 25 ms, 0.065 until 100 ms, <= 2.1e-3 later, never bit-identical again under signal; a 0.1 step 0.0126 (-38.0 dBFS); amp EQ 0.50 (-6.0 dBFS) |
| **Undeclared**: lane tap slide | A split session that automates `amp_on` in a lane holding a resampled model (44.1 or 96 kHz): old-new max **0.094 (-20.6 dBFS)** at 96 kHz, 0.044 to 0.069 at 44.1 kHz, for the 10 ms slide, then the DC blocker's decay. The old build blanked the other lane for 27 / 42 samples there, so this is an improvement, but it is a difference no document lists |

## 4. Latency truth [M]

`py/t_latency.py`: 6 types x 3 slots x series / lane A / lane B x 4 rates = 216 combinations,
each rendered off, on, and toggled on -> off -> on while running; band-limited noise 200 Hz to
4 kHz at -50 dBFS, drive gain 0, tone 12 kHz; reference = the same chain without the drive.

| Type | Rate | Reported on | Reported off | Measured on (cross-correlation, all 9 placements) | Measured minus reported | Off = pure delay of the reported figure | Toggling moves latency |
|---|---|---|---|---|---|---|---|
| Classic | 44.1k | 3 | 0 | 3.81 | +0.81 = +18 us | bit-exact 9/9 | yes, 3 -> 0 -> 3, audio thread |
| Classic | 48k | 3 | 0 | 3.90 | +0.90 = +19 us | bit-exact 9/9 | yes |
| Classic | 96k | 3 | 0 | 4.91 | +1.91 = +20 us | bit-exact 9/9 | yes |
| Classic | 192k | 3 | 0 | 6.80 | +3.80 = +20 us | bit-exact 9/9 | yes |
| Screamer | 44.1k | 5 | 5 | 5.97 | +0.97 = +22 us | bit-exact 9/9 | no |
| Screamer | 48k | 5 | 5 | 6.07..6.08 | +1.08 = +22 us | bit-exact 9/9 | no |
| Screamer | 96k | 4 | 4 | 6.41..6.42 | +2.41 = +25 us | bit-exact 9/9 | no |
| Screamer | 192k | 1 | 1 | 6.56..6.57 | +5.56 = +29 us | bit-exact 9/9 | no |
| Distortion | 44.1k | 5 | 5 | 5.35 | +0.35 = +8 us | bit-exact 9/9 | no |
| Distortion | 48k | 5 | 5 | 5.36 | +0.36 = +8 us | bit-exact 9/9 | no |
| Distortion | 96k | 5 | 5 | 5.20 | +0.20 = +2 us | bit-exact 9/9 | no |
| Distortion | 192k | 4 | 4 | 4.67 | +0.67 = +3 us | bit-exact 9/9 | no |
| Fuzz | 44.1k | 5 | 5 | 13.10 | +8.10 = +184 us | bit-exact 9/9 | no |
| Fuzz | 48k | 5 | 5 | 13.79..13.80 | +8.79 = +183 us | bit-exact 9/9 | no |
| Fuzz | 96k | 4 | 4 | 21.69..21.70 | +17.69 = +184 us | bit-exact 9/9 | no |
| Fuzz | 192k | 2 | 2 | 37.21 | +35.21 = +183 us | bit-exact 9/9 | no |
| Boost | all | 0 | 0 | 0.43 / 0.50 / 1.18 / 2.44 | +10 to +13 us | bit-exact 9/9 | no |
| Treble | all | 0 | 0 | 0.00 | 0 | bit-exact 9/9 | no |

- The excess is constant in time per type across four rates, so it is the modelled circuit's
  group delay, not a rate-dependent error in the reported figure [M, I for the cause].
- Dry lane next to a drive lane: measured delay equals the reported latency in all 144 lane
  rows; largest difference from the reference delayed by that figure 5.6e-9 (the pan law's
  -147 dB leak).
- Deferred Tight gate at 44.1 kHz with a resampled model: latency 27 (gate, amp), 32 (a
  bypassed or running Screamer between), 30 (Classic drive on), 37 (bypassed Fuzz and
  Distortion between); the attack is kept the same in all six (-0.2 dB in the first ms)
  (`py/t_misc.py gatealign`).
- `drive_type` changed mid-playback, drive on (`py/t_drivetype.py`, 30 pairs x 3 rates): the
  reported latency changes in the same callback, **from the audio thread** (for example 3 -> 5,
  5 -> 0); no gap (longest zero run 0), no step above the steady renders, a level dip of -6 to
  -12.5 dB (2 ms RMS) while the engine's own switcher fades. In a split the other lane slides
  for 10 ms (row 3 above).
- Drive off: see 6.2.
- With no audio running the reported latency is stale after a `drive_type` or `drive_on`
  change (5 where 0 is right) until the next `processBlock` or `prepareToPlay` (6.6).

## 5. State [M]

`py/t_state.py`.
- 261 parameters, 260 at a non-default value (`amp_slim` left alone), 5 seeds x 3 chains: 0
  value differences through host state, through a preset, and through host state onto a live
  instance in another state and order.
- A state saved by this build, chain reordered and types changed, then restored by host state
  and again by preset while audio runs: chain and all values equal, 0 non-finite.
- Unknown future parameters, an unknown root attribute and child, an unknown chain token, and
  all 95 hint-4 ids removed; 4 chains x host / preset x fresh / live instance: no error, old
  ids kept, and exactly the type ids of blocks **in the saved chain** read 0 (for
  `gate,drive2,eq3,mod2,amp`: `gate_mode`, `drive2_type`, `eq3_type`, `mod2_*`; the other 13
  stay at 1). Chain `amp`: no id pinned.
- Edge [M]: a chain string that holds only unknown tokens loads as the classic nine-block
  chain (the legacy fallback for an empty parse), not as an empty chain.
- Parameters: `tubamp_golden --params` equals `tests/golden/params-v4.tsv` byte for byte; the
  old build's 166 rows are a byte-identical prefix of the new 261 over 23 columns (index, id,
  name, hint, class, range, interval, skew, default, steps, flags, choices, texts); hints
  44 x 1, 83 x 2, 39 x 3, 95 x 4.

## 6. New findings, worst first

1. **Every latency change reported from the audio thread takes a lock, and in the AU wrapper
   posts a message.** [M + I] `juce::AudioProcessor::setLatencySamples` ->
   `updateHostDisplay` -> `getListenerLocked` takes `listenerLock` once per listener: the guard
   counts exactly one mutex lock per audio-thread latency change (2 302 of 2 302, 1 617 of
   1 617). JUCE's AU wrapper then calls `triggerAsyncUpdate()` off the message thread
   (`juce_audio_plugin_client_AU_1.mm:2027`); the same code in my listener costs 318
   allocations, 2 069 locks and 43 system calls for 472 changes. The mechanism is the old
   build's (model staging); what is new is how often it fires: every Classic `drive_on` toggle
   and every `drive_type` change that moves latency, both automatable. Likelihood: certain when
   those are automated; consequence: a small chance of a dropout per event, and a host that
   only re-reads latency at transport start plays that track 3 to 5 samples off until then [G].
2. **Changing `drive_type` on a bypassed drive clicks.** [M] The dry signal jumps by the
   latency difference with no crossfade (Classic or Boost or Treble <-> Screamer, Distortion,
   Fuzz: 5 samples at 48 kHz): -37.0 dBFS in the >5 kHz band on a -12 dBFS chord, largest step
   12 to 15.5 dB over steady (`py/t_drivetype.py`); -43.0 dBFS on a 110 Hz sine.
3. **Two lane-latency changes within 10 ms step the waiting lane.** [M] The second slide starts
   from the first one's target, not from where the tap was: step 0.17 to 0.24 on a 0.25 sine at
   fs/10 (steady 0.104), `drive_type` Boost -> Distortion -> Boost 3 ms apart (`py/t_lane.py`).
   A discrete parameter swept by automation does this.
4. **Undeclared difference from the old build**: the lane slide of 3.3.
5. **Crossfade dips are larger than reported.** [M] `py/t_mod.py`, 220 Hz sine, one-period RMS
   against the band the two steady renders span: phaser Small Stone -> Phase 90 **-15.4 dB**,
   Phase 90 -> Small Stone -12.9 dB, outside +-1 dB for 12.8 to 13.8 ms (engineer: up to 9 dB);
   `mod_type` Small Stone -> chorus CE **-17.1 dB**, longest 22.7 ms; chorus Wide -> Tri
   -5.5 dB, CE -> Classic -3.3 dB. No rise above the band anywhere (<= +0.1 dB). Into tremolo
   Smooth stays inside the tremolo's own swing in my metric, so the engineer's 5 dB is neither
   confirmed nor refuted. Audible as a short dip on a held note, not as a click.
6. **Reported latency is stale while no audio runs.** [M] `chain drive; drive_type Boost`:
   5 reported until the next block or prepare.
7. **Tremolo Optical after a restart**: -68.1 dBFS in the >5 kHz band at re-enable and once per
   LFO cycle after a transport change (-67.0 dBFS), 22 to 35 dB above its steady render. [M]
   Engine start-up or carrier-phase splatter rather than the fader [G].
8. **Chain edits**: +4.2 to +4.4 dB peak over any steady order during a reorder at blocks of
   256 and 2048 (0 dB at block size 1); the bounded reset drain bypasses re-entering blocks for
   up to seven callbacks. Old mechanism, old sessions bit-identical. [M]
9. **Mixer pan gains ramp from un-panned for 20 ms after `prepareToPlay`** (old code too). [M]
10. **NAM garbage slots** (old code, not in the list of known issues): section 7.

Attacks that found nothing [M]:
- Fader restarted mid-fade: 17 kinds, toggle periods 0.5 to 21 ms, blocks 1 / 64 / 512 / 4096
  (306 renders): 0 non-finite, peak <= +0.44 dB over steady, step <= +0.3 dB, >5 kHz band
  <= -46.6 dBFS (Fuzz -43.3 dBFS, its own steady content) (`py/t_fader.py fast`).
- Comb during the fade (dry delayed by the reported latency against the engine): dip <= 1.2 dB
  from 500 Hz to 12 kHz for Screamer, Distortion, Boost, Treble; the Fuzz passes through a
  null at 2 to 5 kHz because its output is 17 to 65 dB below dry and out of phase
  (`py/t_fader.py comb`).
- 13 non-Classic blocks flipped every audio block for 10 s (up to 390 013 events): 0
  non-finite, peak not above steady, >5 kHz <= -54.7 dBFS, 0 allocations / locks / system
  calls, CPU 5.1 -> 5.3 % of a core, worst block 33 % of its budget against 35 % steady
  (`py/t_fader2.py blockrate`, `py/t_cpu.py`).
- All 13 switched in one block: >5 kHz <= -84.1 dBFS, stale <= -214.6 dBFS. The same inside
  split lanes, mono included: 0 non-finite (`py/t_lanetoggle.py`).
- Type changed while bypassed, then enabled, 114 pairs (`py/t_fader2.py typeoff`): into a
  non-Classic type <= -107.4 dBFS except the drive's own content; the click of 6.2 is the
  only artefact.
- State restore mid-playback (blob, preset, three within 15 ms), blocks 1 to 1024: 0
  non-finite, +0.5 dB peak, converges on the restored state (3.6e-7 after 1 s).
- `prepareToPlay` at a new rate 3 ms into a fade, 17 kinds x 4: output equals a block already
  in the target state, difference 0.
- Transport stop, start, locate, tempo change, loop, no position, no play head on seven synced
  voicings: 0 non-finite, >5 kHz <= -95.9 dBFS except Optical (`py/t_misc.py tempo`).
- Meters read by `exchange` at 30 Hz: compressor 14.98 dB against 14.98 from the audio (Classic
  14.72 / 14.69); 0 one poll after `comp_on` off and when removed; gate state -1 when absent or
  off, 40.0 dB = range when closed, level -6.04 dBFS for a -6 dBFS sine (`py/t_misc.py meters`).

## 7. Real-time safety and threads

- Guard (`harness/guard.c`, dyld interposing of malloc, calloc, realloc, free, pthread mutex
  and rwlock, os_unfair_lock, condition variables, read, write, close, mmap, munmap, madvise,
  sleeps, sched_yield, mach_msg, pthread_create; self-test sees 2 allocations, 2 frees, 1 lock,
  1 write) [M]: thread stress 40 s x 2 (13.5 M and 8.5 M blocks; toggles, type switches,
  reorders with and without split, state restores, re-prepare at six rates, knobs, transport,
  model swaps, IR loads): **0 allocations, 0 frees, 0 system calls**; locks = audio-thread
  latency changes exactly (6.1). Seven composite single-thread scenarios (22.05 to 192 kHz,
  mono, blocks larger than prepared, NaN bursts): the same.
- AddressSanitizer + UndefinedBehaviorSanitizer [M]: 3 x 70 s thread stress (18.9 M blocks) and
  the seven composite scenarios: **0 reports**, 0 non-finite.
- ThreadSanitizer [M]: 4 x 60 s. Toggles, type switches, knobs and transport alone: **0 reports
  in plugin code** (the 5 are my play head). `logs/tsan-symbolized.txt`.

| Known issue | Still there | How likely in a real host |
|---|---|---|
| `setLatencySamples` from two threads | Yes [M]: 13 stacks, audio thread against `updateLatency` from `setChainOrder`, `setStateInformation`, `applyStateVar`, `applyFxRecords` | Needs a chain edit, preset load or restore in the same instant as a latency-changing block; rare, and the loser is one int [G] |
| Off-thread `setStateInformation` against the parameter flush | Yes [M]: ValueTree listener list and `~SharedObject` on the host thread against `flushToTree` | Only in hosts that restore state off the main thread while the instance is live; a crash is possible [G] |
| CabSim flag | Yes [M]: `clearImpulseResponse` / `loadImpulseResponse` against `processChainBlock`; it also fires on **every state restore without an IR**, not only on IR loads. JUCE's convolution queue is flagged too | A one-byte flag; harmless in practice [G] |
| NAM latency short | Yes [M] (`py/t_namlat.py`): 44.1 kHz reported 27, measured 28.92; 88.2 kHz 39 / 41.84; 96 kHz 42 / 45.01; 22.05 kHz 21 / 22.46; **192 kHz 70 / 75.03 (5 short, not 2 to 3)** | Always, whenever the host rate is not the model's; 26 to 44 us |
| A NaN produced inside the chain | Yes [M] (`py/t_huge.py`): one finite sample of 3e38 with `input_trim` +12 dB overflows after the sanitiser; all 72 000 later samples non-finite until the next prepare. Without the trim it stays finite and rings at 1e25 through the DC blocker | Needs a broken upstream plugin or a hosted FX slot that emits NaN (not testable headless) [G] |
| Not in the list: NAM garbage slots | [M] 3 TSan stacks under model swaps: `NamEngine::collectGarbage` (message thread) against `applyStaging` and `process`. The four slots are plain `unique_ptr`s written by both threads (`NamEngine.cpp:293, 336-348, 565-569`). `NamEngine.*` is untouched by this branch | Only while a model is loaded or cleared with audio running; worst case by reading is a leaked model, not a crash [I] |

## 8. Fences [M]

- `git diff --stat bf81728 -- src/dsp/Reverb.* src/dsp/Delay.* src/dsp/FxPrimitives.h
  docs/REVERB-DELAY.md`: empty. `CabSim` class and its six definitions hash the same at
  `bf81728` and HEAD; `NamEngine.*` unchanged.
- `compat-run/r2_fence.py`: 43 old states of my own (14 reverb, 18 delay, 6 mixed orders of
  all six instances, one split, three cab with and without IR and model; random settings
  inside the old ranges), my own input, 7 configurations (three rates, mono, random blocks,
  host at 0.5 / 1 / 63 / 97 / 141.5 / 300 BPM), with and without 15 mid-stream changes:
  **378 of 378 bit-identical**, latency equal, quietest render peak 0.041.

## 9. Not tested

- The shipped AU (LTO, wrapper), `auval`, a real host. That would add: what Logic does with a
  latency change reported mid-playback (6.1) and with the 3-sample Classic drive change in old
  projects; the wrapper's real cost on the audio thread; off-thread state restore; the WebView
  editor's meter polling and type menus; offline bounce.
- Hosted AU slots: the 65 535 curve clamp, FX-slot latency in the lane ring, a hosted plugin
  that emits NaN.
- Sound quality of the engines, and their own type switches (other verifiers).
- The fader on blocks larger than prepared was run for memory safety only, not for clicks.
- 176.4 kHz; sample rates below 22.05 kHz.

## 10. Files

`build-verify/processor-r2/`: `harness/r2.cpp` (scenario runner and thread stress),
`harness/guard.c`, `harness/CMakeLists.txt` (`-DR2_SAN=address,undefined` or `thread`), `py/*.py`
(one script per section, `r2lib.py` shared), `compat-harness/` and `compat-run/` (round 1's
harness and scripts pointed at a build of HEAD, plus `r2_auto.py`, `r2_fence.py`, `r2_cpu.py`),
`logs/`, `work/*.json`. `rel/` and `hb-new/` are the release builds; the sanitizer builds were
deleted after use. Python with scipy is `/usr/bin/python3`. No WAV was kept.
