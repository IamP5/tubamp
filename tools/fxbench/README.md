# tubamp_fxbench, fxcheck and the golden renders

Tools for building the seven effect engines of `docs/FX-ENGINES.md` without touching the
plugin. `tubamp_fxbench` runs one engine (or the frozen pre-change code it must match) over
a WAV or a generated signal; `fxcheck.py` and the `check_<effect>.py` scripts turn that into
acceptance tables; `tubamp_golden` proves the whole plugin did not change.

| File | Owner |
|---|---|
| `main.cpp`, `FxBench.h`, `fxcheck.py`, this README | scaffold (engines never edit; ask the integrator) |
| `legacy/LegacyFx.h`, `legacy/LegacyFx.cpp` | frozen copy of the base commit (`bf81728`); never edited |
| `fx_<effect>.cpp`, `check_<effect>.py` | that effect's engineer |
| `tools/golden/GoldenRender.cpp`, `tests/golden/*` | scaffold capture; `expected-changes.tsv` only with the owner's sign-off |

## Building

Every engineer builds in their own tree with only their effect compiled, so a
half-finished engine elsewhere can never break their build:

```sh
cmake -S . -B build-fx/gate -DCMAKE_BUILD_TYPE=Release -DTUBAMP_BUNDLED_UI=OFF \
  -DFETCHCONTENT_SOURCE_DIR_JUCE=/Users/tuba/Dev/projects/tubamp/build/_deps/juce-src \
  -DFETCHCONTENT_SOURCE_DIR_NEURALAMPMODELERCORE=/Users/tuba/Dev/projects/tubamp/build/_deps/neuralampmodelercore-src \
  -DTUBAMP_FXBENCH_ONLY=gate
cmake --build build-fx/gate --target tubamp_fxbench -j 10
export FXBENCH=$PWD/build-fx/gate/tubamp_fxbench_artefacts/Release/tubamp_fxbench
python3 tools/fxbench/check_gate.py --quick   # not -I: it drops the user site-packages numpy may live in
```

Replace `gate` (three times) with `comp`, `drive`, `eq`, `chorus`, `phaser` or `tremolo`.
`TUBAMP_FXBENCH_ONLY=<effect>` compiles `main.cpp`, the whole legacy reference,
`fx_<effect>.cpp` and that effect's engine (`src/dsp/Gate.cpp`, `Compressor.cpp`,
`Drive.cpp`, `Eq.cpp`, `Chorus.cpp`, `Phaser.cpp`, `Tremolo.cpp`) and nothing else.
`build-fx/` is git-ignored. Use the default Unix Makefiles generator (Xcode fails here).
An edit followed by a build in the same second can be skipped by make: `touch` the file.

Without the option (empty, the default) the target builds all seven: that is the
integrator's and the coordinator's build, in `build/`. The plugin and the console targets
pick the engine sources up through the existing `src/*.cpp` glob; no engineer edits CMake.

## tubamp_fxbench

```
tubamp_fxbench <effect> <in.wav | gen:SPEC> <out.wav> [key=value ...] [options]
  key=value              engine parameter, units as Parameters.h
  key@T=value            step at T seconds
  key@T0..T1=V0..V1      linear ramp, evaluated once per block
  gen:SPEC               sine:<hz>:<dBFS peak> | noise:white|pink:<dBFS rms> | impulse[:<dBFS>]
                         | music | silence   (--sr=<Hz> --seconds=<s> --channels=<1|2>;
                         defaults 48000, 6, 2)
  --block=N              default 512
  --trace=<path.wav>     per-sample trace (gain etc.), Effect::trace
  --meters=<path.tsv>    per-block Effect::meterRow (first column: block start sample)
  --latency              print Effect::latency after the render
  --classic-null         engine at typeKey=0 against the legacy reference; prints the number
                         of differing samples and the first index; exit 1 if any differ
  --legacy               render the legacy reference instead of the engine
  --reset-repeat         render, reset(), render the same input; exit 1 if not identical
  --rt-check             count allocations inside setParams/process; exit 1 on any
  --no-ftz               do not wrap processing in juce::ScopedNoDenormals
  --bench                time setParams+process, print ns/sample and % of real time
                         (min of 5 runs); generator defaults become 48 kHz, block 64, 10 s
```

Exit status: 0 ok, 1 a check failed, 2 usage or I/O error. `tubamp_fxbench --help` lists
the effects the binary was built with and their keys. Output WAVs are 32-bit float.
The sections' `--trace=gain` means "the gain trace"; the option's value is the WAV path. The
check scripts (`fxcheck.invoke`) give `--trace` and `--meters` a temporary path when none is given.

**Options have two dashes.** The sample rate and the block size are options, `--sr=<Hz>` and
`--block=N`. `--sr` applies to `gen:` inputs only; a WAV input keeps its own rate. A token with no
dashes is an engine key or timeline event (`key=value`, `key@T=value`), so **`sr=44100` is taken as an
engine key**, which no adapter reads: it is ignored without a message and the render stays at
48 kHz. An option that is not in the list above is refused (`unknown option`, exit 2).

**Timeline.** Each block, the timeline is evaluated at the block's first sample
(`start / sampleRate`); steps and ramps are therefore per block, exactly like host
automation reaching `setParameters()`. A key that has no constant and no event yet is
absent, so the adapter's default applies. Keys are passed to the adapter as doubles.

**Keys** (each adapter's own; `--help` prints them):

| effect | type key | keys |
|---|---|---|
| gate | `mode` | `threshold release hold range sc_hpf delay deferred on lag prepare_block` |
| comp | `type` | `threshold ratio attack release makeup knee mix sc_hpf auto_makeup on` |
| drive | `type` | `gain tone level on` |
| eq | `type` | `bass mid treble lowcut highcut mid_freq mid_q tilt level voice g100 g200 g400 g800 g1k6 g3k2 g6k4 on reenable` |
| chorus | `mode` | `rate depth mix width tone sync_phase solo on reset host_bpm host_beats host_jitter host_play host_loop_len host_loop_end` |
| phaser | `model` | `rate depth mix color sync_phase on reset_on_enable freeze_u start_reset host_bpm host_note host_ppq host_jitter host_drift` |
| tremolo | `voice` | `rate depth mix width tone shape wave sync_phase on host_sync host_bpm host_beats host_play host_ppq host_jitter` |

`on=0` is the processor's bypass: comp calls `skip()`, the others neither update nor run
the block (gate: no measure, no apply). Gate `deferred=1` applies the curve to the
channels' mono mean and copies it back, which is what the processor does when the amp
follows the gate; `delay=N` is `setCurveDelay`.

Keys that are not engine parameters (test hooks in the adapters):

| effect | key | meaning |
|---|---|---|
| gate | `lag` | with `deferred=1`, the mono signal the curve is applied to is delayed by this many samples (clamped to `0..GateFx::kMaxCurveDelaySamples`); default 0 |
| gate | `prepare_block` | `N > 0` re-prepares the gate for blocks of at most N samples and calls `reset()` when the value changes; 0 (default) is the spec's own block size |
| eq | `reenable` | default 1: when `on` goes from 0 to 1 and the type is not Classic, `reset()` is called, as the processor does (processor: in progress). 0 replays the frozen state, which is what the old processor did |
| chorus | `reset` | a change of this value (any number, compared with the previous block's) calls `reset()` at that block, on the engine and on the legacy reference (its own `reset()`, as `ChorusFx::reset()` does for Classic); default 0 |
| chorus | `solo` | routes one voice (its index) to both outputs at mix 1 (`ChorusFx::setDebugSolo`); `-1`, the default, is off. The `--trace` below is written only while a voice is soloed |
| chorus | `host_bpm` | a simulated play head: when above 0 the adapter resolves `rate` and `sync_phase` from the tempo as the dispatcher does and runs a song position that advances by each block's tempo; 0 (default) is off and `rate` / `sync_phase` are the plain keys |
| chorus | `host_beats` | beats per LFO cycle (the note value; 1 = 1/4, 0.25 = 1/16); default 1 |
| chorus | `host_jitter` | N: each block's reported song position is off by up to N samples (uniform, not accumulated); default 0 |
| chorus | `host_play` | 0 is a stopped transport (`sync_phase` is not sent, the LFO runs free); default 1 |
| chorus | `host_loop_len`, `host_loop_end` | `host_loop_len > 0` wraps the song position back by that many beats once it reaches `host_loop_end` (beats); default 0 (no loop) |
| phaser | `reset_on_enable` | 1: when `on` goes from 0 to 1, `reset()` is called; default 0 (frozen state replayed) |
| phaser | `freeze_u` | test only: holds the sweep position at this value in [-1, 1] (-1 is the bottom; `PhaserFx::freezeSweep`), the frozen-response probe; absent means the LFO runs |
| phaser | `start_reset` | 1: `reset()` is called after the first block's parameters are set, so the engine starts as a voicing switched in does; without it Classic's LFO glides from juce's 1 Hz to the set rate; default 0 |
| phaser | `host_bpm` | a simulated host: present means synced; the tempo in BPM (default 120 when the key is present), setting `rate` and `sync_phase` |
| phaser | `host_note` | LFO cycles per beat (default 1); the phase is song position times this, so a change of `host_note` is a jump |
| phaser | `host_ppq` | the song position of the first block, in beats (default 0); the tempo is integrated between block starts as a straight line, as a host ramping its tempo does |
| phaser | `host_jitter` | N samples: from the second block on, each block's reported position is off by a whole number of samples drawn uniformly from -N..N (not accumulated); default 0 |
| phaser | `host_drift` | D: the song position advances 1 + D times as fast as the reported tempo says, as when the tempo parameter lags the host's own; the `--trace` host phase follows it; default 0 |
| tremolo | (`on`) | no extra key: when `on` goes from 0 to 1 with `voice` other than 0 the adapter calls `reset()` before the next block, as the processor will |
| tremolo | `host_sync` | 1: the adapter stands in for the processor's dispatcher: `rate = host_bpm / (60 * host_beats)` (limited to 0.01-20 Hz) and, while `host_play` is 1, `sync_phase = frac (ppq / host_beats)` at each block start; default 0 (the plain `rate` and `sync_phase` keys) |
| tremolo | `host_bpm`, `host_beats` | tempo (default 120) and beats per LFO cycle (default 1) |
| tremolo | `host_play` | 1 (default): the transport runs (the song position advances by each block's tempo) and `sync_phase` is sent; 0: the transport is stopped, `sync_phase` is not sent and the position does not advance |
| tremolo | `host_ppq` | a change of this value moves the song position there (locate, loop, start position); default 0 |
| tremolo | `host_jitter` | J: a pseudo-random whole number of samples in [-J, J] is added to each block's reported position; default 0 |

What `--trace` and `--meters` write, per adapter:

| effect | `--trace` | `--meters` (columns after the block start sample) |
|---|---|---|
| gate | the applied gain, linear, per sample | state (0 closed, 1 hold, 2 open), gain reduction in dB, detector level in dBFS |
| comp | the gain reduction in dB per sample (before makeup, a positive number) | the block's gain reduction in dB |
| chorus | three channels per sample, while a voice is soloed: the soloed voice's tap delay in samples, its outgoing tap's delay (equal to the first outside a tap crossfade), the crossfade stage (0 none, 1 outgoing tap alone, 2 fading directly, 3 fading through the midpoint tap) | song-position jumps taken, tap crossfades started for knob changes (both counted since `prepare()`) |
| phaser | three channels per block, written to every sample of it: the LFO phase in cycles at the block's last sample, the song-position jumps glided so far, the simulated host's exact (unjittered) phase at the block's last sample; use `--block=1` for a per-sample trace | none |
| tremolo | the left (and right) gain, voices 1-4 (Harmonic: the low band's) | song-position jumps crossfaded, and the engine's LFO phase at block start minus the true (unjittered) host phase, in cycles |
| drive, eq | none | none |

The check scripts' `--rows` and `--verbose`: `check_gate.py --rows=A,B,...` runs only the named
groups (`common` the common checks, `I` the rule's invariants, `P` the engine against a numpy port of the plain
hysteresis rule and the round-5 verifier's findings, `G` G1-G19, `X` X1-X21, `A` to `F` and `R` the other rows,
`T` the targets on seeded draws; a name that is not a group exits 1), for iterating (the full run is the
acceptance); rows named `info ...`
print figures and are never enforced; `check_gate.py --verbose` lists every dip that rows B and C count. `check_comp.py`
also takes `--rows=C3,C10,...` (and `--no-common`, `--no-tables`), `check_eq.py` takes `--only=E5,E12`
and `--measure`, `check_drive.py` takes `--only-specific`.

**`--classic-null`** forces the type key to 0 for both the engine and `makeLegacy()`, feeds
both the same input and the same per-block Args, and compares the outputs **bitwise**
(the golden hashes compare bytes, so −0 against +0 counts). Exit 1 and a sample count on
any difference.

**`--rt-check`** replaces every global `operator new`/`delete` and, on macOS, installs
libmalloc's `malloc_logger` hook, so it also sees `malloc`/`free` that never pass through
`operator new` (`juce::HeapBlock`, hence `AudioBuffer::setSize`). Counting is armed only
around `setParams()` and `process()`, and **only on the thread that runs them**: the hooks see
every thread, and a WAV input leaves reader threads freeing their buffers while the render runs
(before this, 19 of 20 identical compressor runs with a WAV input failed with `malloc 0, free N`).
The thread is compared by identity (`pthread_self`), not with a `thread_local`, which libmalloc would
itself allocate inside the hook. The adapters must not allocate there either.
Proven on a scratch copy: a `std::vector` growing in `process()` reported
`operator new 11, delete 10; malloc 11, free 10`, an `AudioBuffer::setSize` reported
`malloc 563, free 562`, both exit 1.

**`gen:music`** (§5.3.5), the standard input of `--classic-null` and of the golden tool,
is defined in `FxBench.h` (`fxbench::fillMusic`): plucked 82.41 / 110 / 196 Hz notes
(0–2 s, harmonics 1/0.6/0.4/0.25, 5 ms attack, exp(−t/0.4 s), right channel 3 ms later,
notes normalised to −6 dBFS peak), the chord plus white noise at −30 dBFS RMS (2–3 s),
1 kHz at −20 dBFS with 5 ms fades (3–4 s), noise again (4–4.5 s), silence (4.5–6 s).
Noise is the LCG `x = x·1664525 + 1013904223 mod 2³²` from seed 12345. `fxcheck.music()`
is the same algorithm in numpy but not bit-equal (libm, FMA): use `"gen:music"` as the
input when exactness matters.

## Writing an adapter (`fx_<effect>.cpp`)

Implement `fxbench::Effect` (see `FxBench.h`) and register it with
`FXBENCH_REGISTER ("name", Class, "help")`; the scaffold's version of each file already
drives the stub engine and the legacy reference. Allocate everything in `prepare()`. Map
keys with `args.get ("key", default)` (keys are short, so the `std::string` lookup stays in
the small-string buffer and does not allocate). Add test hooks (`--trace`, meters) here, not
in `main.cpp`. A new `fx_*.cpp` is picked up by the CMake glob after a re-configure.

## fxcheck.py and the check scripts

```sh
python3 -I tools/fxbench/check_<effect>.py [--quick] [--types 0,2]
```

`common_checks(effect, types, ranges, budgets, variants, type_key, null_keys)` runs §2.11
for every type: finite output on the 20 s torture input with every float key at its min
and at its max; 2 s of zeros below 1e-9; `--reset-repeat`; `--rt-check`; block-size
independence (new types: blocks 1 and 37 against 512 within 1e-6); Classic null at 44.1 /
48 / 96 kHz, blocks 512 and 37, at each of `null_keys`; denormals (`--no-ftz --bench` on
1 s of music then 30 s of silence, at most 1.5× the FTZ figure, new types); CPU against the
budget (`None` = Classic as today: at most 1.2× the legacy reference's own figure). The
full matrix is 22.05–192 kHz × mono/stereo × blocks 1/37/512; `--quick` is 48 kHz stereo,
blocks 37 and 512. Each script prints a PASS/FAIL/TODO table and exits 1 on any FAIL;
`specific_checks()` is where the effect's own table goes (a few easy rows are already in:
G1 deferred, C1 ramps and skip, D1 steps, E2 exact flat).

Classic gate and Classic drive are not reset-deterministic in the frozen code
(`NoiseGate::reset()` only drops the curve; `Drive::reset()` snaps its smoothers to their
targets), so `common_checks` accepts a Classic `--reset-repeat` failure only when the
legacy reference fails it too.

Also in `fxcheck.py`: `invoke()` / `run()`, WAV I/O (from `tools/re/sig.py`), generators
(`sine`, `sweep`, `burst` with a raised-cosine fade, `chug`, `hover`, `step`, `white`
with a fixed seed, `pink`, `extreme`, `music`) and analyses (`fundamental_residual`
least-squares, `alias_spectrum` harmonic/alias/inharmonic split, `swept_response`,
`settle_times` t50/t63/t90, `first_crossing`, `analytic`, `pitch_track`, `envelope`,
`correlation`, `zipper_lines` at multiples of fs/block, `zipper_metric`).

## tubamp_golden

```sh
G=build/tubamp_golden_artefacts/Release/tubamp_golden
M=/Users/tuba/Dev/projects/tubamp/build/_deps/neuralampmodelercore-src/example_models/A2.nam
$G --verify tests/golden/fx-golden.tsv --model $M                       # strict: every row identical
$G --verify tests/golden/fx-golden.tsv --model $M --expected tests/golden/expected-changes.tsv
$G --params build/params.tsv && diff <(head -167 build/params.tsv) tests/golden/params-base.tsv
$G --both-orders host|preset --model $M                                 # restore before and after prepareToPlay: equal renders
$G --legacy-check [--dump <dir>]
$G --list
```

`fx-golden.tsv` was captured on the unchanged base (`bf81728`, arm64, Release) with
`--model A2.nam`: 101 cases, 339 rows (case, fs, block, SHA-256 of the interleaved float
output, latency). Each case is a fresh processor, `setChainOrder`, the §2.7.5 type ids set to
0 when they exist, the case's parameters, `prepareToPlay`, `gen:music`. Without `--model`
the six `ampM`/`gateampM` rows are skipped. `--only <glob>` restricts the cases. Strict
`--verify` stays the check during the parallel phase; `--expected` applies
`expected-changes.tsv` (§5.4.3) once the integration lands (until then its drive latency
and exact-flat rows fail by design).

`--both-orders host|preset` renders every row twice and requires the same hash and latency: the case's old
session (every newer id left out) restored before `prepareToPlay`, and onto an instance already prepared,
with 100 ms of silence ahead of the music (`host` restores through `setStateInformation`, `preset` through
`applyStateVar`). It compares the two orders with each other, not with the base capture.

`--legacy-check` proves `legacy/` is the code the processor runs: it renders each case
through the processor with a one-block chain and, in lockstep, through the legacy class fed
the processor's own parameter values, wrapped in the processor's always-on stages (input
trim, DC blocker, output level, and for the deferred gate the amp block's collapse and
gains), and requires bit-identical output. `--dump` also writes each case's legacy block
output with the `tubamp_fxbench --legacy` command that reproduces it.

## Things the contract does not say

- Parameter values reaching the processor are not the nominal numbers: defaults and set
  values read back through the APVTS range snapping slightly off (comp threshold
  −19.9999981, comp attack 4.99999952, drive tone 3999.99976, drive level 3.6e-7 dB, input
  trim not exactly 0 dB). Never special-case a nominal float other than where the spec says
  so (EQ knobs at 5.0 do read back as exactly 5.0f).
- The gate adapter's `--trace` reads the curve by calling `apply()` on a buffer of ones
  before applying it to the audio, so `apply()` must not advance any state; change the
  adapter if your design needs that.
- Every engine header has `numTypes` (what `Parameters.cpp`'s `static_assert` uses); gate,
  chorus, phaser and tremolo also have `numModes` / `numModels` / `numVoices`, equal to it.
