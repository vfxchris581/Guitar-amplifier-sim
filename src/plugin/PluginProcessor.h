#pragma once

#include "engine/AmpEngine.h"
#include "plugin/CabEngine.h"
#include "plugin/CaptureController.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace ParamIDs
{
inline constexpr const char* inputGain          = "inputGain";
inline constexpr const char* outputGain         = "outputGain";
inline constexpr const char* outputMode         = "outputMode";
inline constexpr const char* calibrateInput     = "calibrateInput";
inline constexpr const char* interfaceInputDbu  = "interfaceInputDbu";
inline constexpr const char* interfaceOutputDbu = "interfaceOutputDbu";
inline constexpr const char* inputChannel       = "inputChannel";
inline constexpr const char* cabMode            = "cabMode";

/** Per-microphone parameter IDs: mic index 0 = "A", 1 = "B". */
inline juce::String mic (int index, const char* name) { return juce::String ("mic") + (index == 0 ? "A" : "B") + "_" + name; }
} // namespace ParamIDs

class GuitarAmpProcessor : public juce::AudioProcessor,
                           private juce::Timer
{
public:
    GuitarAmpProcessor();
    ~GuitarAmpProcessor() override;

    // AudioProcessor
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return GAS_PRODUCT_NAME; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // Model handling (message thread)
    void loadModel (const juce::File& file);
    void unloadModel();
    juce::File getLastBrowseDirectory() const { return lastBrowseDir; }

    gas::AmpEngine& getEngine() noexcept { return engine; }
    CaptureController& getCapture() noexcept { return capture; }
    CabEngine& getCab() noexcept { return cab; }

    enum class CabMode { Auto = 0, On = 1, Off = 2 };
    /** Whether the cabinet stage is processing, and why (shown in the CAB page). */
    bool isCabActive (juce::String* reason = nullptr) const;
    bool loadIR (int mic, const juce::File& file, juce::String& error);
    void clearIR (int mic);
    juce::String getIRPath (int mic) const { return irPaths[(size_t) mic]; }
    double getHostSampleRate() const noexcept { return hostRate.load(); }
    juce::AudioProcessorValueTreeState& getParameters() noexcept { return parameters; }
    double getCpuLoadPercent() const { return loadMeasurer.getLoadAsPercentage(); }
    int getXRunCount() const { return loadMeasurer.getXRunCount(); }

    enum class InputChannel { Left = 0, Right = 1, Sum = 2 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override; // reports latency changes to the host
    void pushParametersToEngine() noexcept;
    bool processCaptureOrAudition (juce::AudioBuffer<float>&) noexcept;

    gas::AmpEngine engine;
    CaptureController capture;
    CabEngine cab;
    std::vector<float> monoScratch;
    std::atomic<bool> cabActive { false };
    std::array<juce::String, 2> irPaths;
    std::array<float, 2> appliedIrLength { -1.0f, -1.0f };
    std::array<int, 2> appliedIrNormalise { -1, -1 };
    void pushCabParameters() noexcept;
    void updateCabActive();
    std::vector<float> captureIn, captureOut; // preallocated in prepareToPlay
    std::atomic<double> hostRate { 0.0 };
    juce::AudioProcessorValueTreeState parameters;
    juce::AudioProcessLoadMeasurer loadMeasurer;
    juce::File lastBrowseDir;
    int reportedLatency = -1;

    std::atomic<float>* inputGain = nullptr;
    std::atomic<float>* outputGain = nullptr;
    std::atomic<float>* outputMode = nullptr;
    std::atomic<float>* calibrateInput = nullptr;
    std::atomic<float>* interfaceInputDbu = nullptr;
    std::atomic<float>* interfaceOutputDbu = nullptr;
    std::atomic<float>* inputChannel = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GuitarAmpProcessor)
};
