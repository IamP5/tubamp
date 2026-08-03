#include "PluginProcessor.h"

// Console targets (tubamp_smoke) compile the DSP/state/library sources with
// JUCE_WEB_BROWSER=0, where the WebView editor cannot compile at all. They set
// TUBAMP_HEADLESS=1 and get JUCE's generic editor instead.
#ifndef TUBAMP_HEADLESS
 #define TUBAMP_HEADLESS 0
#endif

#if ! TUBAMP_HEADLESS
 #include "WebEditor.h"
#endif

#include <array>
#include <cmath>
#include <utility>

namespace tubamp
{
namespace
{
constexpr double kGainSmoothingSeconds = 0.02;
constexpr int kStateVersion = 1;

/** Applies a smoothed gain to the whole buffer as a single ramp — one pass, no zipper. */
void applySmoothedGain (juce::AudioBuffer<float>& buffer, int numSamples,
                        juce::SmoothedValue<float>& smoothed)
{
    const float start = smoothed.getCurrentValue();
    const float end = smoothed.skip (numSamples);

    if (juce::approximatelyEqual (start, end))
        buffer.applyGain (0, numSamples, start);
    else
        buffer.applyGainRamp (0, numSamples, start, end);
}

bool isOn (const std::atomic<float>* p) noexcept
{
    return p != nullptr && p->load (std::memory_order_relaxed) > 0.5f;
}

float valueOf (const std::atomic<float>* p, float fallback = 0.0f) noexcept
{
    return p != nullptr ? p->load (std::memory_order_relaxed) : fallback;
}

/** Drops duplicates and out-of-range ids, keeping the caller's ordering. Unlike
    chain::fromString this preserves an empty order: the user is allowed to strip the
    chain down to nothing (the editor shows its empty state for that).

    Out-of-range ids are dropped rather than clamped: clamping would silently turn an
    id this build does not know about into whichever block happens to sit at the end
    of the enum, which was harmless when there was only one trailing block type but
    now means "a stale token becomes FX Slot 3". Dropping is the same tolerance
    chain::fromString already applies to unknown tokens. */
chain::Order sanitiseOrder (const chain::Order& order)
{
    chain::Order result;
    std::array<bool, chain::numBlockTypes> seen {};

    for (auto id : order)
    {
        const auto index = (int) id;

        if (index < 0 || index >= chain::numBlockTypes)
            continue;

        if (! std::exchange (seen[(size_t) index], true))
            result.push_back ((chain::BlockId) index);
    }

    return result;
}
} // namespace

//==============================================================================
TubampAudioProcessor::TubampAudioProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", params::createParameterLayout()),
      presets (*this)
{
    const auto raw = [this] (const char* id) { return apvts.getRawParameterValue (id); };

    pp.gateOn        = raw (params::gateOn);
    pp.compOn        = raw (params::compOn);
    pp.driveOn       = raw (params::driveOn);
    pp.ampOn         = raw (params::ampOn);
    pp.cabOn         = raw (params::cabOn);
    pp.eqOn          = raw (params::eqOn);
    pp.modOn         = raw (params::modOn);
    pp.delayOn       = raw (params::delayOn);
    pp.reverbOn      = raw (params::reverbOn);

    pp.inputTrim     = raw (params::inputTrim);
    pp.outputLevel   = raw (params::outputLevel);

    pp.gateThreshold = raw (params::gateThreshold);

    pp.compThreshold = raw (params::compThreshold);
    pp.compRatio     = raw (params::compRatio);
    pp.compAttack    = raw (params::compAttack);
    pp.compRelease   = raw (params::compRelease);
    pp.compMakeup    = raw (params::compMakeup);

    pp.driveGain     = raw (params::driveGain);
    pp.driveTone     = raw (params::driveTone);
    pp.driveLevel    = raw (params::driveLevel);

    pp.ampInput      = raw (params::ampInput);
    pp.ampOutput     = raw (params::ampOutput);
    pp.ampOutMode    = raw (params::ampOutMode);
    pp.ampCalInput   = raw (params::ampCalInput);
    pp.ampCalLevel   = raw (params::ampCalLevel);

    pp.cabLowCut     = raw (params::cabLowCut);
    pp.cabHighCut    = raw (params::cabHighCut);

    pp.eqBass        = raw (params::eqBass);
    pp.eqMid         = raw (params::eqMid);
    pp.eqTreble      = raw (params::eqTreble);

    pp.modType       = raw (params::modType);
    pp.modRate       = raw (params::modRate);
    pp.modDepth      = raw (params::modDepth);
    pp.modMix        = raw (params::modMix);

    pp.delayTime     = raw (params::delayTime);
    pp.delayFeedback = raw (params::delayFeedback);
    pp.delayMix      = raw (params::delayMix);

    pp.reverbSize    = raw (params::reverbSize);
    pp.reverbDamping = raw (params::reverbDamping);
    pp.reverbMix     = raw (params::reverbMix);

    pp.fxOn[0]       = raw (params::fx1On);
    pp.fxOn[1]       = raw (params::fx2On);
    pp.fxOn[2]       = raw (params::fx3On);

    pendingSlim.store (valueOf (raw (params::ampSlim)), std::memory_order_relaxed);
    apvts.addParameterListener (params::ampSlim, this);

    // Single chokepoint: FxHost fires this before retiring any instance, whatever
    // caused the retire (UI, preset, A/B, host state restore), so the editor always
    // gets to close a hosted plugin's window before the plugin behind it dies.
    fxHost.onSlotRetiring = [this] (int slot)
    {
        if (onFxSlotRetiring != nullptr)
            onFxSlotRetiring (slot);
    };

    presets.createFactoryPresetsIfMissing();
}

TubampAudioProcessor::~TubampAudioProcessor()
{
    apvts.removeParameterListener (params::ampSlim, this);
    cancelPendingUpdate();
    *aliveFlag = false;

    // Before anything else: nothing may call back into this half-destroyed processor,
    // and every hosted instance must die here on the message thread.
    fxHost.onSlotRetiring = nullptr;
    onFxSlotRetiring = nullptr;
    onFxSlotChanged = nullptr;

    for (int slot = 0; slot < chain::numFxSlots; ++slot)
        fxHost.clear (slot);

    fxHost.collectGarbage();
    namEngine.collectGarbage();
}

void TubampAudioProcessor::parameterChanged (const juce::String& parameterID, float newValue)
{
    if (parameterID == params::ampSlim)
    {
        pendingSlim.store (newValue, std::memory_order_relaxed);
        triggerAsyncUpdate();
    }
}

void TubampAudioProcessor::handleAsyncUpdate()
{
    namEngine.setSlimSize ((double) pendingSlim.load (std::memory_order_relaxed));

    if (fxRebuildPending.exchange (false, std::memory_order_acq_rel))
    {
        // The host changed sample rate or channel count. The hosted instances were
        // prepared for the old one and are being bypassed; rebuild them from scratch.
        // Re-preparing them in place is not an option — see the FxHost class comment.
        for (int slot = 0; slot < chain::numFxSlots; ++slot)
        {
            refreshFxState (slot);
            fxHost.clear (slot);
            instantiateFxSlot (slot);
        }
    }

    fxHost.collectGarbage();
}

//==============================================================================
TubampAudioProcessor::FxSlotInfo TubampAudioProcessor::getFxSlotInfo (int slot) const
{
    FxSlotInfo info;

    if (slot < 0 || slot >= chain::numFxSlots)
        return info;

    {
        const juce::ScopedLock sl (fxLock);
        const auto& record = fxRecords[(size_t) slot];

        info.identifier = record.desc.fileOrIdentifier;
        info.name = record.desc.name;
        info.manufacturer = record.desc.manufacturerName;
        info.occupied = record.occupied;
        info.missing = record.missing;
        info.loading = record.loading;
        info.error = record.error;
    }

    info.live = fxHost.isLive (slot);
    info.latencySamples = fxHost.latencyFor (slot);

    return info;
}

void TubampAudioProcessor::notifyFxSlotChanged (int slot)
{
    if (onFxSlotChanged != nullptr)
        onFxSlotChanged (slot);
}

void TubampAudioProcessor::loadFxPlugin (int slot, const juce::String& identifier)
{
    if (slot < 0 || slot >= chain::numFxSlots)
        return;

    if (identifier.isEmpty())
    {
        clearFxPlugin (slot);
        return;
    }

    {
        const juce::ScopedLock sl (fxLock);
        auto& record = fxRecords[(size_t) slot];

        record.desc = fxCatalog.descriptionFor (identifier);
        record.state.reset();
        record.occupied = true;
        record.missing = false;
        record.loading = true;
        record.error = {};
    }

    // Drop whatever was running before the replacement arrives: the slot passes audio
    // through in the meantime, which is honest, and the retire hook closes any editor
    // window still showing the outgoing plugin.
    fxHost.clear (slot);
    updateLatency();

    notifyFxSlotChanged (slot);
    instantiateFxSlot (slot);
}

void TubampAudioProcessor::clearFxPlugin (int slot)
{
    if (slot < 0 || slot >= chain::numFxSlots)
        return;

    {
        const juce::ScopedLock sl (fxLock);
        fxRecords[(size_t) slot] = FxRecord { {}, {}, false, false, false, {},
                                              fxRecords[(size_t) slot].epoch + 1 };
    }

    fxHost.clear (slot);
    updateLatency();
    notifyFxSlotChanged (slot);
}

juce::Point<int> TubampAudioProcessor::getEditorSize() const
{
    const juce::ScopedLock sl (editorSizeLock);
    return editorSize;
}

void TubampAudioProcessor::setEditorSize (juce::Point<int> size)
{
    const juce::ScopedLock sl (editorSizeLock);
    editorSize = size;
}

juce::AudioProcessor* TubampAudioProcessor::getFxInstance (int slot)
{
    return fxHost.peekInstance (slot);
}

void TubampAudioProcessor::instantiateFxSlot (int slot)
{
    juce::PluginDescription desc;
    juce::MemoryBlock state;
    uint32_t epoch = 0;

    {
        const juce::ScopedLock sl (fxLock);
        auto& record = fxRecords[(size_t) slot];

        if (! record.occupied)
            return;

        // Every instantiation supersedes any earlier one still in flight, so a burst
        // of A/B recalls or preset loads can never land an older plugin last.
        epoch = ++record.epoch;
        desc = record.desc;
        state = record.state;
        record.loading = true;
        record.error = {};
    }

    const double rate = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const int block = getBlockSize() > 0 ? getBlockSize() : 512;

    std::weak_ptr<bool> alive = aliveFlag;

    fxCatalog.createAsync (desc, rate, block,
        [this, alive, slot, epoch, state] (std::unique_ptr<juce::AudioProcessor> instance,
                                           const juce::String& error) mutable
        {
            // Both guards destroy `instance` right here, on the message thread, which
            // is the only thread allowed to destroy a hosted AudioUnit.
            auto locked = alive.lock();

            if (locked == nullptr || ! *locked)
                return;

            {
                const juce::ScopedLock sl (fxLock);

                if (fxRecords[(size_t) slot].epoch != epoch)
                    return; // superseded while we were loading
            }

            const auto fail = [this, slot, epoch] (const juce::String& message)
            {
                {
                    const juce::ScopedLock sl (fxLock);
                    auto& record = fxRecords[(size_t) slot];

                    if (record.epoch != epoch)
                        return;

                    record.loading = false;
                    record.missing = true;
                    record.error = message;
                }

                notifyFxSlotChanged (slot);
            };

            if (instance == nullptr)
            {
                fail (error.isNotEmpty() ? error : juce::String ("Could not load this plugin"));
                return;
            }

            if (const auto prepareError = fxHost.loadInstance (slot, std::move (instance), &state);
                prepareError.isNotEmpty())
            {
                fail (prepareError);
                return;
            }

            {
                const juce::ScopedLock sl (fxLock);
                auto& record = fxRecords[(size_t) slot];
                record.loading = false;
                record.missing = false;
                record.error = {};
            }

            updateLatency();
            notifyFxSlotChanged (slot);
        });
}

void TubampAudioProcessor::refreshFxState (int slot)
{
    if (slot < 0 || slot >= chain::numFxSlots)
        return;

    // Safe on the message thread: an instance only becomes garbage when its successor
    // is staged, and only this thread stages, so the newest instance of a slot cannot
    // be destroyed underneath us (FxHost.h).
    auto* instance = fxHost.peekInstance (slot);

    if (instance == nullptr)
        return;

    juce::MemoryBlock captured;
    instance->getStateInformation (captured);

    const juce::ScopedLock sl (fxLock);
    auto& record = fxRecords[(size_t) slot];

    if (record.occupied)
        record.state = std::move (captured);
}

void TubampAudioProcessor::refreshAllFxState()
{
    for (int slot = 0; slot < chain::numFxSlots; ++slot)
        refreshFxState (slot);
}

//==============================================================================
juce::ValueTree TubampAudioProcessor::fxSlotsTree() const
{
    juce::ValueTree tree ("FXSLOTS");

    const juce::ScopedLock sl (fxLock);

    for (int slot = 0; slot < chain::numFxSlots; ++slot)
    {
        const auto& record = fxRecords[(size_t) slot];

        if (! record.occupied)
            continue;

        juce::ValueTree child ("SLOT");
        child.setProperty ("index", slot, nullptr);
        child.setProperty ("plugin", record.desc.fileOrIdentifier, nullptr);
        child.setProperty ("name", record.desc.name, nullptr);
        child.setProperty ("manufacturer", record.desc.manufacturerName, nullptr);

        // Written even for a slot whose plugin is missing on this machine: the blob is
        // the user's settings, and dropping it would turn "open the project on the
        // other laptop" into silent data loss.
        if (record.state.getSize() > 0)
            child.setProperty ("state", record.state.toBase64Encoding(), nullptr);

        tree.appendChild (child, nullptr);
    }

    return tree;
}

std::array<TubampAudioProcessor::FxRecord, chain::numFxSlots>
TubampAudioProcessor::parseFxSlotsTree (const juce::ValueTree& tree) const
{
    std::array<FxRecord, chain::numFxSlots> parsed;

    if (! tree.isValid())
        return parsed;

    for (const auto& child : tree)
    {
        if (! child.hasType ("SLOT"))
            continue;

        const int slot = (int) child.getProperty ("index", -1);

        if (slot < 0 || slot >= chain::numFxSlots)
            continue;

        const juce::String identifier = child.getProperty ("plugin", juce::String()).toString();

        if (identifier.isEmpty())
            continue;

        auto& record = parsed[(size_t) slot];
        record.occupied = true;
        record.desc.pluginFormatName = "AudioUnit";
        record.desc.fileOrIdentifier = identifier;
        record.desc.name = child.getProperty ("name", juce::String()).toString();
        record.desc.manufacturerName = child.getProperty ("manufacturer", juce::String()).toString();

        const juce::String encoded = child.getProperty ("state", juce::String()).toString();

        if (encoded.isNotEmpty())
            record.state.fromBase64Encoding (encoded);
    }

    return parsed;
}

void TubampAudioProcessor::applyFxRecords (const std::array<FxRecord, chain::numFxSlots>& incoming)
{
    for (int slot = 0; slot < chain::numFxSlots; ++slot)
    {
        const auto& want = incoming[(size_t) slot];

        // Same plugin already loaded: push the settings into the running instance
        // instead of tearing it down. This is what makes A/B between two variants of
        // the same rig gapless — rebuilding an AudioUnit takes long enough to hear.
        bool reuseInPlace = false;

        {
            const juce::ScopedLock sl (fxLock);
            const auto& have = fxRecords[(size_t) slot];

            reuseInPlace = want.occupied && have.occupied && ! have.missing && ! have.loading
                           && have.desc.fileOrIdentifier == want.desc.fileOrIdentifier
                           && fxHost.isLive (slot);
        }

        if (reuseInPlace)
        {
            if (auto* instance = fxHost.peekInstance (slot); instance != nullptr && want.state.getSize() > 0)
                instance->setStateInformation (want.state.getData(), (int) want.state.getSize());

            {
                const juce::ScopedLock sl (fxLock);
                fxRecords[(size_t) slot].state = want.state;
            }

            notifyFxSlotChanged (slot);
            continue;
        }

        if (! want.occupied)
        {
            // Absent fx state must clear the slot, exactly like a preset without a
            // modelPath clears the model. Leaving it loaded would let a plugin survive
            // a preset switch and then be written into the next save.
            const juce::ScopedLock sl (fxLock);
            const bool wasOccupied = fxRecords[(size_t) slot].occupied;
            const auto nextEpoch = fxRecords[(size_t) slot].epoch + 1;
            fxRecords[(size_t) slot] = FxRecord { {}, {}, false, false, false, {}, nextEpoch };

            if (! wasOccupied)
                continue;
        }
        else
        {
            const juce::ScopedLock sl (fxLock);
            auto& record = fxRecords[(size_t) slot];
            const auto nextEpoch = record.epoch + 1;
            record = want;
            record.epoch = nextEpoch;
            record.loading = true;
        }

        fxHost.clear (slot);
        notifyFxSlotChanged (slot);
        instantiateFxSlot (slot);
    }

    updateLatency();
}

float TubampAudioProcessor::ampInputDb() const noexcept
{
    return valueOf (pp.ampInput)
           + namEngine.getInputCalibrationDb (isOn (pp.ampCalInput), valueOf (pp.ampCalLevel, 12.0f));
}

float TubampAudioProcessor::ampOutputDb() const noexcept
{
    const auto mode = (params::OutputMode) juce::jlimit (
        0, params::ampOutModeChoices.size() - 1, (int) valueOf (pp.ampOutMode, 1.0f));

    return valueOf (pp.ampOutput)
           + namEngine.getOutputCompensationDb (mode, valueOf (pp.ampCalLevel, 12.0f));
}

//==============================================================================
bool TubampAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainIn = layouts.getMainInputChannelSet();
    const auto& mainOut = layouts.getMainOutputChannelSet();

    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;

    return mainIn == mainOut;
}

void TubampAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    const auto numChannels = (juce::uint32) juce::jmax (1, getTotalNumOutputChannels());
    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), numChannels };

    // Sized for the largest block the host promised: the AudioDSPTools trigger/gain
    // only ever grow their buffers, so priming at the maximum keeps processBlock
    // allocation-free. Everything else about the gate's character is fixed by spec.
    gate.prepare (sampleRate, juce::jmax (1, samplesPerBlock));
    gate.reset();

    compressor.prepare (spec);
    compressor.reset();

    drive.prepare (spec);
    cab.prepare (spec);
    eq.prepare (spec);
    dcBlocker.prepare (spec);
    modulation.prepare (spec);
    delay.prepare (spec);
    reverbFx.prepare (spec);

    namEngine.prepare (sampleRate, juce::jmax (1, samplesPerBlock));

    // Records the configuration only — it must not touch the hosted instances. The
    // host can call prepareToPlay while holding our callback lock (Logic does, on the
    // kAudioUnitProperty_OfflineRender path taken by every bounce), and preparing a
    // hosted AudioUnit from there deadlocks against CoreAudio's own mutex. Slots whose
    // instance no longer matches the configuration pass audio through until the
    // message thread has rebuilt them, which is what this async update kicks off.
    if (fxHost.prepare (sampleRate, juce::jmax (1, getTotalNumOutputChannels()),
                        juce::jmax (1, samplesPerBlock)))
    {
        fxRebuildPending.store (true, std::memory_order_release);
        triggerAsyncUpdate();
    }

    monoScratch.setSize (1, juce::jmax (1, samplesPerBlock), false, false, true);
    monoScratch.clear();

    for (auto* smoothed : { &inputTrimLin, &outputLevelLin, &ampInLin, &ampOutLin, &compMakeupLin })
        smoothed->reset (sampleRate, kGainSmoothingSeconds);

    inputTrimLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.inputTrim)));
    outputLevelLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.outputLevel)));
    ampInLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (ampInputDb()));
    ampOutLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (ampOutputDb()));
    compMakeupLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.compMakeup)));

    updateLatency();
}

void TubampAudioProcessor::releaseResources()
{
    gate.reset();
    compressor.reset();
    drive.reset();
    cab.reset();
    eq.reset();
    dcBlocker.reset();
    modulation.reset();
    delay.reset();
    reverbFx.reset();
    monoScratch.setSize (1, 1, false, false, true);
}

//==============================================================================
void TubampAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numInputChannels = getTotalNumInputChannels();
    const int numOutputChannels = getTotalNumOutputChannels();

    for (int ch = numInputChannels; ch < numOutputChannels; ++ch)
        buffer.clear (ch, 0, numSamples);

    // Adopt a staged model before anything reads it. The latency report moves below,
    // once the decoded order says whether the amp block is actually in the path.
    namEngine.applyStaging();
    fxHost.applyStaging();

    auto* const playHead = getPlayHead();

    if (numSamples <= 0 || numOutputChannels <= 0)
        return;

    juce::dsp::AudioBlock<float> fullBlock (buffer);
    auto block = fullBlock.getSubsetChannelBlock (0, (size_t) numOutputChannels);
    juce::dsp::ProcessContextReplacing<float> context (block);

    // --- input trim
    inputTrimLin.setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.inputTrim)));
    applySmoothedGain (buffer, numSamples, inputTrimLin);

    {
        const float mag = buffer.getMagnitude (0, numSamples);
        float prev = inputPeak.load (std::memory_order_relaxed);
        while (mag > prev && ! inputPeak.compare_exchange_weak (prev, mag, std::memory_order_relaxed)) {}
    }

    // --- decode the published order once (stack only, no allocation)
    std::array<chain::BlockId, chain::numBlockTypes> order {};
    const int numBlocks = chain::unpackTo (packedChain.load (std::memory_order_relaxed), order);

    std::array<bool, chain::numBlockTypes> present {};
    int gateIndex = -1, ampIndex = -1;

    for (int i = 0; i < numBlocks; ++i)
    {
        const auto id = order[(size_t) i];
        present[(size_t) id] = true;

        if (id == chain::BlockId::gate && gateIndex < 0)
            gateIndex = i;
        else if (id == chain::BlockId::amp && ampIndex < 0)
            ampIndex = i;
    }

    const bool gateOn = isOn (pp.gateOn);
    const bool ampOn = isOn (pp.ampOn);

    // Report latency from here, where the topology is known: only blocks that are
    // actually in the path delay the signal. Hosts tolerate latency changes reported
    // from the audio thread (same contract as the old staging-time report).
    {
        const int wantedLatency = computeWantedLatency (present, ampOn);

        if (wantedLatency != getLatencySamples())
            setLatencySamples (wantedLatency);
    }

    // Gate split: the trigger always measures at the gate's own position. The
    // reduction is deferred onto the model's mono output only when the amp actually
    // runs later in the chain; otherwise there is no post-model point to gate at and
    // it is applied right where the gate sits.
    const bool deferGateToAmp = gateOn && ampOn && gateIndex >= 0 && ampIndex > gateIndex;

    // Blocks that are not in the chain at all keep their gain smoothers in step
    // exactly like a bypassed block, so putting them back never jumps.
    if (! present[(size_t) chain::BlockId::comp])
    {
        compMakeupLin.setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.compMakeup)));
        compMakeupLin.skip (numSamples);
    }

    if (! present[(size_t) chain::BlockId::amp])
    {
        ampInLin.setTargetValue (juce::Decibels::decibelsToGain (ampInputDb()));
        ampOutLin.setTargetValue (juce::Decibels::decibelsToGain (ampOutputDb()));
        ampInLin.skip (numSamples);
        ampOutLin.skip (numSamples);
    }

    for (int i = 0; i < numBlocks; ++i)
    {
        switch (order[(size_t) i])
        {
            // --- gate trigger (analysis only when the reduction lands after the model)
            case chain::BlockId::gate:
            {
                if (gateOn)
                {
                    gate.measure (block, numSamples, valueOf (pp.gateThreshold, -80.0f));

                    if (! deferGateToAmp)
                        gate.apply (block, numSamples);
                }

                break;
            }

            // --- compressor (+ makeup)
            case chain::BlockId::comp:
            {
                if (isOn (pp.compOn))
                {
                    compressor.setThreshold (valueOf (pp.compThreshold, -20.0f));
                    compressor.setRatio (juce::jmax (1.0f, valueOf (pp.compRatio, 4.0f)));
                    compressor.setAttack (juce::jmax (0.0f, valueOf (pp.compAttack, 5.0f)));
                    compressor.setRelease (juce::jmax (0.0f, valueOf (pp.compRelease, 120.0f)));
                    compressor.process (context);

                    compMakeupLin.setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.compMakeup)));
                    applySmoothedGain (buffer, numSamples, compMakeupLin);
                }
                else
                {
                    compMakeupLin.setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.compMakeup)));
                    compMakeupLin.skip (numSamples);
                }

                break;
            }

            // --- drive
            case chain::BlockId::drive:
            {
                if (isOn (pp.driveOn))
                {
                    drive.setParameters (valueOf (pp.driveGain, 12.0f),
                                         valueOf (pp.driveTone, 4000.0f),
                                         valueOf (pp.driveLevel));
                    drive.process (block);
                }

                break;
            }

            // --- mono collapse -> NAM -> expand
            case chain::BlockId::amp:
            {
                ampInLin.setTargetValue (juce::Decibels::decibelsToGain (ampInputDb()));
                ampOutLin.setTargetValue (juce::Decibels::decibelsToGain (ampOutputDb()));

                if (ampOn)
                {
                    float* mono = (monoScratch.getNumSamples() >= numSamples && monoScratch.getNumChannels() > 0)
                                      ? monoScratch.getWritePointer (0)
                                      : buffer.getWritePointer (0);

                    if (numOutputChannels == 1)
                    {
                        if (mono != buffer.getWritePointer (0))
                            juce::FloatVectorOperations::copy (mono, buffer.getReadPointer (0), numSamples);
                    }
                    else
                    {
                        const float scale = 1.0f / (float) numOutputChannels;

                        for (int n = 0; n < numSamples; ++n)
                        {
                            float sum = 0.0f;

                            for (int ch = 0; ch < numOutputChannels; ++ch)
                                sum += buffer.getReadPointer (ch)[n];

                            mono[n] = sum * scale;
                        }
                    }

                    for (int n = 0; n < numSamples; ++n)
                        mono[n] *= ampInLin.getNextValue();

                    namEngine.process (mono, numSamples);

                    // Gate reduction lands on the model's output, before the amp-out gain.
                    if (deferGateToAmp)
                        gate.apply (mono, numSamples);

                    for (int n = 0; n < numSamples; ++n)
                        mono[n] *= ampOutLin.getNextValue();

                    for (int ch = 0; ch < numOutputChannels; ++ch)
                        if (buffer.getWritePointer (ch) != mono)
                            juce::FloatVectorOperations::copy (buffer.getWritePointer (ch), mono, numSamples);
                }
                else
                {
                    // Amp bypassed: keep the smoothers in step so re-enabling doesn't
                    // jump, and leave the buffer untouched (same contract as every
                    // other bypassable block).
                    ampInLin.skip (numSamples);
                    ampOutLin.skip (numSamples);
                }

                break;
            }

            // --- cab IR
            case chain::BlockId::cab:
            {
                if (isOn (pp.cabOn) && cab.hasImpulseResponse())
                {
                    cab.setParameters (valueOf (pp.cabLowCut, 80.0f), valueOf (pp.cabHighCut, 8000.0f));
                    cab.process (block);
                }

                break;
            }

            // --- tone stack
            case chain::BlockId::eq:
            {
                if (isOn (pp.eqOn))
                {
                    eq.setParameters (valueOf (pp.eqBass, 5.0f), valueOf (pp.eqMid, 5.0f),
                                      valueOf (pp.eqTreble, 5.0f));
                    eq.process (block);
                }

                break;
            }

            // --- modulation
            case chain::BlockId::mod:
            {
                if (isOn (pp.modOn))
                {
                    const auto type = (Modulation::Type) juce::jlimit (0, 2, (int) valueOf (pp.modType));
                    modulation.setParameters (type, valueOf (pp.modRate, 1.0f),
                                              valueOf (pp.modDepth, 0.4f), valueOf (pp.modMix, 0.35f));
                    modulation.process (block);
                }

                break;
            }

            // --- delay
            case chain::BlockId::delay:
            {
                if (isOn (pp.delayOn))
                {
                    delay.setParameters (valueOf (pp.delayTime, 420.0f),
                                         valueOf (pp.delayFeedback, 0.35f),
                                         valueOf (pp.delayMix, 0.25f));
                    delay.process (block);
                }

                break;
            }

            // --- reverb
            case chain::BlockId::reverb:
            {
                if (isOn (pp.reverbOn))
                {
                    reverbFx.setParameters (valueOf (pp.reverbSize, 0.5f),
                                            valueOf (pp.reverbDamping, 0.5f),
                                            valueOf (pp.reverbMix, 0.25f));
                    reverbFx.process (block);
                }

                break;
            }

            // --- external AudioUnit slots
            case chain::BlockId::fx1:
            case chain::BlockId::fx2:
            case chain::BlockId::fx3:
            {
                const int slot = chain::fxSlotIndex (order[(size_t) i]);
                fxHost.process (slot, buffer, numSamples, isOn (pp.fxOn[slot]), playHead);
                break;
            }
        }
    }

    // --- DC blocker: always on, never user-facing (spec §4 step 7)
    dcBlocker.process (block);

    // --- output level
    outputLevelLin.setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.outputLevel)));
    applySmoothedGain (buffer, numSamples, outputLevelLin);

    {
        const float mag = buffer.getMagnitude (0, numSamples);
        float prev = outputPeak.load (std::memory_order_relaxed);
        while (mag > prev && ! outputPeak.compare_exchange_weak (prev, mag, std::memory_order_relaxed)) {}
    }
}

//==============================================================================
juce::AudioProcessorEditor* TubampAudioProcessor::createEditor()
{
   #if TUBAMP_HEADLESS
    return new juce::GenericAudioProcessorEditor (*this);
   #else
    return new WebEditor (*this);
   #endif
}

int TubampAudioProcessor::computeWantedLatency (const std::array<bool, chain::numBlockTypes>& present,
                                                bool ampOn) const noexcept
{
    // The resampler's latency only reaches the signal when the amp block actually
    // runs: present in the published order AND enabled. Otherwise report zero so
    // hosts don't compensate for a delay that never happens.
    int latency = (present[(size_t) chain::BlockId::amp] && ampOn) ? namEngine.getLatencySamples() : 0;

    // A loaded FX slot contributes its plugin's latency whether or not the slot is
    // bypassed: fxN_on is a normal automatable parameter, and letting an automation
    // lane move the reported latency mid-playback would shift the track against every
    // other one in the project (hosts only re-apply delay compensation at transport
    // boundaries). FxHost holds a bypassed slot at the same latency instead.
    for (int slot = 0; slot < chain::numFxSlots; ++slot)
        if (present[(size_t) chain::BlockId::fx1 + (size_t) slot])
            latency += fxHost.latencyFor (slot);

    return latency;
}

void TubampAudioProcessor::updateLatency()
{
    std::array<chain::BlockId, chain::numBlockTypes> order {};
    const int numBlocks = chain::unpackTo (packedChain.load (std::memory_order_relaxed), order);

    std::array<bool, chain::numBlockTypes> present {};

    for (int i = 0; i < numBlocks; ++i)
        present[(size_t) order[(size_t) i]] = true;

    setLatencySamples (computeWantedLatency (present, isOn (pp.ampOn)));
}

//==============================================================================
void TubampAudioProcessor::publishChainOrder (const chain::Order& order) noexcept
{
    packedChain.store (chain::pack (order), std::memory_order_relaxed);
}

void TubampAudioProcessor::adoptChainOrder (const chain::Order& order)
{
    uiChainOrder = order;
    publishChainOrder (uiChainOrder);
    updateLatency();

    if (onChainChanged != nullptr)
        onChainChanged();
}

void TubampAudioProcessor::setChainOrder (const chain::Order& order)
{
    adoptChainOrder (sanitiseOrder (order));
}

//==============================================================================
juce::String TubampAudioProcessor::loadModel (const juce::File& namFile)
{
    // Message thread: the old model (if any) has been parked by the audio thread.
    namEngine.collectGarbage();

    // Push the current Slim value first: loadModel() applies whatever the engine last
    // stored to the new model, and a state restore can beat the parameter listener here.
    namEngine.setSlimSize ((double) valueOf (apvts.getRawParameterValue (params::ampSlim)));

    const auto error = namEngine.loadModel (namFile);

    if (error.isNotEmpty())
        return error;

    {
        const juce::ScopedLock sl (pathLock);
        loadedModelPath = namFile.getFullPathName();
    }

    updateLatency();
    return {};
}

void TubampAudioProcessor::clearModel()
{
    namEngine.collectGarbage();
    namEngine.clearModel();

    {
        const juce::ScopedLock sl (pathLock);
        loadedModelPath = {};
    }

    updateLatency();
}

juce::String TubampAudioProcessor::loadIr (const juce::File& wavFile)
{
    const auto error = cab.loadImpulseResponse (wavFile);

    if (error.isNotEmpty())
        return error;

    {
        const juce::ScopedLock sl (pathLock);
        loadedIrPath = wavFile.getFullPathName();
    }

    return {};
}

void TubampAudioProcessor::clearIr()
{
    cab.clearImpulseResponse();

    const juce::ScopedLock sl (pathLock);
    loadedIrPath = {};
}

//==============================================================================
void TubampAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Copy the paths under the lock (the copies are safe to use outside it —
    // juce::String's refcount is atomic; only concurrent reassignment races).
    juce::String modelPathCopy, irPathCopy;

    // Hosts may call this from a save thread, where reading a hosted plugin's state is
    // not safe — the cached blob is used instead. On the message thread (Cmd-S, project
    // close) there is no such constraint, so take the fresh values: otherwise a tweak
    // made in an open plugin window seconds before saving would be lost.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        refreshAllFxState();

    {
        const juce::ScopedLock sl (pathLock);
        modelPathCopy = loadedModelPath;
        irPathCopy = loadedIrPath;
    }

    juce::ValueTree root ("TUBAMP");
    root.setProperty ("modelPath", modelPathCopy, nullptr);
    root.setProperty ("irPath", irPathCopy, nullptr);

    // Read back from the published word rather than uiChainOrder: hosts are allowed
    // to save state from a non-message thread, and the atomic is the only copy that
    // is safe to touch from there.
    root.setProperty ("chainOrder",
                      chain::toString (chain::unpack (packedChain.load (std::memory_order_relaxed))),
                      nullptr);

    if (const auto size = getEditorSize(); size.x > 0 && size.y > 0)
    {
        root.setProperty ("editorWidth", size.x, nullptr);
        root.setProperty ("editorHeight", size.y, nullptr);
    }

    root.appendChild (apvts.copyState(), nullptr);
    root.appendChild (fxSlotsTree(), nullptr);

    if (auto xml = root.createXml())
        copyXmlToBinary (*xml, destData);
}

void TubampAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr)
        return;

    auto root = juce::ValueTree::fromXml (*xml);

    if (! root.isValid() || ! root.hasType ("TUBAMP"))
        return;

    if (auto paramState = root.getChildWithName (apvts.state.getType()); paramState.isValid())
        apvts.replaceState (paramState);

    setEditorSize ({ (int) root.getProperty ("editorWidth", 0),
                     (int) root.getProperty ("editorHeight", 0) });

    const juce::String modelPath = root.getProperty ("modelPath", juce::String()).toString();
    const juce::String irPath = root.getProperty ("irPath", juce::String()).toString();

    // Tolerant parse: missing or unreadable -> default order, so state written before
    // the chain became user-arrangeable still loads.
    const auto order = chain::fromString (root.getProperty ("chainOrder", juce::String()).toString());

    // The audio thread can have the new order immediately (single atomic word); the
    // UI-facing copy and the notification are message-thread only.
    publishChainOrder (order);

    // A missing FXSLOTS child parses to three empty records, which clears every slot —
    // the same contract as a missing modelPath clearing the model.
    const auto fxIncoming = parseFxSlotsTree (root.getChildWithName ("FXSLOTS"));

    // Model/IR loading does file IO and prewarm; it must never happen here if the
    // host restores state from a non-message thread.
    auto restore = [this, modelPath, irPath, order, fxIncoming]
    {
        if (modelPath.isNotEmpty() && juce::File (modelPath).existsAsFile())
            loadModel (juce::File (modelPath));
        else
            clearModel();

        if (irPath.isNotEmpty() && juce::File (irPath).existsAsFile())
            loadIr (juce::File (irPath));
        else
            clearIr();

        applyFxRecords (fxIncoming);
        adoptChainOrder (order);
    };

    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        restore();
    }
    else
    {
        std::weak_ptr<bool> alive = aliveFlag;
        juce::MessageManager::callAsync ([alive, restore]
        {
            if (auto locked = alive.lock(); locked != nullptr && *locked)
                restore();
        });
    }
}

//==============================================================================
juce::var TubampAudioProcessor::captureStateVar()
{
    auto* paramValues = new juce::DynamicObject();

    for (auto* parameter : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            paramValues->setProperty (juce::Identifier (ranged->paramID),
                                      (double) ranged->convertFrom0to1 (ranged->getValue()));

    // Message thread (presets, A/B), so the hosted instances can be asked for their
    // current state directly rather than served from the cache.
    refreshAllFxState();

    auto* state = new juce::DynamicObject();
    state->setProperty ("version", kStateVersion);
    state->setProperty ("params", juce::var (paramValues));
    state->setProperty ("modelPath", loadedModelPath);
    state->setProperty ("irPath", loadedIrPath);
    state->setProperty ("chainOrder", chain::toString (uiChainOrder));
    state->setProperty ("fxSlots", fxSlotsTree().toXmlString());

    return juce::var (state);
}

void TubampAudioProcessor::applyStateVar (const juce::var& state)
{
    auto* obj = state.getDynamicObject();

    if (obj == nullptr)
        return;

    const juce::var paramsVar = obj->getProperty ("params");

    if (auto* paramValues = paramsVar.getDynamicObject())
    {
        for (const auto& entry : paramValues->getProperties())
        {
            if (auto* parameter = apvts.getParameter (entry.name.toString()))
            {
                const auto plain = (float) (double) entry.value;
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (plain));
            }
        }
    }

    // Presets and A/B slots written before the chain became user-arrangeable have no
    // "chainOrder": the void var stringifies to "" and fromString falls back to the
    // default order. Publish before the (slow, synchronous) model/IR loading below so
    // the audio thread never runs the restored parameters against the old topology;
    // adoptChainOrder at the end updates the UI copy and notifies the editor.
    const auto order = chain::fromString (obj->getProperty ("chainOrder").toString());
    publishChainOrder (order);

    const juce::String modelPath = obj->getProperty ("modelPath").toString();

    if (modelPath != loadedModelPath)
    {
        if (modelPath.isNotEmpty() && juce::File (modelPath).existsAsFile())
            loadModel (juce::File (modelPath));
        else
            clearModel();
    }

    const juce::String irPath = obj->getProperty ("irPath").toString();

    if (irPath != loadedIrPath)
    {
        if (irPath.isNotEmpty() && juce::File (irPath).existsAsFile())
            loadIr (juce::File (irPath));
        else
            clearIr();
    }

    // Presets and A/B slots written before the fx slots existed have no "fxSlots":
    // the void var stringifies to "", which parses to three empty records and clears
    // every slot — the same treatment a preset without a modelPath gets.
    applyFxRecords (parseFxSlotsTree (juce::ValueTree::fromXml (obj->getProperty ("fxSlots").toString())));

    // Message thread only (PresetManager, A/B), so adopting directly is safe; the
    // audio thread already got the order via publishChainOrder above.
    adoptChainOrder (order);
}
} // namespace tubamp

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tubamp::TubampAudioProcessor();
}
