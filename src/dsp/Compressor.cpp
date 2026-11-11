#include "Compressor.h"

// Scaffold stub: passes audio through until the compressor engineer lands the engine.
namespace tubamp
{
struct CompFx::Impl
{
    CompParams params;
};

CompFx::CompFx() : impl (std::make_unique<Impl>()) {}
CompFx::~CompFx() = default;

void CompFx::prepare (const juce::dsp::ProcessSpec&) {}
void CompFx::reset() {}
void CompFx::setParameters (const CompParams& params) { impl->params = params; }
void CompFx::process (juce::dsp::AudioBlock<float>) {}
void CompFx::skip (int) noexcept {}

float CompFx::getGainReductionDb() const noexcept { return 0.0f; }
} // namespace tubamp
