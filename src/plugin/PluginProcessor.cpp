#include "plugin/PluginProcessor.h"
#include "plugin/PluginEditor.h"

namespace
{
juce::String dbText (float v, int)  { return juce::String (v, 1) + " dB"; }
juce::String dbuText (float v, int) { return juce::String (v, 1) + " dBu"; }
} // namespace

GuitarAmpProcessor::GuitarAmpProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Guitar In", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "STATE", createLayout())
{
    inputGain          = parameters.getRawParameterValue (ParamIDs::inputGain);
    outputGain         = parameters.getRawParameterValue (ParamIDs::outputGain);
    outputMode         = parameters.getRawParameterValue (ParamIDs::outputMode);
    calibrateInput     = parameters.getRawParameterValue (ParamIDs::calibrateInput);
    interfaceInputDbu  = parameters.getRawParameterValue (ParamIDs::interfaceInputDbu);
    interfaceOutputDbu = parameters.getRawParameterValue (ParamIDs::interfaceOutputDbu);
    inputChannel       = parameters.getRawParameterValue (ParamIDs::inputChannel);

    lastBrowseDir = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    startTimerHz (10);
}

GuitarAmpProcessor::~GuitarAmpProcessor()
{
    stopTimer();
}

juce::AudioProcessorValueTreeState::ParameterLayout GuitarAmpProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    auto dbAttr  = AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText);
    auto dbuAttr = AudioParameterFloatAttributes().withLabel ("dBu").withStringFromValueFunction (dbuText);

    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::inputGain, 1 }, "Input",
                                                       NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, dbAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::outputGain, 1 }, "Output",
                                                       NormalisableRange<float> (-40.0f, 12.0f, 0.1f), 0.0f, dbAttr));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::outputMode, 1 }, "Output Mode",
                                                        StringArray { "Raw", "Normalized", "Calibrated" }, 1));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { ParamIDs::calibrateInput, 1 }, "Calibrate Input",
                                                      false));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::interfaceInputDbu, 1 },
                                                       "Interface Input Level",
                                                       NormalisableRange<float> (-10.0f, 30.0f, 0.1f), 12.0f, dbuAttr));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::interfaceOutputDbu, 1 },
                                                       "Interface Output Level",
                                                       NormalisableRange<float> (-10.0f, 30.0f, 0.1f), 12.0f, dbuAttr));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::inputChannel, 1 }, "Input Channel",
                                                        StringArray { "Left", "Right", "Left + Right" }, 0));

    // Cabinet
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::cabMode, 1 }, "Cab Mode",
                                                        StringArray { "Auto", "On", "Off" }, 0));
    auto hz = AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (
        [] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, 1) + " kHz" : String ((int) v) + " Hz"; });
    auto ms = AudioParameterFloatAttributes().withLabel ("ms").withStringFromValueFunction (
        [] (float v, int) { return String (v, 2) + " ms"; });
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction (
        [] (float v, int) { return std::abs (v) < 0.005f ? String ("C") : (v < 0 ? "L" : "R") + String ((int) std::lround (std::abs (v) * 100)); });
    for (int m = 0; m < 2; ++m)
    {
        const String name = m == 0 ? "Mic A " : "Mic B ";
        auto id = [m] (const char* n) { return ParameterID { ParamIDs::mic (m, n), 1 }; };
        layout.add (std::make_unique<AudioParameterBool> (id ("on"), name + "On", m == 0));
        layout.add (std::make_unique<AudioParameterFloat> (id ("level"), name + "Level", NormalisableRange<float> (-40.0f, 12.0f, 0.1f), 0.0f, dbAttr));
        layout.add (std::make_unique<AudioParameterFloat> (id ("pan"), name + "Pan", NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f, pct));
        layout.add (std::make_unique<AudioParameterBool> (id ("invert"), name + "Phase Invert", false));
        layout.add (std::make_unique<AudioParameterFloat> (id ("delay"), name + "Delay", NormalisableRange<float> (0.0f, CabEngine::maxDelayMs, 0.01f), 0.0f, ms));
        layout.add (std::make_unique<AudioParameterFloat> (id ("hpf"), name + "High-pass", NormalisableRange<float> (20.0f, 1000.0f, 1.0f, 0.4f), 20.0f, hz));
        layout.add (std::make_unique<AudioParameterFloat> (id ("lpf"), name + "Low-pass", NormalisableRange<float> (1000.0f, 20000.0f, 1.0f, 0.4f), 20000.0f, hz));
        layout.add (std::make_unique<AudioParameterFloat> (id ("length"), name + "IR Length", NormalisableRange<float> (10.0f, 1000.0f, 1.0f, 0.5f), 500.0f, ms));
        layout.add (std::make_unique<AudioParameterBool> (id ("normalise"), name + "Normalise IR", true));
    }
    return layout;
}

bool GuitarAmpProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    const auto ok = [] (const juce::AudioChannelSet& s)
    { return s == juce::AudioChannelSet::mono() || s == juce::AudioChannelSet::stereo(); };
    return ok (in) && ok (out);
}

void GuitarAmpProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock);
    loadMeasurer.reset (sampleRate, samplesPerBlock);
    cab.prepare (sampleRate, samplesPerBlock);
    monoScratch.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    captureIn.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    captureOut.assign ((size_t) juce::jmax (1, samplesPerBlock), 0.0f);
    hostRate = sampleRate;
}

void GuitarAmpProcessor::pushParametersToEngine() noexcept
{
    engine.inputGainDb        = inputGain->load();
    engine.outputGainDb       = outputGain->load();
    engine.outputMode         = (int) outputMode->load();
    engine.calibrateInput     = calibrateInput->load() > 0.5f;
    engine.interfaceInputDbu  = interfaceInputDbu->load();
    engine.interfaceOutputDbu = interfaceOutputDbu->load();
}

void GuitarAmpProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::AudioProcessLoadMeasurer::ScopedTimer timer (loadMeasurer, buffer.getNumSamples());
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    if (numSamples == 0 || buffer.getNumChannels() == 0)
        return;

    if (processCaptureOrAudition (buffer))
        return;

    pushParametersToEngine();

    // The amp is a mono device: pick the guitar from the chosen input channel.
    float* mono = buffer.getWritePointer (0);
    if (numIn >= 2)
    {
        switch ((InputChannel) (int) inputChannel->load())
        {
            case InputChannel::Right:
                buffer.copyFrom (0, 0, buffer, 1, 0, numSamples);
                break;
            case InputChannel::Sum:
                buffer.addFrom (0, 0, buffer, 1, 0, numSamples);
                buffer.applyGain (0, 0, numSamples, 0.5f);
                break;
            case InputChannel::Left:
            default:
                break;
        }
    }
    else if (numIn == 0)
    {
        buffer.clear (0, 0, numSamples);
    }

    engine.process (mono, numSamples);

    // Cabinet: mono amp signal in, stereo out
    if (cabActive.load() && ! monoScratch.empty())
    {
        pushCabParameters();
        float* right = numOut > 1 ? buffer.getWritePointer (1) : nullptr;
        for (int done = 0; done < numSamples;)
        {
            const int n = juce::jmin ((int) monoScratch.size(), numSamples - done);
            std::copy (mono + done, mono + done + n, monoScratch.begin());
            if (right != nullptr)
            {
                if (! cab.process (monoScratch.data(), mono + done, right + done, n))
                    std::copy (monoScratch.begin(), monoScratch.begin() + n, right + done);
            }
            else
            {
                // mono output: fold the cab's stereo down
                float* tmpR = captureOut.data(); // free while not capturing
                if (cab.process (monoScratch.data(), mono + done, tmpR, n))
                    for (int i = 0; i < n; ++i)
                        mono[done + i] = 0.5f * (mono[done + i] + tmpR[i]);
            }
            done += n;
        }
        for (int ch = 2; ch < numOut; ++ch)
            buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
        return;
    }

    for (int ch = 1; ch < numOut; ++ch)
        buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
}

void GuitarAmpProcessor::pushCabParameters() noexcept
{
    for (int m = 0; m < CabEngine::numMics; ++m)
    {
        auto& p = cab.mics[(size_t) m];
        auto v = [this, m] (const char* n) { return parameters.getRawParameterValue (ParamIDs::mic (m, n))->load(); };
        p.enabled = v ("on") > 0.5f;
        p.levelDb = v ("level");
        p.pan = v ("pan");
        p.invert = v ("invert") > 0.5f;
        p.delayMs = v ("delay");
        p.hpfHz = v ("hpf");
        p.lpfHz = v ("lpf");
    }
}

bool GuitarAmpProcessor::isCabActive (juce::String* reason) const
{
    const auto mode = (CabMode) (int) parameters.getRawParameterValue (ParamIDs::cabMode)->load();
    bool active = false;
    juce::String why;
    if (mode == CabMode::Off)
        why = "Cabinet switched off.";
    else if (! cab.hasIR (0) && ! cab.hasIR (1))
        why = "No impulse response loaded - the amp is heard without a cabinet.";
    else if (mode == CabMode::Auto)
    {
        const auto status = engine.getStatus();
        if (status.info && status.info->includesCabinet())
            why = "Bypassed automatically: the loaded amp capture already includes a cabinet (gear type \""
                  + juce::String (status.info->gearType) + "\"). Set the cab to On to use both anyway.";
        else
        {
            active = true;
            why = "Active.";
        }
    }
    else
    {
        active = true;
        why = "Active (forced on).";
    }
    if (reason != nullptr)
        *reason = why;
    return active;
}

void GuitarAmpProcessor::updateCabActive()
{
    cabActive = isCabActive();
}

bool GuitarAmpProcessor::loadIR (int mic, const juce::File& file, juce::String& error)
{
    const float length = parameters.getRawParameterValue (ParamIDs::mic (mic, "length"))->load();
    const bool normalise = parameters.getRawParameterValue (ParamIDs::mic (mic, "normalise"))->load() > 0.5f;
    if (! cab.loadIR (mic, file, length, normalise, error))
        return false;
    irPaths[(size_t) mic] = file.getFullPathName();
    appliedIrLength[(size_t) mic] = length;
    appliedIrNormalise[(size_t) mic] = normalise ? 1 : 0;
    updateCabActive();
    return true;
}

void GuitarAmpProcessor::clearIR (int mic)
{
    cab.clearIR (mic);
    irPaths[(size_t) mic].clear();
    updateCabActive();
}

bool GuitarAmpProcessor::processCaptureOrAudition (juce::AudioBuffer<float>& buffer) noexcept
{
    auto& transport = capture.transport;
    const bool capturing = transport.state() == gas::capture::CaptureTransport::State::Running;
    const bool auditioning = capture.audition.getMode() != AuditionPlayer::Off;
    if (! capturing && ! auditioning)
        return false;

    const int numSamples = buffer.getNumSamples();
    const int numIn = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    const int returnCh = juce::jlimit (0, juce::jmax (0, numIn - 1), capture.returnChannel.load());
    const auto send = (CaptureController::SendChannel) capture.sendChannel.load();
    const int maxChunk = (int) captureIn.size();
    if (maxChunk == 0)
        return false;

    for (int done = 0; done < numSamples;)
    {
        const int n = juce::jmin (maxChunk, numSamples - done);
        if (numIn > 0)
            std::copy (buffer.getReadPointer (returnCh) + done, buffer.getReadPointer (returnCh) + done + n, captureIn.begin());
        else
            std::fill (captureIn.begin(), captureIn.begin() + n, 0.0f);

        if (transport.process (captureIn.data(), captureOut.data(), n))
        {
            // Capture signal only on the send channel(s); everything else silent.
            for (int ch = 0; ch < numOut; ++ch)
            {
                const bool isSend = send == CaptureController::SendChannel::Both
                                    || (send == CaptureController::SendChannel::Left && ch == 0)
                                    || (send == CaptureController::SendChannel::Right && ch == 1);
                if (isSend)
                    buffer.copyFrom (ch, done, captureOut.data(), n);
                else
                    buffer.clear (ch, done, n);
            }
        }
        else if (capture.audition.process (captureOut.data(), n))
        {
            for (int ch = 0; ch < numOut; ++ch)
                buffer.copyFrom (ch, done, captureOut.data(), n);
        }
        else
        {
            for (int ch = 0; ch < numOut; ++ch)
                buffer.clear (ch, done, n);
        }
        done += n;
    }
    return true;
}

void GuitarAmpProcessor::timerCallback()
{
    // IR length / normalisation changes are applied here, on the message thread.
    for (int m = 0; m < CabEngine::numMics; ++m)
    {
        if (! cab.hasIR (m))
            continue;
        const float length = parameters.getRawParameterValue (ParamIDs::mic (m, "length"))->load();
        const int normalise = parameters.getRawParameterValue (ParamIDs::mic (m, "normalise"))->load() > 0.5f ? 1 : 0;
        if (std::abs (length - appliedIrLength[(size_t) m]) > 0.5f || normalise != appliedIrNormalise[(size_t) m])
        {
            cab.reloadIR (m, length, normalise == 1);
            appliedIrLength[(size_t) m] = length;
            appliedIrNormalise[(size_t) m] = normalise;
        }
    }
    updateCabActive();

    const int latency = engine.getLatencySamples();
    if (latency != reportedLatency)
    {
        reportedLatency = latency;
        setLatencySamples (latency);
    }
}

// ---------------------------------------------------------------------------
void GuitarAmpProcessor::loadModel (const juce::File& file)
{
    lastBrowseDir = file.getParentDirectory();
    engine.loadModel (gas::utf8ToPath (file.getFullPathName().toStdString()));
}

void GuitarAmpProcessor::unloadModel()
{
    engine.unloadModel();
}

void GuitarAmpProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = parameters.copyState();
    state.setProperty ("modelPath", juce::String (engine.requestedModelPath()), nullptr);
    state.setProperty ("browseDir", lastBrowseDir.getFullPathName(), nullptr);
    state.setProperty ("irPathA", irPaths[0], nullptr);
    state.setProperty ("irPathB", irPaths[1], nullptr);
    state.setProperty ("version", GAS_VERSION_STRING, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void GuitarAmpProcessor::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    parameters.replaceState (state);

    const auto browse = state.getProperty ("browseDir").toString();
    if (browse.isNotEmpty())
        lastBrowseDir = juce::File (browse);

    // Missing files are reported in the AMP page while the path is kept, so the
    // session still knows which model it expects.
    for (int m = 0; m < CabEngine::numMics; ++m)
    {
        const auto ir = state.getProperty (m == 0 ? "irPathA" : "irPathB").toString();
        if (ir.isEmpty())
        {
            if (cab.hasIR (m))
                clearIR (m);
            continue;
        }
        juce::String err;
        if (! loadIR (m, juce::File (ir), err))
            irPaths[(size_t) m] = ir; // keep the reference; the CAB page reports it as missing
    }

    const auto path = state.getProperty ("modelPath").toString();
    if (path.isNotEmpty())
        engine.loadModel (gas::utf8ToPath (path.toStdString()));
    else if (! engine.requestedModelPath().empty())
        engine.unloadModel();
}

juce::AudioProcessorEditor* GuitarAmpProcessor::createEditor()
{
    return new GuitarAmpEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GuitarAmpProcessor();
}
