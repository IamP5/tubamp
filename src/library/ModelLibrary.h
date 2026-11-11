#pragma once

#include <juce_core/juce_core.h>

namespace tubamp
{
/** Local content library on disk:
      ~/Library/Application Support/tubamp/models/  (*.nam)
      ~/Library/Application Support/tubamp/irs/     (*.wav)
      ~/Library/Application Support/tubamp/presets/ (*.json)
    All methods are message-thread only. */
class ModelLibrary
{
public:
    ModelLibrary();

    struct Entry
    {
        juce::File file;
        juce::String displayName; // basename without extension
    };

    static juce::File getRootDir();

    /** Points the whole library at another directory for the rest of the process.
        For the test suite, which must not write into the user's real library. An
        invalid File restores the default. Call before any ModelLibrary exists. */
    static void setRootDirOverride (const juce::File& directory);
    static juce::File getModelsDir();
    static juce::File getIrsDir();
    static juce::File getPresetsDir();

    /** Ensures the directory tree exists. Called from the constructor. */
    void ensureDirectories();

    juce::Array<Entry> getModels() const;  // sorted by name
    juce::Array<Entry> getIrs() const;     // sorted by name

    /** Copies an external .nam/.wav into the library (renaming on clash).
        Returns the installed file, or an invalid File + error message. */
    juce::File importModel (const juce::File& source, juce::String& errorOut);
    juce::File importIr (const juce::File& source, juce::String& errorOut);

    /** A file in `dir` that does not exist yet: `fileName` itself, or with " 2",
        " 3", ... before the extension when that name is taken. */
    static juce::File uniqueChildFile (const juce::File& dir, const juce::String& fileName);

    /** For files that reach the library without going through import*() — a
        download, a file rebuilt from a session — so the UI lists them. */
    void noteExternalChange() { notifyChanged(); }

    /** Simple change notification for UI refresh. */
    std::function<void()> onChanged;

private:
    void notifyChanged();
};
} // namespace tubamp
