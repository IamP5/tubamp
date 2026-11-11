#include "Tremolo.h"

// Scaffold stub: passes audio through until the tremolo engineer lands the engine.
namespace tubamp
{
struct TremoloFx::Impl
{
    TremoloParams params;
};

TremoloFx::TremoloFx() : impl (std::make_unique<Impl>()) {}
TremoloFx::~TremoloFx() = default;

void TremoloFx::prepare (const juce::dsp::ProcessSpec&) {}
void TremoloFx::reset() {}
void TremoloFx::setParameters (const TremoloParams& params) { impl->params = params; }
void TremoloFx::process (juce::dsp::AudioBlock<float>) {}
} // namespace tubamp
