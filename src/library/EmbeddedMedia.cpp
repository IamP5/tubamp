#include "EmbeddedMedia.h"
#include "ModelLibrary.h"
#include "ToneSidecar.h"

namespace tubamp
{
const juce::Identifier EmbeddedMedia::treeType { "MEDIA" };
const juce::Identifier EmbeddedMedia::slotProperty { "slot" };

namespace
{
const juce::Identifier fileProperty { "file" };
const juce::Identifier dataProperty { "data" };
const juce::Identifier sidecarProperty { "sidecar" };
const juce::Identifier sizeProperty { "size" };
const juce::Identifier checksumProperty { "fnv" };

/** FNV-1a, 64 bit, as hex. An integrity check against truncation and bit rot, not
    a defence against anyone: whoever can edit the blob can edit this too. */
juce::String checksumOf (const void* data, size_t numBytes)
{
    juce::uint64 hash = 1469598103934665603ULL;
    const auto* bytes = static_cast<const juce::uint8*> (data);

    for (size_t i = 0; i < numBytes; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }

    return juce::String::toHexString ((juce::int64) hash);
}

bool sameContent (const juce::File& file, const juce::MemoryBlock& bytes)
{
    juce::MemoryBlock existing;
    return file.getSize() == (juce::int64) bytes.getSize()
           && file.loadFileAsData (existing) && existing == bytes;
}

// .nam is JSON full of decimal floats and compresses to well under half; a fast
// level gets nearly all of that. Saving must not stall on megabytes of deflate.
constexpr int kCompressionLevel = 3;
constexpr int kMaxSidecarChars = 16 * 1024;
} // namespace

EmbeddedMedia EmbeddedMedia::fromFile (const juce::File& file)
{
    EmbeddedMedia media;

    if (! file.existsAsFile() || file.getSize() > maxFileBytes)
        return media;

    // Bounded on its own account: the size a path reports is not a promise (a
    // device node says 0 and never ends).
    juce::MemoryBlock raw;

    {
        juce::FileInputStream in (file);

        if (! in.openedOk())
            return media;

        in.readIntoMemoryBlock (raw, maxFileBytes + 1);
    }

    if (raw.isEmpty() || (juce::int64) raw.getSize() > maxFileBytes)
        return media;

    juce::MemoryOutputStream packed;

    {
        juce::GZIPCompressorOutputStream deflate (packed, kCompressionLevel);
        deflate.write (raw.getData(), raw.getSize());
    }

    media.fileName = file.getFileName();
    media.data = juce::Base64::toBase64 (packed.getData(), packed.getDataSize());
    media.size = (juce::int64) raw.getSize();
    media.checksum = checksumOf (raw.getData(), raw.getSize());

    if (const auto sidecar = ToneSidecar::fileFor (file); sidecar.existsAsFile())
        media.sidecar = sidecar.loadFileAsString();

    return media;
}

bool EmbeddedMedia::decode (juce::MemoryBlock& out) const
{
    out.reset();

    if (data.isEmpty())
        return false;

    juce::MemoryOutputStream packed;

    if (! juce::Base64::convertFromBase64 (packed, data))
        return false;

    juce::MemoryInputStream source (packed.getData(), packed.getDataSize(), false);
    juce::GZIPDecompressorInputStream inflate (source);
    juce::MemoryOutputStream unpacked;

    // Bounded: a state blob is untrusted input, and a few kilobytes of deflate can
    // expand without limit.
    unpacked.writeFromInputStream (inflate, maxFileBytes + 1);

    if (unpacked.getDataSize() == 0 || (juce::int64) unpacked.getDataSize() > maxFileBytes)
        return false;

    // A deflate stream that was cut short still inflates to a prefix of the file.
    if (size > 0 && (juce::int64) unpacked.getDataSize() != size)
        return false;

    if (checksum.isNotEmpty() && checksumOf (unpacked.getData(), unpacked.getDataSize()) != checksum)
        return false;

    out.append (unpacked.getData(), unpacked.getDataSize());
    return true;
}

juce::File EmbeddedMedia::materialiseIn (const juce::File& directory, const juce::String& extension,
                                        bool* created) const
{
    if (created != nullptr)
        *created = false;

    // The name comes out of a state blob: keep only its leaf, never a path, and
    // nothing the file system gives a meaning of its own (".." and hidden files, a
    // leading "~" that juce::File would expand to a home directory).
    const auto leaf = juce::File::createLegalFileName (fileName.replaceCharacter ('\\', '/')
                                                               .fromLastOccurrenceOf ("/", false, false));

    juce::MemoryBlock bytes;

    if (leaf.isEmpty() || leaf.startsWithChar ('.') || leaf.startsWithChar ('~')
        || ! leaf.endsWithIgnoreCase ("." + extension) || ! decode (bytes))
        return {};

    directory.createDirectory();
    auto target = directory.getChildFile (leaf);

    if (target.existsAsFile())
    {
        if (sameContent (target, bytes))
            return target;

        // The name is taken by something else. An earlier restore may already have
        // put this file in under a numbered name ("name 2.nam"): reuse that rather
        // than adding another copy on every open, for every instance in the project.
        const auto stem = target.getFileNameWithoutExtension();

        for (const auto& sibling : directory.findChildFiles (juce::File::findFiles, false,
                                                             stem + " *" + target.getFileExtension()))
            if (sameContent (sibling, bytes))
                return sibling;

        target = ModelLibrary::uniqueChildFile (directory, leaf);
    }

    if (! target.replaceWithData (bytes.getData(), bytes.getSize()))
        return {};

    if (created != nullptr)
        *created = true;

    // A sidecar is a few hundred bytes of JSON; anything else in that field is not one.
    if (sidecar.isNotEmpty() && sidecar.length() <= kMaxSidecarChars
        && juce::JSON::parse (sidecar).getDynamicObject() != nullptr)
        ToneSidecar::fileFor (target).replaceWithText (sidecar);

    return target;
}

juce::ValueTree EmbeddedMedia::toTree (const juce::String& slot) const
{
    juce::ValueTree tree (treeType);
    tree.setProperty (slotProperty, slot, nullptr);
    tree.setProperty (fileProperty, fileName, nullptr);
    tree.setProperty (dataProperty, data, nullptr);
    tree.setProperty (sizeProperty, size, nullptr);
    tree.setProperty (checksumProperty, checksum, nullptr);

    if (sidecar.isNotEmpty())
        tree.setProperty (sidecarProperty, sidecar, nullptr);

    return tree;
}

EmbeddedMedia EmbeddedMedia::fromTree (const juce::ValueTree& tree)
{
    EmbeddedMedia media;
    media.fileName = tree.getProperty (fileProperty).toString();
    media.data = tree.getProperty (dataProperty).toString();
    media.sidecar = tree.getProperty (sidecarProperty).toString();
    media.size = (juce::int64) tree.getProperty (sizeProperty, 0);
    media.checksum = tree.getProperty (checksumProperty).toString();
    return media;
}
} // namespace tubamp
