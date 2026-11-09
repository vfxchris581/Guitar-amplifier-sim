// End-to-end tests of the plugin processor (and an editor snapshot) without a DAW.
//
//   processing   stereo host buffers through processBlock with a real model
//   state        save/restore of parameters and the model reference
//   missing      a session whose model file is gone keeps the reference and explains it
//   editor       opens the AMP page and writes a PNG snapshot (needs a display, e.g. Xvfb)

#include "plugin/PluginEditor.h"
#include "plugin/PluginProcessor.h"
#include "plugin/IRLibrary.h"
#include "plugin/Library.h"
#include "common/WavIO.h"
#include "train/WaveNet.h"

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

// ---------------------------------------------------------------------------
static File writeIR (const File& f, const std::vector<float>& ir, double rate)
{
    gas::wav::writeWavMono (f.getFullPathName().toStdString(), ir, rate);
    return f;
}

/** Runs the cab until a newly loaded IR is active (JUCE loads IRs on a background thread). */
static void settleCab (CabEngine& cab)
{
    std::vector<float> in (256, 0.0f), l (256), r (256);
    for (int i = 0; i < 100; ++i)
    {
        cab.process (in.data(), l.data(), r.data(), 256);
        Thread::sleep (5);
    }
}

static std::vector<float> runCab (CabEngine& cab, const std::vector<float>& x, std::vector<float>* right = nullptr)
{
    std::vector<float> l (x.size()), r (x.size());
    for (size_t pos = 0; pos < x.size(); pos += 256)
    {
        const int n = (int) std::min<size_t> (256, x.size() - pos);
        cab.process (x.data() + pos, l.data() + pos, r.data() + pos, n);
    }
    if (right)
        *right = r;
    return l;
}

static void testCabinet (const File& dir)
{
    std::printf ("[cab] convolution, IR handling and mic controls\n");
    dir.createDirectory();
    Random rng (5);
    std::vector<float> noise (48000);
    for (auto& v : noise)
        v = rng.nextFloat() * 2.0f - 1.0f;

    // 1. Exact convolution against a direct reference
    std::vector<float> ir (300);
    for (size_t i = 0; i < ir.size(); ++i)
        ir[i] = std::exp (-(float) i / 40.0f) * (rng.nextFloat() - 0.5f);
    {
        CabEngine cab;
        cab.prepare (48000.0, 256);
        String err;
        CHECK (cab.loadIR (0, writeIR (dir.getChildFile ("ir48.wav"), ir, 48000.0), 1000.0, false, err), "load: %s", err.toRawUTF8());
        cab.mics[1].enabled = false;
        settleCab (cab);
        std::vector<float> right;
        const auto out = runCab (cab, noise, &right);
        double maxErr = 0, maxLR = 0;
        for (size_t n = 1000; n < noise.size(); ++n)
        {
            double ref = 0;
            for (size_t k = 0; k < ir.size(); ++k)
                ref += (double) ir[k] * noise[n - k];
            maxErr = std::max (maxErr, std::abs (ref - out[n]));
            maxLR = std::max (maxLR, (double) std::abs (out[n] - right[n]));
        }
        std::printf ("  mono IR, centre pan: max error vs direct convolution %.2e, L-R difference %.2e\n", maxErr, maxLR);
        CHECK (maxErr < 1e-4, "convolution mismatch");
        CHECK (maxLR < 1e-6, "centred mono IR should give identical channels");

        // 2. Mic controls
        std::vector<float> impulse (4096, 0.0f);
        impulse[100] = 1.0f;
        cab.mics[0].delayMs = 1.0f; // 48 samples at 48 kHz
        cab.mics[0].invert = true;
        settleCab (cab);
        const auto shifted = runCab (cab, impulse);
        int peakAt = 0;
        for (int i = 0; i < 4096; ++i)
            if (std::abs (shifted[(size_t) i]) > std::abs (shifted[(size_t) peakAt]))
                peakAt = i;
        int irPeak = 0;
        for (int i = 0; i < (int) ir.size(); ++i)
            if (std::abs (ir[(size_t) i]) > std::abs (ir[(size_t) irPeak]))
                irPeak = i;
        std::printf ("  1 ms delay + phase invert: peak moved to sample %d (expected %d), sign %s\n", peakAt,
                     100 + irPeak + 48, (shifted[(size_t) peakAt] * ir[(size_t) irPeak] < 0) ? "inverted" : "NOT inverted");
        CHECK (peakAt == 100 + irPeak + 48, "delay wrong");
        CHECK (shifted[(size_t) peakAt] * ir[(size_t) irPeak] < 0, "polarity not inverted");

        cab.mics[0].delayMs = 0.0f;
        cab.mics[0].invert = false;
        cab.mics[0].pan = -1.0f;
        settleCab (cab);
        runCab (cab, noise, &right);
        double rightEnergy = 0;
        for (size_t i = 2000; i < right.size(); ++i)
            rightEnergy += (double) right[i] * right[i];
        CHECK (rightEnergy < 1e-6, "hard-left pan leaks into the right channel");

        cab.mics[0].pan = 0.0f;
        cab.mics[0].hpfHz = 500.0f;
        settleCab (cab);
        std::vector<float> low (48000);
        for (size_t i = 0; i < low.size(); ++i)
            low[i] = 0.5f * std::sin (6.2831853f * 50.0f * (float) i / 48000.0f);
        const auto filtered = runCab (cab, low);
        cab.mics[0].hpfHz = 20.0f;
        settleCab (cab);
        const auto unfiltered = runCab (cab, low);
        auto rmsOf = [] (const std::vector<float>& v) { double e = 0; for (size_t i = 24000; i < v.size(); ++i) e += (double) v[i] * v[i]; return std::sqrt (e / 24000.0); };
        const double attenuation = 20 * std::log10 (rmsOf (filtered) / rmsOf (unfiltered));
        std::printf ("  500 Hz high-pass on 50 Hz: %.1f dB\n", attenuation);
        CHECK (attenuation < -30.0, "high-pass too weak");
    }

    // 3. IR at a different sample rate and IR length trimming
    {
        CabEngine cab;
        cab.prepare (48000.0, 256);
        std::vector<float> longIr (44100 / 5); // 200 ms at 44.1 kHz
        for (size_t i = 0; i < longIr.size(); ++i)
            longIr[i] = std::exp (-(float) i / 2000.0f) * (rng.nextFloat() - 0.5f);
        String err;
        CHECK (cab.loadIR (0, writeIR (dir.getChildFile ("ir441.wav"), longIr, 44100.0), 50.0, true, err), "load 44.1k IR");
        const auto info = cab.getIRInfo (0);
        std::printf ("  44.1 kHz IR: file %.0f ms, using %.1f ms, resampled to the 48 kHz host by the convolution engine\n",
                     info.fileLengthMs, info.usedLengthMs);
        CHECK (std::abs (info.usedLengthMs - 50.0) < 0.1 && std::abs (info.fileLengthMs - 200.0) < 0.1, "length trim");
        settleCab (cab);
        const auto out = runCab (cab, noise);
        double e = 0;
        for (float v : out)
            e += (double) v * v;
        CHECK (e > 1.0 && std::isfinite (e), "resampled IR produced no output");
    }

    // 4. Double-cab protection and state
    {
        GuitarAmpProcessor p;
        p.setPlayConfigDetails (2, 2, 48000.0, 256);
        p.prepareToPlay (48000.0, 256);
        String err;
        CHECK (p.loadIR (0, dir.getChildFile ("ir48.wav"), err), "processor IR load");
        String why;
        CHECK (p.isCabActive (&why), "cab should be active with an IR: %s", why.toRawUTF8());

        // a model whose metadata says it includes a cabinet
        gas::train::WaveNet net (gas::train::WaveNetSpec::fromPreset (gas::train::Preset::Nano));
        net.initialise (1);
        const auto namFile = dir.getChildFile ("with_cab.nam");
        namFile.replaceWithText (net.exportNam (R"({"name":"Amp+Cab","gear_type":"amp_cab"})", 48000.0));
        p.loadModel (namFile);
        p.getEngine().waitUntilWorkerIdle();
        CHECK (! p.isCabActive (&why), "Auto mode must bypass the cab for a cab-inclusive capture");
        std::printf ("  cab-inclusive capture loaded -> \"%s\"\n", why.toRawUTF8());
        p.getParameters().getParameter (ParamIDs::cabMode)->setValueNotifyingHost (
            p.getParameters().getParameter (ParamIDs::cabMode)->convertTo0to1 (1.0f)); // On
        CHECK (p.isCabActive(), "On mode must force the cab on");

        MemoryBlock state;
        p.getStateInformation (state);
        GuitarAmpProcessor q;
        q.prepareToPlay (48000.0, 256);
        q.setStateInformation (state.getData(), (int) state.getSize());
        CHECK (q.getCab().hasIR (0) && q.getIRPath (0) == dir.getChildFile ("ir48.wav").getFullPathName(),
               "IR not restored from state");
    }
}

// ---------------------------------------------------------------------------
static void setParam (GuitarAmpProcessor& p, const String& id, float value)
{
    auto* prm = p.getParameters().getParameter (id);
    prm->setValueNotifyingHost (prm->convertTo0to1 (value));
}

static std::vector<float> runModule (fx::Module& m, const std::vector<float>& x, std::vector<float>* right = nullptr)
{
    std::vector<float> l = x, r = x;
    for (size_t pos = 0; pos < x.size(); pos += 128)
    {
        const int n = (int) std::min<size_t> (128, x.size() - pos);
        m.process (l.data() + pos, right ? r.data() + pos : nullptr, n);
    }
    if (right) *right = r;
    return l;
}

static std::vector<float> sineWave (float freq, float amp, size_t n, double rate = 48000.0)
{
    std::vector<float> x (n);
    for (size_t i = 0; i < n; ++i)
        x[i] = amp * (float) std::sin (6.283185307179586 * freq * (double) i / rate);
    return x;
}

static double rmsDb (const std::vector<float>& x, size_t from, size_t to)
{
    double e = 0;
    for (size_t i = from; i < to; ++i) e += (double) x[i] * x[i];
    return 10.0 * std::log10 (e / (double) (to - from) + 1e-30);
}

/** Magnitude (dB) of a single frequency via Goertzel-style correlation. */
static double toneDb (const std::vector<float>& x, double f, size_t from, size_t to)
{
    double re = 0, im = 0;
    for (size_t i = from; i < to; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * MathConstants<double>::pi * (double) (i - from) / (double) (to - from));
        re += w * x[i] * std::cos (2.0 * MathConstants<double>::pi * f * (double) i / 48000.0);
        im += w * x[i] * std::sin (2.0 * MathConstants<double>::pi * f * (double) i / 48000.0);
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / ((double) (to - from) * 0.5) + 1e-30);
}

static void testEffects()
{
    std::printf ("[effects] stompboxes and post effects\n");
    GuitarAmpProcessor p;
    p.prepareToPlay (48000.0, 128);
    auto& chain = p.getChain();
    auto mod = [&] (const char* id) { return chain.get (id); };

    // Noise gate: quiet signal muted, loud signal passes
    {
        setParam (p, "gate_threshold", -40.0f);
        auto* g = mod ("gate");
        g->reset();
        const auto quiet = runModule (*g, sineWave (200, 0.003f, 48000));
        const auto loud = runModule (*g, sineWave (200, 0.3f, 48000));
        std::printf ("  gate (-40 dB): -50 dBFS tone -> %.1f dB, -10 dBFS tone -> %.1f dB\n",
                     rmsDb (quiet, 24000, 48000) - rmsDb (sineWave (200, 0.003f, 48000), 24000, 48000),
                     rmsDb (loud, 24000, 48000) - rmsDb (sineWave (200, 0.3f, 48000), 24000, 48000));
        CHECK (rmsDb (quiet, 24000, 48000) < -100.0, "gate does not close");
        CHECK (std::abs (rmsDb (loud, 24000, 48000) - rmsDb (sineWave (200, 0.3f, 48000), 24000, 48000)) < 0.1, "gate alters open signal");
    }
    // Compressor: -6 dBFS peak sine, threshold -20, ratio 4
    {
        setParam (p, "comp_threshold", -20.0f);
        setParam (p, "comp_ratio", 4.0f);
        auto* c = mod ("comp");
        c->reset();
        const auto x = sineWave (500, 0.5f, 48000);
        const auto y = runModule (*c, x);
        const double inDb = rmsDb (x, 24000, 48000), outDb = rmsDb (y, 24000, 48000);
        std::printf ("  compressor 4:1 @ -20 dB: input %.1f dB RMS -> output %.1f dB RMS (reduction %.1f dB)\n", inDb, outDb, inDb - outDb);
        CHECK (inDb - outDb > 7.0 && inDb - outDb < 12.0, "compression amount implausible");
    }
    // Drives: harmonics rise with drive, aliasing stays low thanks to oversampling
    for (auto id : { "od", "dist", "fuzz" })
    {
        auto* d = mod (id);
        const String driveId = String (id) + (String (id) == "fuzz" ? "_fuzz" : "_drive");
        setParam (p, driveId, 0.1f);
        d->reset();
        const auto x = sineWave (1000, 0.3f, 48000);
        const auto lo = runModule (*d, x);
        setParam (p, driveId, 0.9f);
        d->reset();
        const auto hi = runModule (*d, x);
        auto thd = [&] (const std::vector<float>& y)
        {
            double h = 0;
            for (int k = 2; k <= 9; ++k) h += std::pow (10.0, toneDb (y, 1000.0 * k, 8000, 48000) / 10.0);
            return 10.0 * std::log10 (h + 1e-30) - toneDb (y, 1000, 8000, 48000);
        };
        // aliasing: a 7.3 kHz tone at full drive; energy at the first fold-back products
        const auto hf = runModule (*d, sineWave (7300, 0.3f, 48000));
        const double fund = toneDb (hf, 7300, 8000, 48000);
        double worstAlias = -300;
        for (int k = 4; k <= 9; ++k)
        {
            double f = std::fmod (7300.0 * k, 48000.0);
            if (f > 24000.0) f = 48000.0 - f;
            worstAlias = std::max (worstAlias, toneDb (hf, f, 8000, 48000) - fund);
        }
        std::printf ("  %-5s THD drive 10%% %.1f dB, 90%% %.1f dB; worst alias at 7.3 kHz %.1f dB\n", id, thd (lo), thd (hi), worstAlias);
        CHECK (thd (hi) > thd (lo) + 6.0, "%s: drive does not add harmonics", id);
        CHECK (worstAlias < -35.0, "%s: aliasing too high", id);
        bool finite = true;
        for (float v : hi) finite = finite && std::isfinite (v);
        CHECK (finite, "%s: non-finite output", id);
    }
    // EQ band accuracy
    {
        setParam (p, "eq_b2Freq", 1000.0f);
        setParam (p, "eq_b2Gain", 12.0f);
        setParam (p, "eq_b2Q", 1.0f);
        auto* e = mod ("eq");
        e->prepare (48000.0, 128);
        const auto x = sineWave (1000, 0.1f, 48000);
        std::vector<float> r;
        const auto y = runModule (*e, x, &r);
        const double gain = toneDb (y, 1000, 8000, 48000) - toneDb (x, 1000, 8000, 48000);
        const double off = toneDb (runModule (*e, sineWave (100, 0.1f, 48000)), 100, 8000, 48000) - toneDb (sineWave (100, 0.1f, 48000), 100, 8000, 48000);
        std::printf ("  EQ +12 dB @ 1 kHz: measured %+.2f dB at 1 kHz, %+.2f dB at 100 Hz\n", gain, off);
        CHECK (std::abs (gain - 12.0) < 0.2, "EQ band gain wrong");
        CHECK (std::abs (off) < 1.5, "EQ band too wide");
        setParam (p, "eq_b2Gain", 0.0f);
    }
    // Delay timing, feedback, tempo sync
    {
        auto* d = mod ("delay");
        setParam (p, "delay_time", 250.0f);
        setParam (p, "delay_mix", 1.0f);
        setParam (p, "delay_feedback", 0.5f);
        setParam (p, "delay_lowcut", 20.0f);
        setParam (p, "delay_highcut", 20000.0f);
        d->reset();
        std::vector<float> imp (48000, 0.0f);
        imp[0] = 1.0f;
        const auto y = runModule (*d, imp);
        auto peakNear = [&] (size_t c) { size_t best = c - 20; for (size_t i = c - 20; i < c + 20; ++i) if (std::abs (y[i]) > std::abs (y[best])) best = i; return best; };
        const size_t e1 = peakNear (12000), e2 = peakNear (24000);
        std::printf ("  delay 250 ms: echoes at %zu and %zu samples, second/first %.2f\n", e1, e2, std::abs (y[e2] / y[e1]));
        CHECK (e1 == 12000 && e2 == 24000, "delay time wrong");
        CHECK (std::abs (std::abs (y[e2] / y[e1]) - 0.5) < 0.05, "feedback wrong");
        setParam (p, "delay_sync", 3.0f); // 1/4
        chain.bpm = 120.0;
        d->reset();
        const auto ys = runModule (*d, std::vector<float> (imp.begin(), imp.end()));
        size_t peak = 1;
        for (size_t i = 1; i < 30000; ++i) if (std::abs (ys[i]) > std::abs (ys[peak])) peak = i;
        std::printf ("  tempo sync 1/4 at 120 BPM: first echo at %.1f ms\n", peak / 48.0);
        CHECK (std::abs ((double) peak - 24000.0) < 2.0, "tempo sync wrong");
        setParam (p, "delay_sync", 0.0f);
    }
    // Reverbs: decay tracks the setting and stays stable
    for (int type = 0; type < 4; ++type)
    {
        auto* rv = mod ("reverb");
        setParam (p, "reverb_type", (float) type);
        setParam (p, "reverb_decay", 2.0f);
        setParam (p, "reverb_mix", 1.0f);
        setParam (p, "reverb_predelay", 0.0f);
        rv->prepare (48000.0, 128);
        std::vector<float> imp (48000 * 4, 0.0f);
        imp[0] = 1.0f;
        std::vector<float> r;
        const auto y = runModule (*rv, imp, &r);
        // T60 from the energy decay between 0.2 s and 1.2 s
        const double e1 = rmsDb (y, 9600, 19200), e2 = rmsDb (y, 57600, 67200);
        const double t60 = 60.0 / ((e1 - e2) / 1.0);
        bool finite = true;
        for (float v : y) finite = finite && std::isfinite (v);
        const char* names[] = { "room", "hall", "plate", "spring" };
        std::printf ("  reverb %-6s decay 2.0 s -> measured T60 %.2f s\n", names[type], t60);
        CHECK (finite && t60 > 1.0 && t60 < 4.0, "reverb decay off or unstable");
    }
    // Modulation stays stable and actually modulates
    for (int type = 0; type < 3; ++type)
    {
        auto* m = mod ("mod");
        setParam (p, "mod_type", (float) type);
        setParam (p, "mod_feedback", 0.7f);
        m->prepare (48000.0, 128);
        const auto x = sineWave (440, 0.3f, 96000);
        std::vector<float> r;
        const auto y = runModule (*m, x, &r);
        double diff = 0, peakAbs = 0;
        for (size_t i = 0; i < y.size(); ++i) { diff += std::abs (y[i] - x[i]); peakAbs = std::max (peakAbs, (double) std::abs (y[i])); }
        CHECK (diff / (double) y.size() > 1e-3 && peakAbs < 2.0, "modulation type %d inactive or unstable", type);
    }
    // Chain order validation
    CHECK (! chain.setOrder (true, "gate,comp,od"), "partial order accepted");
    CHECK (! chain.setOrder (true, "gate,gate,od,boost,dist,fuzz"), "duplicate accepted");
    CHECK (chain.setOrder (true, "fuzz,dist,boost,od,comp,gate") && chain.getOrder (true) == "fuzz,dist,boost,od,comp,gate", "reorder failed");
    CHECK (chain.setOrder (false, "reverb,delay,mod,eq"), "post reorder failed");

    // Whole rig CPU: every module on
    for (auto& m : fx::allModules())
        setParam (p, String (m.id) + "_on", 1.0f);
    AudioBuffer<float> buf (2, 128);
    MidiBuffer midi;
    const auto x = sineWave (110, 0.3f, 48000 * 5);
    const auto t0 = Time::getMillisecondCounterHiRes();
    for (size_t pos = 0; pos + 128 <= x.size(); pos += 128)
    {
        buf.copyFrom (0, 0, x.data() + pos, 128);
        buf.copyFrom (1, 0, x.data() + pos, 128);
        p.processBlock (buf, midi);
    }
    const double ms = Time::getMillisecondCounterHiRes() - t0;
    std::printf ("  all 10 effects on (no amp model): %.1f%% of one core at 48 kHz\n", ms / 50.0);
    CHECK (std::isfinite (buf.getSample (0, 10)), "rig output not finite");
}

// ---------------------------------------------------------------------------
static void testLibraries (const File& dir)
{
    std::printf ("[library] models and presets\n");
    dir.deleteRecursively();
    ModelLibrary::rootOverride = dir;
    PresetLibrary::folderOverride = dir.getChildFile ("Presets");

    // two real NAM files with different metadata
    gas::train::WaveNet net (gas::train::WaveNetSpec::fromPreset (gas::train::Preset::Nano));
    net.initialise (3);
    const auto a = dir.getSiblingFile ("lead.nam"), b = dir.getSiblingFile ("clean.nam"), junk = dir.getSiblingFile ("junk.nam");
    a.replaceWithText (net.exportNam (R"({"name":"5150 Lead","gear_make":"Peavey","gear_model":"5150","gear_type":"amp","tone_type":"hi_gain","modeled_by":"Me"})", 48000.0));
    b.replaceWithText (net.exportNam (R"({"name":"Twin Clean","gear_make":"Fender","gear_model":"Twin Reverb","gear_type":"amp_cab","tone_type":"clean"})", 48000.0));
    junk.replaceWithText ("{ not a model");

    ModelLibrary lib;
    StringArray problems;
    const int n = lib.importFiles ({ a, b, junk }, problems);
    CHECK (n == 2 && problems.size() == 1, "import: %d imported, %d problems", n, problems.size());
    ModelLibrary::Filter f;
    f.toneType = "hi_gain";
    CHECK (lib.filtered (f).size() == 1 && lib.filtered (f)[0].gearModel == "5150", "tone filter");
    f = {};
    f.search = "fender";
    CHECK (lib.filtered (f).size() == 1, "search");
    const auto twin = lib.filtered (f)[0].file;
    lib.setFavourite (twin, true);
    lib.setDisplayName (twin, "My Twin");
    lib.setCollections (twin, { "Cleans", "Live" });
    lib.setDescription (twin, "Bright switch on");
    ModelLibrary again; // persisted in the index
    const auto* e = again.find (twin);
    CHECK (e != nullptr && e->favourite && e->displayName == "My Twin" && e->collections.contains ("Live") && e->description == "Bright switch on",
           "user data not persisted");
    f = {};
    f.favouritesOnly = true;
    CHECK (again.filtered (f).size() == 1, "favourites filter");
    String err;
    CHECK (again.remove (twin, err) && again.entries().size() == 1 && ! twin.existsAsFile(), "delete");

    // presets restore the full rig
    GuitarAmpProcessor p;
    p.prepareToPlay (48000.0, 256);
    setParam (p, "delay_on", 1.0f);
    setParam (p, "delay_time", 420.0f);
    setParam (p, "inputGain", 3.0f);
    p.getChain().setOrder (false, "reverb,eq,mod,delay");
    MemoryBlock state;
    p.getStateInformation (state);
    PresetLibrary presets;
    const auto pf = presets.save ("Lead Rig", state, err);
    CHECK (pf.existsAsFile(), "preset save: %s", err.toRawUTF8());
    CHECK (presets.rename (pf, "Lead Rig 2", err) && presets.presets().size() == 1 && presets.presets()[0].name() == "Lead Rig 2", "rename");

    GuitarAmpProcessor q;
    q.prepareToPlay (48000.0, 256);
    MemoryBlock loaded;
    CHECK (presets.load (presets.presets()[0].file, loaded, err), "preset load");
    q.setStateInformation (loaded.getData(), (int) loaded.getSize());
    CHECK (*q.getParameters().getRawParameterValue ("delay_on") > 0.5f
               && std::abs (*q.getParameters().getRawParameterValue ("delay_time") - 420.0f) < 0.5f
               && std::abs (*q.getParameters().getRawParameterValue ("inputGain") - 3.0f) < 0.05f,
           "preset parameters not restored");
    CHECK (q.getChain().getOrder (false) == "reverb,eq,mod,delay", "preset chain order not restored");
    presets.setFavourite (presets.presets()[0].file, true);
    CHECK (PresetLibrary().presets()[0].favourite, "preset favourite");
    std::printf ("  import/filter/search/favourite/rename/collections/delete and preset save/load/rename verified\n");
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

    // IRs for the CAB page snapshot (library in a temp folder, never the user's Documents)
    const auto irDir = png.getParentDirectory().getChildFile ("snapshot_irs");
    irDir.deleteRecursively();
    irDir.createDirectory();
    IRLibrary::folderOverride = irDir.getChildFile ("library");
    {
        Random rng (2);
        Array<File> files;
        for (auto name : { "4x12 V30 SM57 cap edge", "4x12 V30 R121 off-axis", "2x12 Blue AC open back", "1x12 Jensen C10Q" })
        {
            std::vector<float> ir (4800);
            for (size_t i = 0; i < ir.size(); ++i)
                ir[i] = std::exp (-(float) i / 300.0f) * (rng.nextFloat() - 0.5f);
            files.add (writeIR (irDir.getChildFile (String (name) + ".wav"), ir, 48000.0));
        }
        IRLibrary lib;
        StringArray skipped;
        lib.importFiles (files, skipped);
        lib.setTags (lib.folder().getChildFile ("4x12 V30 SM57 cap edge.wav"), { "4x12", "Closed-back", "Modern", "Metal" });
        lib.setTags (lib.folder().getChildFile ("4x12 V30 R121 off-axis.wav"), { "4x12", "Closed-back", "Rock" });
        lib.setTags (lib.folder().getChildFile ("2x12 Blue AC open back.wav"), { "2x12", "Open-back", "Vintage", "Clean" });
        lib.setTags (lib.folder().getChildFile ("1x12 Jensen C10Q.wav"), { "1x12", "Open-back", "Vintage" });
        String err;
        p.loadIR (0, lib.folder().getChildFile ("4x12 V30 SM57 cap edge.wav"), err);
        p.loadIR (1, lib.folder().getChildFile ("4x12 V30 R121 off-axis.wav"), err);
        p.getParameters().getParameter (ParamIDs::mic (1, "on"))->setValueNotifyingHost (1.0f);
    }

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

    if (auto* ed = dynamic_cast<GuitarAmpEditor*> (editor.get()))
    {
        ed->showSection (2);
        Timer::callPendingTimersSynchronously();
        Thread::sleep (300);
        Timer::callPendingTimersSynchronously();
        auto cabImage = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.5f);
        const auto cabPng = png.getSiblingFile ("cab_page.png");
        cabPng.deleteFile();
        FileOutputStream cabOut (cabPng);
        PNGImageFormat().writeImageToStream (cabImage, cabOut);

        for (auto id : { "comp", "od", "eq", "delay", "reverb" })
            setParam (p, String (id) + "_on", 1.0f);
        setParam (p, "eq_b2Gain", 4.0f);
        setParam (p, "eq_b4Gain", -3.0f);
        setParam (p, "eq_hpf", 90.0f);
        for (int section = 3; section <= 6; ++section)
        {
            ed->showSection (section);
            for (int k = 0; k < 6; ++k)
            {
                for (size_t pos = 0; pos + 256 <= di.size(); pos += 256)
                {
                    buf.copyFrom (0, 0, di.data() + pos, 256);
                    buf.copyFrom (1, 0, di.data() + pos, 256);
                    p.processBlock (buf, midi);
                    if (pos % (256 * 16) == 0) { Thread::sleep (40); Timer::callPendingTimersSynchronously(); }
                }
            }
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.5f);
            const char* names[] = { "", "", "", "stomp_page.png", "effects_page.png", "library_page.png", "settings_page.png" };
            const auto f = png.getSiblingFile (names[section]);
            f.deleteFile();
            FileOutputStream os (f);
            PNGImageFormat().writeImageToStream (img, os);
        }
    }
    editor->removeFromDesktop();
}

int main (int argc, char** argv)
{
    ScopedJuceInitialiser_GUI juce;

    testProcessing();
    testStateRoundTrip();
    testMissingModel();
    testEffects();
    testLibraries (File::getSpecialLocation (File::tempDirectory).getChildFile ("gas_library_test"));
    testCabinet (File::getSpecialLocation (File::tempDirectory).getChildFile ("gas_cab_test"));
    testCaptureMilestone (File::getSpecialLocation (File::tempDirectory).getChildFile ("gas_capture_test"));

    if (argc > 1 && SystemStats::getEnvironmentVariable ("DISPLAY", {}).isNotEmpty())
        snapshotEditor (File::getCurrentWorkingDirectory().getChildFile (argv[1]));

    std::printf (gFailures == 0 ? "\nALL PLUGIN TESTS PASSED\n" : "\n%d CHECK(S) FAILED\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
