#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>
#include <vector>

namespace tubamp
{
/**
    Discovery and instantiation of third-party AudioUnit effects.

    Discovery is deliberately metadata-only. JUCE's PluginDirectoryScanner builds its
    descriptions by *fully instantiating* every component it finds
    (juce_AudioUnitPluginFormatHeadless.mm:42-67 calls createInstanceFromDescription),
    which inside a plugin means loading every AudioUnit installed on the machine into
    the host's address space — including the ones the host's own validation already
    rejected, and including instruments the user will never pick. Its try/catch only
    catches C++ exceptions, so one bad component takes the session down.

    Everything the picker needs (name, manufacturer, version) is available from the
    component registry without instantiating anything: AudioComponentFindNext +
    AudioComponentGetDescription + AudioComponentCopyName + AudioComponentGetVersion.
    The identifier we synthesize is byte-identical to JUCE's own
    AudioUnitFormatHelpers::createPluginIdentifier, which is the only field
    createPluginInstanceAsync actually needs. So exactly one component is ever loaded:
    the one the user picked.
*/
class FxCatalog
{
public:
    FxCatalog();
    ~FxCatalog();

    struct Entry
    {
        /** PluginDescription::fileOrIdentifier — "AudioUnit:Effects/aufx,subt,manu". */
        juce::String identifier;
        juce::String name;
        juce::String manufacturer;
        juce::String version;
    };

    /** Enumerates installed AUv2 effects, sorted by manufacturer then name. Cheap
        (a registry walk, no plugin code runs), so it is simply called on demand
        rather than cached to disk. Message thread. */
    std::vector<Entry> enumerateEffects() const;

    /** Look up one entry by identifier. */
    juce::PluginDescription descriptionFor (const juce::String& identifier) const;

    /** Asynchronously instantiates `description`. The callback runs on the message
        thread with either an instance or a non-empty error. */
    using Callback = std::function<void (std::unique_ptr<juce::AudioProcessor>, const juce::String& error)>;

    void createAsync (const juce::PluginDescription& description,
                      double sampleRate, int blockSize, Callback callback);

    /** True when this build can host anything at all (JUCE_PLUGINHOST_AU). */
    static bool isSupported() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxCatalog)
};
} // namespace tubamp
