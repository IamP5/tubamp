#include "ToneSidecar.h"

#include <cstring>

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
    artFileFor (media).deleteFile();
}

juce::File ToneSidecar::artFileFor (const juce::File& media)
{
    return media.getSiblingFile (media.getFileName() + ".t3k.art");
}

const char* ToneSidecar::imageMime (const void* data, size_t numBytes) noexcept
{
    const auto* bytes = static_cast<const unsigned char*> (data);
    const auto startsWith = [&] (size_t offset, const char* text)
    {
        const auto length = std::strlen (text);
        return numBytes >= offset + length && std::memcmp (bytes + offset, text, length) == 0;
    };

    if (startsWith (0, "\xff\xd8\xff"))               return "image/jpeg";
    if (startsWith (0, "\x89PNG\r\n\x1a\n"))          return "image/png";
    if (startsWith (0, "GIF8"))                       return "image/gif";
    if (startsWith (0, "RIFF") && startsWith (8, "WEBP")) return "image/webp";
    if (startsWith (4, "ftypavif"))                   return "image/avif";
    return nullptr;
}

bool ToneSidecar::fetchArt (const juce::String& imageUrl, const juce::File& media)
{
    constexpr int kTimeoutMs = 6000;
    constexpr size_t kMaxBytes = 8 * 1024 * 1024;

    if (! imageUrl.startsWithIgnoreCase ("https://"))
        return false;

    int statusCode = 0;
    const auto stream = juce::URL (imageUrl).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (kTimeoutMs)
            .withStatusCode (&statusCode));

    if (stream == nullptr || statusCode != 200)
        return false;

    juce::MemoryBlock data;
    const auto read = stream->readIntoMemoryBlock (data, (juce::int64) kMaxBytes + 1);

    if (read == 0 || data.getSize() > kMaxBytes || imageMime (data.getData(), data.getSize()) == nullptr)
        return false;

    return artFileFor (media).replaceWithData (data.getData(), data.getSize());
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
    object->setProperty ("imageUrl", imageUrl);
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
    sidecar.imageUrl     = object["imageUrl"].toString();
    return sidecar;
}
} // namespace tubamp
