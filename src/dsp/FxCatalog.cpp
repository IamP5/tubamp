#include "FxCatalog.h"

#if JUCE_PLUGINHOST_AU
 #include <AudioToolbox/AudioToolbox.h>
#endif

namespace tubamp
{
namespace
{
#if JUCE_PLUGINHOST_AU
/** tubamp's own AU identity. Loading ourselves into one of our own slots would
    recurse a whole processor (and its factory-preset filesystem work) per instance;
    the codes are frozen in CMakeLists.txt (PLUGIN_CODE / PLUGIN_MANUFACTURER_CODE). */
constexpr OSType kSelfSubType = 'Tamp';
constexpr OSType kSelfManufacturer = 'Tuba';

juce::String osTypeToString (OSType type)
{
    const juce::juce_wchar s[4] { (juce::juce_wchar) ((type >> 24) & 0xff),
                                  (juce::juce_wchar) ((type >> 16) & 0xff),
                                  (juce::juce_wchar) ((type >> 8) & 0xff),
                                  (juce::juce_wchar) (type & 0xff) };
    return juce::String (s, 4);
}

/** Byte-identical to AudioUnitFormatHelpers::createPluginIdentifier
    (juce_AudioUnitPluginFormatImpl.h:98-121) for the component types we accept.
    That function is internal to the module, so it is mirrored rather than called. */
juce::String makeIdentifier (const AudioComponentDescription& desc)
{
    juce::String s ("AudioUnit:");

    if (desc.componentType == kAudioUnitType_MusicEffect || desc.componentType == kAudioUnitType_Effect)
        s << "Effects/";

    s << osTypeToString (desc.componentType) << ","
      << osTypeToString (desc.componentSubType) << ","
      << osTypeToString (desc.componentManufacturer);

    return s;
}

void splitName (juce::String& name, juce::String& manufacturer)
{
    // AudioComponentCopyName gives "Manufacturer: Name" by convention.
    if (name.containsChar (':'))
    {
        manufacturer = name.upToFirstOccurrenceOf (":", false, false).trim();
        name = name.fromFirstOccurrenceOf (":", false, false).trim();
    }

    if (name.isEmpty())
        name = "<Unknown>";
}
#endif
} // namespace

//==============================================================================
struct FxCatalog::Impl
{
    juce::AudioPluginFormatManager formatManager;
};

FxCatalog::FxCatalog()
    : impl (std::make_unique<Impl>())
{
   #if JUCE_PLUGINHOST_AU
    // Only the AU format, and only added here — never anything that scans at
    // construction time. auval instantiates the plugin in a strict harness and a
    // constructor that walks plugin directories is a classic way to fail it.
    impl->formatManager.addFormat (new juce::AudioUnitPluginFormat());
   #endif
}

FxCatalog::~FxCatalog() = default;

bool FxCatalog::isSupported() noexcept
{
   #if JUCE_PLUGINHOST_AU
    return true;
   #else
    return false;
   #endif
}

//==============================================================================
std::vector<FxCatalog::Entry> FxCatalog::enumerateEffects() const
{
    std::vector<Entry> entries;

   #if JUCE_PLUGINHOST_AU
    AudioComponentDescription search {};
    search.componentType = 0;        // any — filtered below
    search.componentSubType = 0;
    search.componentManufacturer = 0;

    AudioComponent component = nullptr;

    while ((component = AudioComponentFindNext (component, &search)) != nullptr)
    {
        AudioComponentDescription desc {};

        if (AudioComponentGetDescription (component, &desc) != noErr)
            continue;

        // Effects only. Instruments, generators, mixers, panners and MIDI processors
        // have no meaning as an insert in a guitar chain, and instantiating a sampler
        // by accident is exactly the kind of surprise this filter exists to prevent.
        if (desc.componentType != kAudioUnitType_Effect
            && desc.componentType != kAudioUnitType_MusicEffect)
            continue;

        // AUv3 components load out of process over XPC. Nesting that inside a plugin
        // that is itself already hosted out of process (Logic's AUHostingService) is
        // the configuration with the worst track record, so v1 skips them entirely.
        if ((desc.componentFlags & kAudioComponentFlag_IsV3AudioUnit) != 0)
            continue;

        if (desc.componentSubType == kSelfSubType && desc.componentManufacturer == kSelfManufacturer)
            continue;

        Entry entry;
        entry.identifier = makeIdentifier (desc);

        {
            CFStringRef cfName = nullptr;

            if (AudioComponentCopyName (component, &cfName) == noErr && cfName != nullptr)
            {
                entry.name = juce::String::fromCFString (cfName);
                CFRelease (cfName);
            }
        }

        splitName (entry.name, entry.manufacturer);

        if (entry.manufacturer.isEmpty())
            entry.manufacturer = osTypeToString (desc.componentManufacturer);

        UInt32 versionNumber = 0;

        if (AudioComponentGetVersion (component, &versionNumber) == noErr)
            entry.version << (int) (versionNumber >> 16) << "."
                          << (int) ((versionNumber >> 8) & 0xff) << "."
                          << (int) (versionNumber & 0xff);

        entries.push_back (std::move (entry));
    }

    std::sort (entries.begin(), entries.end(), [] (const Entry& a, const Entry& b)
    {
        const auto byMaker = a.manufacturer.compareIgnoreCase (b.manufacturer);
        return byMaker != 0 ? byMaker < 0 : a.name.compareIgnoreCase (b.name) < 0;
    });
   #endif

    return entries;
}

juce::PluginDescription FxCatalog::descriptionFor (const juce::String& identifier) const
{
    juce::PluginDescription description;
    description.pluginFormatName = "AudioUnit";
    description.fileOrIdentifier = identifier;

   #if JUCE_PLUGINHOST_AU
    // Fill in the display fields from the registry so a slot restored from state can
    // name its plugin without instantiating it.
    for (const auto& entry : enumerateEffects())
    {
        if (entry.identifier == identifier)
        {
            description.name = entry.name;
            description.descriptiveName = entry.name;
            description.manufacturerName = entry.manufacturer;
            description.version = entry.version;
            description.category = "Effect";
            description.isInstrument = false;
            break;
        }
    }
   #endif

    return description;
}

void FxCatalog::createAsync (const juce::PluginDescription& description,
                             double sampleRate, int blockSize, Callback callback)
{
   #if JUCE_PLUGINHOST_AU
    impl->formatManager.createPluginInstanceAsync (
        description, sampleRate, blockSize,
        [cb = std::move (callback)] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                     const juce::String& error)
        {
            cb (std::move (instance), error);
        });
   #else
    juce::ignoreUnused (description, sampleRate, blockSize);
    callback (nullptr, "This build has no plugin hosting support");
   #endif
}
} // namespace tubamp
