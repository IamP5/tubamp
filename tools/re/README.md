# Effect reverse-engineering kit

Tools and the working contract for studying how the reference plugins installed on this
Mac implement an effect, so tubamp's own version of that effect can be improved from
evidence. One orchestrator owns one effect. Read this file once, then your brief.

## Setup

```bash
export RE_STATE=<the shared state directory your brief names>
export RE=<repo>/tools/re
```

`$RE_STATE` already holds the plugin inventory (`inventory.tsv`), demangled symbol
lists (`symbols/*.txt`), the built AU host (`bin/auhost`) and the Hopper cache.

## Tools

| Tool | Use | Shared? |
| --- | --- | --- |
| `python3 $RE/disasm.py <binary> --list/--dump/--addr/--floats/--doubles/--strings` | arm64 disassembly by symbol, literal pools, strings. Instant. | parallel-safe |
| `python3 $RE/hopper.py <binary> 0xADDR... \| --match '<regex>'` | Hopper pseudo-code through the one daemon, cached | queued |
| `$RE_STATE/bin/auhost params\|render\|state\|edit ...` | headless AU host for black-box measurement | parallel-safe except `edit` |
| `python3 $RE/sig.py gen\|fr\|thd\|curve\|env\|notches\|stats` | test signals and analyses (numpy only, no scipy) | parallel-safe |
| `python3 $RE/ngrc_xml.py` then `python3 $RE/gr6state.py` | build a Guitar Rig 6 AU state holding one rack component, headless | parallel-safe |
| `python3 $RE/lock.py acquire\|refresh\|release gui <owner>` | the screen lock | one holder |
| `python3 $RE/inventory.py` | rebuild `inventory.tsv` | not needed normally |

Each tool documents itself in its header or `--help`.

### Hopper rules

Hopper is one GUI app and takes one binary at a time. Exactly one daemon drives it.

- Only reach Hopper through `hopper.py`. Never run `rea decompile`, never call an
  `mcp__rea__*` tool that needs Hopper, never touch the Hopper window.
- Switching binaries costs a full re-analysis: about 40 s for a 5 MB code section,
  much longer for Guitar Rig 6 and AmpliTube 5. So batch: one call with every address
  you want from a binary, not one call per address.
- Guitar Rig 6 is pre-warmed (AmpliTube 5 only in part): those DSP classes are in the
  cache and answer instantly. An uncached request on a binary that is not the open one
  can wait a long time. `hopper.py --status` shows what the daemon is doing.
- `disasm.py` is the dependable primary for DSP code. Hopper prints many floating-point
  and NEON instructions as raw `asm { ... }`, so you need the disassembly anyway. Use
  the pseudo-code for control flow and structure.
- Binaries without symbols (Plugin Alliance, TONEX, Gateway) have no names to match.
  Find code by address: strings and their references, the AU render entry point, then
  follow calls. Ask Hopper by `0x` address.

### Black-box rules

- `auhost params <type> <subtype> <manu>` lists parameters. `inventory.tsv` has the
  four-character codes. Quote codes that contain `$`.
- Render at 48 kHz unless the question is about sample-rate behaviour. Keep takes short.
- **Guitar Rig 6** (`aufx 'Ni$R' -NI-`) picks its effect inside the plugin, but its AU
  state is zlib-compressed XML, so it can be authored without the screen:
  `auhost state` for the default, `ngrc_xml.py` to cut a component's factory preset out
  of its `.ngrc`, `gr6state.py` to wrap it into a state with parameter overrides, then
  `auhost render --state`. The header of `gr6state.py` has the sequence.
- **TONEX** exposes its sections as plain AU parameters, but its switches do not land
  through `render --set`: author a state with `auhost state --set` first.
- **AmpliTube 5** crashes in its constructor under `auhost` (also in `edit`). It is
  static-only.
- **Native Instruments plugins queue parameter changes** and apply them inside render.
  `auhost` runs silence after a set, so `params --set` and `state --set` read back true.
- **JUCE-based plugins load impulse responses on a background thread.** Give offline
  renders a long `--warmup`.
- `auhost edit` under the `gui` lock is the last resort for anything else: hold the lock
  while a window is open, refresh it every few minutes, release it on close. Only do
  this if your brief allows GUI work.
- Every command but `edit` is killed after `AUHOST_TIMEOUT` seconds (default 300).
- Never instantiate: `MPXNativeReverb`, `LX480 Essentials`, `Auto-Tune Access`,
  `Little Plate`, `Little Radiator`, `Sonnox Soften` (licence dialogs or hardware keys).
  These hang or raise a modal alert on render: UADx `API 2500`, `API Vision Channel
  Strip`, `Hitsville EQ`, `Studio D Chorus`.
- A plugin that fails to load, shows a dialog or returns silence is a recorded gap, not
  something to fight. Note it and move on. Do not sample, trace or attach to a process
  that is hosting a PACE-protected plugin. (Rule added on 2026-11-11 after the first
  study, in which one agent ran `sample` on its own hung host.)
- Loading a vendor binary into your own process and calling its DSP classes directly is
  not an approved route. Ask the coordinator first. (Rule added on 2026-11-11 after the first study,
  in which the drive agent did this; see `docs/research/fx/drive/verification.md`.)

### Measurement pitfalls found in the first study

- **Sample-locked test tones flatter time-varying effects.** 100 Hz at 48 kHz repeats
  every 480 samples, so a detector hold or a gate edge can line up with it exactly and
  report zero distortion. Check at least two frequencies that are not divisors of the
  sample rate (82.4 Hz, 98 Hz), and a carrier phase that is not a zero crossing.
- **`sig.py thd` counts harmonics only.** Gain modulation shows up as inharmonic
  sidebands, which it does not see. Look at the spectrum around the carrier too.
- **Do not read a sweep range from the extremes of a tracked trajectory.** Single
  outlier frames set them. Use a multitone or stepped-sine ratio and a robust statistic.
- **A model of our code is not the plugin.** Where a claim about tubamp matters, measure
  the installed AU with the block in its chain, and say which build that is.
- **Release times read from a truncated tail are too short.** Render past the settle.
- Keep WAV scratch small and delete it once the table is written: the system disk on
  this Mac is nearly full.

## Fences

- **Copy protection.** PACE/iLok-protected plugins (every `uaudio_*`, Soundtoys, Relab,
  Sonnox, Antares, Lexicon) are measured black-box only. Do not disassemble, decrypt,
  dump from memory, attach a debugger or Frida to them, or patch anything.
- **What we take.** Structure, technique, parameter ranges and measured behaviour. The
  recommended tubamp design must be our own: derive constants from the analog circuit,
  from published literature, or by fitting to measurements. Do not lift lookup tables
  or long code sequences from a vendor binary into the recommendation, and do not paste
  more than about 30 lines of any one decompiled listing into the report.
- **Write scope.** Only `docs/research/fx/<effect>/` and `$RE_STATE/work/<effect>/`.
  Do not edit `src/`, `ui/`, `tools/`, `CMakeLists.txt` or anything else. No git
  commands that change state (no add, commit, stash, checkout, reset). If a tool here is
  wrong, work around it inside your own directory and report it in your final message.
- **Machine.** Do not install software, do not open Logic Pro, do not change system or
  audio settings. Do not kill processes you did not start.
- **Context.** Send big dumps to files under `$RE_STATE/work/<effect>/` and read them
  in slices. Never print a whole dump into your context.

## Method

1. **Read ours first.** The files and lines in your brief, plus the `juce::dsp` class
   they wrap (source under `/Users/tuba/Dev/projects/tubamp/build/_deps/juce-src/modules/juce_dsp`,
   read-only). Write down exactly what our block does today.
2. **Static reading.** For each reference: find the DSP class, then read its
   per-sample process function, its coefficient or control-rate update, its parameter
   setters and its sample-rate setup. Recover the signal flow, the filter types and
   coefficient formulas, the nonlinearity, the detector or LFO, parameter mapping
   curves, smoothing, oversampling, stereo handling, and the constants (read literal
   pools with `--floats` / `--doubles`).
3. **Black-box confirmation.** Where a reference loads in `auhost`, measure what the
   code reading predicts, and what code does not show easily (frequency response,
   harmonic profile against level, static curve, attack and release, modulation
   trajectory, aliasing).
4. **Literature.** Cross-check against the standard published work for the effect
   (DAFx and AES papers, circuit analyses). Cite the URL you actually read.
5. **Synthesize.** Compare ours against the references, then rank improvements by
   audible benefit against cost.

You may delegate bounded reading or web research to helper subagents if your session
offers the Agent tool: `sonnet` for literature summaries and bulk disassembly reading of
secondary targets, `opus` with `effort: "medium"` for hard DSP reading. Keep every helper
under about 128k tokens of context by giving it file paths and address lists, not dumps.
You own their output: check it against the binary before you rely on it.

## Evidence

Every factual claim in the report carries one tag in the same sentence:

- `[decompiled: <binary> <symbol or 0xaddr>]` you read it in the code.
- `[measured: <evidence file>]` you rendered and analysed it.
- `[literature: <url>]` a source you opened.
- `[inferred]` reasoning from the above. `[guess]` when you cannot back it.

A claim about a constant names the address it was read from. A measurement names the
exact `auhost` and `sig.py` commands in the evidence file's first lines so anyone can
rerun it.

## Deliverables

All under `docs/research/fx/<effect>/`:

1. `report.md` with these sections, in this order:
   1. **Summary.** Five lines at most.
   2. **tubamp today.** What our block does, with `file:line`.
   3. **Reference implementations.** One subsection per plugin studied: signal flow,
      algorithms, constants and ranges, parameter mapping.
   4. **Measurements.** Tables, each pointing at its evidence file.
   5. **Industry practice.** What the references agree on, and what the literature says.
   6. **Gap analysis.** A table: aspect, ours, references, consequence for the player.
   7. **Recommendations.** Ranked. For each: what to build, the evidence for it, a
      design sketch of our own (pseudo-code or equations), new parameters (id, range,
      default; parameter ids are append-only and existing ones are frozen), CPU and
      latency cost, compatibility (old sessions must sound identical: follow the
      pattern where type 0 stays the old algorithm bit-for-bit), and the measurement
      that would prove the improvement, with an acceptance threshold.
   8. **Open questions and unverified items.**
2. `claims.tsv`: the 10 to 20 claims your recommendations rest on. Columns:
   `id`, `claim`, `kind` (decompiled, measured, literature), `where` (binary and
   address, or evidence file), `recheck` (the exact command that shows it). These get
   checked by an independent verifier, so make them checkable.
3. `evidence/`: small files only (TSV tables, short annotated excerpts with their
   address, parameter listings). Large dumps and WAVs stay in `$RE_STATE/work/<effect>/`.

Write plainly: short declarative sentences, no filler, no marketing words.

## Final message

At most 300 words: `PASS`, `ISSUES` or `BLOCKED`; the five most important findings;
the five top recommendations; what you could not do; any problem with the tools.
