#include "ToneSidecar.h"

namespace tubamp
{
juce::File ToneSidecar::fileFor (const juce::File& media)
{
    return media.getSiblingFile (media.getFileName() + ".t3k.json");
}

std::optional<ToneSidecar> ToneSidecar::read (const juce::File& media)
{
    const auto file = fileFor (media);

    if (! file.existsAsFile())
        return std::nullopt;

    const auto parsed = juce::JSON::parse (file.loadFileAsString());

    if (parsed.getDynamicObject() == nullptr)
        return std::nullopt;

    return fromVar (parsed);
}

bool ToneSidecar::write (const juce::File& media) const
{
    return fileFor (media).replaceWithText (juce::JSON::toString (toVar()));
}

void ToneSidecar::remove (const juce::File& media)
{
    fileFor (media).deleteFile();
}

juce::String ToneSidecar::toneUrl() const
{
    return toneId > 0 ? "https://www.tone3000.com/tones/" + juce::String (toneId) : juce::String();
}

juce::var ToneSidecar::toVar() const
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("toneId", toneId);
    object->setProperty ("modelId", modelId);
    object->setProperty ("title", title);
    object->setProperty ("modelName", modelName);
    object->setProperty ("creator", creator);
    object->setProperty ("gear", gear);
    object->setProperty ("format", format);
    object->setProperty ("size", size);
    object->setProperty ("architecture", architecture);
    object->setProperty ("license", license);
    return juce::var (object);
}

ToneSidecar ToneSidecar::fromVar (const juce::var& object)
{
    ToneSidecar sidecar;

    // int64 round-trips through juce::var as either int, int64 or double depending
    // on who wrote it (the WebView hands numbers over as doubles).
    auto idOf = [&object] (const char* key)
    {
        const auto value = object[key];
        return value.isDouble() ? (juce::int64) (double) value : (juce::int64) value;
    };

    sidecar.toneId       = idOf ("toneId");
    sidecar.modelId      = idOf ("modelId");
    sidecar.title        = object["title"].toString();
    sidecar.modelName    = object["modelName"].toString();
    sidecar.creator      = object["creator"].toString();
    sidecar.gear         = object["gear"].toString();
    sidecar.format       = object["format"].toString();
    sidecar.size         = object["size"].toString();
    sidecar.architecture = object["architecture"].toString();
    sidecar.license      = object["license"].toString();
    return sidecar;
}
} // namespace tubamp
