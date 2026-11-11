#include "Phaser.h"

// Scaffold stub: passes audio through until the phaser engineer lands the engine.
namespace tubamp
{
struct PhaserFx::Impl
{
    PhaserParams params;
};

PhaserFx::PhaserFx() : impl (std::make_unique<Impl>()) {}
PhaserFx::~PhaserFx() = default;

void PhaserFx::prepare (const juce::dsp::ProcessSpec&) {}
void PhaserFx::reset() {}
void PhaserFx::setParameters (const PhaserParams& params) { impl->params = params; }
void PhaserFx::process (juce::dsp::AudioBlock<float>) {}
} // namespace tubamp
