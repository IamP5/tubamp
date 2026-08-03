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

/** Drops duplicates and clamps ids, keeping the caller's ordering. Unlike
    chain::fromString this preserves an empty order: the user is allowed to strip the
    chain down to nothing (the editor shows its empty state for that). */
chain::Order sanitiseOrder (const chain::Order& order)
{
    chain::Order result;
    std::array<bool, chain::numBlockTypes> seen {};

    for (auto id : order)
    {
        const auto index = (size_t) juce::jlimit (0, chain::numBlockTypes - 1, (int) id);

        if (! std::exchange (seen[index], true))
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

    pendingSlim.store (valueOf (raw (params::ampSlim)), std::memory_order_relaxed);
    apvts.addParameterListener (params::ampSlim, this);

    presets.createFactoryPresetsIfMissing();
}

TubampAudioProcessor::~TubampAudioProcessor()
{
    apvts.removeParameterListener (params::ampSlim, this);
    cancelPendingUpdate();
    *aliveFlag = false;
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

    // Report latency from here, where the topology is known: only an amp block that
    // actually runs delays the signal. Hosts tolerate latency changes reported from
    // the audio thread (same contract as the old staging-time report).
    {
        const int wantedLatency = (present[(size_t) chain::BlockId::amp] && ampOn)
                                      ? namEngine.getLatencySamples()
                                      : 0;

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

void TubampAudioProcessor::updateLatency()
{
    // The resampler's latency only reaches the signal when the amp block actually
    // runs: present in the published order AND enabled. Otherwise report zero so
    // hosts don't compensate for a delay that never happens.
    std::array<chain::BlockId, chain::numBlockTypes> order {};
    const int numBlocks = chain::unpackTo (packedChain.load (std::memory_order_relaxed), order);

    bool ampPresent = false;

    for (int i = 0; i < numBlocks; ++i)
        ampPresent = ampPresent || order[(size_t) i] == chain::BlockId::amp;

    setLatencySamples (ampPresent && isOn (pp.ampOn) ? namEngine.getLatencySamples() : 0);
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

    root.appendChild (apvts.copyState(), nullptr);

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

    const juce::String modelPath = root.getProperty ("modelPath", juce::String()).toString();
    const juce::String irPath = root.getProperty ("irPath", juce::String()).toString();

    // Tolerant parse: missing or unreadable -> default order, so state written before
    // the chain became user-arrangeable still loads.
    const auto order = chain::fromString (root.getProperty ("chainOrder", juce::String()).toString());

    // The audio thread can have the new order immediately (single atomic word); the
    // UI-facing copy and the notification are message-thread only.
    publishChainOrder (order);

    // Model/IR loading does file IO and prewarm; it must never happen here if the
    // host restores state from a non-message thread.
    auto restore = [this, modelPath, irPath, order]
    {
        if (modelPath.isNotEmpty() && juce::File (modelPath).existsAsFile())
            loadModel (juce::File (modelPath));
        else
            clearModel();

        if (irPath.isNotEmpty() && juce::File (irPath).existsAsFile())
            loadIr (juce::File (irPath));
        else
            clearIr();

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

    auto* state = new juce::DynamicObject();
    state->setProperty ("version", kStateVersion);
    state->setProperty ("params", juce::var (paramValues));
    state->setProperty ("modelPath", loadedModelPath);
    state->setProperty ("irPath", loadedIrPath);
    state->setProperty ("chainOrder", chain::toString (uiChainOrder));

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
