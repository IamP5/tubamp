#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace tubamp
{
/**
    A model or IR file carried inside the plugin's saved state.

    A session used to remember only the path of its .nam and .wav. Open the project
    on another machine, or delete the file from the library, and the amp came back as
    a silent pass-through with nothing to say why. The reference TONE3000 plugin
    stores the active model's bytes in the DAW state for exactly this reason
    (its ProcessorState.cpp, "ModelCache"); this is the same idea on top of tubamp's
    on-disk library: the bytes travel with the session and are written back into the
    library when — and only when — the file they came from is not there.

    Only the host's session state carries them. Presets and A/B slots stay
    references into the local library: they are local files themselves, and a copy
    of a model in every preset would be megabytes of duplication per preset.
*/
struct EmbeddedMedia
{
    juce::String fileName;  // name of the file the bytes came from
    juce::String data;      // base64 of the zlib-compressed file
    juce::String sidecar;   // the file's TONE3000 sidecar (JSON text), when it had one

    // Size and FNV-1a 64 of the original bytes. A truncated or damaged deflate
    // stream inflates to *something*; these are how decode() knows it is not the
    // file. Zero / empty in a blob that carries no check (then nothing is checked).
    juce::int64 size = 0;
    juce::String checksum;

    bool isEmpty() const noexcept { return data.isEmpty(); }

    /** Files above this size are referenced by path only. */
    static constexpr juce::int64 maxFileBytes = 32 * 1024 * 1024;

    /** Reads and packs `file`. Empty when it is missing, unreadable or too large.
        Any thread; does file IO. */
    static EmbeddedMedia fromFile (const juce::File& file);

    /** The file's original bytes. False when the data does not decode. */
    bool decode (juce::MemoryBlock& out) const;

    /** Puts the file (back) into `directory` and returns it: an existing file of the
        same name and identical content is reused; a different file of that name is
        left alone and the bytes go in under a free name. Restores the sidecar too.
        Returns an invalid File when nothing could be written, or when the embedded
        name does not carry `extension` ("nam", "wav") — a state blob does not get
        to put a file of any other kind into the library. `created`, when given, says
        whether a file was written (false when an existing one was reused). */
    juce::File materialiseIn (const juce::File& directory, const juce::String& extension,
                              bool* created = nullptr) const;

    /** A "MEDIA" child for the state tree, tagged with the slot it belongs to. */
    juce::ValueTree toTree (const juce::String& slot) const;
    static EmbeddedMedia fromTree (const juce::ValueTree& tree);

    static const juce::Identifier treeType;     // "MEDIA"
    static const juce::Identifier slotProperty; // "slot"
};
} // namespace tubamp
