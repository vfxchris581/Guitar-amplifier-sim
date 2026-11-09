// End-to-end tests of the plugin processor (and an editor snapshot) without a DAW.
//
//   processing   stereo host buffers through processBlock with a real model
//   state        save/restore of parameters and the model reference
//   missing      a session whose model file is gone keeps the reference and explains it
//   editor       opens the AMP page and writes a PNG snapshot (needs a display, e.g. Xvfb)

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "common/WavIO.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <cstdio>
#include <thread>

using namespace juce;

static int gFailures = 0;
#define CHECK(cond, ...)                                                           \
    do {                                                                           \
        if (! (cond)) { ++gFailures; std::printf ("  FAIL line %d: ", __LINE__); std::printf (__VA_ARGS__); std::printf ("\n"); } \
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
    gas::wav::readWavMono (NAM_EXAMPLE_AUDIO, di, rate);

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

// ---------------------------------------------------------------------------
// CAPTURE milestone, in process: a simulated amp sits between the plugin's
// output and input, exactly where the reamp chain would be.
struct AmpInTheLoop
{
    std::unique_ptr<nam::DSP> amp = nam::get_dsp (std::filesystem::path (exampleModel ("lstm.nam").getFullPathName().toStdString()));
    int latency = 300;                // round trip through interface + reamp + load box
    std::vector<float> returned;      // what the interface input receives
    size_t pos = 0;
    AmpInTheLoop() { amp->Reset (48000.0, 512); }

    /** One host block: build the input from earlier output, let the plugin run, feed its send to the amp. */
    void block (GuitarAmpProcessor& p, AudioBuffer<float>& buf, int sendCh, int returnCh)
    {
        const int n = buf.getNumSamples();
        buf.clear();
        for (int i = 0; i < n; ++i)
            buf.setSample (returnCh, i, pos + (size_t) i < returned.size() ? returned[pos + (size_t) i] : 0.0f);
        MidiBuffer midi;
        p.processBlock (buf, midi);
        std::vector<float> out ((size_t) n);
        float* in = buf.getWritePointer (sendCh);
        float* o = out.data();
        amp->process (&in, &o, n);
        if (returned.size() < pos + (size_t) latency)
            returned.resize (pos + (size_t) latency, 0.0f);
        returned.insert (returned.end(), out.begin(), out.end());
        pos += (size_t) n;
    }
};

static std::vector<float> syntheticCaptureSignal()
{
    // v3 layout (190 s): validation, silence, blips at 504000 / 552000, training, silence, validation repeat
    const size_t n = 190 * 48000;
    std::vector<float> x (n, 0.0f);
    juce::Random rng (3);
    auto notes = [&] (size_t from, size_t to)
    {
        for (size_t pos = from; pos < to;)
        {
            const size_t len = 4800 + (size_t) rng.nextInt (24000);
            const float f0 = 82.0f * std::pow (2.0f, rng.nextFloat() * 3.0f), amp = 0.05f + 0.6f * rng.nextFloat();
            for (size_t i = 0; i < len && pos + i < to; ++i)
            {
                const float t = (float) i / 48000.0f;
                x[pos + i] = amp * std::exp (-4.0f * t) * (std::sin (6.2831853f * f0 * t) + 0.3f * std::sin (6.2831853f * 2 * f0 * t));
            }
            pos += len;
        }
    };
    notes (0, 432000);
    x[504000] = x[552000] = 0.5f;
    notes (600000, n - 432000 - 24000);
    std::copy (x.begin(), x.begin() + 432000, x.end() - 432000);
    return x;
}

static void testCaptureMilestone (const File& outDir)
{
    std::printf ("[capture] level check -> capture -> train -> save -> play, through processBlock\n");
    GuitarAmpProcessor p;
    p.setPlayConfigDetails (2, 2, 48000.0, 128);
    p.prepareToPlay (48000.0, 128);
    auto& cap = p.getCapture();
    cap.setSignalForTesting (syntheticCaptureSignal());
    cap.sendChannel = 0;   // left out -> reamp
    cap.returnChannel = 1; // right in <- amp
    CaptureController::capturesFolderOverride = outDir.getChildFile ("Captures");

    AmpInTheLoop rig;
    AudioBuffer<float> buf (2, 128);
    String err;

    CHECK (cap.startLevelCheck (48000.0, err), "level check did not start: %s", err.toRawUTF8());
    while (cap.transport.state() == gas::capture::CaptureTransport::State::Running)
        rig.block (p, buf, 0, 1);
    cap.poll();
    CHECK (cap.levelCheck.has_value(), "no level check result");
    if (cap.levelCheck)
    {
        std::printf ("  level check: %s\n", cap.levelCheck->latency.message.c_str());
        CHECK (cap.levelCheck->latency.detected == std::optional<int> (rig.latency), "wrong latency");
    }

    // While nothing is running the plugin is the amp again (send channel is not the capture signal)
    CHECK (cap.startCapture (48000.0, err), "capture did not start: %s", err.toRawUTF8());
    while (cap.transport.state() == gas::capture::CaptureTransport::State::Running)
        rig.block (p, buf, 0, 1);
    cap.poll();
    CHECK (cap.capture && cap.capture->usable, "capture unusable");
    if (! cap.capture || ! cap.capture->usable)
        return;
    std::printf ("  capture: %s, replicate ESR %.2e\n", cap.capture->latency.message.c_str(), cap.capture->replicate.esr);

    gas::train::TrainOptions opt;
    opt.preset = gas::train::Preset::Nano;
    opt.epochs = 2;
    CaptureController::ModelDetails d;
    d.name = "Test Amp Lead";
    d.gearMake = "Test";
    d.gearModel = "Amp";
    d.toneType = "hi_gain";
    d.settingsNote = "Gain 7";
    CHECK (cap.startTraining (opt, d, err), "training did not start: %s", err.toRawUTF8());
    while (cap.isTraining())
        Thread::sleep (50);
    const auto st = cap.trainingStatus();
    std::printf ("  training: %s\n", st.message.toRawUTF8());
    CHECK (st.finished && cap.validation.has_value(), "no trained model");
    if (! cap.validation)
        return;
    std::printf ("  validation ESR %.4f\n", cap.validation->esr);

    // A/B audition plays level-matched material through the plugin output
    cap.audition.setMode (AuditionPlayer::Reference);
    MidiBuffer midi;
    buf.clear();
    p.processBlock (buf, midi);
    CHECK (buf.getMagnitude (0, 0, 128) > 0.0f, "audition produced no audio");
    cap.audition.setMode (AuditionPlayer::Off);

    const auto saved = cap.saveModel (err);
    CHECK (saved.existsAsFile(), "model not saved: %s", err.toRawUTF8());
    CHECK (saved.getSiblingFile ("output.wav").existsAsFile(), "raw capture not saved");
    std::printf ("  saved %s\n", saved.getFullPathName().toRawUTF8());

    // The saved model must load and play in the AMP section with its metadata
    p.loadModel (saved);
    waitForModel (p, buf);
    const auto status = p.getEngine().getStatus();
    CHECK (status.state == gas::AmpEngine::Status::State::Ready, "saved model did not load: %s", status.message.c_str());
    if (status.info)
    {
        std::printf ("  loaded into AMP: \"%s\" (%s %s, %s), loudness %.1f dB, ESR %.4f\n", status.info->name.c_str(),
                     status.info->gearMake.c_str(), status.info->gearModel.c_str(), status.info->toneType.c_str(),
                     status.info->loudnessDb.value_or (-999), status.info->validationEsr.value_or (-1));
        CHECK (status.info->name == "Test Amp Lead" && status.info->loudnessDb.has_value()
                   && status.info->validationEsr.has_value(),
               "metadata missing");
    }

    // Screenshots of each CAPTURE step with real results in them
    if (SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty())
    {
        std::unique_ptr<AudioProcessorEditor> editor (p.createEditor());
        auto* ed = dynamic_cast<GuitarAmpEditor*> (editor.get());
        ed->addToDesktop (0);
        ed->setVisible (true);
        ed->showSection (1);
        for (int step = 0; step < 5; ++step)
        {
            ed->getCapturePage().showStep (step);
            ed->getCapturePage().refreshNow();
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.5f);
            auto f = outDir.getChildFile ("capture_step" + String (step + 1) + ".png");
            f.deleteFile();
            FileOutputStream os (f);
            PNGImageFormat().writeImageToStream (img, os);
        }
        ed->removeFromDesktop();
        std::printf ("  CAPTURE page snapshots in %s\n", outDir.getFullPathName().toRawUTF8());
    }
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
    gas::wav::readWavMono (NAM_EXAMPLE_AUDIO, di, rate);
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
    testCaptureMilestone (File::getSpecialLocation (File::tempDirectory).getChildFile ("gas_capture_test"));

    if (argc > 1 && SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty())
        snapshotEditor (File::getCurrentWorkingDirectory().getChildFile (argv[1]));

    std::printf (gFailures == 0 ? "\nALL PLUGIN TESTS PASSED\n" : "\n%d CHECK(S) FAILED\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
