// Headless editor screenshot: instantiates the real processor + editor offscreen,
// pumps the message loop briefly so timers/async init settle, and writes a PNG.
// No window, no screen-recording permission needed — createComponentSnapshot paints
// straight into an image. Used to iterate on the UI without a DAW.
//
//   tubamp_uishot <out.png> [model.nam]

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

#include <cstdio>
#include <memory>

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: tubamp_uishot <out.png> [model.nam]\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI gui;

    tubamp::TubampAudioProcessor proc;
    proc.prepareToPlay (48000.0, 512);

    if (argc > 2)
        if (auto error = proc.loadModel (juce::File (juce::String (argv[2]))); error.isNotEmpty())
            std::printf ("model load failed (continuing): %s\n", error.toRawUTF8());

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());

    if (editor == nullptr)
    {
        std::printf ("FAIL: no editor\n");
        return 1;
    }

    // Let editor timers (meters, chain view refresh) and deferred layout run.
    juce::MessageManager::getInstance()->runDispatchLoopUntil (600);

    auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true);

    const juce::File outFile = juce::File::getCurrentWorkingDirectory()
                                   .getChildFile (juce::String (argv[1]));
    outFile.deleteFile();

    juce::FileOutputStream stream (outFile);

    if (! stream.openedOk())
    {
        std::printf ("FAIL: cannot write %s\n", outFile.getFullPathName().toRawUTF8());
        return 1;
    }

    juce::PNGImageFormat png;

    if (! png.writeImageToStream (image, stream))
    {
        std::printf ("FAIL: png encode\n");
        return 1;
    }

    std::printf ("wrote %s (%dx%d)\n", outFile.getFullPathName().toRawUTF8(),
                 image.getWidth(), image.getHeight());

    // The editor must go before the processor (same order a host guarantees).
    editor = nullptr;
    return 0;
}
