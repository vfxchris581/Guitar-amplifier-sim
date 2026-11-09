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

    for (int ch = 1; ch < numOut; ++ch)
        buffer.copyFrom (ch, 0, buffer, 0, 0, numSamples);
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
