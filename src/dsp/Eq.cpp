#include "Eq.h"

// Scaffold stub: passes audio through until the EQ engineer lands the engine.
namespace tubamp
{
struct EqFx::Impl
{
    EqParams params;
};

EqFx::EqFx() : impl (std::make_unique<Impl>()) {}
EqFx::~EqFx() = default;

void EqFx::prepare (const juce::dsp::ProcessSpec&) {}
void EqFx::reset() {}
void EqFx::setParameters (const EqParams& params) { impl->params = params; }
void EqFx::process (juce::dsp::AudioBlock<float>) {}
} // namespace tubamp
