#include "Gate.h"

// Scaffold stub: passes audio through until the gate engineer lands the engine.
namespace tubamp
{
struct GateFx::Impl
{
    GateParams params;
};

GateFx::GateFx() : impl (std::make_unique<Impl>()) {}
GateFx::~GateFx() = default;

void GateFx::prepare (const juce::dsp::ProcessSpec&) {}
void GateFx::reset() {}
void GateFx::setParameters (const GateParams& params) { impl->params = params; }
void GateFx::setCurveDelay (int) noexcept {}
void GateFx::measure (const juce::dsp::AudioBlock<float>&, int) {}
void GateFx::apply (juce::dsp::AudioBlock<float>, int) {}
void GateFx::apply (float*, int) {}

int GateFx::getState() const noexcept { return 2; }
float GateFx::getGainReductionDb() const noexcept { return 0.0f; }
float GateFx::getDetectorLevelDb() const noexcept { return -120.0f; }
} // namespace tubamp
