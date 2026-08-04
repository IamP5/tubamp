# External AudioUnit slots — design record

Three chain blocks (`fx1`, `fx2`, `fx3` — "FX Slot 1..3") each host one third-party
AudioUnit effect, anywhere in the user-arranged order. Unlike the nine built-in blocks
they carry no DSP of their own; an unoccupied slot is a bit-exact pass-through.

Implementation: `src/dsp/FxHost.{h,cpp}` (the slots, the threading contract),
`src/dsp/FxCatalog.{h,cpp}` (discovery + instantiation), `src/PluginProcessor.{h,cpp}`
(records, state, the UI-facing API), `src/WebEditor.cpp` (bridge). UI contract:
docs/REACT-UI.md §External AudioUnit slots.

This document exists because tubamp is an AudioUnit hosting AudioUnits inside a host
that is itself sandboxing it, and four things that look like the obvious implementation
are wrong there in ways that only show up in Logic, only sometimes, and rarely with a
useful stack trace. Each section is one decision and the failure it avoids.

## (a) Discovery is a metadata-only registry walk, not PluginDirectoryScanner

`FxCatalog::enumerateEffects()` walks the AudioComponent registry directly —
`AudioComponentFindNext` + `AudioComponentGetDescription` + `AudioComponentCopyName` +
`AudioComponentGetVersion` — and synthesizes the identifier itself. JUCE's
`PluginDirectoryScanner` / `AudioPluginFormat::findAllTypesForFile` are not used.

Why: JUCE builds its descriptions by *fully instantiating every component it finds*.
`AudioUnitPluginFormatHeadless::findAllTypesForFile`
(`build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatHeadless.mm:42-67`)
calls `createInstanceFromDescription (desc, 44100.0, 512)` at line 58 and reads the
description back off the resulting instance. In a standalone host that is a scan that
runs once, out of process, behind a crash-recovery list. Inside a plugin it means
loading *every AudioUnit installed on the machine* into the host's own address space:
the ones Logic's validation already rejected, other people's beta builds, instruments
the user will never insert here, anything with a static initializer that phones home or
pops a nag window. The only protection around it is `catch (...)` at line 63 with the
comment `// crashed while loading...` — which catches a C++ exception and nothing else.
A component that segfaults, deadlocks on the message thread, or throws an Obj-C
exception takes the user's session down while they were just opening a picker menu.

Everything the picker needs — name, manufacturer, version — is in the registry without
running a single line of plugin code. The identifier we build is byte-identical to
JUCE's internal `AudioUnitFormatHelpers::createPluginIdentifier`
(`.../format_types/juce_AudioUnitPluginFormatImpl.h:98-121`) for the component types we
accept, i.e. `"AudioUnit:Effects/aufx,subt,manu"`, and `fileOrIdentifier` is the only
field `createPluginInstanceAsync` actually needs. That function is module-internal, so
it is mirrored rather than called (`FxCatalog.cpp`, `makeIdentifier`) — a mirrored
function is a maintenance cost we accept in exchange for not instantiating the world.

Net effect: exactly one component is ever loaded, the one the user picked. The walk is
cheap enough that it is not cached to disk at all; it is called on demand, which also
means a plugin installed while the session is open shows up without a rescan UI.

Filters applied during the walk:

- **Effects only** (`kAudioUnitType_Effect`, `kAudioUnitType_MusicEffect`).
  Instruments, generators, mixers, panners and MIDI processors have no meaning as an
  insert in a strictly serial guitar chain, and accidentally instantiating a
  multi-gigabyte sampler is exactly the surprise this whole design is avoiding.
- **AUv3 excluded** (`kAudioComponentFlag_IsV3AudioUnit`). AUv3 components load out of
  process over XPC. Nesting that inside a plugin that is *itself* already hosted out of
  process (Logic's AUHostingService) is the configuration with the worst track record —
  view-service lifetime, sandbox extensions and state restore all get harder at once.
  v1 skips them; they simply do not appear in the picker.
- **Self excluded** — subtype `'Tamp'` / manufacturer `'Tuba'`, the codes frozen in
  `CMakeLists.txt`. Loading tubamp into one of tubamp's own slots would recurse a whole
  processor (and its factory-preset filesystem work) per instance, and nothing stops the
  nested copy doing it again.

`fxSupported` in `UiState` is `FxCatalog::isSupported()`, i.e. whether this build has
`JUCE_PLUGINHOST_AU` at all. The `FxCatalog` constructor only registers the AU format;
it never scans. A constructor that walks plugin directories is a classic way to fail
`auval`, which instantiates the plugin in a strict, timed harness.

## (b) Hosted instances are never prepared from our prepareToPlay

`FxHost::prepare()` records the sample rate, channel count and block size and returns
whether the change invalidates prepared instances. It deliberately does not touch a
single hosted instance. When it returns true, `prepareToPlay` sets `fxRebuildPending`
and calls `triggerAsyncUpdate()`; `handleAsyncUpdate()` — message thread — pulls each
slot's state, clears it and re-instantiates it from scratch.

Why not just call `prepareToPlay` on the hosted instance from ours: because our own AU
wrapper calls *our* `prepareToPlay` while holding the callback lock. On the
`kAudioUnitProperty_OfflineRender` property path
(`build/_deps/juce-src/modules/juce_audio_plugin_client/juce_audio_plugin_client_AU_1.mm:794-809`)
the wrapper takes `const ScopedLock sl (juceFilter->getCallbackLock())` at line 804 and
then calls `juceFilter->prepareToPlay (getSampleRate(), (int) GetMaxFramesPerSlice())`
at line 809. Logic sets that property on every bounce, freeze and offline render, in
both directions. Preparing a hosted AudioUnit from inside that scope reaches
`AudioUnitInitialize`, which takes CoreAudio's own mutex — while the render thread,
blocked on our callback lock, is holding it. Textbook ABBA: the bounce hangs, the
process has to be killed, and the only symptom the user reports is "Logic freezes when I
bounce".

The same reasoning is why a *reconfiguration* rebuilds rather than re-prepares. There is
no safe in-place path; `prepareToPlay` on a hosted AU is only ever called from
`FxHost::makePrepared()`, on the message thread, with no locks held.

In the window between the configuration change and the rebuild, the audio thread sees a
`Prepared` whose `configEpoch` no longer matches `FxHost::configEpoch` and passes audio
straight through. Honest, click-free, and finite.

Block-size changes alone deliberately do **not** invalidate anything: `FxHost::process`
chunks the buffer to whatever maximum block size each instance was actually prepared
for, so a host handing us a bigger buffer costs an extra loop iteration instead of a
full teardown.

## (c) A bypassed-but-loaded slot still reports its plugin's latency

`FxHost::latencyFor()` returns the live instance's latency regardless of the bypass
parameter, and `computeWantedLatency()` adds it for every fx block *present in the
order*, bypassed or not. A slot that is bypassed runs its signal through a
`BypassDelay` ring buffer of exactly that length instead, so the delay stays in the path
when the plugin does not.

Why: `fxN_on` is a normal, fully automatable APVTS parameter. If reported latency
tracked it, an automation lane toggling an FX slot mid-song would change
`setLatencySamples` mid-playback — and hosts only re-apply plugin delay compensation at
transport boundaries, not on the fly. The audible result is the whole track sliding a
few milliseconds against every other track in the project, at an arbitrary bar, and
sliding back at the next stop/start. Reporting a constant latency for a loaded slot
costs the user a fixed delay they can see in the UI; the alternative costs them their
timing, silently.

The delay line is always written and only read while bypassed, so the delayed signal is
already coherent the instant bypass engages — no discontinuity on the toggle itself.
The compensation is sized once, in `makePrepared()`, *after* state restore, because
restoring a plugin preset can change its reported latency (linear-phase mode,
oversampling factor, lookahead).

An **unoccupied** slot reports zero and adds nothing: it is a pass-through with no
delay line at all.

## (d) Hosted instances are never destroyed on the audio thread

`FxHost::Slot::retire()` only parks a retired `Prepared` in one of six atomic garbage
slots. `collectGarbage()` — message thread, called before every slot mutation and from
`handleAsyncUpdate()` — deletes them. When all six garbage slots are full, `retire()`
**leaks the instance deliberately** (`jassertfalse` in debug) rather than deleting it.

Why leaking is the correct action there: JUCE's AU instance destructor posts a
`CallbackMessage` to the message thread and *blocks on it*.
`~AudioUnitPluginInstanceHeadless`
(`build/_deps/juce-src/modules/juce_audio_processors_headless/format_types/juce_AudioUnitPluginFormatImpl.h:848-887`)
takes a lock, and when it is not running on the message thread constructs an `AUDeleter`
CallbackMessage, `post()`s it (line 883) and calls `completionEvent.wait()` (line 884).
Running that on the render thread stalls audio for as long as the message thread takes
to get around to it — an unbounded wait on a thread that has a hard deadline, i.e. a
dropout at best and a deadlock if the message thread is itself blocked. Six garbage
slots full means the message thread has not run in a very long time, which is precisely
the situation in which blocking on it is worst. A leaked plugin instance is a bounded,
silent cost; the alternative is an audible one.

This is the same staged-swap pattern as `NamEngine` (`src/dsp/NamEngine.cpp`) with the
garbage slots made atomic: three slots and several drain triggers turn NamEngine's
unsynchronised plain-`unique_ptr` array from a theoretical race into a real one.

Two more consequences of the same rule:

- The instantiation callback in `PluginProcessor::instantiateFxSlot` destroys the
  incoming instance *inside* the callback (message thread) on both of its bail-out
  paths — processor already dead, or superseded epoch.
- `FxHost::onSlotRetiring` fires on the message thread immediately *before* an
  instance is retired, so an open plugin editor window can be closed first: an
  `AudioProcessor` must outlive its `AudioProcessorEditor`. Every mutation path — UI
  load/clear, preset load, A/B recall, host state restore, reconfiguration rebuild —
  funnels through `FxHost::loadInstance` / `FxHost::clear`, so that hook is a single
  chokepoint rather than a thing each caller must remember.

## Threading summary

| Operation | Thread |
|---|---|
| `process` / `applyStaging` / `latencyFor` | audio only |
| `prepare` (records config), `loadInstance`, `clear`, `collectGarbage`, `peekInstance` | message only |
| discovery (`enumerateEffects`, `descriptionFor`), `createAsync` + its callback | message only |
| `getStateInformation` / `fxSlotsTree` | message *or* a host save thread — guarded by `fxLock` |

Instances cross between the two threads only through a single-slot atomic exchange;
ownership travels with the swap, so the two threads never touch the same object. The
message thread may read a slot's *newest* instance (to serialize its state) because an
instance only becomes garbage when a successor is staged, and only the message thread
stages.

## State

Serialized as a `FXSLOTS` value tree with one `SLOT` child per **occupied** slot:

```xml
<FXSLOTS>
  <SLOT index="0" plugin="AudioUnit:Effects/aufx,subt,manu"
        name="…" manufacturer="…" state="…base64…"/>
</FXSLOTS>
```

One shape, two containers: `getStateInformation` appends the tree as a child of the
root; `captureStateVar` (presets and A/B slots) stores its XML string under the
`"fxSlots"` property. `parseFxSlotsTree` reads both.

- `name` / `manufacturer` are stored so a slot restored on a machine that does not have
  the plugin can still *name* what is missing, without instantiating anything.
- `state` is the hosted plugin's own `getStateInformation` blob, refreshed from the live
  instance immediately before serializing and before any rebuild.
- An **absent** `FXSLOTS` child, or a preset/A-B slot with no `"fxSlots"` property,
  parses to three empty records and therefore *clears* every slot. That is intentional
  and symmetric with a preset that has no model path clearing the model: leaving a
  plugin loaded across a preset switch would let it survive into the next save. Presets
  written before the FX slots existed simply clear the slots.
- `applyFxRecords` reuses a live instance in place when the incoming record names the
  same plugin, pushing the settings in with `setStateInformation` instead of tearing the
  instance down. This is what makes A/B between two variants of the same rig gapless —
  rebuilding an AudioUnit takes long enough to hear.
- Every mutation bumps the record's `epoch`. An in-flight instantiation whose epoch no
  longer matches has been superseded (rapid A/B, preset thrash) and discards itself
  instead of landing late in the slot.

### Missing plugin

A slot whose plugin cannot be instantiated here — nearly always "not installed on this
machine" — is marked `missing` with the failure text in `error`. Its
`PluginDescription` **and its state blob are kept and re-saved untouched**. Opening the
project on the laptop that has the plugin restores it with all of its settings; opening
it on the one that does not, saving, and going back must not be a silent data loss
event. The UI must therefore never render a missing slot as an empty slot — it renders
the name, the error, and the fact that the settings are preserved.

## (e) The hosted instance never runs on our buffer

`makePrepared` negotiates the instance's *main* bus to our channel width, falling back
to mono (`process` then collapses in and fans back out). A plugin that accepts neither
is refused with a message that lands in `FxSlotState.error` — running it at some third
width silently would be worse.

Aux and sidechain buses are a different matter, and this is the decision worth writing
down. The obvious move is to disable them: v1 feeds no sidechain, so a disabled bus is
the honest description. It does not survive contact with reality. A large fraction of
shipping AudioUnits — most of Native Instruments' range, much of Plugin Alliance's —
publish channel configurations with no representation for a disabled aux bus, and
asking for one gets the *whole layout* refused. Measured on a machine with 111 installed
AUv2 effects, insisting on disabled aux buses rejected 15 of them, including Guitar Rig
6, Supercharger and Shadow Hills.

Leaving those buses enabled and simply not feeding them is worse still, and not by a
little: JUCE's AU host maps every enabled bus straight into whatever buffer it is given
(`juce_AudioUnitPluginFormatImpl.h`, `processAudio`) and guards the overflow with a
`jassert` only. Handing a 2-channel buffer to an instance that kept a sidechain bus is
therefore a read from a null channel pointer in a release build. That is not
theoretical — it segfaulted this probe inside `AudioUnitRender`, and the guards say so
in a comment: *"If these are hit, we might allocate in the process block!"*

So the instance never touches our buffer. `makePrepared` sizes a private `ioScratch`
from what the instance actually ended up with —
`max(mainChannels, getTotalNumInputChannels(), getTotalNumOutputChannels())` — and
`process` copies our channels into the main bus, clears whatever aux channels the plugin
insisted on keeping, runs it there, and copies the main bus back. The copy costs nothing
we were not already paying: JUCE's AU host does `inputBuffer.makeCopyOf (buffer, true)`
on every block regardless.

With this in place all 15 previously-rejected plugins load and process. The only
remaining refusal on that machine is a mid-side decoder that genuinely cannot run as a
stereo insert, which is the correct answer.

`process` still returns early — passing audio through — if our own channel count has
changed out from under a prepared instance, which is the §(b) rebuild window.

## Known limitations (v1)

- **Hosted parameters are not host-automatable.** They have no APVTS parameter and no
  relay, because a relay needs a static parameter id at editor-construction time and a
  hosted plugin's parameter list does not exist until it loads (and changes wholesale
  when the slot is reloaded). They are driven over the bridge instead — `fxSetParam` /
  `fxParamValues`, always normalised 0..1, displayed with the plugin's own text. Logic
  cannot see or automate them. What *is* automatable is `fxN_on`, an ordinary APVTS
  bool, which is also why §(c) exists. The UI states this rather than letting a user
  discover it via an automation lane that does nothing.
- **No sidechain, no MIDI.** A sidechain bus the plugin refuses to give up is fed
  silence (§e); the MIDI buffer handed to each instance is a cleared scratch buffer.
  Music effects appear in the picker (they are `kAudioUnitType_MusicEffect`) but
  receive no notes.
- **A plugin can stall the message thread while it loads.** `createPluginInstanceAsync`
  defers to the message thread and then instantiates *synchronously* there, so a plugin
  that blocks inside `AudioComponentInstanceNew` — waiting on a licence check, say —
  freezes the UI for as long as it takes. One installed plugin (Lexicon MPXReverb) hangs
  indefinitely this way under `tubamp_fxprobe`. Whether it also hangs inside a real host
  is untested: the probe is a console app without a full application run loop, which may
  itself be what the plugin is waiting on. Loading is user-initiated and one slot at a
  time, so the blast radius is bounded, but a slot restored from a project can stall
  project load. Moving instantiation off the message thread is not available — JUCE's AU
  format requires it for plugins that declare
  `requiresUnblockedMessageThreadDuringCreation`.
- **In-process hosting.** The plugin runs in tubamp's address space, which is in the
  host's (or Logic's AUHostingService's). A hosted plugin that crashes takes the host
  process down with it. This is the price of not going out-of-process in v1; the AUv3
  exclusion in §(a) is the same trade-off seen from the other side.
- **Three slots, fixed.** The chain order is packed into a single 128-bit word at 5
  bits per entry plus a 5-bit count (`src/dsp/ChainOrder.h`, widened from the original
  `uint64` / 4-bit-per-entry encoding to fit the comp/drive/eq/mod/delay/reverb
  instance pool), and the static asserts there cap the design at 31 block ids total; 24
  are used (the original 12 plus instances 2/3 of the six duplicable kinds). More slots
  are possible, more *block ids* eventually are not without widening the packing again.
- **No plugin-level parameter automation recording, no plugin preset browsing.** The
  plugin's own editor (`fxOpenEditor`) is the way to reach both — and on Logic it is
  also the only place its text fields get keyboard focus.
