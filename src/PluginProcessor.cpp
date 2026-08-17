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

/** split_mode's choice order, frozen with the parameter (docs/SPLIT.md §2). */
enum class SplitMode { copy = 0, leftRight = 1, crossover = 2 };

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

    Entries past chain::maxChainLength are dropped for the same reason: the packed word
    only carries that many, and a mirror the audio thread cannot be running is a lie the
    UI would then draw.

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

        if ((int) result.order.size() >= chain::maxChainLength)
            continue;

        result.order.push_back ((chain::BlockId) index);

        if (rowIndex < counts.size())
            ++counts[rowIndex];
    }

    // The length cap above is applied to ids, not to structure, so it can decapitate a
    // well-formed split region — drop `mix` (or `lane2`) and leave a dangling `split`
    // behind. The packed word may only ever hold a valid structure or none
    // (docs/SPLIT.md §1), so the grammar is re-enforced on whatever survived: the
    // truncated chain flattens cleanly to serial instead of publishing a half structure
    // the audio thread would refuse and the next sanitize would strip for good. Rows
    // cannot describe an order that just lost entries, so they degrade to auto.
    if (auto restructured = chain::sanitizeStructure (result.order);
        restructured.size() != result.order.size())
    {
        result.order = std::move (restructured);
        counts.clear();
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

    pp.amp2On        = raw (params::amp2On);
    pp.amp2Input     = raw (params::amp2Input);
    pp.amp2Output    = raw (params::amp2Output);

    pp.splitOn       = raw (params::splitOn);
    pp.splitMode     = raw (params::splitMode);
    pp.splitXover    = raw (params::splitXover);

    pp.mixOn         = raw (params::mixOn);
    pp.mixALevel     = raw (params::mixALevel);
    pp.mixBLevel     = raw (params::mixBLevel);
    pp.mixAPan       = raw (params::mixAPan);
    pp.mixBPan       = raw (params::mixBPan);
    pp.mixBPhase     = raw (params::mixBPhase);
    pp.mixLevel      = raw (params::mixLevel);

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
        pp.delayMode[k]     = raw (params::delayModeIds[k]);
        pp.delayRatio[k]    = raw (params::delayRatioIds[k]);
        pp.delayWidth[k]    = raw (params::delayWidthIds[k]);

        pp.reverbOn[k]      = raw (params::reverbOnIds[k]);
        pp.reverbSize[k]    = raw (params::reverbSizeIds[k]);
        pp.reverbDamping[k] = raw (params::reverbDampingIds[k]);
        pp.reverbMix[k]     = raw (params::reverbMixIds[k]);
        pp.reverbWidth[k]   = raw (params::reverbWidthIds[k]);

        pp.reverbAlgo[k]      = raw (params::reverbAlgoIds[k]);
        pp.reverbDecay[k]     = raw (params::reverbDecayIds[k]);
        pp.reverbPredelay[k]  = raw (params::reverbPredelayIds[k]);
        pp.reverbDiffusion[k] = raw (params::reverbDiffusionIds[k]);
        pp.reverbLowCut[k]    = raw (params::reverbLowCutIds[k]);
        pp.reverbHighCut[k]   = raw (params::reverbHighCutIds[k]);
        pp.reverbMod[k]       = raw (params::reverbModIds[k]);
        pp.reverbBassMult[k]  = raw (params::reverbBassMultIds[k]);
        pp.reverbErLevel[k]   = raw (params::reverbErLevelIds[k]);
        pp.reverbColor[k]     = raw (params::reverbColorIds[k]);
        pp.reverbTilt[k]      = raw (params::reverbTiltIds[k]);
        pp.reverbDuck[k]      = raw (params::reverbDuckIds[k]);

        pp.reverbShimmer[k]         = raw (params::reverbShimmerIds[k]);
        pp.reverbShimmerInterval[k] = raw (params::reverbShimmerIntervalIds[k]);
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
    namEngineB.collectGarbage();
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
    // One Slim control for both engines: an amp whose two halves ran at different
    // model sizes would not be one amp.
    const auto slim = (double) pendingSlim.load (std::memory_order_relaxed);
    namEngine.setSlimSize (slim);
    namEngineB.setSlimSize (slim);

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

float TubampAudioProcessor::amp2InputDb() const noexcept
{
    // Same sharing rule as amp2OutputDb() below: engine B's own calibration metadata,
    // the amp block's calibration knobs. Input calibration feeds a NONLINEAR model, so
    // skipping it would not just shift lane B's level — it would change how hard the
    // second amp is driven relative to the first.
    return valueOf (pp.amp2Input)
           + namEngineB.getInputCalibrationDb (isOn (pp.ampCalInput), valueOf (pp.ampCalLevel, 12.0f));
}

float TubampAudioProcessor::amp2OutputDb() const noexcept
{
    // Engine B's own metadata, but the amp block's mode and calibration knobs: amp2
    // deliberately has no duplicates of them, and sharing them is what keeps two lanes
    // loudness-matched by default — the entire point of a dual-amp rig. Without this,
    // lane B would sit at the model's raw level while lane A is normalized, an
    // arbitrary (often tens of dB) imbalance no mixer knob range could bridge.
    const auto mode = (params::OutputMode) juce::jlimit (
        0, params::ampOutModeChoices.size() - 1, (int) valueOf (pp.ampOutMode, 1.0f));

    return valueOf (pp.amp2Output)
           + namEngineB.getOutputCompensationDb (mode, valueOf (pp.ampCalLevel, 12.0f));
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

    splitLow.prepare (spec);
    splitLow.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    splitLow.reset();

    splitHigh.prepare (spec);
    splitHigh.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
    splitHigh.reset();

    maxSplitCutoffHz = (float) juce::jmax (100.0, sampleRate * 0.45);

    namEngine.prepare (sampleRate, juce::jmax (1, samplesPerBlock));
    namEngineB.prepare (sampleRate, juce::jmax (1, samplesPerBlock));

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

    // Two channels whatever the host runs: processBlock views it with the host's own
    // channel count, and a mono host simply never touches the second row.
    laneScratch.setSize (2, juce::jmax (1, samplesPerBlock), false, false, true);
    laneScratch.clear();

    for (auto& ring : laneRing)
    {
        ring.setSize (2, kLaneLatencyRingSamples, false, false, true);
        ring.clear();
    }

    laneRingWrite = 0;
    laneRingDelays = { 0, 0 };
    laneRingPrimed = false;

    for (auto* smoothed : { &inputTrimLin, &outputLevelLin, &ampInLin, &ampOutLin,
                            &amp2InLin, &amp2OutLin, &mixLevelLin })
        smoothed->reset (sampleRate, kGainSmoothingSeconds);

    inputTrimLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.inputTrim)));
    outputLevelLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.outputLevel)));
    ampInLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (ampInputDb()));
    ampOutLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (ampOutputDb()));
    amp2InLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (amp2InputDb()));
    amp2OutLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (amp2OutputDb()));
    mixLevelLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.mixLevel)));

    for (int ch = 0; ch < 2; ++ch)
    {
        mixAGain[(size_t) ch].reset (sampleRate, kGainSmoothingSeconds);
        mixBGain[(size_t) ch].reset (sampleRate, kGainSmoothingSeconds);
        mixAGain[(size_t) ch].setCurrentAndTargetValue (
            juce::Decibels::decibelsToGain (valueOf (pp.mixALevel)));
        mixBGain[(size_t) ch].setCurrentAndTargetValue (
            juce::Decibels::decibelsToGain (valueOf (pp.mixBLevel)));
    }

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

    // docs/REVERB.md §6.4: the first block after a prepare must not glide — the engines
    // were just cleared and reconfigured, so their geometry starts where the parameters
    // already say it is.
    armReverbSnap();

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
    splitLow.reset();
    splitHigh.reset();
    monoScratch.setSize (1, 1, false, false, true);
    laneScratch.setSize (2, 1, false, false, true);

    for (auto& ring : laneRing)
        ring.setSize (2, 1, false, false, true);

    laneRingPrimed = false;
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

    // Adopt a staged model before anything reads it, on both engines and whatever the
    // chain looks like: a swap staged into B must never sit there going stale because
    // the amp2 block happens to be out of the chain this block. The latency report moves
    // below, once the decoded order says which blocks are actually in the path.
    namEngine.applyStaging();
    namEngineB.applyStaging();
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

    // chain::findStructure's rule, applied to the decoded stack array: the Order-taking
    // helper would have to build a vector to run here. No id occurs twice, so every
    // "first occurrence" below is the only one.
    chain::Structure structure;

    for (int i = 0; i < numBlocks; ++i)
    {
        const auto id = order[(size_t) i];
        present[(size_t) id] = true;

        // No id can occur twice, so these are the blocks' positions.
        if (id == chain::BlockId::gate)
            gateIndex = i;
        else if (id == chain::BlockId::amp)
            ampIndex = i;
        else if (id == chain::BlockId::split)
            structure.splitAt = i;
        else if (id == chain::BlockId::lane2)
            structure.lane2At = i;
        else if (id == chain::BlockId::mix)
            structure.mixAt = i;
    }

    const bool gateOn = isOn (pp.gateOn);
    const bool ampOn = isOn (pp.ampOn);
    const bool amp2On = isOn (pp.amp2On);

    // The published order is sanitized before it is packed, so a structure here is a
    // well-formed one; the check is against a word that was never written by us. Lane B
    // also needs somewhere to live: a block bigger than prepareToPlay was promised has
    // nowhere to put it, and runs the chain flat rather than dropping half of it.
    const bool lanes = structure.splitAt >= 0 && structure.valid()
                       && laneScratch.getNumSamples() >= numSamples
                       && laneScratch.getNumChannels() >= numOutputChannels;

    // Report latency from here, where the topology is known: only blocks that are
    // actually in the path delay the signal, and lanes cost what the slower one costs.
    // Hosts tolerate latency changes reported from the audio thread (same contract as
    // the old staging-time report).
    const auto chainLatency = computeChainLatency (order.data(), numBlocks, ampOn, amp2On, lanes);

    {
        const int wantedLatency = chainLatency.reported();

        if (wantedLatency != getLatencySamples())
            setLatencySamples (wantedLatency);
    }

    // Which of the four segments an entry runs in: 0 = serial before the split (and
    // everything in a chain without one), 1 = lane A, 2 = lane B, 3 = serial after the
    // mixer.
    const auto segmentOf = [&structure, lanes] (int index) noexcept
    {
        if (! lanes || index < structure.splitAt) return 0;
        if (index < structure.lane2At)            return 1;
        if (index < structure.mixAt)              return 2;

        return 3;
    };

    // Gate split: the trigger always measures at the gate's own position. The reduction
    // is deferred onto the model's mono output only when the amp runs later in the SAME
    // segment; across a split there is no shared post-model point to gate at (the other
    // lane never saw the model), so it is applied right where the gate sits. Without a
    // split every entry is in segment 0 and this is exactly the old rule.
    const bool deferGateToAmp = gateOn && ampOn && gateIndex >= 0 && ampIndex > gateIndex
                                && segmentOf (gateIndex) == segmentOf (ampIndex);

    // A block that left the chain and comes back must not replay what it was holding
    // when it left — a delay line keeps minutes of audio. The reset is deferred through
    // pendingReset and drained a bounded few per callback: clearing three 4-second
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

        // A block whose clear is chunked re-arms itself until its last chunk is done
        // (the reverb's, docs/REVERB.md §6.3) — still one slot of the budget per
        // callback, so the bound on per-callback memset work is unchanged.
        pendingReset[(size_t) index] = ! resetBlockInstance ((chain::BlockId) index);
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

    if (! present[(size_t) chain::BlockId::amp2])
    {
        amp2InLin.setTargetValue (juce::Decibels::decibelsToGain (amp2InputDb()));
        amp2OutLin.setTargetValue (juce::Decibels::decibelsToGain (amp2OutputDb()));
        amp2InLin.skip (numSamples);
        amp2OutLin.skip (numSamples);
    }

    // The amp blocks' mono-collapse scratch, or null when this block is bigger than
    // prepareToPlay was promised — they then collapse into their target buffer's own
    // first channel, exactly as before.
    float* const monoScratchData = (monoScratch.getNumChannels() > 0
                                    && monoScratch.getNumSamples() >= numSamples)
                                       ? monoScratch.getWritePointer (0)
                                       : nullptr;

    const auto runEntry = [&] (int index, juce::AudioBuffer<float>& target,
                               juce::dsp::AudioBlock<float> targetBlock)
    {
        processChainBlock (order[(size_t) index], target, targetBlock, numSamples,
                           monoScratchData, gateOn, deferGateToAmp, ampOn, amp2On, playHead);
    };

    if (! lanes)
    {
        // No split region in the order: one pass over the host buffer, which is what
        // every chain did before parallel paths existed and what old sessions still get.
        for (int i = 0; i < numBlocks; ++i)
            runEntry (i, buffer, block);
    }
    else
    {
        // A non-owning view of laneScratch with the HOST's channel count: a lane block
        // must see the geometry it sees on the main buffer (FxHost passes a slot through
        // untouched when the two disagree).
        std::array<float*, 2> lanePointers { laneScratch.getWritePointer (0),
                                             laneScratch.getNumChannels() > 1
                                                 ? laneScratch.getWritePointer (1)
                                                 : nullptr };

        juce::AudioBuffer<float> laneBuffer (lanePointers.data(), numOutputChannels, numSamples);
        juce::dsp::AudioBlock<float> laneBlock (laneBuffer);

        for (int i = 0; i < structure.splitAt; ++i)
            runEntry (i, buffer, block);

        splitLanes (buffer, laneBuffer, numOutputChannels, numSamples);

        // Lane A stays on the host buffer, lane B runs on the scratch. Lane B runs even
        // with split_on off — its blocks keep their state warm, so re-enabling the split
        // is click-free — and the mute happens at the mixer.
        for (int i = structure.splitAt + 1; i < structure.lane2At; ++i)
            runEntry (i, buffer, block);

        for (int i = structure.lane2At + 1; i < structure.mixAt; ++i)
            runEntry (i, laneBuffer, laneBlock);

        // The shorter lane waits for the longer one, so the two line up where they meet
        // and the single figure reported to the host is true for both.
        const int maxLane = juce::jmax (chainLatency.laneA, chainLatency.laneB);
        compensateLaneLatency (buffer, laneBuffer, numOutputChannels, numSamples,
                               maxLane - chainLatency.laneA, maxLane - chainLatency.laneB);

        mixLanes (buffer, laneBuffer, numOutputChannels, numSamples);

        for (int i = structure.mixAt + 1; i < numBlocks; ++i)
            runEntry (i, buffer, block);
    }

    // Whatever the rings hold belongs to a run of consecutive blocks that ran the lanes;
    // any block that did not (no split in the order, a scratch too small) ends that run.
    laneRingPrimed = lanes;

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

void TubampAudioProcessor::processAmpMono (NamEngine& engine, juce::dsp::AudioBlock<float> block,
                                           int numSamples, float* monoScratchData,
                                           juce::SmoothedValue<float>& inGain,
                                           juce::SmoothedValue<float>& outGain,
                                           bool applyDeferredGate, bool useToneStack) noexcept
{
    const int numChannels = (int) block.getNumChannels();

    if (numChannels < 1)
        return;

    // Collapse the TARGET buffer — the host's or a lane's — into the scratch. Without
    // one (a block bigger than prepareToPlay promised) channel 0 is collapsed in place,
    // which is the same fallback the chain has always taken.
    float* mono = monoScratchData != nullptr ? monoScratchData : block.getChannelPointer (0);

    if (numChannels == 1)
    {
        if (mono != block.getChannelPointer (0))
            juce::FloatVectorOperations::copy (mono, block.getChannelPointer (0), numSamples);
    }
    else
    {
        const float scale = 1.0f / (float) numChannels;

        for (int n = 0; n < numSamples; ++n)
        {
            float sum = 0.0f;

            for (int ch = 0; ch < numChannels; ++ch)
                sum += block.getChannelPointer ((size_t) ch)[n];

            mono[n] = sum * scale;
        }
    }

    for (int n = 0; n < numSamples; ++n)
        mono[n] *= inGain.getNextValue();

    engine.process (mono, numSamples);

    // Gate reduction lands on the model's output, before the amp-out gain.
    if (applyDeferredGate)
        gate.apply (mono, numSamples);

    // The amp's own tone stack, on the mono path: model -> gate gain -> tone stack is
    // the reference plugin's order (docs/research/nam-plugin-params.md §4), and running
    // it before the stereo expansion is what makes it part of the amp rather than a
    // second EQ block after it. Deliberate — do not move it later. amp2 has none: it is
    // the same NAM core without the amp block's tone controls (docs/SPLIT.md §3), and a
    // user who wants them adds an eq instance behind it.
    if (useToneStack && isOn (pp.ampEqOn))
    {
        ampEq.setParameters (valueOf (pp.ampEqBass, 5.0f), valueOf (pp.ampEqMid, 5.0f),
                             valueOf (pp.ampEqTreble, 5.0f));
        ampEq.process (juce::dsp::AudioBlock<float> (&mono, 1, (size_t) numSamples));
    }

    for (int n = 0; n < numSamples; ++n)
        mono[n] *= outGain.getNextValue();

    for (int ch = 0; ch < numChannels; ++ch)
        if (block.getChannelPointer ((size_t) ch) != mono)
            juce::FloatVectorOperations::copy (block.getChannelPointer ((size_t) ch), mono, numSamples);
}

// One case per KIND: an instance id falls through to its kind's case and `k` picks the
// instance's own DSP object and parameter pointers. Every id is listed rather than
// switching on chain::kindOf(): -Wswitch-enum wants the whole enum either way, and this
// way adding an instance is a compile error until it is wired up.
//
// Nothing here knows whether it is running on the host buffer or on a lane's — that is
// the whole point of the split. `buf` and `block` are two views of the same audio; the
// cases that need an AudioBuffer (FxHost) take the first, everything else the second.
void TubampAudioProcessor::processChainBlock (chain::BlockId id, juce::AudioBuffer<float>& buf,
                                              juce::dsp::AudioBlock<float> block, int numSamples,
                                              float* monoScratchData, bool gateOn,
                                              bool deferGateToAmp, bool ampOn, bool amp2On,
                                              juce::AudioPlayHead* playHead) noexcept
{
    const auto k = (size_t) chain::instanceOf (id);
    juce::dsp::ProcessContextReplacing<float> context (block);

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
                applySmoothedGain (buf, numSamples, compMakeupLin[k]);
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

        // --- mono collapse -> NAM engine A -> expand
        case chain::BlockId::amp:
        {
            ampInLin.setTargetValue (juce::Decibels::decibelsToGain (ampInputDb()));
            ampOutLin.setTargetValue (juce::Decibels::decibelsToGain (ampOutputDb()));

            if (ampOn)
            {
                processAmpMono (namEngine, block, numSamples, monoScratchData,
                                ampInLin, ampOutLin, deferGateToAmp, true);
            }
            else
            {
                // Amp bypassed: keep the smoothers in step so re-enabling doesn't jump,
                // and leave the buffer untouched (same contract as every other
                // bypassable block).
                ampInLin.skip (numSamples);
                ampOutLin.skip (numSamples);
            }

            break;
        }

        // --- mono collapse -> NAM engine B -> expand (no tone stack, never gated)
        case chain::BlockId::amp2:
        {
            amp2InLin.setTargetValue (juce::Decibels::decibelsToGain (amp2InputDb()));
            amp2OutLin.setTargetValue (juce::Decibels::decibelsToGain (amp2OutputDb()));

            // hasLiveModel(), not hasModel(): hasModel() goes true the moment a model is
            // staged, and running the block on that would collapse to mono and apply
            // both gains for one block with the engine still passing audio through dry.
            // No live model = pass-through, which is also the zero this block then
            // contributes to the reported latency.
            if (amp2On && namEngineB.hasLiveModel())
            {
                processAmpMono (namEngineB, block, numSamples, monoScratchData,
                                amp2InLin, amp2OutLin, false, false);
            }
            else
            {
                amp2InLin.skip (numSamples);
                amp2OutLin.skip (numSamples);
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
                                        valueOf (pp.delayMix[k], 0.25f),
                                        (int) valueOf (pp.delayMode[k]),
                                        valueOf (pp.delayRatio[k], 100.0f),
                                        valueOf (pp.delayWidth[k], 1.0f));
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
                // docs/REVERB.md §5, in the spec's order. The fallbacks are the layout's
                // defaults, so a build without one of these ids behaves as Room does —
                // the same absent-means-default keystone the restore paths use (§5.3).
                ReverbFx::Params rp;
                rp.algo = (int) valueOf (pp.reverbAlgo[k]);
                rp.size01 = valueOf (pp.reverbSize[k], 0.5f);
                rp.damping01 = valueOf (pp.reverbDamping[k], 0.5f);
                rp.mix01 = valueOf (pp.reverbMix[k], 0.25f);
                rp.width01 = valueOf (pp.reverbWidth[k], 1.0f);
                rp.decayS = valueOf (pp.reverbDecay[k], 2.0f);
                rp.predelayMs = valueOf (pp.reverbPredelay[k], 0.0f);
                rp.diffusion01 = valueOf (pp.reverbDiffusion[k], 0.7f);
                rp.lowCutHz = valueOf (pp.reverbLowCut[k], 20.0f);
                rp.highCutHz = valueOf (pp.reverbHighCut[k], 20000.0f);
                rp.mod01 = valueOf (pp.reverbMod[k], 0.35f);
                rp.bassMult = valueOf (pp.reverbBassMult[k], 1.0f);
                rp.erLevel01 = valueOf (pp.reverbErLevel[k], 0.5f);
                rp.color01 = valueOf (pp.reverbColor[k], 0.0f);
                rp.tilt = valueOf (pp.reverbTilt[k], 0.0f);
                rp.duck01 = valueOf (pp.reverbDuck[k], 0.0f);
                rp.shimmer01 = valueOf (pp.reverbShimmer[k], 0.0f);
                rp.shimmerInterval = (int) valueOf (pp.reverbShimmerInterval[k], 2.0f);

                // §6.4: one block of "glide nothing" after a clear or a state restore.
                // Consumed here, where the engine actually reads its parameters — a
                // pending block never gets this far, so the flag survives until it runs.
                rp.snap = reverbSnap[(size_t) k].exchange (false, std::memory_order_acq_rel);

                reverbFx[k].setParameters (rp);
                reverbFx[k].process (block);

                // §6.2 step 3: the wet has faded out, so hand the clear to pendingReset.
                // The guard above already bypasses the block while it is pending, so the
                // transition is silence rather than a click.
                if (reverbFx[k].wantsReset())
                    pendingReset[(size_t) id] = true;
            }

            break;
        }

        // --- external AudioUnit slots
        case chain::BlockId::fx1:
        case chain::BlockId::fx2:
        case chain::BlockId::fx3:
        {
            const int slot = chain::fxSlotIndex (id);
            fxHost.process (slot, buf, numSamples, isOn (pp.fxOn[slot]), playHead);
            break;
        }

        // --- structural furniture: the lane machinery in processBlock runs these, and
        // an order carrying one without a valid structure runs flat (they are no-ops).
        case chain::BlockId::split:
        case chain::BlockId::lane2:
        case chain::BlockId::mix:
            break;
    }
}

void TubampAudioProcessor::splitLanes (juce::AudioBuffer<float>& main, juce::AudioBuffer<float>& lane,
                                       int numChannels, int numSamples) noexcept
{
    if (numChannels < 1)
        return;

    const auto mode = (SplitMode) juce::jlimit (0, params::splitModeChoices.size() - 1,
                                                (int) valueOf (pp.splitMode));

    // L/R: lane A takes the left channel, lane B the right, each on all of its channels.
    // A mono host has no two channels to take, so it splits like Copy.
    if (mode == SplitMode::leftRight && numChannels >= 2)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy (lane.getWritePointer (ch), main.getReadPointer (1), numSamples);

        for (int ch = 1; ch < numChannels; ++ch)
            juce::FloatVectorOperations::copy (main.getWritePointer (ch), main.getReadPointer (0), numSamples);

        return;
    }

    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::copy (lane.getWritePointer (ch), main.getReadPointer (ch), numSamples);

    if (mode == SplitMode::crossover)
    {
        const float cutoff = juce::jlimit (100.0f, maxSplitCutoffHz, valueOf (pp.splitXover, 800.0f));

        splitLow.setCutoffFrequency (cutoff);
        splitHigh.setCutoffFrequency (cutoff);

        auto mainBlock = juce::dsp::AudioBlock<float> (main)
                             .getSubsetChannelBlock (0, (size_t) numChannels)
                             .getSubBlock (0, (size_t) numSamples);
        auto laneBlock = juce::dsp::AudioBlock<float> (lane)
                             .getSubsetChannelBlock (0, (size_t) numChannels)
                             .getSubBlock (0, (size_t) numSamples);

        splitLow.process (juce::dsp::ProcessContextReplacing<float> (mainBlock));
        splitHigh.process (juce::dsp::ProcessContextReplacing<float> (laneBlock));
    }
}

void TubampAudioProcessor::mixLanes (juce::AudioBuffer<float>& main, juce::AudioBuffer<float>& lane,
                                     int numChannels, int numSamples) noexcept
{
    if (numChannels < 1)
        return;

    const bool mixOn = isOn (pp.mixOn);
    const bool stereo = numChannels >= 2;

    // split_on is the SPLIT's own enable, not one of the mixer's parameters: it mutes
    // lane B here whether or not the mixer itself is bypassed. Lane B still ran.
    const bool laneBMuted = ! isOn (pp.splitOn);

    const float aLevel = mixOn ? juce::Decibels::decibelsToGain (valueOf (pp.mixALevel)) : 1.0f;
    const float bLevel = laneBMuted ? 0.0f
                                    : (mixOn ? juce::Decibels::decibelsToGain (valueOf (pp.mixBLevel)) : 1.0f);
    const float bSign = (mixOn && isOn (pp.mixBPhase)) ? -1.0f : 1.0f;

    // Equal-power pan, gL = cos((p+1)pi/4) / gR = sin((p+1)pi/4). A mono host has no
    // stereo field to place anything in, so the pans are inert there and the gains
    // collapse to the levels alone.
    const auto panGain = [] (float pan, int channel) noexcept
    {
        const float angle = (juce::jlimit (-1.0f, 1.0f, pan) + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        return channel == 0 ? std::cos (angle) : std::sin (angle);
    };

    for (int ch = 0; ch < 2; ++ch)
    {
        const float aPan = (mixOn && stereo) ? panGain (valueOf (pp.mixAPan), ch) : 1.0f;
        const float bPan = (mixOn && stereo) ? panGain (valueOf (pp.mixBPan), ch) : 1.0f;

        mixAGain[(size_t) ch].setTargetValue (aLevel * aPan);
        mixBGain[(size_t) ch].setTargetValue (bLevel * bPan * bSign);
    }

    mixLevelLin.setTargetValue (mixOn ? juce::Decibels::decibelsToGain (valueOf (pp.mixLevel)) : 1.0f);

    const int capacity = monoScratch.getNumSamples();
    float* const gains = monoScratch.getNumChannels() > 0 ? monoScratch.getWritePointer (0) : nullptr;

    if (gains == nullptr || capacity < 1)
    {
        // No scratch to materialize a ramp in (never after prepareToPlay): sum at unity
        // rather than drop lane B, and keep every smoother in step.
        for (int ch = 0; ch < 2; ++ch)
        {
            mixAGain[(size_t) ch].skip (numSamples);
            mixBGain[(size_t) ch].skip (numSamples);
        }

        for (int ch = 0; ch < numChannels; ++ch)
            juce::FloatVectorOperations::add (main.getWritePointer (ch), lane.getReadPointer (ch), numSamples);

        applySmoothedGain (main, numSamples, mixLevelLin);
        return;
    }

    // A SmoothedValue has to advance exactly numSamples per block, and every lane/channel
    // pair has its own gain — so each one's ramp is materialized once into monoScratch
    // (idle from here on) and applied as a vector. The chunking is for the pathological
    // block that outgrew the scratch; normally it runs exactly once.
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto& aSmoothed = mixAGain[(size_t) ch];
        auto& bSmoothed = mixBGain[(size_t) ch];

        float* const mainData = main.getWritePointer (ch);
        const float* const laneData = lane.getReadPointer (ch);

        for (int offset = 0; offset < numSamples;)
        {
            const int chunk = juce::jmin (capacity, numSamples - offset);

            for (int n = 0; n < chunk; ++n)
                gains[n] = aSmoothed.getNextValue();

            juce::FloatVectorOperations::multiply (mainData + offset, gains, chunk);

            for (int n = 0; n < chunk; ++n)
                gains[n] = bSmoothed.getNextValue();

            juce::FloatVectorOperations::addWithMultiply (mainData + offset, laneData + offset, gains, chunk);

            offset += chunk;
        }
    }

    // Channels the host does not have still advance, so switching to a stereo layout
    // does not resume from a stale gain.
    for (int ch = numChannels; ch < 2; ++ch)
    {
        mixAGain[(size_t) ch].skip (numSamples);
        mixBGain[(size_t) ch].skip (numSamples);
    }

    applySmoothedGain (main, numSamples, mixLevelLin);
}

void TubampAudioProcessor::compensateLaneLatency (juce::AudioBuffer<float>& laneA,
                                                  juce::AudioBuffer<float>& laneB,
                                                  int numChannels, int numSamples,
                                                  int delayA, int delayB) noexcept
{
    const int capacity = laneRing[0].getNumSamples();

    if (numChannels < 1 || laneRing[1].getNumSamples() != capacity || capacity < 2)
        return;

    // The ring is sized for realistic hosted-plugin lookahead (kLaneLatencyRingSamples),
    // and ChainLatency::reported() caps what the host is told at the same figure — so a
    // pairing that lands here is one neither the ring nor the report can align, and the
    // clamp keeps it quiet rather than out of bounds.
    jassert (delayA < capacity && delayB < capacity);

    const std::array<int, 2> delays { juce::jlimit (0, capacity - 1, delayA),
                                      juce::jlimit (0, capacity - 1, delayB) };

    // Stale content: a delay that just changed (a model swap, a hosted plugin landing in
    // a lane) or a run of lane blocks that was interrupted. Cheap, and only on those
    // edges.
    if (! laneRingPrimed || delays != laneRingDelays)
    {
        laneRing[0].clear();
        laneRing[1].clear();
        laneRingWrite = 0;
        laneRingDelays = delays;
    }

    juce::AudioBuffer<float>* const lanes[2] { &laneA, &laneB };

    for (int lane = 0; lane < 2; ++lane)
    {
        const int delay = delays[(size_t) lane];

        // Equal latencies (the common case, and always so when neither lane holds an amp
        // or an fx slot): nothing to compensate, and the ring is not touched at all.
        if (delay == 0)
            continue;

        for (int ch = 0; ch < juce::jmin (numChannels, laneRing[(size_t) lane].getNumChannels()); ++ch)
        {
            auto* ring = laneRing[(size_t) lane].getWritePointer (ch);
            auto* data = lanes[lane]->getWritePointer (ch);
            int write = laneRingWrite;

            for (int n = 0; n < numSamples; ++n)
            {
                // Written before it is read, so the read (at least one sample behind) can
                // never see what this iteration just stored — which is what makes a block
                // longer than the ring harmless.
                ring[write] = data[n];

                int read = write - delay;

                if (read < 0)
                    read += capacity;

                data[n] = ring[read];

                if (++write == capacity)
                    write = 0;
            }
        }
    }

    laneRingWrite = (laneRingWrite + numSamples) % capacity;
}

TubampAudioProcessor::ChainLatency
TubampAudioProcessor::computeChainLatency (const chain::BlockId* order, int numBlocks,
                                           bool ampOn, bool amp2On,
                                           bool lanesRunning) const noexcept
{
    ChainLatency result;

    if (order == nullptr)
        return result;

    // chain::findStructure's rule again (see processBlock): first occurrence of each,
    // and the message thread's order is a vector while the audio thread's is a stack
    // array — the walk is shared instead of the container.
    int splitAt = -1, lane2At = -1, mixAt = -1;

    for (int i = 0; i < numBlocks; ++i)
    {
        if (order[i] == chain::BlockId::split && splitAt < 0)
            splitAt = i;
        else if (order[i] == chain::BlockId::lane2 && lane2At < 0)
            lane2At = i;
        else if (order[i] == chain::BlockId::mix && mixAt < 0)
            mixAt = i;
    }

    // `lanesRunning` is the caller's veto: the audio thread also needs lane scratch big
    // enough for the block, and when it does not have it the chain runs FLAT — every
    // block in series, so the latency it costs is the serial sum, not serial + the
    // slower lane. The message thread never vetoes (prepareToPlay sized the scratch for
    // the block size the host promised).
    const bool lanes = lanesRunning && splitAt >= 0 && lane2At > splitAt && mixAt > lane2At;

    for (int i = 0; i < numBlocks; ++i)
    {
        const auto id = order[i];
        int latency = 0;

        // A resampler's latency only reaches the signal when its block actually runs:
        // in the published order AND enabled. Otherwise it contributes zero, so hosts
        // don't compensate for a delay that never happens. amp2 also needs a LIVE model
        // — the same test its DSP path makes.
        if (id == chain::BlockId::amp)
            latency = ampOn ? namEngine.getLatencySamples() : 0;
        else if (id == chain::BlockId::amp2)
            latency = (amp2On && namEngineB.hasLiveModel()) ? namEngineB.getLatencySamples() : 0;
        // A loaded FX slot contributes its plugin's latency whether or not the slot is
        // bypassed: fxN_on is a normal automatable parameter, and letting an automation
        // lane move the reported latency mid-playback would shift the track against every
        // other one in the project (hosts only re-apply delay compensation at transport
        // boundaries). FxHost holds a bypassed slot at the same latency instead.
        else if (chain::isFxSlot (id))
            latency = fxHost.latencyFor (chain::fxSlotIndex (id));

        if (latency == 0)
            continue;

        if (! lanes || i < splitAt || i > mixAt)
            result.serial += latency;
        else if (i < lane2At)
            result.laneA += latency;
        else
            result.laneB += latency;
    }

    return result;
}

int TubampAudioProcessor::computeWantedLatency (const chain::BlockId* order, int numBlocks,
                                                bool ampOn, bool amp2On) const noexcept
{
    // The lanes run side by side and compensateLaneLatency delays the shorter one up to
    // the longer, so the pair costs the longer one — once.
    return computeChainLatency (order, numBlocks, ampOn, amp2On).reported();
}

void TubampAudioProcessor::updateLatency()
{
    // The mirror, not the packed word: this runs on the message thread (and on the
    // host's restore threads), where the pair behind chainLock is the source of truth.
    // The audio thread reports its own latency inline in processBlock, through the same
    // rule — lane membership included, which is why the whole order is copied here and
    // not just a present[] table.
    chain::Order orderCopy;

    {
        const juce::ScopedLock sl (chainLock);
        orderCopy = uiChainOrder;
    }

    setLatencySamples (computeWantedLatency (orderCopy.data(), (int) orderCopy.size(),
                                             isOn (pp.ampOn), isOn (pp.amp2On)));
}

bool TubampAudioProcessor::resetBlockInstance (chain::BlockId id) noexcept
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

        // The one chunked clear (docs/REVERB.md §6.3): one delay line per call, so the
        // worst case stays in the same class as a DelayFx clear even at 192 kHz. The
        // first step also adopts a pending mode/window reconfigure (§6.2 step 4).
        // `snap` is armed for whichever block runs next: after a clear the engine must
        // land on its stored size, not glide up to it (§6.4).
        case chain::BlockId::reverb:
        case chain::BlockId::reverb2:
        case chain::BlockId::reverb3:
        {
            const bool done = reverbFx[k].resetStep();
            reverbSnap[k].store (true, std::memory_order_release);
            return done;
        }

        // The crossover's filter state is a hundred samples of whatever the chain
        // sounded like when the split left it.
        case chain::BlockId::split:   splitLow.reset(); splitHigh.reset(); break;

        // The mixer holds no audio, only gains — and they must not ramp up from
        // wherever they were when it was removed, so they snap to their last targets.
        case chain::BlockId::mix:
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                mixAGain[(size_t) ch].setCurrentAndTargetValue (mixAGain[(size_t) ch].getTargetValue());
                mixBGain[(size_t) ch].setCurrentAndTargetValue (mixBGain[(size_t) ch].getTargetValue());
            }

            mixLevelLin.setCurrentAndTargetValue (mixLevelLin.getTargetValue());
            break;
        }

        // Lifecycles owned elsewhere: the models and the hosted plugins are swapped in
        // and out by their own staging, the cab's tail belongs to the IR that is
        // loaded, and the gate holds nothing to replay. lane2 is a marker, not a block.
        case chain::BlockId::gate:
        case chain::BlockId::amp:
        case chain::BlockId::amp2:
        case chain::BlockId::cab:
        case chain::BlockId::fx1:
        case chain::BlockId::fx2:
        case chain::BlockId::fx3:
        case chain::BlockId::lane2:   break;
    }

    return true;
}

void TubampAudioProcessor::armReverbSnap() noexcept
{
    for (int k = 0; k < params::maxInstances; ++k)
        reverbSnap[(size_t) k].store (true, std::memory_order_release);
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
    // Structure first, then ids and rows: chain::sanitizeStructure either keeps a
    // well-formed split region or flattens the chain to serial, and sanitiseOrderAndRows
    // re-fits the row layout around whatever survived. The packed word therefore only
    // ever holds a structure the audio thread can execute (docs/SPLIT.md §1).
    const auto sanitised = sanitiseOrderAndRows (chain::sanitizeStructure (order), rows);
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

juce::String TubampAudioProcessor::loadModelB (const juce::File& namFile)
{
    // Same message-thread contract as loadModel(), on the second engine. The two parse
    // the file independently even when it is the same one: nam_core shares no weights
    // between instances (docs/STEREO.md §1), and loading is a UI action.
    namEngineB.collectGarbage();
    namEngineB.setSlimSize ((double) valueOf (apvts.getRawParameterValue (params::ampSlim)));

    const auto error = namEngineB.loadModel (namFile);

    if (error.isNotEmpty())
        return error;

    {
        const juce::ScopedLock sl (pathLock);
        loadedModelPathB = namFile.getFullPathName();
    }

    updateLatency();
    return {};
}

void TubampAudioProcessor::clearModelB()
{
    namEngineB.collectGarbage();
    namEngineB.clearModel();

    {
        const juce::ScopedLock sl (pathLock);
        loadedModelPathB = {};
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
    juce::String modelPathCopy, modelPathBCopy, irPathCopy;

    // Hosts may call this from a save thread, where reading a hosted plugin's state is
    // not safe — the cached blob is used instead. On the message thread (Cmd-S, project
    // close) there is no such constraint, so take the fresh values: otherwise a tweak
    // made in an open plugin window seconds before saving would be lost.
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
        refreshAllFxState();

    {
        const juce::ScopedLock sl (pathLock);
        modelPathCopy = loadedModelPath;
        modelPathBCopy = loadedModelPathB;
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
    // Absent in everything written before engine B existed, which reads back as "no B
    // model" — exactly what such a session had.
    root.setProperty ("modelPathB", modelPathBCopy, nullptr);
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
    {
        // Absent means default, on THIS path too (docs/REVERB.md §5.3) — the mirror of
        // applyStateVar's sweep below, and the reason a session saved before
        // reverb_algo existed resolves to Room rather than to whatever the user last
        // selected. replaceState does not do it for us: valueTreeRedirected creates a
        // fresh child for a parameter the incoming tree does not mention and flushes
        // that parameter's CURRENT value into it, which on a live instance (Logic's
        // "Load Setting…", AU user-preset recall, some hosts' A/B) is the last value
        // dialled in. Setting the defaults first makes the flush write the defaults.
        //
        // The children are APVTS's own: type PARAM, keyed by an "id" property.
        for (auto* parameter : getParameters())
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                if (! paramState.getChildWithProperty ("id", ranged->paramID).isValid())
                    ranged->setValueNotifyingHost (ranged->getDefaultValue());
        }

        apvts.replaceState (paramState);
    }

    // §6.4: whatever the incoming state says, the reverbs adopt it on their next block
    // rather than gliding into it.
    armReverbSnap();

    setEditorSize ({ (int) root.getProperty ("editorWidth", 0),
                     (int) root.getProperty ("editorHeight", 0) });

    const juce::String modelPath = root.getProperty ("modelPath", juce::String()).toString();
    const juce::String modelPathB = root.getProperty ("modelPathB", juce::String()).toString();
    const juce::String irPath = root.getProperty ("irPath", juce::String()).toString();

    // Tolerant parse: missing or unreadable -> the classic order, so state written
    // before the chain became user-arrangeable still loads as the chain it had.
    // chainOrderV2 wins when it is there (see serializeOrder).
    // A hand-edited (or half-written) order with a dangling split flattens to serial
    // here rather than reaching the audio thread — see setChainOrder.
    const juce::String orderV2 = root.getProperty ("chainOrderV2", juce::String()).toString();
    const auto chainState = sanitiseOrderAndRows (
        chain::sanitizeStructure (
            chain::parseOrderOrLegacy (orderV2.isNotEmpty()
                                           ? orderV2
                                           : root.getProperty ("chainOrder", juce::String()).toString())),
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
    auto restore = [this, modelPath, modelPathB, irPath, order, rows, fxIncoming]
    {
        if (modelPath.isNotEmpty() && juce::File (modelPath).existsAsFile())
            loadModel (juce::File (modelPath));
        else
            clearModel();

        if (modelPathB.isNotEmpty() && juce::File (modelPathB).existsAsFile())
            loadModelB (juce::File (modelPathB));
        else
            clearModelB();

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
    state->setProperty ("modelPathB", loadedModelPathB);
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
        chain::sanitizeStructure (
            chain::parseOrderOrLegacy (orderV2.isNotEmpty() ? orderV2
                                                            : obj->getProperty ("chainOrder").toString())),
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

    // A preset written before engine B existed has no "modelPathB": the void var
    // stringifies to "", which clears engine B — the same treatment an absent modelPath
    // gets, and the state such a preset was captured in.
    const juce::String modelPathB = obj->getProperty ("modelPathB").toString();

    if (modelPathB != loadedModelPathB)
    {
        if (modelPathB.isNotEmpty() && juce::File (modelPathB).existsAsFile())
            loadModelB (juce::File (modelPathB));
        else
            clearModelB();
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

    // docs/REVERB.md §6.4: a recall is a scene change, not a knob gesture — the reverbs
    // land on the recalled size and pre-delay instead of rubber-banding into them.
    armReverbSnap();
}
} // namespace tubamp

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tubamp::TubampAudioProcessor();
}
