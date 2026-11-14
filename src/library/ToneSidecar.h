#pragma once

#include <juce_core/juce_core.h>
#include <optional>

namespace tubamp
{
/**
    What TONE3000 said about a file at the moment the user downloaded it, kept next
    to the file as `<file>.t3k.json`.

    A .wav carries no metadata at all and a .nam only what its trainer wrote, so
    without this the library is a folder of slugified file names: the creator's
    name is gone (the API terms require it to stay visible), an IR cannot be told
    apart as a cabinet or a room (which decides its level, see CabSim), and nothing
    links a file back to its tone page.

    The sidecar describes one downloaded file, never the catalog: it is written
    only for a file the user asked for, and deleted with it.
*/
struct ToneSidecar
{
    juce::int64 toneId = 0;
    juce::int64 modelId = 0;

    juce::String title;         // tone title, as shown on tone3000.com
    juce::String modelName;     // this file's own name within the tone
    juce::String creator;       // username
    juce::String gear;          // amp | amp-cab | pedal | outboard | cab | space | ...
    juce::String format;        // nam | ir
    juce::String size;          // standard | lite | feather | nano | custom
    juce::String architecture;  // "1" | "2" | "custom" | empty
    juce::String license;       // t3k | cc-by | ... (empty when the API did not say)

    /** `<media>.t3k.json`, beside the media file. The full file name is kept (not
        just the stem) so `a.nam` and `a.wav` can never share a sidecar. */
    static juce::File fileFor (const juce::File& media);

    /** nullopt when there is no sidecar or it does not parse. Never throws. */
    static std::optional<ToneSidecar> read (const juce::File& media);

    /** Writes (replaces) the sidecar for `media`. False when the write failed. */
    bool write (const juce::File& media) const;

    /** Removes the sidecar of a media file that is being deleted. */
    static void remove (const juce::File& media);

    /** https://www.tone3000.com/tones/<id>, or empty without a tone id. */
    juce::String toneUrl() const;

    juce::var toVar() const;
    static ToneSidecar fromVar (const juce::var& object);
};
} // namespace tubamp
