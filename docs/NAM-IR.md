# NAM models and impulse responses — loading, levels, swapping, sessions

How a `.nam` and a `.wav` get from disk into the signal path, at what level, how one
replaces another while audio runs, and what a saved session carries. The reference for
every number here is the open-source TONE3000 plugin (`tone-3000/tone3000-plugin`,
`plugin/src/ProcessorModelLoader.cpp`, `Processor.cpp`, `ChainBlock.h`): the same file
is meant to sit at the same level and behave the same way in both. Where tubamp
deliberately differs, it says so.

Code: `src/dsp/NamEngine.*`, `src/dsp/CabSim.*`, `src/dsp/EngineHandoff.h`,
`src/dsp/SwapFade.h`, `src/library/EmbeddedMedia.*`, `src/library/ToneSidecar.*`.
Tests: `tests/EngineTests.cpp` (part of `tubamp_smoke`).

## 1. Swapping an engine (model or IR)

One mechanism for both, `EngineHandoff<Engine>`:

- The message thread builds the new engine completely — file IO, allocation, prewarm,
  convolver warm-up — and stages it in a single atomic slot. The engine that is
  playing keeps playing meanwhile, and after a failed load.
- The audio thread sees the request at the top of the next block. If the block is in
  the signal path, its output closes over **25 ms** (linear), the engines are
  exchanged on the silent sample, and the new one opens over another 25 ms. While a
  fade runs the block is processed in short slices, so the swap lands within a
  millisecond of silence rather than at the next host block.
- Every audible transition goes through silence: model → model, nothing → model,
  model → nothing, and the same for IRs. For the cab only the convolved share rides
  the fade; the dry share of the mix holds, so at 100 % wet the un-cabbed signal is
  never heard.
- A swap nobody can hear — the stream is starting, the block is bypassed or out of the
  chain — is done at once, with no ramps. So is one that is still pending when
  `prepareToPlay` runs: the staged engine becomes the live one there, so the latency
  and gains the processor reports and seeds right afterwards describe the model that
  will play (a session restore stages its model before the stream is prepared).
- Clearing a slot that is already empty is not a transition: nothing fades.
- The retired engine is parked and destroyed later on the message thread. Neither a
  model nor a `juce::dsp::Convolution` may be destroyed on the audio thread.

The amp's output gain carries the model's own loudness compensation (§2), which can be
20 dB and more. `NamEngine::process()` therefore reports the sample at which the model
changed, and the processor switches that gain there, in one inaudible step. Ramping it
would play the end of one model — or the unprocessed signal around a first load or a
clear — at the other model's gain.

The `amp2` block is an untouched pass-through while engine B has no model (stereo, no
gains, no latency), unlike `amp`, which always runs its own mono path. The model-less
side of an amp2 swap is therefore faded on the lane's own channels
(`NamEngine::fadeUnprocessed`), and the mono/gained path and the untouched one only
ever meet at silence.

Differences from the reference: it swaps under a mutex with a fade handshake between
its loader thread and the audio thread; tubamp does the same splice lock-free. A first
engine there cross-fades in from the dry signal; here it goes through silence like any
other swap, because of the gain coupling above.

## 2. NAM models

- **Rate.** A model runs at its own sample rate (48 kHz when the file does not say)
  behind a Lanczos resampler pair (AudioDSPTools `ResamplingContainer`, filter size
  12), bypassed when the host rate matches. (The reference runs its whole chain at a
  fixed 48 kHz behind one resampler and feeds every model that rate regardless of what
  it declares; for 48 kHz models the two are equivalent.)
- **Latency** is the resampler's, and is *measured*: a band-limited pulse through a
  scratch resampler with an identity "model", peak to peak. The container's own
  `GetLatency()` is only its priming length and leaves out the group delay of the two
  kernels — a few samples, which inside a split is a comb filter between the lanes.
  Per-model latency is not compensated (nor is it in the reference).
- **Output modes** (`amp_out_mode`): Raw; Normalized = `-18 dB − loudness`, limited to
  **±24 dB**; Calibrated = `output_level_dbu − cal level`. Input calibration
  (`amp_cal_input`) = `cal level − input_level_dbu`.
- **Metadata is not trusted.** A loudness that is not finite or lies outside
  −100…0 dB, and an input/output level beyond ±60 dBu, are treated as absent, which
  makes the mode that needs them behave as Raw.
- **Signal safety.** Non-finite samples are replaced by silence before they reach the
  model (they would poison a recurrent state for good) and after it.
  `NamEngine::getFaultCount()` counts the blocks where that happened.
- Loading is synchronous on the message thread (10–15 ms for a standard A2 model),
  with `SetSlimmableSize` applied after the prewarm, as in the reference.

## 3. Impulse responses

Conditioning, in order:

1. At most **10 s** of the file, first two channels. A file cut at the limit gets a
   20 ms ramp at the cut (the reference hard-cuts).
2. `juce::dsp::Convolution` with `Trim::yes` (leading and trailing samples below
   −80 dBFS removed) and `Normalise::no` (kernel scaled by `fileRate / hostRate`, so
   its frequency response does not depend on either rate).
3. **Gain** `g = clamp(1 / sqrt(Σ x² · fileRate / 48000), −48 dB, 0 dB)`, the sum
   taken over *every* channel of the untrimmed file. Unit energy referred to 48 kHz;
   attenuation only — a quiet IR is not boosted.
4. **Cab pad.** A cabinet-like IR gets a further fixed **−18 dB** on the convolved
   signal; a reverb-like one does not. Reverb-like = trimmed kernel longer than 1 s,
   unless TONE3000 tagged the file: gear `cab` forces cabinet, `space` forces room
   (the tag comes from the file's sidecar, §5).

Running it:

- Zero latency. Kernels up to a second (untrimmed) use the uniform partitioned
  convolver, longer ones the two-stage one with an 8192-sample head.
- The convolver is prepared for `min(host block, 256)` samples and fed in slices of
  that: JUCE transforms a block of the *prepared* size on every call, whatever it is
  given. The output does not depend on the host block size (tested to 1e-4).
- **Stereo.** A two-channel file gets two convolvers: one applying the left kernel to
  every channel, one true-stereo (L through the left kernel, R through the right).
  True stereo runs only where the path stays stereo — when no enabled amp follows the
  cab (lane B is not downstream of lane A). Otherwise the left kernel is used alone.
  The verdict is reported to the UI as `ir.image`. When the path changes under a
  loaded IR (an amp behind the cab is added, removed or bypassed) the switch between
  the two convolvers goes through the same 25 ms fade as an IR swap, and the incoming
  convolver is cleared on the silent sample.
- A file with NaN or Inf in it is refused; so is anything over 50 MB, measured while
  reading rather than from the size the path reports.
- `cab_mix` (0–100 %, default 100) is a linear dry/wet; low and high cut act on the
  convolved signal only. A first IR loaded into an empty cab *from the UI* sets the
  mix to 100 % for a cabinet and 50 % for a room; swapping IRs and restoring state
  keep the user's value.
- The host's tail length covers the loaded kernel.

Deliberately not copied: the reference convolves at a fixed 48 kHz; tubamp convolves
at the host rate. The level rules above are rate-independent, so the same file lands
at the same level.

## 4. Sessions carry their files

A saved plugin state used to hold only the paths of its model and IR. It now also
holds the files themselves (`MEDIA` children: zlib + base64 with the original size and
an FNV-1a checksum, one per slot `model`, `modelB`, `ir`; files over 32 MB are left
out). The packed copy is read and compressed on the first save after a load and
cached until the next load of that slot.

Restoring a slot (`TubampAudioProcessor::restoreMedia`):

1. The file is where the state says → load it. Nothing is written.
2. Otherwise, the state carries the file and it decodes to what it claims to be
   (size, checksum, `.nam`/`.wav` name) → write it into the library (`models/` or
   `irs/`), reusing a file of the same name and identical content — also one an
   earlier restore put in under a numbered name — and never overwriting a different
   one (it goes in as `name 2.ext`), restore its sidecar, load it. A rebuilt file
   that then fails to load is removed again.
3. Otherwise, a file of that name is in the library → load that (a session moved
   between machines before files were embedded, or one whose embedded copy is
   damaged).
4. Otherwise the slot is empty and the path is remembered as **missing**
   (`getMissingMediaPath`). The path — and the embedded copy, if the state had one —
   is written again on the next save, so the session heals when the file comes back;
   loading or clearing the slot drops it. A preset that names no file for a slot
   clears another preset's missing report for it.

Presets and A/B slots stay references into the library (they are local files; a copy
of the model in every preset would be megabytes each) and restore through steps 1, 3
and 4.

## 5. TONE3000 sidecars

A file downloaded through the browser gets `<file>.t3k.json` beside it
(`ToneSidecar`): tone id and title, creator, gear, size, architecture, license. It is
what lets the panels credit the creator and link to the tone page, and what tells the
cab whether an IR is a cabinet or a space. It describes one downloaded file, never the
catalog, and travels with the file inside a session (§4).

## 6. What the UI is told

`UiState.model` / `modelB` gain `tone?` (the sidecar). `UiState.ir` is an `IrInfo`:
`path`, `name`, `channels`, `sampleRateHz`, `seconds`, `reverbLike`, `gainDb`,
`truncated`, `image` (`none | stereo | mono-file | mono-path`), `tone?`.
`UiState.missingMedia` is `{ model, modelB, ir }`, each the missing path or null,
with a `missingMediaChanged` event. See docs/REACT-UI.md for the bridge contract.
