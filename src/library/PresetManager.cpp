#include "PresetManager.h"

#include "../Parameters.h"
#include "../PluginProcessor.h"
#include "ModelLibrary.h"

#include <algorithm>

namespace tubamp
{
namespace
{
juce::var buildPresetVar (const juce::String& name, const juce::StringArray& tags,
                          bool favorite, const juce::var& state)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name", name);

    juce::Array<juce::var> tagsVar;
    for (auto& t : tags)
        tagsVar.add (t);
    obj->setProperty ("tags", tagsVar);

    obj->setProperty ("favorite", favorite);
    obj->setProperty ("state", state);
    return juce::var (obj);
}

juce::File presetFileForName (const juce::String& name)
{
    return ModelLibrary::getPresetsDir().getChildFile (juce::File::createLegalFileName (name) + ".json");
}
} // namespace

PresetManager::PresetManager (TubampAudioProcessor& processor) : proc (processor)
{
}

juce::Array<PresetManager::PresetInfo> PresetManager::getPresets() const
{
    juce::Array<PresetInfo> result;

    for (const auto& file : ModelLibrary::getPresetsDir().findChildFiles (juce::File::findFiles, false, "*.json"))
    {
        auto parsed = juce::JSON::parse (file);
        if (parsed.getDynamicObject() == nullptr)
            continue;

        PresetInfo info;
        info.file = file;
        info.name = parsed.getProperty ("name", file.getFileNameWithoutExtension()).toString();
        info.favorite = (bool) parsed.getProperty ("favorite", false);

        if (auto* arr = parsed.getProperty ("tags", juce::var()).getArray())
            for (auto& t : *arr)
                info.tags.add (t.toString());

        result.add (info);
    }

    std::sort (result.begin(), result.end(), [] (const PresetInfo& a, const PresetInfo& b)
    {
        return a.name.compareIgnoreCase (b.name) < 0;
    });

    return result;
}

juce::String PresetManager::getCurrentPresetName() const
{
    return currentName;
}

juce::Result PresetManager::savePreset (const juce::String& name)
{
    if (name.trim().isEmpty())
        return juce::Result::fail ("Preset name cannot be empty.");

    const auto file = presetFileForName (name);

    // Overwriting an existing preset preserves its favorite flag and tags.
    juce::StringArray tags;
    bool favorite = false;

    if (file.existsAsFile())
    {
        auto existing = juce::JSON::parse (file);
        favorite = (bool) existing.getProperty ("favorite", false);

        if (auto* arr = existing.getProperty ("tags", juce::var()).getArray())
            for (auto& t : *arr)
                tags.add (t.toString());
    }

    ModelLibrary::getPresetsDir().createDirectory();

    const auto presetVar = buildPresetVar (name, tags, favorite, proc.captureStateVar());

    if (! file.replaceWithText (juce::JSON::toString (presetVar)))
        return juce::Result::fail ("Could not write preset file: " + file.getFullPathName());

    currentName = name;

    if (onPresetChanged != nullptr)
        onPresetChanged();

    return juce::Result::ok();
}

juce::Result PresetManager::loadPreset (const juce::File& presetFile)
{
    if (! presetFile.existsAsFile())
        return juce::Result::fail ("Preset file not found: " + presetFile.getFullPathName());

    auto parsed = juce::JSON::parse (presetFile);
    if (parsed.getDynamicObject() == nullptr)
        return juce::Result::fail ("Invalid preset file: " + presetFile.getFullPathName());

    proc.applyStateVar (parsed.getProperty ("state", juce::var()));
    currentName = parsed.getProperty ("name", presetFile.getFileNameWithoutExtension()).toString();

    if (onPresetChanged != nullptr)
        onPresetChanged();

    return juce::Result::ok();
}

juce::Result PresetManager::deletePreset (const juce::File& presetFile)
{
    if (! presetFile.existsAsFile())
        return juce::Result::fail ("Preset file not found: " + presetFile.getFullPathName());

    const auto parsed = juce::JSON::parse (presetFile);
    const auto deletedName = parsed.getProperty ("name", presetFile.getFileNameWithoutExtension()).toString();

    if (! presetFile.deleteFile())
        return juce::Result::fail ("Could not delete preset: " + presetFile.getFullPathName());

    if (deletedName == currentName)
        currentName = "Init";

    if (onPresetChanged != nullptr)
        onPresetChanged();

    return juce::Result::ok();
}

void PresetManager::setFavorite (const juce::File& presetFile, bool favorite)
{
    if (! presetFile.existsAsFile())
        return;

    auto parsed = juce::JSON::parse (presetFile);
    auto* obj = parsed.getDynamicObject();
    if (obj == nullptr)
        return;

    obj->setProperty ("favorite", favorite);

    if (presetFile.replaceWithText (juce::JSON::toString (parsed)) && onPresetChanged != nullptr)
        onPresetChanged();
}

void PresetManager::createFactoryPresetsIfMissing()
{
    const auto presetsDir = ModelLibrary::getPresetsDir();
    presetsDir.createDirectory();

    if (presetsDir.getNumberOfChildFiles (juce::File::findFiles, "*.json") > 0)
        return; // already initialised (or the user has their own presets)

    // Each factory preset is a whole rig on the classic arrangement, built from the parameter
    // defaults and its own settings, and referencing no model or IR. It is never staged on
    // the live parameters: this runs in the constructor of the first instance on a machine,
    // which must come up exactly as every later instance does.
    const auto writeFactoryPreset = [this] (const juce::String& name, std::vector<std::pair<juce::String, float>> values)
    {
        // Every engine type at Classic (docs/FX-ENGINES.md §8 D-2): installs that wrote
        // their presets before the types existed load them pinned to Classic, so a
        // fresh install writes the same sound rather than the recommended types.
        for (const auto& pin : kLegacyTypePins)
            for (int k = 0; k < chain::numInstancesOf (pin.kind); ++k)
                values.emplace_back (pin.ids[k], 0.0f);

        const auto state = proc.makeStateVar (chain::classicOrder(), values);
        presetFileForName (name).replaceWithText (juce::JSON::toString (buildPresetVar (name, {}, false, state)));
    };

    // 1. Init - untouched defaults.
    writeFactoryPreset ("Init", {});

    // 2. Clean + Room - a little ambience, nothing else engaged.
    writeFactoryPreset ("Clean + Room", { { params::reverbOn, 1.0f }, { params::reverbMix, 0.35f } });

    // 3. Crunch Drive - drive block on, moderate gain.
    writeFactoryPreset ("Crunch Drive", { { params::driveOn, 1.0f }, { params::driveGain, 18.0f } });

    // 4. Ambient Lead - delay + reverb + modulation, all engaged, over Crunch Drive's drive:
    // the old staging restored the live state only once, so every install so far wrote it
    // that way, and the factory sound stays the same everywhere (D-2).
    writeFactoryPreset ("Ambient Lead", { { params::driveOn, 1.0f }, { params::driveGain, 18.0f },
                                          { params::delayOn, 1.0f }, { params::delayMix, 0.35f },
                                          { params::reverbOn, 1.0f }, { params::reverbMix, 0.4f },
                                          { params::modOn, 1.0f } });
}

void PresetManager::captureToSlot (int slot)
{
    jassert (slot == 0 || slot == 1);
    slots[slot] = proc.captureStateVar();
    activeSlot = slot;
}

void PresetManager::recallSlot (int slot)
{
    jassert (slot == 0 || slot == 1);
    if (! slotHasState (slot))
        return;

    proc.applyStateVar (slots[slot]);
    activeSlot = slot;
}

bool PresetManager::slotHasState (int slot) const
{
    jassert (slot == 0 || slot == 1);
    return ! slots[slot].isVoid();
}

int PresetManager::getActiveSlot() const
{
    return activeSlot;
}
} // namespace tubamp
