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

#include <algorithm>
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

// A lane whose alignment delay jumps slides, or crossfades, to the new one over this long
// (BypassFader's 10 ms).
constexpr double kLaneTapFadeSeconds = 0.01;

// The parameters a latency depends on besides the chain, the models and the hosted plugins.
// Their listeners only raise a flag, for the timer (reportLatency).
constexpr const char* kLatencyParameterIds[] { params::driveOn, params::drive2On, params::drive3On,
                                               params::driveType, params::drive2Type, params::drive3Type,
                                               params::ampOn, params::amp2On };

// How often the message thread hands the audio thread's latency to the host (the only place it
// does, so at most one report per tick).
constexpr int kLatencyTimerMs = 20;

// A sample this large or larger is no audio signal (+120 dBFS): the engines treat it as
// non-finite, and so does the processor, at its input and at its output.
constexpr float kAbsurdSample = 1.0e6f;

/** Replaces every non-finite sample, and every one of kAbsurdSample or more, with 0; anything
    else is left as it is, bit for bit. */
void zeroAbsurd (float* data, int numSamples) noexcept
{
    for (int n = 0; n < numSamples; ++n)
        if (! (std::abs (data[n]) < kAbsurdSample))
            data[n] = 0.0f;
}

double smoothstep (double x) noexcept
{
    x = juce::jlimit (0.0, 1.0, x);
    return x * x * (3.0 - 2.0 * x);
}

/** split_mode's choice order, frozen with the parameter (docs/SPLIT.md §2). */
enum class SplitMode { copy = 0, leftRight = 1, crossover = 2 };

// 2 = the chain carries block instances and a row layout. 3 = the gate, comp, drive, EQ
// and mod engine ids exist. applyStateVar still ignores this number, deliberately: every
// property added since v1 is optional and "absent means default" — or, for the engine
// type ids, absent means Classic (kLegacyTypePins), which keys on the id itself — so an
// older preset loads correctly without ever consulting it. It is here for the first
// change that cannot be expressed that way.
constexpr int kStateVersion = 3;

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

/** The meters' accumulate step: the inputPeak loop, for any reading. */
void publishMax (std::atomic<float>& meter, float value) noexcept
{
    float prev = meter.load (std::memory_order_relaxed);
    while (value > prev && ! meter.compare_exchange_weak (prev, value, std::memory_order_relaxed)) {}
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

/** The state var presets and A/B slots store, the shape applyStateVar reads. */
juce::var stateVar (juce::DynamicObject* paramValues, const chain::Order& order, const std::vector<int>& rows,
                    const juce::String& modelPath, const juce::String& modelPathB, const juce::String& irPath,
                    const juce::String& fxSlotsXml)
{
    const auto serialized = serializeOrder (order);

    auto* state = new juce::DynamicObject();
    state->setProperty ("version", kStateVersion);
    state->setProperty ("params", juce::var (paramValues));
    state->setProperty ("modelPath", modelPath);
    state->setProperty ("modelPathB", modelPathB);
    state->setProperty ("irPath", irPath);
    state->setProperty ("chainOrder", serialized.v1);

    if (serialized.v2.isNotEmpty())
        state->setProperty ("chainOrderV2", serialized.v2);

    state->setProperty ("chainRows", rowsToString (rows));
    state->setProperty ("fxSlots", fxSlotsXml);

    return juce::var (state);
}

/** Every id kLegacyTypePins covers whose block instance is in `order`: the ids a restore
    sets to 0 (Classic) when the restored state does not carry them. */
std::vector<juce::String> legacyPinnedIds (const chain::Order& order)
{
    std::vector<juce::String> ids;

    for (auto id : order)
        for (const auto& pin : kLegacyTypePins)
            if (chain::kindOf (id) == pin.kind)
                ids.emplace_back (pin.ids[chain::instanceOf (id)]);

    return ids;
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

    pp.gateMode      = raw (params::gateMode);
    pp.gateRelease   = raw (params::gateRelease);
    pp.gateHold      = raw (params::gateHold);
    pp.gateRange     = raw (params::gateRange);
    pp.gateScHpf     = raw (params::gateScHpf);

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
        pp.delayType[k]     = raw (params::delayTypeIds[k]);
        pp.delaySync[k]     = raw (params::delaySyncIds[k]);
        pp.delayLowCut[k]   = raw (params::delayLowCutIds[k]);
        pp.delayHighCut[k]  = raw (params::delayHighCutIds[k]);
        pp.delayModAmt[k]   = raw (params::delayModAmtIds[k]);
        pp.delayColor[k]    = raw (params::delayColorIds[k]);
        pp.delayDuck[k]     = raw (params::delayDuckIds[k]);

        pp.reverbOn[k]      = raw (params::reverbOnIds[k]);
        pp.reverbSize[k]    = raw (params::reverbSizeIds[k]);
        pp.reverbDamping[k] = raw (params::reverbDampingIds[k]);
        pp.reverbMix[k]     = raw (params::reverbMixIds[k]);
        pp.reverbWidth[k]   = raw (params::reverbWidthIds[k]);
        pp.reverbType[k]     = raw (params::reverbTypeIds[k]);
        pp.reverbPreDelay[k] = raw (params::reverbPreDelayIds[k]);
        pp.reverbDecay[k]    = raw (params::reverbDecayIds[k]);
        pp.reverbLowCut[k]   = raw (params::reverbLowCutIds[k]);
        pp.reverbHighCut[k]  = raw (params::reverbHighCutIds[k]);
        pp.reverbModAmt[k]   = raw (params::reverbModAmtIds[k]);

        pp.compType[k]       = raw (params::compTypeIds[k]);
        pp.compKnee[k]       = raw (params::compKneeIds[k]);
        pp.compMix[k]        = raw (params::compMixIds[k]);
        pp.compScHpf[k]      = raw (params::compScHpfIds[k]);
        pp.compAutoMakeup[k] = raw (params::compAutoMakeupIds[k]);

        pp.driveType[k]      = raw (params::driveTypeIds[k]);

        pp.eqType[k]         = raw (params::eqTypeIds[k]);
        pp.eqLowCut[k]       = raw (params::eqLowCutIds[k]);
        pp.eqHighCut[k]      = raw (params::eqHighCutIds[k]);
        pp.eqMidFreq[k]      = raw (params::eqMidFreqIds[k]);
        pp.eqMidQ[k]         = raw (params::eqMidQIds[k]);
        pp.eqTilt[k]         = raw (params::eqTiltIds[k]);
        pp.eqLevel[k]        = raw (params::eqLevelIds[k]);
        pp.eqVoice[k]        = raw (params::eqVoiceIds[k]);

        for (int band = 0; band < params::numEqGraphicBands; ++band)
            pp.eqGraphic[k][band] = raw (params::eqGraphicIds[band][k]);

        pp.modChorusMode[k]  = raw (params::modChorusModeIds[k]);
        pp.modPhaserModel[k] = raw (params::modPhaserModelIds[k]);
        pp.modTremVoice[k]   = raw (params::modTremVoiceIds[k]);
        pp.modSync[k]        = raw (params::modSyncIds[k]);
        pp.modWidth[k]       = raw (params::modWidthIds[k]);
        pp.modTone[k]        = raw (params::modToneIds[k]);
        pp.modShape[k]       = raw (params::modShapeIds[k]);
        pp.modWave[k]        = raw (params::modWaveIds[k]);
        pp.modColor[k]       = raw (params::modColorIds[k]);
    }

    pendingSlim.store (valueOf (raw (params::ampSlim)), std::memory_order_relaxed);
    apvts.addParameterListener (params::ampSlim, this);

    for (const auto* id : kLatencyParameterIds)
        apvts.addParameterListener (id, this);

    startTimer (kLatencyTimerMs);

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
    stopTimer();
    apvts.removeParameterListener (params::ampSlim, this);

    for (const auto* id : kLatencyParameterIds)
        apvts.removeParameterListener (id, this);

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
        return;
    }

    // One of kLatencyParameterIds. Host automation calls this on the audio thread, where only an
    // atomic may be touched (asking which thread this is takes a lock in JUCE): the figure the
    // audio thread published before this is out of date, and the timer reports the next one.
    latencyGeneration.fetch_add (1, std::memory_order_acq_rel);
}

void TubampAudioProcessor::timerCallback()
{
    reportLatency();
}

void TubampAudioProcessor::reportLatency()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto callbacks = audioCallbacks.load (std::memory_order_acquire);

    if (callbacks != seenCallbacks)
    {
        seenCallbacks = callbacks;
        lastAudioMs = now;
        audioSeen = true;
    }

    const bool audioRunning = audioSeen && now - lastAudioMs < audioIdleMs.load (std::memory_order_relaxed);
    const auto generation = latencyGeneration.load (std::memory_order_acquire);
    const auto published = audioLatency.load (std::memory_order_acquire);
    const bool current = (std::uint32_t) (published >> 32) == generation;
    int wanted = 0;

    if (current)
    {
        // What the audio thread applied since the last change; with the audio stopped it stands.
        wanted = (int) (std::uint32_t) published;
    }
    else
    {
        // A change the audio thread has not run yet: while it runs, wait for a callback that did;
        // with it stopped, work the figure out here, once per change.
        if (audioRunning || idleGeneration == generation)
            return;

        idleGeneration = generation;

        chain::Order orderCopy;

        {
            const juce::ScopedLock sl (chainLock);
            orderCopy = uiChainOrder;
        }

        wanted = computeWantedLatency (orderCopy.data(), (int) orderCopy.size(), isOn (pp.ampOn), isOn (pp.amp2On));
    }

    if (wanted != getLatencySamples())
        setLatencySamples (wanted);
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

    preparedSampleRate.store (sampleRate, std::memory_order_relaxed);

    // No callback for two blocks and a little: the audio has stopped, and the message thread
    // works the latency out itself. Bounded for blocks of 1 sample and of many thousands.
    audioIdleMs.store ((std::uint32_t) juce::jlimit (40.0, 250.0, 2000.0 * juce::jmax (1, samplesPerBlock) / sampleRate + 15.0),
                       std::memory_order_relaxed);

    // Every instance is prepared whether or not it is in the current order: a block the
    // user drops onto the board has to run in the same block, and preparing one there
    // would allocate on the audio thread.
    gate.prepare (spec);

    for (int k = 0; k < params::maxInstances; ++k)
    {
        comp[(size_t) k].prepare (spec);
        drive[(size_t) k].prepare (spec);
        eq[(size_t) k].prepare (spec);
        modulation[(size_t) k].prepare (spec);
        delay[(size_t) k].prepare (spec);
        reverbFx[(size_t) k].prepare (spec);
    }

    cab.prepare (spec);
    ampEq.prepare ({ spec.sampleRate, spec.maximumBlockSize, 1 });
    dcBlocker.prepare (spec);

    gateFader.prepare (spec);

    for (int k = 0; k < params::maxInstances; ++k)
        for (auto* fader : { &compFader[(size_t) k], &driveFader[(size_t) k], &eqFader[(size_t) k], &modFader[(size_t) k] })
            fader->prepare (spec);

    // Every engine was just prepared, so none holds anything from before.
    outputSounding = false;
    engineRan.fill (false);
    engineRunsNow.fill (false);
    engineStale.fill (false);

    for (auto& timeline : driveTimeline)
        timeline.prepare (sampleRate, juce::jmax (1, samplesPerBlock));

    driveDelays.setSize (2 * params::maxInstances, juce::jmax (1, samplesPerBlock), false, false, true);
    driveDelays.clear();
    driveReported.fill (0);

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
    laneRingPrimed = false;
    laneDelta.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0);
    laneSlideLength = juce::jmax (1, (int) std::lround (kLaneTapFadeSeconds * sampleRate));
    laneSlideDone = laneFadeDone = laneSlideLength;
    laneLast = laneOffset = laneOffsetStart = 0.0;
    laneFromCount = 0;

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

        for (int index = 0; index < chain::numBlockTypes; ++index)
            lastSignature[(size_t) index] = blockSignature ((chain::BlockId) index);
    }

    updateLatency();
}

void TubampAudioProcessor::releaseResources()
{
    gate.reset();

    for (int k = 0; k < params::maxInstances; ++k)
    {
        comp[(size_t) k].reset();
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

    // A non-finite sample from the host would live on in every recursive state it reached (the
    // always-on DC blocker first) and leave the plugin silent or non-finite until the next
    // prepare, and one near FLT_MAX overflows to infinity in the input trim: either becomes
    // silence here, before anything reads it, on every channel and so on every path. Input
    // below kAbsurdSample passes untouched.
    for (int ch = 0; ch < juce::jmin (buffer.getNumChannels(), numOutputChannels); ++ch)
        zeroAbsurd (buffer.getWritePointer (ch), numSamples);

    // Read before the order is decoded: a figure published with this generation was computed
    // from an order at least as new as the message thread's edit that set it.
    const auto generation = latencyGeneration.load (std::memory_order_acquire);

    // Adopt a staged model before anything reads it, on both engines and whatever the
    // chain looks like: a swap staged into B must never sit there going stale because
    // the amp2 block happens to be out of the chain this block. The latency report moves
    // below, once the decoded order says which blocks are actually in the path.
    namEngine.applyStaging();
    namEngineB.applyStaging();
    fxHost.applyStaging();

    auto* const playHead = getPlayHead();

    // One transport reading for the whole callback, shared by every delay and mod instance.
    HostTransport transport;

    if (playHead != nullptr)
        if (const auto position = playHead->getPosition())
        {
            if (const auto bpm = position->getBpm(); bpm.hasValue() && *bpm > 0.0)
            {
                transport.bpm = *bpm;

                if (std::isfinite (*bpm))
                    transport.hostBpm = *bpm;
            }

            if (const auto ppq = position->getPpqPosition(); ppq.hasValue() && position->getIsPlaying())
            {
                transport.ppqPosition = *ppq;
                transport.playing = true;
            }
        }

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

    // A whole state applied (host restore, preset, A/B) while nothing is heard (a session loading,
    // an offline render, a host running silence) takes effect at once, as the old build did: a
    // block entering the chain is reset here, however many there are (no drain, no dry pass); one
    // whose type the restore changed is reset onto it rather than crossfading; and every fader snaps
    // to the block's restored on or off, with no settle and no fade. So such a render does not
    // depend on whether the host prepared or restored first. Under sound a reset or a snap would cut
    // it in one sample, so a restore then changes blocks as the same change by hand does.
    const bool restored = stateRestoring.load (std::memory_order_acquire)
                          || stateRestored.exchange (false, std::memory_order_acq_rel);

    if (restored && ! outputSounding)
    {
        for (int index = 0; index < chain::numBlockTypes; ++index)
        {
            const auto id = (chain::BlockId) index;
            const bool retyped = present[(size_t) index] && ! pendingReset[(size_t) index]
                                 && blockSignature (id) != lastSignature[(size_t) index];

            if (pendingReset[(size_t) index] || retyped)
                resetBlockInstance (id, true);

            pendingReset[(size_t) index] = false;
        }

        gateFader.snapNext();

        for (int k = 0; k < params::maxInstances; ++k)
            for (auto* fader : { &compFader[(size_t) k], &driveFader[(size_t) k], &eqFader[(size_t) k], &modFader[(size_t) k] })
                fader->snapNext();
    }

    for (int index = 0, drained = 0; index < chain::numBlockTypes; ++index)
    {
        if (present[(size_t) index])
            lastSignature[(size_t) index] = blockSignature ((chain::BlockId) index);

        if (! pendingReset[(size_t) index] || drained == kMaxResetsPerCallback)
            continue;

        resetBlockInstance ((chain::BlockId) index, false);
        pendingReset[(size_t) index] = false;
        ++drained;
    }

    // Gate split: the trigger always measures at the gate's own position. The reduction
    // is deferred onto the model's mono output only when the amp runs later in the SAME
    // segment; across a split there is no shared post-model point to gate at (the other
    // lane never saw the model), so it is applied right where the gate sits. Without a
    // split every entry is in segment 0 and this is exactly the old rule. A gate whose
    // re-entry reset is still pending runs as bypassed, like every other block.
    const bool gateRuns = gateOn && ! pendingReset[(size_t) chain::BlockId::gate];

    // The gate fades its gain in and out, Classic too; it measures, and its curve lands, until a
    // fade out is over.
    gateFading = present[(size_t) chain::BlockId::gate];
    gateFadeOn = gateRuns;
    const bool gateEngine = gateFading ? gateFader.needsEngine (gateRuns) : gateRuns;
    const bool deferGateToAmp = gateEngine && ampOn && gateIndex >= 0 && ampIndex > gateIndex
                                && segmentOf (gateIndex) == segmentOf (ampIndex);

    // A deferred curve meets audio that is late by everything from the gate to the amp's
    // output, the amp's own resampler included; Tight and Smooth wait for it (Classic
    // ignores the delay, FX-ENGINES.md D-3). The span lies inside one segment, so this is
    // its share of the latency the host is told about, by the same per-entry rule.
    {
        int curveDelay = 0;

        if (deferGateToAmp)
            for (int i = gateIndex + 1; i <= ampIndex; ++i)
                curveDelay += blockLatency (order[(size_t) i], ampOn, amp2On, pendingReset.data(), true);

        gate.setCurveDelay (juce::jmin (curveDelay, GateFx::kMaxCurveDelaySamples));
    }

    if (! present[(size_t) chain::BlockId::gate] || ! gateRuns)
        gateState.store (-1, std::memory_order_relaxed);

    // Blocks that are not in the chain at all keep their gain smoothers in step
    // exactly like a bypassed block, so putting them back never jumps.
    for (int k = 0; k < params::maxInstances; ++k)
    {
        if (! present[(size_t) chain::instanceId (chain::BlockId::comp, k)])
        {
            comp[(size_t) k].setParameters (compParams (k));
            comp[(size_t) k].skip (numSamples);
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
                           monoScratchData, gateEngine, deferGateToAmp, ampOn, amp2On, playHead,
                           transport);
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

        // The shorter lane waits for the longer one, sample by sample, so the two line up
        // where they meet and the single figure reported to the host is true for both.
        computeLaneDelta (order.data(), structure, numSamples, ampOn, amp2On);
        compensateLaneLatency (buffer, laneBuffer, numOutputChannels, numSamples);

        mixLanes (buffer, laneBuffer, numOutputChannels, numSamples);

        for (int i = structure.mixAt + 1; i < numBlocks; ++i)
            runEntry (i, buffer, block);
    }

    // Whatever the rings hold belongs to a run of consecutive blocks that ran the lanes;
    // any block that did not (no split in the order, a scratch too small) ends that run.
    laneRingPrimed = lanes;

    // An engine that ran last callback and not this one now holds audio from the past.
    for (size_t index = 0; index < engineRan.size(); ++index)
        if (engineRan[index] && ! engineRunsNow[index])
            engineStale[index] = true;

    engineRan = engineRunsNow;
    engineRunsNow.fill (false);

    // What the chain applied this callback, counted after it ran (a drive's figure is what it
    // applies): published for the message thread, which alone reports it (reportLatency).
    {
        const auto chainLatency = computeChainLatency (order.data(), numBlocks, ampOn, amp2On, lanes,
                                                       pendingReset.data(), true);
        audioLatency.store (((std::uint64_t) generation << 32) | (std::uint32_t) chainLatency.reported(),
                            std::memory_order_release);
        audioCallbacks.fetch_add (1, std::memory_order_release);
    }

    // A non-finite or absurd value made inside the chain (a hosted plugin, the neural model, an
    // engine pushed past float range) must neither reach the host nor live on in the DC blocker:
    // it becomes silence here, and the next callback starts clean. Anything below kAbsurdSample
    // (+120 dBFS, far above a full-scale input through every gain stage) passes untouched.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        zeroAbsurd (buffer.getWritePointer (ch), numSamples);

    // --- DC blocker: always on, never user-facing (spec §4 step 7)
    dcBlocker.process (block);

    // --- output level
    outputLevelLin.setTargetValue (juce::Decibels::decibelsToGain (valueOf (pp.outputLevel)));
    applySmoothedGain (buffer, numSamples, outputLevelLin);

    {
        const float mag = buffer.getMagnitude (0, numSamples);
        float prev = outputPeak.load (std::memory_order_relaxed);
        while (mag > prev && ! outputPeak.compare_exchange_weak (prev, mag, std::memory_order_relaxed)) {}
        outputSounding = mag > 0.0f;
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

GateParams TubampAudioProcessor::gateParams() const noexcept
{
    GateParams gp;
    gp.mode        = (int) valueOf (pp.gateMode, (float) GateFx::Mode::tight);
    gp.thresholdDb = valueOf (pp.gateThreshold, -80.0f);
    gp.releaseMs   = valueOf (pp.gateRelease, kGateReleaseDefault[(int) GateFx::Mode::tight]);
    gp.holdMs      = valueOf (pp.gateHold, kGateHoldDefault[(int) GateFx::Mode::tight]);
    gp.rangeDb     = valueOf (pp.gateRange, -90.0f);
    gp.scHpfHz     = valueOf (pp.gateScHpf, kGateScHpfDefault[(int) GateFx::Mode::tight]);
    return gp;
}

DriveParams TubampAudioProcessor::driveParams (int instance) const noexcept
{
    const auto k = (size_t) instance;
    DriveParams dp;
    dp.type    = (int) valueOf (pp.driveType[k], (float) DriveFx::Type::screamer);
    dp.gainDb  = valueOf (pp.driveGain[k], kDriveGainDefault[(int) DriveFx::Type::screamer]);
    dp.toneHz  = valueOf (pp.driveTone[k], kDriveToneDefault[(int) DriveFx::Type::screamer]);
    dp.levelDb = valueOf (pp.driveLevel[k], kDriveLevelDefault[(int) DriveFx::Type::screamer]);
    return dp;
}

EqParams TubampAudioProcessor::eqParams (int instance) const noexcept
{
    const auto k = (size_t) instance;
    EqParams ep;
    ep.type      = (int) valueOf (pp.eqType[k], (float) EqFx::Type::parametric);
    ep.bass      = valueOf (pp.eqBass[k], 5.0f);
    ep.mid       = valueOf (pp.eqMid[k], 5.0f);
    ep.treble    = valueOf (pp.eqTreble[k], 5.0f);
    ep.lowCutHz  = valueOf (pp.eqLowCut[k], 20.0f);
    ep.highCutHz = valueOf (pp.eqHighCut[k], 20000.0f);
    ep.midFreqHz = valueOf (pp.eqMidFreq[k], 425.0f);
    ep.midQ      = valueOf (pp.eqMidQ[k], 0.7f);
    ep.tiltDb    = valueOf (pp.eqTilt[k]);
    ep.levelDb   = valueOf (pp.eqLevel[k]);
    ep.voice     = (int) valueOf (pp.eqVoice[k], (float) EqFx::Voice::british);

    for (int band = 0; band < params::numEqGraphicBands; ++band)
        ep.graphicDb[band] = valueOf (pp.eqGraphic[k][band]);

    return ep;
}

CompParams TubampAudioProcessor::compParams (int instance) const noexcept
{
    const auto k = (size_t) instance;
    CompParams cp;
    cp.type        = (int) valueOf (pp.compType[k], (float) CompFx::Type::clean);
    cp.thresholdDb = valueOf (pp.compThreshold[k], -20.0f);
    cp.ratio       = valueOf (pp.compRatio[k], 4.0f);
    cp.attackMs    = valueOf (pp.compAttack[k], 5.0f);
    cp.releaseMs   = valueOf (pp.compRelease[k], 120.0f);
    cp.makeupDb    = valueOf (pp.compMakeup[k]);
    cp.kneeDb      = valueOf (pp.compKnee[k], kCompKneeDefault[(int) CompFx::Type::clean]);
    cp.mixPct      = valueOf (pp.compMix[k], 100.0f);
    cp.scHpfHz     = valueOf (pp.compScHpf[k], 20.0f);
    cp.autoMakeup  = isOn (pp.compAutoMakeup[k]);
    return cp;
}

ModParams TubampAudioProcessor::modParams (int instance, const HostTransport& transport) const noexcept
{
    const auto k = (size_t) instance;
    ModParams mp;
    mp.type        = (int) valueOf (pp.modType[k]);
    mp.chorusMode  = (int) valueOf (pp.modChorusMode[k], (float) ChorusFx::Mode::wide);
    mp.phaserModel = (int) valueOf (pp.modPhaserModel[k], (float) PhaserFx::Model::phase90);
    mp.tremVoice   = (int) valueOf (pp.modTremVoice[k], (float) TremoloFx::Voice::smooth);
    mp.rateHz      = valueOf (pp.modRate[k], 1.0f);
    mp.depth       = valueOf (pp.modDepth[k], 0.4f);
    mp.mix         = valueOf (pp.modMix[k], 0.35f);
    mp.sync        = (int) valueOf (pp.modSync[k]);
    mp.width       = valueOf (pp.modWidth[k], 1.0f);
    mp.tone        = valueOf (pp.modTone[k], 0.5f);
    mp.shape       = valueOf (pp.modShape[k], 0.5f);
    mp.wave        = valueOf (pp.modWave[k]);
    mp.color       = valueOf (pp.modColor[k], 1.0f) > 0.5f;
    mp.bpm         = transport.hostBpm;
    mp.ppqPosition = transport.ppqPosition;
    mp.playing     = transport.playing;
    return mp;
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
        applyGate (juce::dsp::AudioBlock<float> (&mono, 1, (size_t) numSamples), numSamples, true);

    // The amp's own tone stack, on the mono path: model -> gate gain -> tone stack is
    // the reference plugin's order (docs/research/nam-plugin-params.md §4), and running
    // it before the stereo expansion is what makes it part of the amp rather than a
    // second EQ block after it. Deliberate — do not move it later. amp2 has none: it is
    // the same NAM core without the amp block's tone controls (docs/SPLIT.md §3), and a
    // user who wants them adds an eq instance behind it.
    if (useToneStack && isOn (pp.ampEqOn))
    {
        EqParams ep;
        ep.type   = (int) EqFx::Type::classic;
        ep.bass   = valueOf (pp.ampEqBass, 5.0f);
        ep.mid    = valueOf (pp.ampEqMid, 5.0f);
        ep.treble = valueOf (pp.ampEqTreble, 5.0f);

        ampEq.setParameters (ep);
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
                                              juce::AudioPlayHead* playHead,
                                              const HostTransport& transport) noexcept
{
    const auto k = (size_t) chain::instanceOf (id);

    switch (id)
    {
        // --- gate trigger (analysis only when the reduction lands after the model). `gateOn`
        // is whether the gate's engine runs: on, or fading out.
        case chain::BlockId::gate:
        {
            if (! gateOn)
            {
                // Resting off: the fader still keeps its input history and its rest state.
                if (gateFading)
                    gateFader.process (block, false, 0, [] (juce::dsp::AudioBlock<float>) {});
            }
            else
            {
                if (engineStarts (id))
                {
                    gate.reset();
                    gateFader.restart();
                }

                gate.setParameters (gateParams());
                gate.measure (block, numSamples);

                if (! deferGateToAmp)
                    applyGate (block, numSamples, false);

                if (gateFadeOn)
                {
                    publishMax (gateGrDb, gate.getGainReductionDb());
                    publishMax (gateLevelDb, gate.getDetectorLevelDb());
                    gateState.store (gate.getState(), std::memory_order_relaxed);
                }
            }

            break;
        }

        // --- compressor (+ makeup); a bypassed one still steps its makeup smoother
        case chain::BlockId::comp:
        case chain::BlockId::comp2:
        case chain::BlockId::comp3:
        {
            const auto cp = compParams ((int) k);
            const bool runs = isOn (pp.compOn[k]) && ! pendingReset[(size_t) id];

            if (compFader[k].needsEngine (runs))
            {
                if (engineStarts (id))
                {
                    comp[k].reset();
                    compFader[k].restart();
                }

                comp[k].setParameters (cp);
                compFader[k].process (block, runs, 0, [this, k] (juce::dsp::AudioBlock<float> sub) { comp[k].process (sub); });

                if (runs)
                    publishMax (compGrDb[k], comp[k].getGainReductionDb());
            }
            else
            {
                comp[k].setParameters (cp);
                compFader[k].process (block, false, 0, [] (juce::dsp::AudioBlock<float>) {});
                comp[k].skip (numSamples);
            }

            break;
        }

        // --- drive. A non-Classic drive holds its input back by its latency while it is off
        // or waiting, which is why its latency stays counted then (blockLatency). A Classic one
        // delays nothing while off and 3 samples while it runs: its input path slides between
        // the two while the engine settles unheard (on) and after its fade (off), so the fade
        // itself always meets the engine in line.
        case chain::BlockId::drive:
        case chain::BlockId::drive2:
        case chain::BlockId::drive3:
        {
            const auto dp = driveParams ((int) k);
            const bool runs = isOn (pp.driveOn[k]) && ! pendingReset[(size_t) id];
            const int type = juce::jlimit (0, DriveFx::numTypes - 1, dp.type);
            const bool classic = type == (int) DriveFx::Type::classic;
            const bool engine = driveFader[k].needsEngine (runs);
            const double rate = preparedSampleRate.load (std::memory_order_relaxed);

            if (engine)
            {
                if (engineStarts (id))
                {
                    drive[k].reset();
                    driveTimeline[k].reset();
                    driveFader[k].restart();
                }

                drive[k].setParameters (dp);
            }

            // Too big a block for the delay rows (larger than prepareToPlay promised: the lanes run
            // flat then, so nothing reads them): the timeline still follows the engine.
            const bool tracked = numSamples <= driveDelays.getNumSamples();
            float* const engineDelay = tracked ? driveDelays.getWritePointer (2 * (int) k) : nullptr;
            float* const applied = tracked ? driveDelays.getWritePointer (2 * (int) k + 1) : nullptr;
            const float* const blockStart = block.getChannelPointer (0);

            driveFader[k].process (block, runs, classic ? 0.0 : (double) DriveFx::latencyFor (type, rate), engineDelay,
                                   [this, k, type, engineDelay, blockStart] (juce::dsp::AudioBlock<float> sub)
                                   {
                                       const auto offset = sub.getChannelPointer (0) - blockStart;
                                       driveTimeline[k].run (type, (int) sub.getNumSamples(),
                                                             engineDelay != nullptr ? engineDelay + offset : nullptr);
                                       drive[k].process (sub);
                                   },
                                   applied);

            // The figure counted for it: what it applies, a switch or a slide taken at its larger
            // end; a Classic drive by the old rule, which knows only drive_on (D-7).
            int reported = classic ? (runs ? DriveFx::latencyFor (type, rate) : 0) : DriveFx::latencyFor (type, rate);

            if (engine && driveTimeline[k].switching())
                reported = juce::jmax (reported, driveTimeline[k].reach());

            if (! classic)
                reported = juce::jmax (reported, (int) std::ceil (driveFader[k].delayReach() - 1.0e-6));

            driveReported[k] = reported;
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

        // --- EQ
        case chain::BlockId::eq:
        case chain::BlockId::eq2:
        case chain::BlockId::eq3:
        {
            const auto ep = eqParams ((int) k);
            const bool runs = isOn (pp.eqOn[k]) && ! pendingReset[(size_t) id];

            if (eqFader[k].needsEngine (runs))
            {
                if (engineStarts (id))
                {
                    eq[k].reset();
                    eqFader[k].restart();
                }

                eq[k].setParameters (ep);
            }

            eqFader[k].process (block, runs, 0, [this, k] (juce::dsp::AudioBlock<float> sub) { eq[k].process (sub); });
            break;
        }

        // --- modulation
        case chain::BlockId::mod:
        case chain::BlockId::mod2:
        case chain::BlockId::mod3:
        {
            // Voicing and colour switches go straight to the running engine, which fades them
            // itself: nothing here resets or fades for them.
            const auto mp = modParams ((int) k, transport);
            const bool runs = isOn (pp.modOn[k]) && ! pendingReset[(size_t) id];

            if (modFader[k].needsEngine (runs))
            {
                if (engineStarts (id))
                {
                    modulation[k].reset();
                    modFader[k].restart();
                }

                modulation[k].setParameters (mp);
            }

            modFader[k].process (block, runs, 0, [this, k] (juce::dsp::AudioBlock<float> sub) { modulation[k].process (sub); });
            break;
        }

        // --- delay
        case chain::BlockId::delay:
        case chain::BlockId::delay2:
        case chain::BlockId::delay3:
        {
            if (isOn (pp.delayOn[k]) && ! pendingReset[(size_t) id])
            {
                DelayParams dp;
                dp.timeMs    = valueOf (pp.delayTime[k], 420.0f);
                dp.feedback  = valueOf (pp.delayFeedback[k], 0.35f);
                dp.mix       = valueOf (pp.delayMix[k], 0.25f);
                dp.mode      = (int) valueOf (pp.delayMode[k]);
                dp.ratioPct  = valueOf (pp.delayRatio[k], 100.0f);
                dp.width     = valueOf (pp.delayWidth[k], 1.0f);
                dp.type      = (int) valueOf (pp.delayType[k], (float) DelayFx::Type::digital);
                dp.sync      = (int) valueOf (pp.delaySync[k]);
                dp.bpm       = transport.bpm;
                dp.lowCutHz  = valueOf (pp.delayLowCut[k], 80.0f);
                dp.highCutHz = valueOf (pp.delayHighCut[k], kDelayHighCutDefault[(int) DelayFx::Type::digital]);
                dp.mod       = valueOf (pp.delayModAmt[k], 0.2f);
                dp.color     = valueOf (pp.delayColor[k], kDelayColorDefault[(int) DelayFx::Type::digital]);
                dp.duck      = valueOf (pp.delayDuck[k], kDelayDuckDefault[(int) DelayFx::Type::digital]);

                delay[k].setParameters (dp);
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
                ReverbParams rp;
                rp.type         = (int) valueOf (pp.reverbType[k], (float) ReverbFx::Type::plate);
                rp.size         = valueOf (pp.reverbSize[k], 0.5f);
                rp.damping      = valueOf (pp.reverbDamping[k], 0.5f);
                rp.mix          = valueOf (pp.reverbMix[k], 0.25f);
                rp.width        = valueOf (pp.reverbWidth[k], 1.0f);
                rp.preDelayMs   = valueOf (pp.reverbPreDelay[k], 10.0f);
                rp.decaySeconds = valueOf (pp.reverbDecay[k], kReverbDecayDefault[(int) ReverbFx::Type::plate]);
                rp.lowCutHz     = valueOf (pp.reverbLowCut[k], 120.0f);
                rp.highCutHz    = valueOf (pp.reverbHighCut[k], 8000.0f);
                rp.mod          = valueOf (pp.reverbModAmt[k], 0.35f);

                reverbFx[k].setParameters (rp);
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

void TubampAudioProcessor::applyGate (juce::dsp::AudioBlock<float> block, int numSamples, bool deferred) noexcept
{
    const auto apply = [this, deferred] (juce::dsp::AudioBlock<float> sub)
    {
        if (deferred)
            gate.apply (sub.getChannelPointer (0), (int) sub.getNumSamples());
        else
            gate.apply (sub, (int) sub.getNumSamples());
    };

    if (gateFading)
        gateFader.process (block, gateFadeOn, 0, apply);
    else if (deferred)
        gate.apply (block.getChannelPointer (0), numSamples);
    else
        gate.apply (block, numSamples);
}

bool TubampAudioProcessor::engineStarts (chain::BlockId id) noexcept
{
    const auto index = (size_t) id;
    const bool startsFresh = engineStale[index];

    engineRunsNow[index] = true;
    engineStale[index] = false;
    return startsFresh;
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

void TubampAudioProcessor::computeLaneDelta (const chain::BlockId* order, const chain::Structure& structure,
                                             int numSamples, bool ampOn, bool amp2On) noexcept
{
    const int n = juce::jmin (numSamples, (int) laneDelta.size());
    int fixed = 0;

    for (int i = structure.splitAt + 1; i < structure.mixAt; ++i)
        if (i != structure.lane2At && chain::kindOf (order[i]) != chain::BlockId::drive)
            fixed += (i < structure.lane2At ? 1 : -1) * blockLatency (order[i], ampOn, amp2On, pendingReset.data(), true);

    std::fill (laneDelta.begin(), laneDelta.begin() + n, (double) fixed);

    for (int i = structure.splitAt + 1; i < structure.mixAt; ++i)
    {
        if (i == structure.lane2At || chain::kindOf (order[i]) != chain::BlockId::drive)
            continue;

        const float* applied = driveDelays.getReadPointer (2 * chain::instanceOf (order[i]) + 1);
        const double sign = i < structure.lane2At ? 1.0 : -1.0;

        for (int s = 0; s < n; ++s)
            laneDelta[(size_t) s] += sign * applied[s];
    }
}

void TubampAudioProcessor::compensateLaneLatency (juce::AudioBuffer<float>& laneA,
                                                  juce::AudioBuffer<float>& laneB,
                                                  int numChannels, int numSamples) noexcept
{
    const int capacity = laneRing[0].getNumSamples();

    if (numChannels < 1 || laneRing[1].getNumSamples() != capacity || capacity < 8
        || numSamples > (int) laneDelta.size())
        return;

    // The ring is sized for realistic hosted-plugin lookahead (kLaneLatencyRingSamples),
    // and ChainLatency::reported() caps what the host is told at the same figure — so a
    // pairing that lands here is one neither the ring nor the report can align, and the
    // clamp keeps it quiet rather than out of bounds.
    const double limit = (double) (capacity - 4);

    if (! laneRingPrimed)
    {
        // Stale content: the run of lane blocks was interrupted. Cheap, and only on that edge.
        laneRing[0].clear();
        laneRing[1].clear();
        laneRingWrite = 0;
        laneLast = juce::jlimit (-limit, limit, laneDelta[0]);
        laneOffset = laneOffsetStart = 0.0;
        laneSlideDone = laneFadeDone = laneSlideLength;
        laneFromCount = 0;
    }

    juce::AudioBuffer<float>* const lanes[2] { &laneA, &laneB };
    const int channels = juce::jmin (numChannels, laneRing[0].getNumChannels());
    const double length = (double) laneSlideLength;

    // Lane 0 (A) waits for a negative difference, lane 1 (B) for a positive one.
    const auto delayOf = [] (int lane, double position) { return juce::jmax (0.0, lane == 0 ? -position : position); };

    const auto read = [&] (const float* ring, int write, double d) noexcept
    {
        const auto at = [&] (int back) { int index = write - back; if (index < 0) index += capacity; return ring[index]; };
        const int whole = (int) d;

        if (juce::exactlyEqual ((double) whole, d))
            return at (whole);

        // Taps whole - 1 ... whole + 2, shifted up where whole - 1 would be in the future.
        const int b = juce::jmax (0, whole - 1);
        const double x = d - b;
        const double w0 = -(x - 1.0) * (x - 2.0) * (x - 3.0) * (1.0 / 6.0);
        const double w1 = x * (x - 2.0) * (x - 3.0) * 0.5;
        const double w2 = -x * (x - 1.0) * (x - 3.0) * 0.5;
        const double w3 = x * (x - 1.0) * (x - 2.0) * (1.0 / 6.0);
        return (float) (w0 * at (b) + w1 * at (b + 1) + w2 * at (b + 2) + w3 * at (b + 3));
    };

    int write = laneRingWrite;

    for (int n = 0; n < numSamples; ++n)
    {
        const double target = juce::jlimit (-limit, limit, laneDelta[(size_t) n]);
        const double jump = target - laneLast;

        if (std::abs (jump) > DelayGlide::kJumpSamples)
        {
            const double heard = laneLast + laneOffset;

            if (std::abs (jump) <= kLaneSlideSamples)
            {
                // A small jump slides out from where the taps are now (a second one retargets).
                laneOffset = laneOffsetStart = heard - target;
                laneSlideDone = 0;
            }
            else
            {
                // A large one crossfades from what is heard now. Inside a crossfade that is
                // three taps; the weakest is dropped (two large jumps within 10 ms).
                std::array<double, 3> pos { heard, 0.0, 0.0 }, weight { 1.0, 0.0, 0.0 };
                int count = 1;

                if (laneFadeDone < laneSlideLength)
                {
                    const double h = smoothstep (laneFadeDone / length);
                    weight[0] = h;

                    for (int j = 0; j < laneFromCount; ++j, ++count)
                    {
                        pos[(size_t) count] = laneFromPos[(size_t) j];
                        weight[(size_t) count] = (1.0 - h) * laneFromWeight[(size_t) j];
                    }
                }

                if (count == 3)
                {
                    const auto weakest = (size_t) std::distance (weight.begin(), std::min_element (weight.begin(), weight.end()));
                    pos[weakest] = pos[2];
                    weight[weakest] = weight[2];
                    count = 2;
                }

                const double total = juce::jmax (1.0e-12, weight[0] + (count > 1 ? weight[1] : 0.0));
                laneFromCount = count;

                for (int j = 0; j < count; ++j)
                {
                    laneFromPos[(size_t) j] = pos[(size_t) j];
                    laneFromWeight[(size_t) j] = weight[(size_t) j] / total;
                }

                laneOffset = laneOffsetStart = 0.0;
                laneSlideDone = laneSlideLength;
                laneFadeDone = 0;
            }
        }

        laneLast = target;

        if (laneSlideDone < laneSlideLength)
            laneOffset = laneOffsetStart * (1.0 - smoothstep (++laneSlideDone / length));

        const bool fading = laneFadeDone < laneSlideLength;
        const double h = fading ? smoothstep (++laneFadeDone / length) : 1.0;
        const double position = target + laneOffset;

        for (int lane = 0; lane < 2; ++lane)
        {
            const double d = delayOf (lane, position);

            for (int ch = 0; ch < channels; ++ch)
            {
                auto* ring = laneRing[(size_t) lane].getWritePointer (ch);
                auto& sample = lanes[lane]->getWritePointer (ch)[n];

                // Written before it is read, so the read (at least one sample behind, or this
                // very sample at no delay) never sees a sample from a ring's length ago — which
                // is what makes a block longer than the ring harmless. A lane that waits for
                // nothing is still written: its history is what a later delay reads.
                ring[write] = sample;

                if (! fading)
                {
                    if (d != 0.0)
                        sample = read (ring, write, d);

                    continue;
                }

                double mixed = h * read (ring, write, d);

                for (int j = 0; j < laneFromCount; ++j)
                    mixed += (1.0 - h) * laneFromWeight[(size_t) j] * read (ring, write, delayOf (lane, laneFromPos[(size_t) j]));

                sample = (float) mixed;
            }
        }

        if (++write == capacity)
            write = 0;
    }

    laneRingWrite = write;
}

TubampAudioProcessor::ChainLatency
TubampAudioProcessor::computeChainLatency (const chain::BlockId* order, int numBlocks,
                                           bool ampOn, bool amp2On,
                                           bool lanesRunning, const bool* waiting,
                                           bool audioThread) const noexcept
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
        const int latency = blockLatency (order[i], ampOn, amp2On, waiting, audioThread);

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

int TubampAudioProcessor::blockLatency (chain::BlockId id, bool ampOn, bool amp2On,
                                        const bool* waiting, bool audioThread) const noexcept
{
    // A resampler's latency only reaches the signal when its block actually runs:
    // in the published order AND enabled. Otherwise it contributes zero, so hosts
    // don't compensate for a delay that never happens. amp2 also needs a LIVE model
    // — the same test its DSP path makes.
    if (id == chain::BlockId::amp)
        return ampOn ? namEngine.getLatencySamples() : 0;

    if (id == chain::BlockId::amp2)
        return (amp2On && namEngineB.hasLiveModel()) ? namEngineB.getLatencySamples() : 0;

    // The drive's oversampling delay. latencyFor is a pure function of type and rate: this
    // runs on the message thread too. A non-Classic drive counts it whenever it is in the
    // chain, the FX-slot rule: bypassed or waiting for its reset it holds its input back by the
    // same amount (BypassFader), so switching it crossfades without moving the audio in time.
    // Classic keeps the amp's rule (FX-ENGINES.md D-7) — delaying a bypassed Classic drive
    // would change old sessions — and counts only while it runs. The audio thread counts what
    // the drive applies instead (driveReported): during a type switch, or a slide of its delay,
    // the larger of the two figures until it is over.
    if (chain::kindOf (id) == chain::BlockId::drive)
    {
        const auto k = (size_t) chain::instanceOf (id);

        if (audioThread)
            return driveReported[k];

        const int type = juce::jlimit (0, DriveFx::numTypes - 1, (int) valueOf (pp.driveType[k]));
        const int latency = DriveFx::latencyFor (type, preparedSampleRate.load (std::memory_order_relaxed));

        if (type != (int) DriveFx::Type::classic)
            return latency;

        return isOn (pp.driveOn[k]) && (waiting == nullptr || ! waiting[(size_t) id]) ? latency : 0;
    }

    // A loaded FX slot contributes its plugin's latency whether or not the slot is
    // bypassed: fxN_on is a normal automatable parameter, and letting an automation
    // lane move the reported latency mid-playback would shift the track against every
    // other one in the project (hosts only re-apply delay compensation at transport
    // boundaries). FxHost holds a bypassed slot at the same latency instead.
    if (chain::isFxSlot (id))
        return fxHost.latencyFor (chain::fxSlotIndex (id));

    return 0;
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
    // Whatever the audio thread published before this edit is now out of date; the timer reports
    // the new figure at its next tick (from the audio thread's first callback with the edit, or
    // worked out on the message thread when no audio runs), whichever thread the edit came from.
    latencyGeneration.fetch_add (1, std::memory_order_acq_rel);
}

void TubampAudioProcessor::DriveTimeline::prepare (double rate, int maximumBlockSize) noexcept
{
    sampleRate = rate;
    maxBlock = juce::jmax (1, maximumBlockSize);
    // Drive.cpp's kSwitchSettleSeconds, kSwitchFadeSeconds, kSwitchSlideSeconds, as it rounds them.
    const auto samples = [rate] (double seconds) { return juce::jmax (8, (int) std::lround (seconds * rate)); };
    settle = samples (0.010);
    fade = samples (0.020);
    slide = samples (0.010);
    reset();
}

void TubampAudioProcessor::DriveTimeline::run (int type, int numSamples, float* out) noexcept
{
    type = juce::jlimit (0, DriveFx::numTypes - 1, type);

    for (int start = 0; start < numSamples; start += maxBlock)
    {
        const int piece = juce::jmin (maxBlock, numSamples - start);

        if (snap)
        {
            snap = false;
            active = type;
            phase = Phase::steady;
        }

        // A type asked for while a switch runs waits for it to finish (DriveFx::process).
        if (phase == Phase::steady && type != active)
        {
            const int latencyOut = latencyOf (active), latencyIn = latencyOf (type);
            common = juce::jmax (latencyOut, latencyIn);
            extraOut = common - latencyOut;
            extraIn = common - latencyIn;
            phase = Phase::crossfade;
            position = 0;
            active = type;   // the incoming type; latencyOut is common - extraOut from here on
        }

        for (int offset = 0; offset < piece;)
        {
            float* const o = out != nullptr ? out + start + offset : nullptr;

            if (phase == Phase::steady)
            {
                if (o != nullptr)
                    std::fill (o, o + (piece - offset), (float) latencyOf (active));

                break;
            }

            const bool crossfading = phase == Phase::crossfade;
            const int left = crossfading ? settle + fade - position : slide - position;
            const int length = juce::jmin (piece - offset, left);

            for (int i = 0; o != nullptr && i < length; ++i)
            {
                const int pos = position + i;

                // Outgoing alone (its rise slid in from 4 samples on) until the fade starts; then
                // both at the common latency; after it, the incoming type's drop slid out.
                if (crossfading)
                    o[i] = (float) (pos + 1 <= settle ? (common - extraOut) + extraOut * smoothstep ((pos - 4) / (double) (settle - 4))
                                                      : (double) common);
                else
                    o[i] = (float) (latencyOf (active) + extraIn * (1.0 - smoothstep ((pos + 1) / (double) slide)));
            }

            position += length;
            offset += length;

            if (position >= (crossfading ? settle + fade : slide))
            {
                position = 0;
                phase = crossfading && extraIn > 0 ? Phase::slide : Phase::steady;
            }
        }
    }
}

int TubampAudioProcessor::blockSignature (chain::BlockId id) const noexcept
{
    const auto k = (size_t) chain::instanceOf (id);

    switch (chain::kindOf (id))
    {
        case chain::BlockId::gate:  return (int) valueOf (pp.gateMode);
        case chain::BlockId::comp:  return (int) valueOf (pp.compType[k]);
        case chain::BlockId::drive: return (int) valueOf (pp.driveType[k]);
        case chain::BlockId::eq:    return (int) valueOf (pp.eqType[k]) * 16 + (int) valueOf (pp.eqVoice[k]);
        case chain::BlockId::mod:
            return ((int) valueOf (pp.modType[k]) * 16 + (int) valueOf (pp.modChorusMode[k])) * 256
                   + (int) valueOf (pp.modPhaserModel[k]) * 32 + (int) valueOf (pp.modTremVoice[k]) * 2 + (isOn (pp.modColor[k]) ? 1 : 0);
        default:                    return 0;
    }
}

void TubampAudioProcessor::resetBlockInstance (chain::BlockId id, bool snap) noexcept
{
    const auto k = (size_t) chain::instanceOf (id);

    // The engine below starts clean, so engineStarts() has nothing to reset; the block fades
    // in from its input as it comes back, or after a state restore takes its state at once.
    engineStale[(size_t) id] = false;

    switch (id)
    {
        case chain::BlockId::comp:
        case chain::BlockId::comp2:
        case chain::BlockId::comp3:   comp[k].reset();       compFader[k].reset (snap);  break;

        // Tight and Smooth would resume from the gain they had when the gate left; a
        // reset starts them closed. Classic only drops its curve, as it always did.
        case chain::BlockId::gate:    gate.reset();          gateFader.reset (snap);     break;

        case chain::BlockId::drive:
        case chain::BlockId::drive2:
        case chain::BlockId::drive3:  drive[k].reset();      driveFader[k].reset (snap); driveTimeline[k].reset(); break;

        case chain::BlockId::eq:
        case chain::BlockId::eq2:
        case chain::BlockId::eq3:     eq[k].reset();         eqFader[k].reset (snap);    break;

        case chain::BlockId::mod:
        case chain::BlockId::mod2:
        case chain::BlockId::mod3:    modulation[k].reset(); modFader[k].reset (snap);   break;

        case chain::BlockId::delay:
        case chain::BlockId::delay2:
        case chain::BlockId::delay3:  delay[k].reset();      break;

        case chain::BlockId::reverb:
        case chain::BlockId::reverb2:
        case chain::BlockId::reverb3: reverbFx[k].reset();   break;

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
        // and out by their own staging, and the cab's tail belongs to the IR that is
        // loaded. lane2 is a marker, not a block.
        case chain::BlockId::amp:
        case chain::BlockId::amp2:
        case chain::BlockId::cab:
        case chain::BlockId::fx1:
        case chain::BlockId::fx2:
        case chain::BlockId::fx3:
        case chain::BlockId::lane2:   break;
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

    // Until the state is in, every callback treats what it finds as restored (no fades); the
    // first one after it does so once more.
    const RestoreScope restoring (*this);

    if (auto paramState = root.getChildWithName (apvts.state.getType()); paramState.isValid())
    {
        // The pin goes into the tree itself, so replaceState sets each pinned id straight
        // to Classic: the audio thread never sees the recommended type for a block of an
        // older session, not even for one callback.
        for (const auto& id : legacyPinnedIds (order))
            if (! paramState.getChildWithProperty ("id", id).isValid())
                paramState.appendChild (juce::ValueTree ("PARAM", { { "id", id }, { "value", 0.0f } }), nullptr);

        apvts.replaceState (paramState);
    }

    setEditorSize ({ (int) root.getProperty ("editorWidth", 0),
                     (int) root.getProperty ("editorHeight", 0) });

    const juce::String modelPath = root.getProperty ("modelPath", juce::String()).toString();
    const juce::String modelPathB = root.getProperty ("modelPathB", juce::String()).toString();
    const juce::String irPath = root.getProperty ("irPath", juce::String()).toString();

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

    return stateVar (paramValues, orderCopy, rowsCopy, loadedModelPath, loadedModelPathB, loadedIrPath,
                     fxSlotsTree().toXmlString());
}

juce::var TubampAudioProcessor::makeStateVar (const chain::Order& order,
                                              const std::vector<std::pair<juce::String, float>>& values) const
{
    auto* paramValues = new juce::DynamicObject();

    for (auto* parameter : getParameters())
    {
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
        {
            float normalised = ranged->getDefaultValue();

            for (const auto& [id, value] : values)
                if (id == ranged->paramID)
                    normalised = ranged->convertTo0to1 (ranged->convertFrom0to1 (ranged->convertTo0to1 (value)));

            // What captureStateVar() reads back once the value has been set on the parameter.
            paramValues->setProperty (juce::Identifier (ranged->paramID), (double) ranged->convertFrom0to1 (normalised));
        }
    }

    return stateVar (paramValues, order, {}, {}, {}, {}, juce::ValueTree ("FXSLOTS").toXmlString());
}

void TubampAudioProcessor::applyStateVar (const juce::var& state)
{
    auto* obj = state.getDynamicObject();

    if (obj == nullptr)
        return;

    // Presets and A/B slots written before the chain became user-arrangeable have no
    // "chainOrder": the void var stringifies to "" and parseOrderOrLegacy answers with
    // the classic order. Parsed ahead of the parameters because the legacy pin depends on
    // which blocks the restored chain holds.
    const juce::String orderV2 = obj->getProperty ("chainOrderV2").toString();
    const auto chainState = sanitiseOrderAndRows (
        chain::sanitizeStructure (
            chain::parseOrderOrLegacy (orderV2.isNotEmpty() ? orderV2
                                                            : obj->getProperty ("chainOrder").toString())),
        rowsFromString (obj->getProperty ("chainRows").toString()));

    const juce::var paramsVar = obj->getProperty ("params");
    const RestoreScope restoring (*this);

    if (auto* paramValues = paramsVar.getDynamicObject())
    {
        // Absent means default: a preset written by a build without a parameter
        // (every v2 instance-pool and amp_eq_* id, from a v1 preset) must not leave
        // whatever the user last dialled in ringing through the recalled sound.
        // Captures from this build carry every parameter, so this only fires for
        // old files. The exception is the legacy pin: an absent engine type id of a
        // block in the restored chain means Classic, the type the preset was made with.
        const auto pinned = legacyPinnedIds (chainState.order);

        for (auto* parameter : getParameters())
        {
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
            {
                if (paramValues->hasProperty (ranged->paramID))
                    continue;

                const bool pin = std::find (pinned.begin(), pinned.end(), ranged->paramID) != pinned.end();
                ranged->setValueNotifyingHost (pin ? ranged->convertTo0to1 (0.0f) : ranged->getDefaultValue());
            }
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

    // Publish before the (slow, synchronous) model/IR loading below so the audio thread
    // never runs the restored parameters against the old topology; adoptChainOrder at
    // the end re-reports latency and notifies the editor.
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
}
} // namespace tubamp

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tubamp::TubampAudioProcessor();
}
