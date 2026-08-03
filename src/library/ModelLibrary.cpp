#include "ModelLibrary.h"

#include <algorithm>

namespace tubamp
{
namespace
{
/** Returns a file in `dir` that doesn't clash with an existing one, by appending
    " 2", " 3", ... before the extension when needed. */
juce::File declash (const juce::File& dir, const juce::String& fileName)
{
    juce::File candidate = dir.getChildFile (fileName);
    if (! candidate.exists())
        return candidate;

    const juce::String base = candidate.getFileNameWithoutExtension();
    const juce::String ext = candidate.getFileExtension(); // includes leading '.'

    for (int n = 2; n < 1000; ++n)
    {
        juce::File next = dir.getChildFile (base + " " + juce::String (n) + ext);
        if (! next.exists())
            return next;
    }

    // Pathological case (hundreds of clashes) - fall back to a timestamped name.
    return dir.getChildFile (base + " " + juce::String (juce::Time::getCurrentTime().toMilliseconds()) + ext);
}

juce::Array<ModelLibrary::Entry> scanDir (const juce::File& dir, const juce::String& wildcard)
{
    juce::Array<ModelLibrary::Entry> entries;

    for (const auto& file : dir.findChildFiles (juce::File::findFiles, false, wildcard))
    {
        ModelLibrary::Entry e;
        e.file = file;
        e.displayName = file.getFileNameWithoutExtension();
        entries.add (e);
    }

    std::sort (entries.begin(), entries.end(), [] (const ModelLibrary::Entry& a, const ModelLibrary::Entry& b)
    {
        return a.displayName.compareIgnoreCase (b.displayName) < 0;
    });

    return entries;
}

/** Shared copy-in logic for importModel/importIr. */
juce::File importInto (const juce::File& destDir, const juce::String& requiredExtension,
                       const juce::File& source, juce::String& errorOut)
{
    errorOut.clear();

    if (! source.existsAsFile())
    {
        errorOut = "File does not exist: " + source.getFullPathName();
        return {};
    }

    if (! source.hasFileExtension (requiredExtension))
    {
        errorOut = "Expected a ." + requiredExtension + " file.";
        return {};
    }

    destDir.createDirectory();

    const juce::File dest = declash (destDir, source.getFileName());

    if (! source.copyFileTo (dest))
    {
        errorOut = "Could not copy file into the library.";
        return {};
    }

    return dest;
}
} // namespace

ModelLibrary::ModelLibrary()
{
    ensureDirectories();
}

juce::File ModelLibrary::getRootDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("tubamp");
}

juce::File ModelLibrary::getModelsDir()  { return getRootDir().getChildFile ("models"); }
juce::File ModelLibrary::getIrsDir()     { return getRootDir().getChildFile ("irs"); }
juce::File ModelLibrary::getPresetsDir() { return getRootDir().getChildFile ("presets"); }

void ModelLibrary::ensureDirectories()
{
    getModelsDir().createDirectory();
    getIrsDir().createDirectory();
    getPresetsDir().createDirectory();
}

juce::Array<ModelLibrary::Entry> ModelLibrary::getModels() const
{
    return scanDir (getModelsDir(), "*.nam");
}

juce::Array<ModelLibrary::Entry> ModelLibrary::getIrs() const
{
    return scanDir (getIrsDir(), "*.wav");
}

juce::File ModelLibrary::importModel (const juce::File& source, juce::String& errorOut)
{
    auto result = importInto (getModelsDir(), "nam", source, errorOut);
    if (result != juce::File())
        notifyChanged();
    return result;
}

juce::File ModelLibrary::importIr (const juce::File& source, juce::String& errorOut)
{
    auto result = importInto (getIrsDir(), "wav", source, errorOut);
    if (result != juce::File())
        notifyChanged();
    return result;
}

void ModelLibrary::notifyChanged()
{
    if (onChanged != nullptr)
        onChanged();
}
} // namespace tubamp
