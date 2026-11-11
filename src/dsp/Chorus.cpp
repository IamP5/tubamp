#include "Chorus.h"

// Scaffold stub: passes audio through until the chorus engineer lands the engine.
namespace tubamp
{
struct ChorusFx::Impl
{
    ChorusParams params;
    int solo = -1;
};

ChorusFx::ChorusFx() : impl (std::make_unique<Impl>()) {}
ChorusFx::~ChorusFx() = default;

void ChorusFx::prepare (const juce::dsp::ProcessSpec&) {}
void ChorusFx::reset() {}
void ChorusFx::setParameters (const ChorusParams& params) { impl->params = params; }
void ChorusFx::process (juce::dsp::AudioBlock<float>) {}
void ChorusFx::setDebugSolo (int voice) noexcept { impl->solo = voice; }
} // namespace tubamp
