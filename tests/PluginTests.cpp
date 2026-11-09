// End-to-end tests of the plugin processor (and an editor snapshot) without a DAW.
//
//   processing   stereo host buffers through processBlock with a real model
//   state        save/restore of parameters and the model reference
//   missing      a session whose model file is gone keeps the reference and explains it
//   editor       opens the AMP page and writes a PNG snapshot (needs a display, e.g. Xvfb)

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "WavIO.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace juce;

static int gFailures = 0;
#define CHECK(cond, msg)                                                           \
    do {                                                                           \
        if (! (cond)) { ++gFailures; std::printf ("  FAIL line %d: %s\n", __LINE__, msg); } \
    } while (0)

static File exampleModel (const char* name) { return File (NAM_EXAMPLE_MODELS_DIR).getChildFile (name); }

static bool waitForModel (GuitarAmpProcessor& p, AudioBuffer<float>& scratch)
{
    MidiBuffer midi;
    p.getEngine().waitUntilWorkerIdle();
    for (int i = 0; i < 2000 && ! p.getEngine().isSettled(); ++i)
    {
        scratch.clear();
        p.processBlock (scratch, midi);
    }
    return p.getEngine().isSettled();
}

static void testProcessing()
{
    std::printf ("[processing] stereo buffers through processBlock\n");
    GuitarAmpProcessor p;
    p.setPlayConfigDetails (2, 2, 48000.0, 256);
    p.prepareToPlay (48000.0, 256);
    p.loadModel (exampleModel ("wavenet_a1_standard.nam"));

    AudioBuffer<float> buf (2, 256);
    CHECK (waitForModel (p, buf), "model never became active");

    std::vector<float> di;
    double rate = 0;
    gas::test::readWavMono (NAM_EXAMPLE_AUDIO, di, rate);

    MidiBuffer midi;
    double energy = 0;
    bool finite = true, channelsMatch = true;
    for (size_t pos = 0; pos + 256 <= di.size(); pos += 256)
    {
        buf.copyFrom (0, 0, di.data() + pos, 256);
        buf.clear (1, 0, 256); // signal on the left channel only, as with a typical mono DI
        p.processBlock (buf, midi);
        for (int i = 0; i < 256; ++i)
        {
            const float l = buf.getSample (0, i), r = buf.getSample (1, i);
            finite = finite && std::isfinite (l);
            channelsMatch = channelsMatch && std::abs (l - r) < 1e-9f;
            energy += (double) l * l;
        }
    }
    std::printf ("  output RMS %.4f\n", std::sqrt (energy / (double) di.size()));
    CHECK (finite, "non-finite output");
    CHECK (channelsMatch, "left and right outputs should carry the same mono amp signal");
    CHECK (energy > 1e-3, "output is silent");
    CHECK (p.getLatencySamples() == 0 || p.getLatencySamples() == p.getEngine().getLatencySamples(), "latency mismatch");
}

static void testStateRoundTrip()
{
    std::printf ("[state] parameters and model reference survive save/restore\n");
    MemoryBlock saved;
    const auto model = exampleModel ("lstm.nam");
    {
        GuitarAmpProcessor p;
        p.prepareToPlay (48000.0, 128);
        p.loadModel (model);
        p.getEngine().waitUntilWorkerIdle();
        p.getParameters().getParameter (ParamIDs::inputGain)->setValueNotifyingHost (0.75f);
        p.getParameters().getParameter (ParamIDs::outputMode)->setValueNotifyingHost (0.0f); // Raw
        p.getStateInformation (saved);
    }

    GuitarAmpProcessor q;
    q.prepareToPlay (44100.0, 512);
    q.setStateInformation (saved.getData(), (int) saved.getSize());
    q.getEngine().waitUntilWorkerIdle();

    const auto status = q.getEngine().getStatus();
    CHECK (status.state == gas::AmpEngine::Status::State::Ready, "model was not restored");
    CHECK (status.info && status.info->fileName == "lstm.nam", "wrong model restored");
    CHECK (status.resampling, "restored at 44.1 kHz, so the 48 kHz model should be resampled");
    CHECK (std::abs (q.getParameters().getParameter (ParamIDs::inputGain)->getValue() - 0.75f) < 1e-4f,
           "input gain not restored");
    CHECK ((int) *q.getParameters().getRawParameterValue (ParamIDs::outputMode) == 0, "output mode not restored");
}

static void testMissingModel()
{
    std::printf ("[missing] session refers to a model file that no longer exists\n");
    MemoryBlock saved;
    const auto tmp = File::getSpecialLocation (File::tempDirectory).getChildFile ("gas_moved_model.nam");
    exampleModel ("lstm.nam").copyFileTo (tmp);
    {
        GuitarAmpProcessor p;
        p.prepareToPlay (48000.0, 128);
        p.loadModel (tmp);
        p.getEngine().waitUntilWorkerIdle();
        p.getStateInformation (saved);
    }
    tmp.deleteFile();

    GuitarAmpProcessor q;
    q.prepareToPlay (48000.0, 128);
    q.setStateInformation (saved.getData(), (int) saved.getSize());
    q.getEngine().waitUntilWorkerIdle();
    const auto status = q.getEngine().getStatus();
    std::printf ("  status: \"%s\"\n", status.message.c_str());
    CHECK (status.state == gas::AmpEngine::Status::State::Error, "missing model should be reported");
    CHECK (String (q.getEngine().requestedModelPath()) == tmp.getFullPathName(),
           "the session must keep its model reference");
}

static void snapshotEditor (const File& png)
{
    std::printf ("[editor] AMP page snapshot -> %s\n", png.getFullPathName().toRawUTF8());
    GuitarAmpProcessor p;
    p.setPlayConfigDetails (2, 2, 48000.0, 256);
    p.prepareToPlay (48000.0, 256);
    p.loadModel (exampleModel ("wavenet_a1_standard.nam"));
    AudioBuffer<float> buf (2, 256);
    waitForModel (p, buf);

    std::unique_ptr<AudioProcessorEditor> editor (p.createEditor());
    editor->setVisible (true);
    editor->addToDesktop (0);

    // play NAM's example test signal (it starts after 1 s of silence) so the meters show level
    std::vector<float> di;
    double rate = 0;
    gas::test::readWavMono (NAM_EXAMPLE_AUDIO, di, rate);
    MidiBuffer midi;
    for (size_t pos = (size_t) rate; pos + 256 <= (size_t) (1.4 * rate); pos += 256)
    {
        buf.copyFrom (0, 0, di.data() + pos, 256);
        buf.copyFrom (1, 0, di.data() + pos, 256);
        p.processBlock (buf, midi);
        if ((pos / 256) % 8 == 0)
        {
            Thread::sleep (34);
            Timer::callPendingTimersSynchronously();
        }
    }
    Timer::callPendingTimersSynchronously();

    auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
    png.deleteFile();
    FileOutputStream out (png);
    CHECK (out.openedOk() && PNGImageFormat().writeImageToStream (image, out), "could not write snapshot");
    editor->removeFromDesktop();
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juce;

    testProcessing();
    testStateRoundTrip();
    testMissingModel();

    if (argc > 1 && SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty())
        snapshotEditor (File::getCurrentWorkingDirectory().getChildFile (argv[1]));

    std::printf (gFailures == 0 ? "\nALL PLUGIN TESTS PASSED\n" : "\n%d CHECK(S) FAILED\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
