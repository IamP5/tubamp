// Golden renders of the whole plugin (docs/FX-ENGINES.md §5.4): every case is a fresh
// TubampAudioProcessor with one chain and a few parameters, fed gen:music and hashed.
// Captured once on the base commit; every later build must reproduce the hashes
// (or match tests/golden/expected-changes.tsv).
//
//   tubamp_golden --capture <file.tsv> [--model <a.nam>] [--ir <ir.wav>] [--only <glob>]
//   tubamp_golden --verify <file.tsv> [--expected <expected-changes.tsv>] [--model ...] [--ir ...] [--only <glob>]
//   tubamp_golden --params <file.tsv>
//   tubamp_golden --legacy-check [--dump <dir>]
//   tubamp_golden --list [--model ...] [--ir ...]
//
// Hashes are architecture- and build-type-specific: capture and verify on the same
// machine, Release.
#include <juce_events/juce_events.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_cryptography/juce_cryptography.h>

#include "../../src/PluginProcessor.h"
#include "../fxbench/FxBench.h"
#include "../fxbench/legacy/LegacyFx.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{
using tubamp::chain::BlockId;
namespace chain = tubamp::chain;

struct Case
{
    std::string name;
    chain::Order order;
    std::vector<std::pair<std::string, float>> settings;
    bool star = false;
    bool model = false;
    bool ir = false;
};

struct Config
{
    double sampleRate;
    int block;
};

struct Result
{
    std::string hash;
    int latency = 0;
};

/** Instance k (0-based) of a duplicable kind's parameter id: "comp", "comp2", "comp3". */
std::string idOf (const char* kind, int k, const char* key)
{
    return std::string (kind) + (k == 0 ? "" : std::to_string (k + 1)) + "_" + key;
}

/** The type/mode/voicing ids of docs/FX-ENGINES.md §2.7.5. A build that has them
    defaults fresh instances to the recommended types, so every case pins them to
    Classic (0) first; on the base commit none exist and this does nothing. */
std::vector<std::string> classicPinIds()
{
    std::vector<std::string> ids { "gate_mode" };

    for (int k = 0; k < 3; ++k)
    {
        ids.push_back (idOf ("comp", k, "type"));
        ids.push_back (idOf ("drive", k, "type"));
        ids.push_back (idOf ("eq", k, "type"));
        ids.push_back (idOf ("mod", k, "chorus_mode"));
        ids.push_back (idOf ("mod", k, "phaser_model"));
        ids.push_back (idOf ("mod", k, "trem_voice"));
    }

    return ids;
}

std::vector<Case> buildCases (bool withModel, bool withIr)
{
    std::vector<Case> cases;
    const auto add = [&cases] (std::string name, chain::Order order,
                               std::vector<std::pair<std::string, float>> settings, bool star = false)
    {
        cases.push_back ({ std::move (name), std::move (order), std::move (settings), star });
    };

    add ("empty", {}, {}, true);
    add ("gate0", { BlockId::gate }, { { "gate_threshold", -80.0f } }, true);
    add ("gate1", { BlockId::gate }, { { "gate_threshold", -40.0f } });
    add ("gateamp", { BlockId::gate, BlockId::amp }, { { "gate_threshold", -40.0f } });

    for (int k = 0; k < 3; ++k)
    {
        const auto id = chain::instanceId (BlockId::comp, k);
        const auto p = [k] (const char* key) { return idOf ("comp", k, key); };
        const auto n = "comp" + std::to_string (k + 1);

        add (n + "_def", { id }, {}, k == 0);
        add (n + "_fast", { id }, { { p ("threshold"), -40.0f }, { p ("ratio"), 20.0f }, { p ("attack"), 0.1f },
                                    { p ("release"), 10.0f }, { p ("makeup"), 12.0f } });
        add (n + "_slow", { id }, { { p ("threshold"), -30.0f }, { p ("ratio"), 2.0f }, { p ("attack"), 30.0f },
                                    { p ("release"), 1000.0f }, { p ("makeup"), 3.0f } });
    }

    add ("compoff", { BlockId::comp }, { { "comp_on", 0.0f }, { "comp_makeup", 6.0f } });

    for (int k = 0; k < 3; ++k)
    {
        const auto id = chain::instanceId (BlockId::drive, k);
        const auto n = "drive" + std::to_string (k + 1);
        const auto s = [k] (float gain, float tone, float level)
        {
            return std::vector<std::pair<std::string, float>> {
                { idOf ("drive", k, "on"), 1.0f }, { idOf ("drive", k, "gain"), gain },
                { idOf ("drive", k, "tone"), tone }, { idOf ("drive", k, "level"), level } };
        };

        add (n + "_def", { id }, s (12.0f, 4000.0f, 0.0f), k == 0);
        add (n + "_hot", { id }, s (36.0f, 12000.0f, -6.0f));
        add (n + "_dark", { id }, s (0.0f, 500.0f, 0.0f));
    }

    for (int k = 0; k < 3; ++k)
    {
        const auto id = chain::instanceId (BlockId::eq, k);
        const auto n = "eq" + std::to_string (k + 1);
        const auto s = [k] (float bass, float mid, float treble)
        {
            return std::vector<std::pair<std::string, float>> {
                { idOf ("eq", k, "bass"), bass }, { idOf ("eq", k, "mid"), mid }, { idOf ("eq", k, "treble"), treble } };
        };

        add (n + "_flat", { id }, s (5.0f, 5.0f, 5.0f), k == 0);
        add (n + "_shape", { id }, s (10.0f, 0.0f, 7.0f));
        add (n + "_cut", { id }, s (0.0f, 0.0f, 0.0f));
        add (n + "_near", { id }, s (5.0f, 5.0f, 5.1f));
    }

    add ("amp_noeq", { BlockId::amp }, { { "amp_eq_on", 0.0f } });
    add ("ampeq_flat", { BlockId::amp }, { { "amp_eq_on", 1.0f }, { "amp_eq_bass", 5.0f },
                                           { "amp_eq_mid", 5.0f }, { "amp_eq_treble", 5.0f } });
    add ("ampeq_shape", { BlockId::amp }, { { "amp_eq_on", 1.0f }, { "amp_eq_bass", 8.0f },
                                            { "amp_eq_mid", 3.0f }, { "amp_eq_treble", 6.0f } });

    static constexpr const char* modTypes[] { "cho", "pha", "tre" };
    struct ModSetting { const char* name; float rate, depth, mix; };
    static constexpr ModSetting modSettings[] { { "def", 1.0f, 0.4f, 0.35f },
                                                { "fast", 10.0f, 1.0f, 1.0f },
                                                { "slow", 0.05f, 0.1f, 0.5f } };

    for (int k = 0; k < 3; ++k)
        for (int t = 0; t < 3; ++t)
            for (const auto& s : modSettings)
                add ("mod" + std::to_string (k + 1) + "_" + modTypes[t] + "_" + s.name,
                     { chain::instanceId (BlockId::mod, k) },
                     { { idOf ("mod", k, "type"), (float) t }, { idOf ("mod", k, "rate"), s.rate },
                       { idOf ("mod", k, "depth"), s.depth }, { idOf ("mod", k, "mix"), s.mix } },
                     k == 0 && std::string (s.name) == "def");

    for (int k = 0; k < 3; ++k)
        for (int t = 0; t < 5; ++t)
            add ("dl" + std::to_string (k + 1) + "_t" + std::to_string (t),
                 { chain::instanceId (BlockId::delay, k) }, { { idOf ("delay", k, "type"), (float) t } });

    for (int k = 0; k < 3; ++k)
        for (int t = 0; t < 5; ++t)
            add ("rv" + std::to_string (k + 1) + "_t" + std::to_string (t),
                 { chain::instanceId (BlockId::reverb, k) }, { { idOf ("reverb", k, "type"), (float) t } });

    add ("cab0", { BlockId::cab }, {});
    add ("classic", chain::classicOrder(), { { "drive_on", 1.0f } }, true);
    add ("splitA", { BlockId::split, BlockId::drive, BlockId::lane2, BlockId::mix },
         { { "drive_on", 1.0f }, { "drive_gain", 0.0f } });
    add ("splitAB", { BlockId::split, BlockId::drive, BlockId::lane2, BlockId::drive2, BlockId::mix },
         { { "drive_on", 1.0f }, { "drive_gain", 0.0f }, { "drive2_on", 1.0f }, { "drive2_gain", 0.0f } });

    if (withModel)
    {
        add ("ampM", { BlockId::amp }, {});
        cases.back().model = true;
        add ("gateampM", { BlockId::gate, BlockId::amp }, { { "gate_threshold", -40.0f } });
        cases.back().model = true;
    }

    if (withIr)
    {
        add ("cabIR", { BlockId::cab }, {});
        cases.back().ir = true;
    }

    return cases;
}

std::vector<Config> configsFor (const Case& c)
{
    std::vector<Config> configs { { 44100.0, 512 }, { 48000.0, 512 }, { 96000.0, 512 } };

    if (c.star)
    {
        configs.push_back ({ 22050.0, 512 });
        configs.push_back ({ 192000.0, 512 });
        configs.push_back ({ 48000.0, 1 });
        configs.push_back ({ 48000.0, 37 });
    }

    return configs;
}

bool setParam (tubamp::TubampAudioProcessor& proc, const std::string& id, float value)
{
    auto* p = proc.apvts.getParameter (id);

    if (p == nullptr)
        return false;

    p->setValueNotifyingHost (p->convertTo0to1 (value));
    return true;
}

struct Inputs
{
    juce::File model, ir;
};

Result render (const Case& c, const Config& cfg, const Inputs& inputs)
{
    tubamp::TubampAudioProcessor proc;
    proc.setChainOrder (c.order, {});

    for (const auto& id : classicPinIds())
        setParam (proc, id, 0.0f);

    for (const auto& [id, value] : c.settings)
        if (! setParam (proc, id, value))
            std::fprintf (stderr, "warning: %s: no parameter '%s'\n", c.name.c_str(), id.c_str());

    if (c.model)
        if (const auto error = proc.loadModel (inputs.model); error.isNotEmpty())
            std::fprintf (stderr, "error: %s: loadModel: %s\n", c.name.c_str(), error.toRawUTF8());

    if (c.ir)
        if (const auto error = proc.loadIr (inputs.ir); error.isNotEmpty())
            std::fprintf (stderr, "error: %s: loadIr: %s\n", c.name.c_str(), error.toRawUTF8());

    proc.prepareToPlay (cfg.sampleRate, cfg.block);

    if (c.ir)
    {
        // juce::dsp::Convolution installs a new IR from a background thread on a later
        // process() call; give it time so the first block already convolves.
        juce::Thread::sleep (500);
    }

    const int numFrames = (int) std::llround (6.0 * cfg.sampleRate);
    juce::AudioBuffer<float> audio (2, numFrames);
    fxbench::fillMusic (audio, cfg.sampleRate);

    juce::MidiBuffer midi;

    for (int start = 0; start < numFrames; start += cfg.block)
    {
        const int n = juce::jmin (cfg.block, numFrames - start);
        float* ptrs[2] { audio.getWritePointer (0, start), audio.getWritePointer (1, start) };
        juce::AudioBuffer<float> view (ptrs, 2, n);
        proc.processBlock (view, midi);
    }

    juce::HeapBlock<float> interleaved ((size_t) numFrames * 2);

    for (int i = 0; i < numFrames; ++i)
    {
        interleaved[(size_t) (2 * i)] = audio.getSample (0, i);
        interleaved[(size_t) (2 * i + 1)] = audio.getSample (1, i);
    }

    const juce::SHA256 sha (interleaved.get(), (size_t) numFrames * 2 * sizeof (float));
    return { sha.toHexString().toStdString(), proc.getLatencySamples() };
}

std::string fileSha (const juce::File& f)
{
    juce::FileInputStream in (f);
    return in.openedOk() ? juce::SHA256 (in).toHexString().toStdString() : std::string ("missing");
}

std::string rowKey (const std::string& name, double fs, int block)
{
    return name + "\t" + std::to_string ((int) fs) + "\t" + std::to_string (block);
}

struct Row
{
    std::string name;
    int fs = 0, block = 0;
    std::string hash;
    int latency = 0;
};

struct GoldenFile
{
    std::vector<Row> rows;
    std::string modelSha;
};

bool readGolden (const juce::File& file, GoldenFile& out)
{
    juce::StringArray lines;
    file.readLines (lines);

    if (lines.isEmpty())
        return false;

    for (const auto& line : lines)
    {
        if (line.startsWith ("# model "))
            out.modelSha = line.fromFirstOccurrenceOf ("# model ", false, false).upToFirstOccurrenceOf ("\t", false, false).toStdString();

        if (line.startsWith ("#") || line.trim().isEmpty())
            continue;

        juce::StringArray f;
        f.addTokens (line, "\t", "");

        if (f.size() < 5)
            continue;

        out.rows.push_back ({ f[0].toStdString(), f[1].getIntValue(), f[2].getIntValue(), f[3].toStdString(), f[4].getIntValue() });
    }

    return true;
}

/** One line of expected-changes.tsv: `pattern  audio  latency`, first match wins.
      audio:   same | any | differs | same_as:<case> | off:<id>[,<id>...]
               (same_as compares with that case's captured hash at the same fs and
               block; off: with this build's render of the same case with those
               parameters at 0)
      latency: same | +N                     (captured latency plus N) */
struct Expectation
{
    juce::String pattern, audio, latency;
};

std::vector<Expectation> readExpected (const juce::File& file)
{
    std::vector<Expectation> list;
    juce::StringArray lines;
    file.readLines (lines);

    for (const auto& line : lines)
    {
        if (line.startsWith ("#") || line.trim().isEmpty())
            continue;

        juce::StringArray f;
        f.addTokens (line, "\t", "");
        f.removeEmptyStrings();

        if (f.size() >= 3)
            list.push_back ({ f[0].trim(), f[1].trim(), f[2].trim() });
    }

    return list;
}

int writeParams (const juce::File& file)
{
    tubamp::TubampAudioProcessor proc;
    juce::String out ("# index\tid\tname\tkind\tmin\tmax\tstep\tskew\tdefault\thint\tchoices\n");
    int index = 0;

    for (auto* raw : proc.getParameters())
    {
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (raw);

        if (p == nullptr)
            continue;

        const auto& range = p->getNormalisableRange();
        juce::String kind ("float"), choices;

        if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (p))
        {
            kind = "choice";
            choices = c->choices.joinIntoString ("|");
        }
        else if (dynamic_cast<juce::AudioParameterBool*> (p) != nullptr)
        {
            kind = "bool";
        }
        else if (dynamic_cast<juce::AudioParameterInt*> (p) != nullptr)
        {
            kind = "int";
        }

        out << index++ << "\t" << p->getParameterID() << "\t" << p->getName (256) << "\t" << kind << "\t"
            << juce::String::formatted ("%.9g\t%.9g\t%.9g\t%.9g\t%.9g", (double) range.start, (double) range.end,
                                        (double) range.interval, (double) range.skew,
                                        (double) p->convertFrom0to1 (p->getDefaultValue()))
            << "\t" << p->getVersionHint() << "\t" << choices << "\n";
    }

    if (! file.replaceWithText (out, false, false, "\n"))
    {
        std::fprintf (stderr, "cannot write %s\n", file.getFullPathName().toRawUTF8());
        return 2;
    }

    std::printf ("%d parameters -> %s\n", index, file.getFullPathName().toRawUTF8());
    return 0;
}

//==============================================================================
// --legacy-check: proves tools/fxbench/legacy is the code the processor runs. Each case
// renders gen:music through the real processor with a one-block chain (plus parameter
// steps between blocks) and, in lockstep, through the legacy class fed the processor's
// own parameter values with the calls processChainBlock makes. The processor's
// always-on stages are reproduced around the legacy block exactly as processBlock runs
// them: input trim and output level through the same smoothed-gain helper (their
// default 0 dB does not read back as exactly 0 from the parameter, so they are not an
// identity) and a tubamp::DcBlocker of its own before the output level.
//
// --dump <dir> also writes each case's legacy block output (no trim, DC blocker or output
// level) at 48 kHz / 512 as <case>.wav, and the tubamp_fxbench --legacy command that must
// reproduce it bit for bit to <dir>/fxbench-commands.txt.
//
// The deliberate changes are the only exceptions, and each case that meets one says so: an
// EQ or amp EQ at exactly 5/5/5 is now an exact pass-through (FX-ENGINES.md D2 a), so those
// cases compare with the legacy chain minus the EQ; a knob move is now smoothed (D-4), so a
// case that steps an EQ knob compares only up to its first step; a mod_type change now
// crossfades from the outgoing type (D-5). Leaving the fade's samples out of the comparison
// would not do for that one: the always-on DC blocker carries any difference on for good. So
// the legacy side runs the old class once per type, resets the incoming one where the old
// block reset everything, and mixes the two over the fade exactly as Modulation does; it then
// proves, before its DC blocker, that this differs from the old single-instance jump only
// inside the fades.
/** PluginProcessor.cpp's applySmoothedGain, verbatim. */
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

struct Step
{
    double time;
    std::string id;
    float value;
};

/** A step with this id takes the case's block out of the chain (value 0) or puts it
    back (1), for the absent-block and re-entry paths. */
const std::string kPresentStep = "@present";

enum class LegacyKind { gate, gateDeferred, comp, drive, eq, ampEq, mod };

struct LegacyCase
{
    std::string name;
    LegacyKind kind;
    int instance = 0;
    std::vector<std::pair<std::string, float>> settings;
    std::vector<Step> steps;
    bool dumpable = true;
    /** The EQ sits at exactly 5/5/5: the legacy side leaves it out (D2 a). */
    bool flat = false;
    /** Compared only before this time, in seconds (< 0: the whole render): an EQ knob step
        is smoothed now (D-4). */
    double identicalUntil = -1.0;
};

std::vector<LegacyCase> buildLegacyCases()
{
    std::vector<LegacyCase> cases;
    const auto add = [&cases] (std::string name, LegacyKind kind, int k,
                               std::vector<std::pair<std::string, float>> settings, std::vector<Step> steps = {})
    {
        cases.push_back ({ std::move (name), kind, k, std::move (settings), std::move (steps) });
    };

    // Control: the gate is in the chain but off, so both sides are the DC blocker alone.
    add ("control_dc_only", LegacyKind::gate, 0, { { "gate_on", 0.0f } });
    cases.back().dumpable = false;
    add ("gate_m40", LegacyKind::gate, 0, { { "gate_threshold", -40.0f } });
    add ("gate_m80_steps", LegacyKind::gate, 0, { { "gate_threshold", -80.0f } },
         { { 2.5, "gate_threshold", -30.0f }, { 4.2, "gate_threshold", -55.0f } });
    add ("gate_deferred", LegacyKind::gateDeferred, 0, { { "gate_threshold", -40.0f }, { "amp_eq_on", 0.0f } });
    add ("gate_toggle", LegacyKind::gate, 0, { { "gate_threshold", -40.0f } },
         { { 1.5, "gate_on", 0.0f }, { 2.5, "gate_on", 1.0f }, { 3.5, "gate_threshold", -60.0f } });
    add ("gate_deferred_steps", LegacyKind::gateDeferred, 0, { { "gate_threshold", -40.0f }, { "amp_eq_on", 0.0f } },
         { { 1.5, "gate_on", 0.0f }, { 2.5, "gate_on", 1.0f }, { 3.5, "gate_threshold", -55.0f } });
    // The old processor never reset the gate on re-entry; the engine now does, which
    // Classic must not hear.
    add ("gate_absent", LegacyKind::gate, 0, { { "gate_threshold", -40.0f } },
         { { 2.0, kPresentStep, 0.0f }, { 3.0, kPresentStep, 1.0f } });
    cases.back().dumpable = false;

    add ("comp_def_steps", LegacyKind::comp, 0, {},
         { { 1.5, "comp_makeup", 12.0f }, { 2.2, "comp_threshold", -35.0f }, { 3.0, "comp_on", 0.0f },
           { 3.5, "comp_makeup", 3.0f }, { 4.0, "comp_on", 1.0f } });
    add ("comp_fast", LegacyKind::comp, 0, { { "comp_threshold", -40.0f }, { "comp_ratio", 20.0f }, { "comp_attack", 0.1f },
                                             { "comp_release", 10.0f }, { "comp_makeup", 12.0f } });
    add ("comp_slow", LegacyKind::comp, 0, { { "comp_threshold", -30.0f }, { "comp_ratio", 2.0f }, { "comp_attack", 30.0f },
                                             { "comp_release", 1000.0f }, { "comp_makeup", 3.0f } });
    add ("comp2_steps", LegacyKind::comp, 1, { { "comp2_threshold", -40.0f } },
         { { 2.0, "comp2_ratio", 8.0f }, { 3.3, "comp2_attack", 0.5f } });
    add ("comp3_steps", LegacyKind::comp, 2, { { "comp3_threshold", -30.0f } },
         { { 1.0, "comp3_makeup", 9.0f }, { 2.0, "comp3_on", 0.0f }, { 3.0, "comp3_release", 400.0f },
           { 3.5, "comp3_on", 1.0f } });
    // Absent: the makeup smoother keeps stepping (and moving) while the block is out of
    // the chain; on re-entry the compressor is reset and the makeup is not.
    add ("comp_absent", LegacyKind::comp, 0, { { "comp_makeup", 6.0f } },
         { { 1.5, kPresentStep, 0.0f }, { 2.0, "comp_makeup", 12.0f }, { 3.0, kPresentStep, 1.0f },
           { 4.0, "comp_makeup", 0.0f } });
    cases.back().dumpable = false;

    add ("drive_def_steps", LegacyKind::drive, 0, { { "drive_on", 1.0f } },
         { { 1.0, "drive_gain", 30.0f }, { 2.0, "drive_tone", 800.0f }, { 3.0, "drive_level", -6.0f },
           { 4.0, "drive_on", 0.0f }, { 5.0, "drive_on", 1.0f } });
    add ("drive_hot", LegacyKind::drive, 0, { { "drive_on", 1.0f }, { "drive_gain", 36.0f }, { "drive_tone", 12000.0f },
                                              { "drive_level", -6.0f } });
    add ("drive3_dark", LegacyKind::drive, 2, { { "drive3_gain", 0.0f }, { "drive3_tone", 500.0f } });
    add ("drive2_mid", LegacyKind::drive, 1, { { "drive2_gain", 20.0f }, { "drive2_tone", 2000.0f }, { "drive2_level", 3.0f } });

    add ("eq_flat", LegacyKind::eq, 0, {});
    cases.back().flat = true;
    add ("eq_shape", LegacyKind::eq, 0, { { "eq_bass", 10.0f }, { "eq_mid", 0.0f }, { "eq_treble", 7.0f } });
    add ("eq_cut", LegacyKind::eq, 0, { { "eq_bass", 0.0f }, { "eq_mid", 0.0f }, { "eq_treble", 0.0f } });
    add ("eq2_near", LegacyKind::eq, 1, { { "eq2_treble", 5.1f } });
    add ("eq3_shape", LegacyKind::eq, 2, { { "eq3_bass", 2.0f }, { "eq3_mid", 8.0f }, { "eq3_treble", 3.5f } });
    add ("eq_toggle", LegacyKind::eq, 0, { { "eq_bass", 7.0f }, { "eq_mid", 3.0f }, { "eq_treble", 6.0f } },
         { { 1.5, "eq_on", 0.0f }, { 2.5, "eq_on", 1.0f }, { 3.5, "eq_on", 0.0f }, { 4.0, "eq_on", 1.0f } });
    add ("eq_steps", LegacyKind::eq, 0, { { "eq_bass", 6.0f } },
         { { 1.0, "eq_bass", 8.0f }, { 2.0, "eq_mid", 2.0f }, { 3.0, "eq_mid", 7.0f }, { 4.0, "eq_treble", 0.0f },
           { 4.5, "eq_on", 0.0f }, { 5.0, "eq_on", 1.0f } });
    cases.back().identicalUntil = 1.0;

    // The amp block's tone stack (no model: the engine passes audio through, so the mono
    // path is collapse, amp-in gain, tone stack, amp-out gain, expand).
    add ("ampeq_flat", LegacyKind::ampEq, 0, { { "amp_eq_on", 1.0f } });
    cases.back().flat = true;
    cases.back().dumpable = false;
    add ("ampeq_shape", LegacyKind::ampEq, 0, { { "amp_eq_on", 1.0f }, { "amp_eq_bass", 8.0f }, { "amp_eq_mid", 3.0f },
                                                { "amp_eq_treble", 6.0f } },
         { { 2.0, "amp_eq_on", 0.0f }, { 3.0, "amp_eq_on", 1.0f } });
    cases.back().dumpable = false;
    add ("ampeq_cut", LegacyKind::ampEq, 0, { { "amp_eq_on", 1.0f }, { "amp_eq_bass", 0.0f }, { "amp_eq_mid", 10.0f },
                                              { "amp_eq_treble", 0.0f } });
    cases.back().dumpable = false;

    static constexpr const char* typeNames[] { "cho", "pha", "tre" };
    struct ModSetting { const char* name; float rate, depth, mix; };
    static constexpr ModSetting modSettings[] { { "def", 1.0f, 0.4f, 0.35f },
                                                { "fast", 10.0f, 1.0f, 1.0f },
                                                { "slow", 0.05f, 0.1f, 0.5f } };

    for (int t = 0; t < 3; ++t)
    {
        for (const auto& s : modSettings)
            add (std::string ("mod_") + typeNames[t] + "_" + s.name, LegacyKind::mod, 0,
                 { { "mod_type", (float) t }, { "mod_rate", s.rate }, { "mod_depth", s.depth }, { "mod_mix", s.mix } });

        add (std::string ("mod2_") + typeNames[t] + "_steps", LegacyKind::mod, 1, { { "mod2_type", (float) t } },
             { { 2.0, "mod2_rate", 5.0f }, { 3.0, "mod2_depth", 0.9f }, { 4.0, "mod2_mix", 0.8f },
               { 4.4, "mod2_on", 0.0f }, { 4.8, "mod2_on", 1.0f } });
    }

    add ("mod_typechange", LegacyKind::mod, 0, { { "mod_type", 0.0f } },
         { { 2.0, "mod_type", 2.0f }, { 4.0, "mod_type", 1.0f } });
    cases.back().dumpable = false;
    // Back to a type that ran before, a knob step inside a fade, a change while off (it lands
    // when the block comes back on), and a change during a fade (it cuts that fade short).
    add ("mod3_typesteps", LegacyKind::mod, 2, { { "mod3_type", 1.0f }, { "mod3_depth", 0.6f } },
         { { 1.0, "mod3_type", 0.0f }, { 1.01, "mod3_depth", 0.9f }, { 2.0, "mod3_type", 1.0f },
           { 2.5, "mod3_on", 0.0f }, { 2.8, "mod3_type", 2.0f }, { 3.2, "mod3_on", 1.0f },
           { 4.0, "mod3_type", 0.0f }, { 4.005, "mod3_type", 1.0f }, { 5.0, "mod3_rate", 4.0f } });
    cases.back().dumpable = false;
    // Out of the chain and back with another type: the re-entry reset, then no fade.
    add ("mod_absent", LegacyKind::mod, 0, { { "mod_type", 2.0f } },
         { { 1.5, kPresentStep, 0.0f }, { 2.0, "mod_type", 0.0f }, { 3.0, kPresentStep, 1.0f },
           { 4.0, "mod_type", 2.0f } });
    cases.back().dumpable = false;

    return cases;
}

chain::Order legacyOrder (const LegacyCase& c)
{
    switch (c.kind)
    {
        case LegacyKind::gate:         return { BlockId::gate };
        case LegacyKind::gateDeferred: return { BlockId::gate, BlockId::amp };
        case LegacyKind::comp:         return { chain::instanceId (BlockId::comp, c.instance) };
        case LegacyKind::drive:        return { chain::instanceId (BlockId::drive, c.instance) };
        case LegacyKind::eq:           return { chain::instanceId (BlockId::eq, c.instance) };
        case LegacyKind::ampEq:        return { BlockId::amp };
        case LegacyKind::mod:          return { chain::instanceId (BlockId::mod, c.instance) };
    }

    return {};
}

/** The fxbench keys of a case and the processor parameter each one reads. */
std::vector<std::pair<std::string, std::string>> fxbenchKeys (const LegacyCase& c)
{
    const int k = c.instance;

    switch (c.kind)
    {
        case LegacyKind::gate:
        case LegacyKind::gateDeferred: return { { "threshold", "gate_threshold" }, { "on", "gate_on" } };
        case LegacyKind::comp:  return { { "threshold", idOf ("comp", k, "threshold") }, { "ratio", idOf ("comp", k, "ratio") },
                                         { "attack", idOf ("comp", k, "attack") }, { "release", idOf ("comp", k, "release") },
                                         { "makeup", idOf ("comp", k, "makeup") }, { "on", idOf ("comp", k, "on") } };
        case LegacyKind::drive: return { { "gain", idOf ("drive", k, "gain") }, { "tone", idOf ("drive", k, "tone") },
                                         { "level", idOf ("drive", k, "level") }, { "on", idOf ("drive", k, "on") } };
        case LegacyKind::eq:    return { { "bass", idOf ("eq", k, "bass") }, { "mid", idOf ("eq", k, "mid") },
                                         { "treble", idOf ("eq", k, "treble") }, { "on", idOf ("eq", k, "on") } };
        case LegacyKind::ampEq: return { { "bass", "amp_eq_bass" }, { "mid", "amp_eq_mid" }, { "treble", "amp_eq_treble" },
                                         { "on", "amp_eq_on" } };
        case LegacyKind::mod:   return { { "rate", idOf ("mod", k, "rate") }, { "depth", idOf ("mod", k, "depth") },
                                         { "mix", idOf ("mod", k, "mix") }, { "on", idOf ("mod", k, "on") } };
    }

    return {};
}

struct LegacyOutcome
{
    long differing = 0, total = 0;
    long firstFrame = -1;
    float processorValue = 0.0f, legacyValue = 0.0f;
    /** mod_type crossfades (D-5): how many, the samples they span, how many of those differ
        from the old jump, and how many samples outside them do (must be none). */
    int fades = 0;
    long fadeSamples = 0, fadeDiffering = 0, outsideFadeDiffering = 0;
};

/** The legacy side of a mod case: tubamp::legacy::Modulation once per mod_type, switched and
    crossfaded the way tubamp::Modulation switches its engines. The old block reset all three
    types and jumped; here the incoming type alone is reset (the others are not running, and
    are reset when they come back) and the outgoing one keeps its last parameters while it
    fades out. */
struct LegacyModCrossfade
{
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        for (auto& m : perType)
            m.prepare (spec);

        outgoingAudio.setSize ((int) spec.numChannels, (int) spec.maximumBlockSize);
        fadeLength = tubamp::Modulation::crossfadeSamples (spec.sampleRate);
        reset();
    }

    void reset()
    {
        for (auto& m : perType)
            m.reset();

        active = -1;
        outgoing = -1;
    }

    /** Returns true when this block starts a crossfade. */
    bool process (juce::dsp::AudioBlock<float> block, int type, float rate, float depth, float mix)
    {
        using Type = tubamp::legacy::Modulation::Type;
        const auto n = block.getNumSamples();
        const auto channels = block.getNumChannels();
        bool started = false;

        if (active < 0)
        {
            active = type;
        }
        else if (type != active)
        {
            outgoing = active;
            active = type;
            perType[active].reset();
            fadePosition = 0;
            started = true;
        }

        last[active] = { rate, depth, mix };
        perType[active].setParameters ((Type) active, rate, depth, mix);

        if (outgoing < 0)
        {
            perType[active].process (block);
            return started;
        }

        juce::dsp::AudioBlock<float> old (outgoingAudio.getArrayOfWritePointers(), channels, n);
        old.copyFrom (block);
        const auto& kept = last[outgoing];
        perType[outgoing].setParameters ((Type) outgoing, kept[0], kept[1], kept[2]);
        perType[outgoing].process (old);
        perType[active].process (block);

        const int count = juce::jmin ((int) n, fadeLength - fadePosition);

        for (size_t ch = 0; ch < channels; ++ch)
        {
            auto* data = block.getChannelPointer (ch);
            const auto* o = old.getChannelPointer (ch);

            for (int i = 0; i < count; ++i)
            {
                const float w = (float) (fadePosition + i) / (float) fadeLength;
                data[i] = o[i] + w * (data[i] - o[i]);
            }
        }

        fadePosition += count;

        if (fadePosition >= fadeLength)
            outgoing = -1;

        return started;
    }

    tubamp::legacy::Modulation perType[3];
    std::array<float, 3> last[3] {};
    juce::AudioBuffer<float> outgoingAudio;
    int active = -1, outgoing = -1, fadePosition = 0, fadeLength = 1;
};

LegacyOutcome runLegacyCase (const LegacyCase& c, const Config& cfg, const juce::File& dumpDir, juce::String& commands)
{
    namespace legacy = tubamp::legacy;

    tubamp::TubampAudioProcessor proc;
    proc.setChainOrder (legacyOrder (c), {});

    for (const auto& id : classicPinIds())
        setParam (proc, id, 0.0f);

    for (const auto& [id, value] : c.settings)
        if (! setParam (proc, id, value))
            std::fprintf (stderr, "warning: %s: no parameter '%s'\n", c.name.c_str(), id.c_str());

    const auto raw = [&proc] (const std::string& id) { return proc.apvts.getRawParameterValue (id)->load(); };
    const int k = c.instance;
    const bool onFlag = c.kind == LegacyKind::gate || c.kind == LegacyKind::gateDeferred;
    const std::string onId = onFlag ? std::string ("gate_on")
                           : c.kind == LegacyKind::ampEq ? std::string ("amp_eq_on")
                                    : idOf (c.kind == LegacyKind::comp ? "comp" : c.kind == LegacyKind::drive ? "drive"
                                            : c.kind == LegacyKind::eq ? "eq" : "mod", k, "on");

    proc.prepareToPlay (cfg.sampleRate, cfg.block);

    const juce::dsp::ProcessSpec spec { cfg.sampleRate, (juce::uint32) cfg.block, 2 };
    legacy::NoiseGate gate;
    legacy::LegacyComp comp;
    legacy::Drive drive;
    legacy::ToneStackEQ eq, ampEq;
    legacy::Modulation modulation;
    LegacyModCrossfade modCrossfade;
    tubamp::DcBlocker dc;
    juce::SmoothedValue<float> inputTrimLin { 1.0f }, outputLevelLin { 1.0f }, ampInLin { 1.0f }, ampOutLin { 1.0f };

    gate.prepare (cfg.sampleRate, cfg.block);
    gate.reset();
    comp.prepare (spec);
    drive.prepare (spec);
    eq.prepare (spec);
    ampEq.prepare (spec);
    modulation.prepare (spec);
    modCrossfade.prepare (spec);
    dc.prepare (spec);

    for (auto* smoothed : { &inputTrimLin, &outputLevelLin, &ampInLin, &ampOutLin })
        smoothed->reset (cfg.sampleRate, 0.02);

    // No model is loaded, so ampInputDb() / ampOutputDb() add no calibration term.
    ampInLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (raw ("amp_input") + 0.0f));
    ampOutLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (raw ("amp_output") + 0.0f));

    inputTrimLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (raw ("input_trim")));
    outputLevelLin.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (raw ("output_level")));

    const int numFrames = (int) std::llround (6.0 * cfg.sampleRate);
    juce::AudioBuffer<float> procOut (2, numFrames), legacyOut, legacyPreDc, jumpPreDc;
    fxbench::fillMusic (procOut, cfg.sampleRate);
    legacyOut.makeCopyOf (procOut);
    legacyPreDc.makeCopyOf (procOut);
    jumpPreDc.makeCopyOf (procOut);
    std::vector<std::pair<int, int>> fadeSpans;
    std::vector<float> mono ((size_t) cfg.block), plainMono ((size_t) cfg.block);

    juce::MidiBuffer midi;
    size_t nextStep = 0;
    const auto keys = fxbenchKeys (c);
    std::vector<std::string> stepTokens;
    bool present = true;

    for (int start = 0; start < numFrames; start += cfg.block)
    {
        const int n = juce::jmin (cfg.block, numFrames - start);
        const double t = (double) start / cfg.sampleRate;

        bool reentered = false;

        while (nextStep < c.steps.size() && t >= c.steps[nextStep].time)
        {
            const auto& s = c.steps[nextStep++];

            if (s.id == kPresentStep)
            {
                reentered = ! present && s.value > 0.5f;
                present = s.value > 0.5f;
                proc.setChainOrder (present ? legacyOrder (c) : chain::Order {}, {});
                continue;
            }

            setParam (proc, s.id, s.value);

            for (const auto& [key, id] : keys)
                if (id == s.id)
                    stepTokens.push_back (key + "@" + juce::String (s.time).toStdString() + "="
                                          + juce::String::formatted ("%.9g", (double) raw (id)).toStdString());
        }

        float* pp[2] { procOut.getWritePointer (0, start), procOut.getWritePointer (1, start) };
        juce::AudioBuffer<float> procView (pp, 2, n);
        proc.processBlock (procView, midi);

        // The processor runs its whole callback under ScopedNoDenormals; so must this.
        const juce::ScopedNoDenormals noDenormals;
        float* lp[2] { legacyOut.getWritePointer (0, start), legacyOut.getWritePointer (1, start) };
        juce::AudioBuffer<float> legacyView (lp, 2, n);
        juce::dsp::AudioBlock<float> block (legacyView);
        const bool on = present && raw (onId) > 0.5f;

        inputTrimLin.setTargetValue (juce::Decibels::decibelsToGain (raw ("input_trim")));
        applySmoothedGain (legacyView, n, inputTrimLin);

        switch (c.kind)
        {
            case LegacyKind::gate:
                if (on)
                {
                    gate.measure (block, n, raw ("gate_threshold"));
                    gate.apply (block, n);
                }
                break;

            case LegacyKind::gateDeferred:
                if (on)
                    gate.measure (block, n, raw ("gate_threshold"));

                // The dump is what tubamp_fxbench's deferred=1 renders: collapse, curve,
                // expand, without the amp block's gains around the (absent) model.
                for (int i = 0; i < n; ++i)
                {
                    float sum = 0.0f;

                    for (int ch = 0; ch < 2; ++ch)
                        sum += lp[ch][i];

                    plainMono[(size_t) i] = sum * (1.0f / 2.0f);
                }

                if (on)
                    gate.apply (plainMono.data(), n);

                // processAmpMono's collapse, expression for expression.
                for (int i = 0; i < n; ++i)
                {
                    float sum = 0.0f;

                    for (int ch = 0; ch < 2; ++ch)
                        sum += lp[ch][i];

                    mono[(size_t) i] = sum * (1.0f / 2.0f);
                }

                ampInLin.setTargetValue (juce::Decibels::decibelsToGain (raw ("amp_input") + 0.0f));
                ampOutLin.setTargetValue (juce::Decibels::decibelsToGain (raw ("amp_output") + 0.0f));

                for (int i = 0; i < n; ++i)
                    mono[(size_t) i] *= ampInLin.getNextValue();

                if (on)
                    gate.apply (mono.data(), n);

                for (int i = 0; i < n; ++i)
                    mono[(size_t) i] *= ampOutLin.getNextValue();

                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::copy (lp[ch], mono.data(), n);
                break;

            case LegacyKind::comp:
                comp.setParameters (raw (idOf ("comp", k, "threshold")), raw (idOf ("comp", k, "ratio")),
                                    raw (idOf ("comp", k, "attack")), raw (idOf ("comp", k, "release")),
                                    raw (idOf ("comp", k, "makeup")));
                // The processor's re-entry reset: the compressor, never the makeup.
                if (reentered)
                    comp.reset();

                if (on)
                    comp.process (block);
                else
                    comp.skip (n);
                break;

            case LegacyKind::drive:
                if (on)
                {
                    drive.setParameters (raw (idOf ("drive", k, "gain")), raw (idOf ("drive", k, "tone")),
                                         raw (idOf ("drive", k, "level")));
                    drive.process (block);
                }
                break;

            case LegacyKind::eq:
                if (on && ! c.flat)
                {
                    eq.setParameters (raw (idOf ("eq", k, "bass")), raw (idOf ("eq", k, "mid")), raw (idOf ("eq", k, "treble")));
                    eq.process (block);
                }
                break;

            case LegacyKind::ampEq:
                for (int i = 0; i < n; ++i)
                {
                    float sum = 0.0f;

                    for (int ch = 0; ch < 2; ++ch)
                        sum += lp[ch][i];

                    mono[(size_t) i] = sum * (1.0f / 2.0f);
                }

                ampInLin.setTargetValue (juce::Decibels::decibelsToGain (raw ("amp_input") + 0.0f));
                ampOutLin.setTargetValue (juce::Decibels::decibelsToGain (raw ("amp_output") + 0.0f));

                for (int i = 0; i < n; ++i)
                    mono[(size_t) i] *= ampInLin.getNextValue();

                if (on && ! c.flat)
                {
                    float* monoPtr = mono.data();
                    ampEq.setParameters (raw ("amp_eq_bass"), raw ("amp_eq_mid"), raw ("amp_eq_treble"));
                    ampEq.process (juce::dsp::AudioBlock<float> (&monoPtr, 1, (size_t) n));
                }

                for (int i = 0; i < n; ++i)
                    mono[(size_t) i] *= ampOutLin.getNextValue();

                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::copy (lp[ch], mono.data(), n);
                break;

            case LegacyKind::mod:
            {
                // The old block as it ran: one instance, reset and jump on a type change.
                float* jp[2] { jumpPreDc.getWritePointer (0, start), jumpPreDc.getWritePointer (1, start) };

                for (int ch = 0; ch < 2; ++ch)
                    juce::FloatVectorOperations::copy (jp[ch], lp[ch], n);

                if (reentered)
                {
                    modulation.reset();
                    modCrossfade.reset();
                }

                if (on)
                {
                    const int type = juce::jlimit (0, 2, (int) raw (idOf ("mod", k, "type")));
                    const float rate = raw (idOf ("mod", k, "rate")), depth = raw (idOf ("mod", k, "depth")),
                                mix = raw (idOf ("mod", k, "mix"));
                    modulation.setParameters ((legacy::Modulation::Type) type, rate, depth, mix);
                    modulation.process (juce::dsp::AudioBlock<float> (jp, 2, (size_t) n));

                    if (modCrossfade.process (block, type, rate, depth, mix))
                        fadeSpans.push_back ({ start, start + modCrossfade.fadeLength });
                }
                break;
            }
        }

        for (int ch = 0; ch < 2; ++ch)
            juce::FloatVectorOperations::copy (legacyPreDc.getWritePointer (ch, start),
                                               c.kind == LegacyKind::gateDeferred ? plainMono.data() : lp[ch], n);

        dc.process (block);

        outputLevelLin.setTargetValue (juce::Decibels::decibelsToGain (raw ("output_level")));
        applySmoothedGain (legacyView, n, outputLevelLin);
    }

    LegacyOutcome outcome;

    if (c.kind == LegacyKind::mod)
    {
        std::vector<bool> inFade ((size_t) numFrames, false);

        for (const auto& [from, to] : fadeSpans)
            for (int i = from; i < juce::jmin (to, numFrames); ++i)
                inFade[(size_t) i] = true;

        outcome.fades = (int) fadeSpans.size();

        for (int i = 0; i < numFrames; ++i)
        {
            outcome.fadeSamples += inFade[(size_t) i] ? 2 : 0;

            for (int ch = 0; ch < 2; ++ch)
            {
                const float a = legacyPreDc.getSample (ch, i), b = jumpPreDc.getSample (ch, i);

                if (std::memcmp (&a, &b, sizeof (float)) != 0)
                    ++(inFade[(size_t) i] ? outcome.fadeDiffering : outcome.outsideFadeDiffering);
            }
        }
    }

    const int compared = c.identicalUntil < 0.0 ? numFrames
                                                : juce::jmin (numFrames, (int) std::llround (c.identicalUntil * cfg.sampleRate));
    outcome.total = (long) compared * 2;

    for (int i = 0; i < compared; ++i)
        for (int ch = 0; ch < 2; ++ch)
        {
            const float a = procOut.getSample (ch, i), b = legacyOut.getSample (ch, i);

            if (std::memcmp (&a, &b, sizeof (float)) != 0 && outcome.differing++ == 0)
            {
                outcome.firstFrame = i;
                outcome.processorValue = a;
                outcome.legacyValue = b;
            }
        }

    if (dumpDir != juce::File() && c.dumpable && cfg.sampleRate == 48000.0 && cfg.block == 512)
    {
        const auto wav = dumpDir.getChildFile (juce::String (c.name) + ".wav");
        wav.deleteFile();
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream (wav.createOutputStream());
        const auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                .withSampleRate (cfg.sampleRate)
                                                                .withNumChannels (2)
                                                                .withBitsPerSample (32)
                                                                .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer != nullptr)
            writer->writeFromAudioSampleBuffer (legacyPreDc, 0, numFrames);

        const char* effect = c.kind == LegacyKind::comp ? "comp" : c.kind == LegacyKind::drive ? "drive"
                           : c.kind == LegacyKind::eq || c.kind == LegacyKind::ampEq ? "eq"
                           : c.kind == LegacyKind::mod ? "mod" : "gate";
        juce::String cmd;

        if (c.kind == LegacyKind::mod)
        {
            static constexpr const char* modEffects[] { "chorus", "phaser", "tremolo" };
            effect = modEffects[juce::jlimit (0, 2, (int) std::lround (c.settings.front().second))];
        }

        cmd << c.name << "\t" << effect << " gen:music " << c.name << ".fxbench.wav --legacy --sr=48000 --block=512";

        if (c.kind == LegacyKind::gateDeferred)
            cmd << " deferred=1";

        // The values each key had at the first block, before any step: replay the steps
        // backwards from the end state is not possible, so read them on a fresh instance.
        tubamp::TubampAudioProcessor fresh;

        for (const auto& [id, value] : c.settings)
            setParam (fresh, id, value);

        for (const auto& [key, id] : keys)
            cmd << " " << key << "=" << juce::String::formatted ("%.9g", (double) fresh.apvts.getRawParameterValue (id)->load());

        for (const auto& token : stepTokens)
            cmd << " " << token;

        commands << cmd << "\n";
    }

    return outcome;
}

int legacyCheck (const juce::File& dumpDir)
{
    const std::vector<Config> configs { { 48000.0, 512 }, { 48000.0, 37 }, { 44100.0, 512 }, { 96000.0, 512 } };
    juce::String commands;
    int failures = 0, runs = 0;

    if (dumpDir != juce::File())
        dumpDir.createDirectory();

    for (const auto& c : buildLegacyCases())
    {
        for (const auto& cfg : configs)
        {
            const auto r = runLegacyCase (c, cfg, dumpDir, commands);
            ++runs;
            const bool bad = r.differing != 0 || r.outsideFadeDiffering != 0;
            failures += bad ? 1 : 0;
            const juce::String note = c.flat ? "  (D2 a: against the legacy chain without the EQ)"
                                    : c.identicalUntil >= 0.0 ? juce::String::formatted ("  (D-4: up to the first knob step, %.1f s)",
                                                                                         c.identicalUntil)
                                    : r.fades > 0 ? juce::String::formatted ("  (D-5: %d crossfades, %ld samples; %ld of them differ from "
                                                                             "the old jump, %ld outside them)",
                                                                             r.fades, r.fadeSamples, r.fadeDiffering,
                                                                             r.outsideFadeDiffering)
                                                  : juce::String();
            if (r.differing == 0 && r.outsideFadeDiffering == 0)
                std::printf ("ok   %-18s %6d %4d  0 of %ld samples differ%s\n", c.name.c_str(), (int) cfg.sampleRate,
                             cfg.block, r.total, note.toRawUTF8());
            else
                std::printf ("FAIL %-18s %6d %4d  %ld of %ld samples differ (first frame %ld: processor %.9g, legacy %.9g)%s\n",
                             c.name.c_str(), (int) cfg.sampleRate, cfg.block, r.differing, r.total, r.firstFrame,
                             (double) r.processorValue, (double) r.legacyValue, note.toRawUTF8());
            std::fflush (stdout);
        }
    }

    if (dumpDir != juce::File())
        dumpDir.getChildFile ("fxbench-commands.txt").replaceWithText (commands, false, false, "\n");

    std::printf ("legacy-check: %d renders, %d with differences\n", runs, failures);
    return failures == 0 ? 0 : 1;
}

int usage()
{
    std::printf ("usage:\n"
                 "  tubamp_golden --capture <file.tsv> [--model <a.nam>] [--ir <ir.wav>] [--only <glob>]\n"
                 "  tubamp_golden --verify <file.tsv> [--expected <expected-changes.tsv>] [--model ...] [--ir ...] [--only <glob>]\n"
                 "  tubamp_golden --params <file.tsv>\n"
                 "  tubamp_golden --legacy-check [--dump <dir>]\n"
                 "  tubamp_golden --list [--model ...] [--ir ...]\n");
    return 2;
}
} // namespace

int main (int argc, char** argv)
{
    const juce::ScopedJuceInitialiser_GUI juceInit;

    std::map<std::string, std::string> opts;

    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];

        if (a == "--list" || a == "--legacy-check")
            opts[a] = "1";
        else if (a.rfind ("--", 0) == 0 && i + 1 < argc)
            opts[a] = argv[++i];
        else
            return usage();
    }

    const auto opt = [&opts] (const char* key) { const auto it = opts.find (key); return it != opts.end() ? it->second : std::string(); };
    const auto cwdFile = [] (const std::string& path) { return juce::File::getCurrentWorkingDirectory().getChildFile (path); };

    if (! opt ("--params").empty())
        return writeParams (cwdFile (opt ("--params")));

    if (! opt ("--legacy-check").empty())
        return legacyCheck (opt ("--dump").empty() ? juce::File() : cwdFile (opt ("--dump")));

    Inputs inputs;

    if (! opt ("--model").empty())
        inputs.model = cwdFile (opt ("--model"));

    if (! opt ("--ir").empty())
        inputs.ir = cwdFile (opt ("--ir"));

    if ((inputs.model != juce::File() && ! inputs.model.existsAsFile())
        || (inputs.ir != juce::File() && ! inputs.ir.existsAsFile()))
    {
        std::fprintf (stderr, "model or IR file not found\n");
        return 2;
    }

    const auto cases = buildCases (inputs.model != juce::File(), inputs.ir != juce::File());
    const juce::String only (opt ("--only"));
    const auto selected = [&only] (const std::string& name) { return only.isEmpty() || juce::String (name).matchesWildcard (only, false); };

    if (! opt ("--list").empty())
    {
        int rows = 0;

        for (const auto& c : cases)
        {
            std::printf ("%-18s %zu configs  %s\n", c.name.c_str(), configsFor (c).size(),
                         chain::toString (c.order).toRawUTF8());
            rows += (int) configsFor (c).size();
        }

        std::printf ("%zu cases, %d rows\n", cases.size(), rows);
        return 0;
    }

    if (! opt ("--capture").empty())
    {
        const auto file = cwdFile (opt ("--capture"));
        juce::String out;
        out << "# tubamp_golden capture: case, fs, block, sha256 of the interleaved float output, latency after the last block\n";

        if (inputs.model != juce::File())
            out << "# model " << fileSha (inputs.model) << "\t" << inputs.model.getFileName() << "\n";

        if (inputs.ir != juce::File())
            out << "# ir " << fileSha (inputs.ir) << "\t" << inputs.ir.getFileName() << "\n";

        int count = 0;

        for (const auto& c : cases)
        {
            if (! selected (c.name))
                continue;

            for (const auto& cfg : configsFor (c))
            {
                const auto r = render (c, cfg, inputs);
                out << rowKey (c.name, cfg.sampleRate, cfg.block) << "\t" << r.hash << "\t" << r.latency << "\n";
                std::printf ("%-18s %6d %4d  %s  %d\n", c.name.c_str(), (int) cfg.sampleRate, cfg.block,
                             r.hash.substr (0, 16).c_str(), r.latency);
                std::fflush (stdout);
                ++count;
            }
        }

        if (! file.replaceWithText (out, false, false, "\n"))
        {
            std::fprintf (stderr, "cannot write %s\n", file.getFullPathName().toRawUTF8());
            return 2;
        }

        std::printf ("captured %d rows -> %s\n", count, file.getFullPathName().toRawUTF8());
        return 0;
    }

    if (! opt ("--verify").empty())
    {
        GoldenFile golden;

        if (! readGolden (cwdFile (opt ("--verify")), golden))
        {
            std::fprintf (stderr, "cannot read %s\n", opt ("--verify").c_str());
            return 2;
        }

        if (! golden.modelSha.empty() && inputs.model != juce::File() && fileSha (inputs.model) != golden.modelSha)
        {
            std::fprintf (stderr, "the model's SHA-256 differs from the one the capture used\n");
            return 2;
        }

        std::vector<Expectation> expected;

        if (! opt ("--expected").empty())
            expected = readExpected (cwdFile (opt ("--expected")));

        std::map<std::string, const Case*> byName;

        for (const auto& c : cases)
            byName[c.name] = &c;

        std::map<std::string, std::string> capturedHash;

        for (const auto& row : golden.rows)
            capturedHash[rowKey (row.name, row.fs, row.block)] = row.hash;

        int checked = 0, differences = 0, skipped = 0, identical = 0, changed = 0;

        for (const auto& row : golden.rows)
        {
            if (! selected (row.name))
                continue;

            const auto it = byName.find (row.name);

            if (it == byName.end())
            {
                const bool needsInput = row.name == "ampM" || row.name == "gateampM" || row.name == "cabIR";
                std::printf ("%s %-18s %6d %4d  %s\n", needsInput ? "SKIP" : "FAIL", row.name.c_str(), row.fs, row.block,
                             needsInput ? "needs --model / --ir" : "unknown case");
                needsInput ? ++skipped : ++differences;
                continue;
            }

            const auto r = render (*it->second, { (double) row.fs, row.block }, inputs);
            juce::String audioRule ("same"), latencyRule ("same");

            for (const auto& e : expected)
            {
                if (juce::String (row.name).matchesWildcard (e.pattern, false))
                {
                    audioRule = e.audio;
                    latencyRule = e.latency;
                    break;
                }
            }

            std::string wantHash = row.hash;
            bool audioOk = true;

            if (audioRule == "any")
            {
                audioOk = true;
            }
            else if (audioRule == "differs")
            {
                wantHash = "!" + row.hash;
                audioOk = r.hash != row.hash;
            }
            else if (audioRule.startsWith ("off:"))
            {
                auto variant = *it->second;
                juce::StringArray ids;
                ids.addTokens (audioRule.fromFirstOccurrenceOf ("off:", false, false), ",", "");

                for (const auto& id : ids)
                    variant.settings.push_back ({ id.trim().toStdString(), 0.0f });

                wantHash = render (variant, { (double) row.fs, row.block }, inputs).hash;
                audioOk = r.hash == wantHash;
                std::printf ("     %-18s %6d %4d  off-variant %s\n", row.name.c_str(), row.fs, row.block, wantHash.c_str());
            }
            else if (audioRule.startsWith ("same_as:"))
            {
                const auto other = rowKey (audioRule.fromFirstOccurrenceOf ("same_as:", false, false).toStdString(), row.fs, row.block);
                wantHash = capturedHash.count (other) > 0 ? capturedHash[other] : std::string ("(no such row)");
                audioOk = r.hash == wantHash;
            }
            else
            {
                audioOk = r.hash == wantHash;
            }

            const int wantLatency = latencyRule.startsWith ("+") ? row.latency + latencyRule.substring (1).getIntValue()
                                                                 : row.latency;
            const bool latencyOk = r.latency == wantLatency;
            ++checked;

            if (! audioOk || ! latencyOk)
            {
                ++differences;
                std::printf ("FAIL %-18s %6d %4d  audio %s (want %s, got %s)  latency %d (want %d)\n",
                             row.name.c_str(), row.fs, row.block, audioOk ? "ok" : "DIFFERS",
                             wantHash.substr (0, 16).c_str(), r.hash.substr (0, 16).c_str(), r.latency, wantLatency);
            }
            else if (r.hash == row.hash && r.latency == row.latency)
            {
                ++identical;
                std::printf ("ok   %-18s %6d %4d\n", row.name.c_str(), row.fs, row.block);
            }
            else
            {
                ++changed;
                std::printf ("chg  %-18s %6d %4d  audio %s, latency %d -> %d (as expected: %s / %s)\n", row.name.c_str(),
                             row.fs, row.block, r.hash == row.hash ? "same" : "changed", row.latency, r.latency,
                             audioRule.toRawUTF8(), latencyRule.toRawUTF8());
            }

            std::fflush (stdout);
        }

        std::printf ("verify: %d rows checked: %d identical, %d changed as expected, %d unexpected; %d skipped\n",
                     checked, identical, changed, differences, skipped);
        return differences == 0 && checked > 0 ? 0 : 1;
    }

    return usage();
}
