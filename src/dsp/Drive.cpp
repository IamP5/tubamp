#include "Drive.h"

// Scaffold stub: passes audio through until the drive engineer lands the engine.
namespace tubamp
{
struct DriveFx::Impl
{
    DriveParams params;
};

DriveFx::DriveFx() : impl (std::make_unique<Impl>()) {}
DriveFx::~DriveFx() = default;

void DriveFx::prepare (const juce::dsp::ProcessSpec&) {}
void DriveFx::reset() {}
void DriveFx::setParameters (const DriveParams& params) { impl->params = params; }
void DriveFx::process (juce::dsp::AudioBlock<float>) {}

int DriveFx::getLatencySamples() const noexcept { return 0; }
int DriveFx::latencyFor (int, double) noexcept { return 0; }
} // namespace tubamp
