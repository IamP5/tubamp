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
python3 -I tools/fxbench/check_gate.py --quick
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
The sections' `--trace=gain` means "the gain trace"; the option's value is the WAV path.

**Timeline.** Each block, the timeline is evaluated at the block's first sample
(`start / sampleRate`); steps and ramps are therefore per block, exactly like host
automation reaching `setParameters()`. A key that has no constant and no event yet is
absent, so the adapter's default applies. Keys are passed to the adapter as doubles.

**Keys** (each adapter's own; `--help` prints them):

| effect | type key | keys |
|---|---|---|
| gate | `mode` | `threshold release hold range sc_hpf delay deferred on` |
| comp | `type` | `threshold ratio attack release makeup knee mix sc_hpf auto_makeup on` |
| drive | `type` | `gain tone level on` |
| eq | `type` | `bass mid treble lowcut highcut mid_freq mid_q tilt level voice g100 g200 g400 g800 g1k6 g3k2 g6k4 on` |
| chorus | `mode` | `rate depth mix width tone sync_phase solo on` |
| phaser | `model` | `rate depth mix color sync_phase on` |
| tremolo | `voice` | `rate depth mix width tone shape wave sync_phase on` |

`on=0` is the processor's bypass: comp calls `skip()`, the others neither update nor run
the block (gate: no measure, no apply). Gate `deferred=1` applies the curve to the
channels' mono mean and copies it back, which is what the processor does when the amp
follows the gate; `delay=N` is `setCurveDelay`.

**`--classic-null`** forces the type key to 0 for both the engine and `makeLegacy()`, feeds
both the same input and the same per-block Args, and compares the outputs **bitwise**
(the golden hashes compare bytes, so −0 against +0 counts). Exit 1 and a sample count on
any difference.

**`--rt-check`** replaces every global `operator new`/`delete` and, on macOS, installs
libmalloc's `malloc_logger` hook, so it also sees `malloc`/`free` that never pass through
`operator new` (`juce::HeapBlock`, hence `AudioBuffer::setSize`). Counting is armed only
around `setParams()` and `process()`; the adapters must not allocate there either.
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
