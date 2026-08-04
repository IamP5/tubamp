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

// Re-entry resets drained per audio callback (see pendingReset in the header). Two
// covers the common single-block gesture immediately and bounds a preset switch that
// re-enters many blocks to a few callbacks of extra silence for the stragglers.
constexpr int kMaxResetsPerCallback = 2;

// 2 = the chain carries block instances and a row layout. applyStateVar still ignores
// this number, deliberately: every property added since v1 is optional and "absent
// means default", so a v1 preset loads correctly without ever consulting it. It is
// here for the first change that cannot be expressed that way.
constexpr int kStateVersion = 2;

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

/** An order and the row layout that describes it, always consistent with each other. */
struct SanitisedChain
{
    chain::Order order;
    std::vector<int> rows;
};

/** Drops duplicates and out-of-range ids, keeping the caller's ordering, and re-fits
    the rows around whatever survived. Unlike chain::parseOrderOrLegacy this preserves
    an empty order: the user is allowed to strip the chain down to nothing (the editor
    shows its empty state for that).

    Out-of-range ids are dropped rather than clamped: clamping would silently turn an
    id this build does not know about into whichever block happens to sit at the end
    of the enum, which was harmless when there was only one trailing block type but
    now means "a stale token becomes Reverb 3". Dropping is the same tolerance
    chain::parseOrder already applies to unknown tokens.

    Rows only mean anything when they describe exactly this order — every length at
    least 1, summing to its size. Anything else is answered with "auto" ({}), never
    with a single row: a 24-block lane cannot be framed on the board at all. */
SanitisedChain sanitiseOrderAndRows (const chain::Order& order, const std::vector<int>& rows)
{
    SanitisedChain result;

    size_t total = 0;
    bool rowsUsable = ! rows.empty();

    for (auto length : rows)
    {
        if (length < 1)
        {
            rowsUsable = false;
            break;
        }

        total += (size_t) length;
    }

    rowsUsable = rowsUsable && total == order.size();

    // Entries are walked in order, so the row that owns each one is known as we go;
    // a dropped entry simply never reaches its row's count.
    std::vector<int> counts (rowsUsable ? rows.size() : 0, 0);
    size_t rowIndex = 0;
    int consumed = 0;

    std::array<bool, chain::numBlockTypes> seen {};

    for (auto id : order)
    {
        if (rowsUsable)
        {
            while (rowIndex < rows.size() && consumed >= rows[rowIndex])
            {
                consumed = 0;
                ++rowIndex;
            }

            ++consumed;
        }

        const auto index = (int) id;

        if (index < 0 || index >= chain::numBlockTypes)
            continue;

        if (std::exchange (seen[(size_t) index], true))
            continue;

        result.order.push_back ((chain::BlockId) index);

        if (rowIndex < counts.size())
            ++counts[rowIndex];
    }

    for (auto count : counts)
        if (count > 0)
            result.rows.push_back (count);

    if (result.order.empty())
        result.rows.clear();

    return result;
}

/** "3,4,2"; empty for an auto layout. */
juce::String rowsToString (const std::vector<int>& rows)
{
    juce::StringArray parts;

    for (auto length : rows)
        parts.add (juce::String (length));

    return parts.joinIntoString (",");
}

/** Tolerant parse; whatever comes back still goes through sanitiseOrderAndRows, which
    is what actually decides whether the rows describe the order. */
std::vector<int> rowsFromString (const juce::String& text)
{
    std::vector<int> rows;

    for (const auto& raw : juce::StringArray::fromTokens (text, ",", {}))
        if (const auto trimmed = raw.trim(); trimmed.isNotEmpty())
            rows.push_back (trimmed.getIntValue());

    return rows;
}

/** The order as the two properties that keep an older build honest. That build knows
    only ids 0..11 and parses anything it cannot read at all as the classic nine — a
    loud, wrong chain. So an order made entirely of added instances is written as the
    empty-chain sentinel in "chainOrder" (which it reads as "the user emptied the
    chain", quiet and wrong in the harmless direction) and in full in "chainOrderV2",
    which readers prefer whenever it is there. */
struct SerializedOrder
{
    juce::String v1;
    juce::String v2; // empty when v1 carries the whole order
};

SerializedOrder serializeOrder (const chain::Order& order)
{
    const auto text = chain::toString (order);
    bool v1Readable = order.empty();

    for (auto id : order)
        if ((int) id < chain::numV1BlockIds)
            v1Readable = true;

    if (v1Readable)
        return { text, {} };

    return { chain::emptyChainToken, text };
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
    pp.ampOn         = raw (params::ampOn);
    pp.cabOn         = raw (params::cabOn);

    pp.inputTrim     = raw (params::inputTrim);
    pp.outputLevel   = raw (params::outputLevel);

    pp.gateThreshold = raw (params::gateThreshold);

    pp.ampInput      = raw (params::ampInput);
    pp.ampOutput     = raw (params::ampOutput);
    pp.ampOutMode    = raw (params::ampOutMode);
    pp.ampCalInput   = raw (params::ampCalInput);
    pp.ampCalLevel   = raw (params::ampCalLevel);
    pp.ampEqOn       = raw (params::ampEqOn);
    pp.ampEqBass     = raw (params::ampEqBass);
    pp.ampEqMid      = raw (params::ampEqMid);
    pp.ampEqTreble   = raw (params::ampEqTreble);

    pp.cabLowCut     = raw (params::cabLowCut);
    pp.cabHighCut    = raw (params::cabHighCut);

    pp.fxOn[0]       = raw (params::fx1On);
    pp.fxOn[1]       = raw (params::fx2On);
    pp.fxOn[2]       = raw (params::fx3On);

    // Per-instance pointers, in the same index order chain::instanceOf() answers with.
    for (int k = 0; k < params::maxInstances; ++k)
    {
        pp.compOn[k]        = raw (params::compOnIds[k]);
        pp.compThreshold[k] = raw (params::compThresholdIds[k]);
        pp.compRatio[k]     = raw (params::compRatioIds[k]);
        pp.compAttack[k]    = raw (params::compAttackIds[k]);
        pp.compRelease[k]   = raw (params::compReleaseIds[k]);
        pp.compMakeup[k]    = raw (params::compMakeupIds[k]);

        pp.driveOn[k]       = raw (params::driveOnIds[k]);
        pp.driveGain[k]     = raw (params::driveGainIds[k]);
        pp.driveTone[k]     = raw (params::driveToneIds[k]);
        pp.driveLevel[k]    = raw (params::driveLevelIds[k]);

        pp.eqOn[k]          = raw (params::eqOnIds[k]);
        pp.eqBass[k]        = raw (params::eqBassIds[k]);
        pp.eqMid[k]         = raw (params::eqMidIds[k]);
        pp.eqTreble[k]      = raw (params::eqTrebleIds[k]);

        pp.modOn[k]         = raw (params::modOnIds[k]);
        pp.modType[k]       = raw (params::modTypeIds[k]);
        pp.modRate[k]       = raw (params::modRateIds[k]);
        pp.modDepth[k]      = raw (params::modDepthIds[k]);
        pp.modMix[k]        = raw (params::modMixIds[k]);

        pp.delayOn[k]       = raw (params::delayOnIds[k]);
        pp.delayTime[k]     = raw (params::delayTimeIds[k]);
        pp.delayFeedback[k] = raw (params::delayFeedbackIds[k]);
        pp.delayMix[k]      = raw (params::delayMixIds[k]);

        pp.reverbOn[k]      = raw (params::reverbOnIds[k]);
        pp.reverbSize[k]    = raw (params::reverbSizeIds[k]);
        pp.reverbDamping[k] = raw (params::reverbDampingIds[k]);
        pp.reverbMix[k]     = raw (params::reverbMixIds[k]);
    }

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

    // Every instance is prepared whether or not it is in the current order: a block the
    // user drops onto the board has to run in the same block, and preparing one there
    // would allocate on the audio thread.
    for (int k = 0; k < params::maxInstances; ++k)
    {
        compressor[(size_t) k].prepare (spec);
        compressor[(size_t) k].reset();

        drive[(size_t) k].prepare (spec);
        eq[(size_t) k].prepare (spec);
        modulation[(size_t) k].prepare (spec);
        delay[(size_t) k].prepare (spec);
        reverbFx[(size_t) k].prepare (spec);
    }

    cab.prepare (spec);
    ampEq.prepare (spec);
    dcBlocker.prepare (spec);

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

    for (auto* smoothed : { &inputTrimLin, &outputLevelLin, &ampInLin, &ampOutLin })
        smoothed->reset (sampleRate, kGainSmoothingSeconds);

    inputTrimLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.inputTrim)));
    outputLevelLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.outputLevel)));
    ampInLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (ampInputDb()));
    ampOutLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (ampOutputDb()));

    for (int k = 0; k < params::maxInstances; ++k)
    {
        compMakeupLin[(size_t) k].reset (sampleRate, kGainSmoothingSeconds);
        compMakeupLin[(size_t) k].setCurrentAndTargetValue (
            juce::Decibels::decibelsToGain (valueOf (pp.compMakeup[k])));
    }

    // Everything above was just reset, so nothing now in the chain needs the re-entry
    // reset: seed the edge detector from the published order rather than paying a mass
    // drain (and its callbacks of bypass silence) on the first block after a restore.
    {
        std::array<chain::BlockId, chain::maxChainLength> restored {};
        const int count = chain::unpackTo (packedChain.load (std::memory_order_relaxed), restored);

        prevPresent.fill (false);
        pendingReset.fill (false);

        for (int i = 0; i < count; ++i)
            prevPresent[(size_t) restored[(size_t) i]] = true;
    }

    updateLatency();
}

void TubampAudioProcessor::releaseResources()
{
    gate.reset();

    for (int k = 0; k < params::maxInstances; ++k)
    {
        compressor[(size_t) k].reset();
        drive[(size_t) k].reset();
        eq[(size_t) k].reset();
        modulation[(size_t) k].reset();
        delay[(size_t) k].reset();
        reverbFx[(size_t) k].reset();
    }

    cab.reset();
    ampEq.reset();
    dcBlocker.reset();
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
    std::array<chain::BlockId, chain::maxChainLength> order {};
    const int numBlocks = chain::unpackTo (packedChain.load (std::memory_order_relaxed), order);

    std::array<bool, chain::numBlockTypes> present {};
    int gateIndex = -1, ampIndex = -1;

    for (int i = 0; i < numBlocks; ++i)
    {
        const auto id = order[(size_t) i];
        present[(size_t) id] = true;

        // No id can occur twice, so these are the gate's and the amp's positions.
        if (id == chain::BlockId::gate)
            gateIndex = i;
        else if (id == chain::BlockId::amp)
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

    // A block that left the chain and comes back must not replay what it was holding
    // when it left — a delay line keeps minutes of audio. The reset is deferred through
    // pendingReset and drained a bounded few per callback: clearing three 2-second
    // delay lines in one block is megabytes of memset at high sample rates, and a
    // preset switch can re-enter that many at once. While pending, the block processes
    // as bypassed below — silent, never stale, audible again within a couple of blocks.
    for (int index = 0; index < chain::numBlockTypes; ++index)
    {
        if (present[(size_t) index] && ! prevPresent[(size_t) index])
            pendingReset[(size_t) index] = true;
    }

    prevPresent = present;

    for (int index = 0, drained = 0; index < chain::numBlockTypes; ++index)
    {
        if (! pendingReset[(size_t) index])
            continue;

        if (drained == kMaxResetsPerCallback)
            break;

        resetBlockInstance ((chain::BlockId) index);
        pendingReset[(size_t) index] = false;
        ++drained;
    }

    // Blocks that are not in the chain at all keep their gain smoothers in step
    // exactly like a bypassed block, so putting them back never jumps.
    for (int k = 0; k < params::maxInstances; ++k)
    {
        if (! present[(size_t) chain::instanceId (chain::BlockId::comp, k)])
        {
            compMakeupLin[(size_t) k].setTargetValue (
                juce::Decibels::decibelsToGain (valueOf (pp.compMakeup[k])));
            compMakeupLin[(size_t) k].skip (numSamples);
        }
    }

    if (! present[(size_t) chain::BlockId::amp])
    {
        ampInLin.setTargetValue (juce::Decibels::decibelsToGain (ampInputDb()));
        ampOutLin.setTargetValue (juce::Decibels::decibelsToGain (ampOutputDb()));
        ampInLin.skip (numSamples);
        ampOutLin.skip (numSamples);
    }

    // One case per KIND: an instance id falls through to its kind's case and `k` picks
    // the instance's own DSP object and parameter pointers. Every id is listed rather
    // than switching on chain::kindOf(): -Wswitch-enum wants the whole enum either way,
    // and this way adding an instance is a compile error until it is wired up.
    for (int i = 0; i < numBlocks; ++i)
    {
        const auto id = order[(size_t) i];
        const auto k = (size_t) chain::instanceOf (id);

        switch (id)
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
            case chain::BlockId::comp2:
            case chain::BlockId::comp3:
            {
                if (isOn (pp.compOn[k]) && ! pendingReset[(size_t) id])
                {
                    compressor[k].setThreshold (valueOf (pp.compThreshold[k], -20.0f));
                    compressor[k].setRatio (juce::jmax (1.0f, valueOf (pp.compRatio[k], 4.0f)));
                    compressor[k].setAttack (juce::jmax (0.0f, valueOf (pp.compAttack[k], 5.0f)));
                    compressor[k].setRelease (juce::jmax (0.0f, valueOf (pp.compRelease[k], 120.0f)));
                    compressor[k].process (context);

                    compMakeupLin[k].setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.compMakeup[k])));
                    applySmoothedGain (buffer, numSamples, compMakeupLin[k]);
                }
                else
                {
                    compMakeupLin[k].setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.compMakeup[k])));
                    compMakeupLin[k].skip (numSamples);
                }

                break;
            }

            // --- drive
            case chain::BlockId::drive:
            case chain::BlockId::drive2:
            case chain::BlockId::drive3:
            {
                if (isOn (pp.driveOn[k]) && ! pendingReset[(size_t) id])
                {
                    drive[k].setParameters (valueOf (pp.driveGain[k], 12.0f),
                                            valueOf (pp.driveTone[k], 4000.0f),
                                            valueOf (pp.driveLevel[k]));
                    drive[k].process (block);
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

                    // The amp's own tone stack, on the mono path: model -> gate gain ->
                    // tone stack is the reference plugin's order
                    // (docs/research/nam-plugin-params.md §4), and running it before the
                    // stereo expansion is what makes it part of the amp rather than a
                    // second EQ block after it. Deliberate — do not move it later.
                    if (isOn (pp.ampEqOn))
                    {
                        ampEq.setParameters (valueOf (pp.ampEqBass, 5.0f), valueOf (pp.ampEqMid, 5.0f),
                                             valueOf (pp.ampEqTreble, 5.0f));
                        ampEq.process (juce::dsp::AudioBlock<float> (&mono, 1, (size_t) numSamples));
                    }

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
            case chain::BlockId::eq2:
            case chain::BlockId::eq3:
            {
                if (isOn (pp.eqOn[k]) && ! pendingReset[(size_t) id])
                {
                    eq[k].setParameters (valueOf (pp.eqBass[k], 5.0f), valueOf (pp.eqMid[k], 5.0f),
                                         valueOf (pp.eqTreble[k], 5.0f));
                    eq[k].process (block);
                }

                break;
            }

            // --- modulation
            case chain::BlockId::mod:
            case chain::BlockId::mod2:
            case chain::BlockId::mod3:
            {
                if (isOn (pp.modOn[k]) && ! pendingReset[(size_t) id])
                {
                    const auto type = (Modulation::Type) juce::jlimit (0, 2, (int) valueOf (pp.modType[k]));
                    modulation[k].setParameters (type, valueOf (pp.modRate[k], 1.0f),
                                                 valueOf (pp.modDepth[k], 0.4f), valueOf (pp.modMix[k], 0.35f));
                    modulation[k].process (block);
                }

                break;
            }

            // --- delay
            case chain::BlockId::delay:
            case chain::BlockId::delay2:
            case chain::BlockId::delay3:
            {
                if (isOn (pp.delayOn[k]) && ! pendingReset[(size_t) id])
                {
                    delay[k].setParameters (valueOf (pp.delayTime[k], 420.0f),
                                            valueOf (pp.delayFeedback[k], 0.35f),
                                            valueOf (pp.delayMix[k], 0.25f));
                    delay[k].process (block);
                }

                break;
            }

            // --- reverb
            case chain::BlockId::reverb:
            case chain::BlockId::reverb2:
            case chain::BlockId::reverb3:
            {
                if (isOn (pp.reverbOn[k]) && ! pendingReset[(size_t) id])
                {
                    reverbFx[k].setParameters (valueOf (pp.reverbSize[k], 0.5f),
                                               valueOf (pp.reverbDamping[k], 0.5f),
                                               valueOf (pp.reverbMix[k], 0.25f));
                    reverbFx[k].process (block);
                }

                break;
            }

            // --- external AudioUnit slots
            case chain::BlockId::fx1:
            case chain::BlockId::fx2:
            case chain::BlockId::fx3:
            {
                const int slot = chain::fxSlotIndex (id);
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
    // The mirror, not the packed word: this runs on the message thread (and on the
    // host's restore threads), where the pair behind chainLock is the source of truth.
    // The audio thread reports its own latency inline in processBlock.
    std::array<bool, chain::numBlockTypes> present {};

    {
        const juce::ScopedLock sl (chainLock);

        for (auto id : uiChainOrder)
            present[(size_t) id] = true;
    }

    setLatencySamples (computeWantedLatency (present, isOn (pp.ampOn)));
}

void TubampAudioProcessor::resetBlockInstance (chain::BlockId id) noexcept
{
    const auto k = (size_t) chain::instanceOf (id);

    switch (id)
    {
        case chain::BlockId::comp:
        case chain::BlockId::comp2:
        case chain::BlockId::comp3:   compressor[k].reset(); break;

        case chain::BlockId::drive:
        case chain::BlockId::drive2:
        case chain::BlockId::drive3:  drive[k].reset();      break;

        case chain::BlockId::eq:
        case chain::BlockId::eq2:
        case chain::BlockId::eq3:     eq[k].reset();         break;

        case chain::BlockId::mod:
        case chain::BlockId::mod2:
        case chain::BlockId::mod3:    modulation[k].reset(); break;

        case chain::BlockId::delay:
        case chain::BlockId::delay2:
        case chain::BlockId::delay3:  delay[k].reset();      break;

        case chain::BlockId::reverb:
        case chain::BlockId::reverb2:
        case chain::BlockId::reverb3: reverbFx[k].reset();   break;

        // Lifecycles owned elsewhere: the model and the hosted plugins are swapped in
        // and out by their own staging, the cab's tail belongs to the IR that is
        // loaded, and the gate holds nothing to replay.
        case chain::BlockId::gate:
        case chain::BlockId::amp:
        case chain::BlockId::cab:
        case chain::BlockId::fx1:
        case chain::BlockId::fx2:
        case chain::BlockId::fx3:     break;
    }
}

//==============================================================================
void TubampAudioProcessor::publishChainOrder (const chain::Order& order, const std::vector<int>& rows)
{
    {
        const juce::ScopedLock sl (chainLock);
        uiChainOrder = order;
        uiChainRows = rows;
    }

    packedChain.store (chain::pack (order), std::memory_order_relaxed);
}

void TubampAudioProcessor::adoptChainOrder (const chain::Order& order, const std::vector<int>& rows)
{
    publishChainOrder (order, rows);
    updateLatency();

    // Outside every lock: a listener re-reads the order (and the editor does a lot more
    // than that) and must never do so from inside chainLock.
    if (onChainChanged != nullptr)
        onChainChanged();
}

void TubampAudioProcessor::setChainOrder (const chain::Order& order, const std::vector<int>& rows)
{
    const auto sanitised = sanitiseOrderAndRows (order, rows);
    adoptChainOrder (sanitised.order, sanitised.rows);
}

chain::Order TubampAudioProcessor::getChainOrder() const
{
    const juce::ScopedLock sl (chainLock);
    return uiChainOrder;
}

std::vector<int> TubampAudioProcessor::getChainRows() const
{
    const juce::ScopedLock sl (chainLock);
    return uiChainRows;
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

    // Order and rows in one acquisition: a host autosave that lands in the middle of a
    // state restore must not write the new order against the old row layout. The packed
    // word is not read here — it does not carry the rows, and the mirror is safe to
    // touch from a save thread precisely because of this lock.
    chain::Order orderCopy;
    std::vector<int> rowsCopy;

    {
        const juce::ScopedLock sl (chainLock);
        orderCopy = uiChainOrder;
        rowsCopy = uiChainRows;
    }

    juce::ValueTree root ("TUBAMP");
    root.setProperty ("modelPath", modelPathCopy, nullptr);
    root.setProperty ("irPath", irPathCopy, nullptr);

    const auto serialized = serializeOrder (orderCopy);
    root.setProperty ("chainOrder", serialized.v1, nullptr);

    if (serialized.v2.isNotEmpty())
        root.setProperty ("chainOrderV2", serialized.v2, nullptr);

    root.setProperty ("chainRows", rowsToString (rowsCopy), nullptr);

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

    // Tolerant parse: missing or unreadable -> the classic order, so state written
    // before the chain became user-arrangeable still loads as the chain it had.
    // chainOrderV2 wins when it is there (see serializeOrder).
    const juce::String orderV2 = root.getProperty ("chainOrderV2", juce::String()).toString();
    const auto chainState = sanitiseOrderAndRows (
        chain::parseOrderOrLegacy (orderV2.isNotEmpty()
                                       ? orderV2
                                       : root.getProperty ("chainOrder", juce::String()).toString()),
        rowsFromString (root.getProperty ("chainRows", juce::String()).toString()));

    const auto order = chainState.order;
    const auto rows = chainState.rows;

    // The audio thread can have the new order immediately (single atomic word); the
    // notification is message-thread only.
    publishChainOrder (order, rows);

    // A missing FXSLOTS child parses to three empty records, which clears every slot —
    // the same contract as a missing modelPath clearing the model.
    const auto fxIncoming = parseFxSlotsTree (root.getChildWithName ("FXSLOTS"));

    // Model/IR loading does file IO and prewarm; it must never happen here if the
    // host restores state from a non-message thread.
    auto restore = [this, modelPath, irPath, order, rows, fxIncoming]
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
        adoptChainOrder (order, rows);
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

    chain::Order orderCopy;
    std::vector<int> rowsCopy;

    {
        const juce::ScopedLock sl (chainLock);
        orderCopy = uiChainOrder;
        rowsCopy = uiChainRows;
    }

    const auto serialized = serializeOrder (orderCopy);

    auto* state = new juce::DynamicObject();
    state->setProperty ("version", kStateVersion);
    state->setProperty ("params", juce::var (paramValues));
    state->setProperty ("modelPath", loadedModelPath);
    state->setProperty ("irPath", loadedIrPath);
    state->setProperty ("chainOrder", serialized.v1);

    if (serialized.v2.isNotEmpty())
        state->setProperty ("chainOrderV2", serialized.v2);

    state->setProperty ("chainRows", rowsToString (rowsCopy));
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
        // Absent means default: a preset written by a build without a parameter
        // (every v2 instance-pool and amp_eq_* id, from a v1 preset) must not leave
        // whatever the user last dialled in ringing through the recalled sound.
        // Captures from this build carry every parameter, so this only fires for
        // old files.
        for (auto* parameter : getParameters())
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                if (! paramValues->hasProperty (ranged->paramID))
                    ranged->setValueNotifyingHost (ranged->getDefaultValue());
        }

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
    // "chainOrder": the void var stringifies to "" and parseOrderOrLegacy answers with
    // the classic order. Publish before the (slow, synchronous) model/IR loading below
    // so the audio thread never runs the restored parameters against the old topology;
    // adoptChainOrder at the end re-reports latency and notifies the editor.
    const juce::String orderV2 = obj->getProperty ("chainOrderV2").toString();
    const auto chainState = sanitiseOrderAndRows (
        chain::parseOrderOrLegacy (orderV2.isNotEmpty() ? orderV2
                                                        : obj->getProperty ("chainOrder").toString()),
        rowsFromString (obj->getProperty ("chainRows").toString()));

    publishChainOrder (chainState.order, chainState.rows);

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
    adoptChainOrder (chainState.order, chainState.rows);
}
} // namespace tubamp

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tubamp::TubampAudioProcessor();
}
