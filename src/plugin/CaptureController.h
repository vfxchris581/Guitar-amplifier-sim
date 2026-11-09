#pragma once

#include "capture/CaptureAnalysis.h"
#include "capture/CaptureSignal.h"
#include "capture/CaptureTransport.h"
#include "train/Trainer.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <mutex>
#include <optional>
#include <thread>

/** Plays one of several pre-rendered buffers (looping) for A/B comparison. Real-time safe. */
class AuditionPlayer
{
public:
    enum Mode { Off = 0, Reference = 1, Model = 2, Difference = 3 };

    /** Message thread. Stops playback, then swaps in new material. */
    void setMaterial (std::vector<float> reference, std::vector<float> model, float referenceGain, float modelGain);
    void setMode (Mode m) noexcept { mode.store (m); }
    Mode getMode() const noexcept { return (Mode) mode.load(); }
    bool hasMaterial() const noexcept { return ! ref.empty(); }

    /** Audio thread: writes mono audio and returns true when auditioning. */
    bool process (float* out, int n) noexcept;
    float positionSeconds() const noexcept { return (float) pos.load() / 48000.0f; }

private:
    std::vector<float> ref, mdl;
    float gRef = 1.0f, gModel = 1.0f;
    std::atomic<int> mode { Off };
    std::atomic<bool> inUse { false };
    std::atomic<size_t> pos { 0 };
};

/** Owns the whole capture workflow outside the audio thread: the verified NAM
    capture signal, the transport, analysis, the training job and validation. */
class CaptureController
{
public:
    CaptureController();
    ~CaptureController();

    // ---- capture signal ----------------------------------------------------
    bool loadSignal (const juce::File& file, juce::String& error);
    bool hasSignal() const { return ! signal.empty(); }
    /** Loads the previously chosen signal file on first use (it is ~36 MB, so not at plugin load). */
    bool ensureSignal (juce::String& error);
    juce::String savedSignalPath() const;
    juce::String signalPath() const { return juce::String (signal.sourcePath); }

    // ---- routing (read by the audio thread) --------------------------------
    enum class SendChannel { Left = 0, Right = 1, Both = 2 };
    std::atomic<int> sendChannel { (int) SendChannel::Left };
    std::atomic<int> returnChannel { 1 }; // 0 = left, 1 = right

    // ---- level check & capture --------------------------------------------
    enum class Phase { Idle, LevelCheck, Capturing };
    bool startLevelCheck (double hostRate, juce::String& error);
    bool startCapture (double hostRate, juce::String& error);
    void cancel();
    Phase phase() const { return currentPhase.load(); }

    /** Message thread, polled by the UI: finishes a completed transport run. Returns true if something changed. */
    bool poll();

    gas::capture::CaptureTransport transport;

    struct LevelCheckResult
    {
        gas::capture::LatencyCalibration latency;
        gas::capture::LevelReport levels;
    };
    std::optional<LevelCheckResult> levelCheck;

    struct CaptureResult
    {
        std::vector<float> recording; // full return, input-length + tail
        gas::capture::LatencyCalibration latency;
        gas::capture::LevelReport levels;
        gas::capture::ReplicateCheck replicate;
        bool usable = false;
    };
    std::optional<CaptureResult> capture;

    // ---- training ------------------------------------------------------------
    struct ModelDetails // becomes the .nam metadata
    {
        juce::String name, modeledBy, gearMake, gearModel, gearType = "amp", toneType, settingsNote;
        std::optional<double> inputLevelDbu, outputLevelDbu;
    };

    bool startTraining (const gas::train::TrainOptions&, const ModelDetails&, juce::String& error);
    void cancelTraining();
    bool isTraining() const { return trainingRunning.load(); }

    struct TrainingStatus
    {
        gas::train::TrainProgress progress;
        juce::String message;
        bool finished = false, cancelled = false, failed = false;
    };
    TrainingStatus trainingStatus() const;

    // ---- validation & saving -----------------------------------------------
    struct Validation
    {
        double esr = 0.0;
        std::vector<float> reference, model; // aligned validation section at 48 kHz
        int parameters = 0;
        juce::String architecture;
    };
    std::optional<Validation> validation;
    AuditionPlayer audition;

    /** Writes the trained model (and the aligned recording, NAM-trainer compatible) to the captures folder. */
    juce::File saveModel (juce::String& error);
    void discardModel();
    static juce::File capturesFolder();

    // ---- testing hooks -------------------------------------------------------
    /** Uses an arbitrary signal with the v3 layout (tests only; skips NAM's file check). */
    void setSignalForTesting (std::vector<float> samples) { signal.samples = std::move (samples); signal.sourcePath = "(test)"; }
    static inline juce::File capturesFolderOverride;

private:
    void trainingThreadBody (gas::capture::PreparedData data, gas::train::TrainOptions options, ModelDetails details, int latency);

    gas::capture::CaptureSignal signal;
    std::atomic<Phase> currentPhase { Phase::Idle };

    std::thread trainingThread;
    std::atomic<bool> trainingRunning { false }, cancelTrainingFlag { false };
    mutable std::mutex statusMutex;
    TrainingStatus status;

    juce::String pendingNamJson;     // trained model ready to save
    ModelDetails pendingDetails;
    std::vector<float> stimulusForLoudness;
};
