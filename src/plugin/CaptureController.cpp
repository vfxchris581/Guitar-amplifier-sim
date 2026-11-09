#include "plugin/CaptureController.h"

#include "common/WavIO.h"
#include "json.hpp"

#include <BinaryData.h>
#include <juce_data_structures/juce_data_structures.h>

#include <chrono>
#include <cmath>

using namespace gas;

namespace
{
juce::PropertiesFile& settings()
{
    static juce::PropertiesFile file ([]
    {
        juce::PropertiesFile::Options o;
        o.applicationName = GAS_PRODUCT_NAME;
        o.filenameSuffix = "settings";
        o.folderName = GAS_PRODUCT_NAME;
        o.osxLibrarySubFolder = "Application Support";
        return o;
    }());
    return file;
}

bool is48k (double rate) { return std::abs (rate - 48000.0) < 0.5; }

constexpr size_t kLevelCheckFrom = 480000;  // silence + blips + chirps + noise (10 s .. 17 s)
constexpr size_t kLevelCheckTo = 816000;
constexpr size_t kTail = 48000;              // record 1 s past the end of the playback

float rms (const std::vector<float>& x)
{
    double s = 0.0;
    for (float v : x)
        s += (double) v * v;
    return x.empty() ? 0.0f : (float) std::sqrt (s / (double) x.size());
}
} // namespace

// ---------------------------------------------------------------------------
void AuditionPlayer::setMaterial (std::vector<float> reference, std::vector<float> model, float referenceGain,
                                  float modelGain)
{
    mode.store (Off);
    while (inUse.load())
        std::this_thread::yield();
    ref = std::move (reference);
    mdl = std::move (model);
    gRef = referenceGain;
    gModel = modelGain;
    pos.store (0);
}

bool AuditionPlayer::process (float* out, int n) noexcept
{
    const int m = mode.load();
    if (m == Off)
        return false;
    inUse.store (true);
    if (mode.load() == Off || ref.empty() || ref.size() != mdl.size())
    {
        inUse.store (false);
        return false;
    }
    size_t p = pos.load();
    for (int i = 0; i < n; ++i)
    {
        const float r = ref[p] * gRef, md = mdl[p] * gModel;
        out[i] = m == Reference ? r : (m == Model ? md : r - md);
        if (++p >= ref.size())
            p = 0;
    }
    pos.store (p);
    inUse.store (false);
    return true;
}

// ---------------------------------------------------------------------------
CaptureController::CaptureController()
{
    double rate = 0.0;
    if (! wav::readWavMonoFromMemory (BinaryData::loudness_input_wav, (size_t) BinaryData::loudness_input_wavSize,
                                      stimulusForLoudness, rate)
        || rate != 48000.0)
        stimulusForLoudness.clear();
}

CaptureController::~CaptureController()
{
    cancelTraining();
    if (trainingThread.joinable())
        trainingThread.join();
}

bool CaptureController::loadSignal (const juce::File& file, juce::String& error)
{
    std::string err;
    auto s = capture::CaptureSignal::loadNamV3 (file.getFullPathName().toStdString(), err);
    if (s.empty())
    {
        error = err;
        return false;
    }
    signal = std::move (s);
    settings().setValue ("captureSignalPath", file.getFullPathName());
    settings().saveIfNeeded();
    return true;
}

juce::String CaptureController::savedSignalPath() const
{
    return settings().getValue ("captureSignalPath");
}

bool CaptureController::ensureSignal (juce::String& error)
{
    if (hasSignal())
        return true;
    const auto saved = savedSignalPath();
    if (saved.isEmpty())
    {
        error = "Load NAM's capture signal first.";
        return false;
    }
    return loadSignal (juce::File (saved), error);
}

bool CaptureController::startLevelCheck (double hostRate, juce::String& error)
{
    if (! ensureSignal (error))    return false;
    if (! is48k (hostRate))        { error = "Set your DAW project to 48 kHz: NAM captures are made at 48 kHz."; return false; }
    if (phase() != Phase::Idle)    { error = "Already running."; return false; }
    if (! transport.arm (signal.samples, kLevelCheckFrom, kLevelCheckTo, kTail))
    {
        error = "Could not prepare the level check.";
        return false;
    }
    levelCheck.reset();
    currentPhase = Phase::LevelCheck;
    transport.start();
    return true;
}

bool CaptureController::startCapture (double hostRate, juce::String& error)
{
    if (! ensureSignal (error))    return false;
    if (! is48k (hostRate))        { error = "Set your DAW project to 48 kHz: NAM captures are made at 48 kHz."; return false; }
    if (phase() != Phase::Idle)    { error = "Already running."; return false; }
    if (isTraining())              { error = "Wait for training to finish (or cancel it) first."; return false; }
    if (! transport.arm (signal.samples, 0, signal.samples.size(), kTail))
    {
        error = "Could not prepare the capture.";
        return false;
    }
    capture.reset();
    currentPhase = Phase::Capturing;
    transport.start();
    return true;
}

void CaptureController::cancel()
{
    transport.cancel();
}

bool CaptureController::poll()
{
    const auto st = transport.state();
    const auto ph = phase();
    if (ph == Phase::Idle)
        return false;

    if (st == capture::CaptureTransport::State::Cancelled)
    {
        transport.reset();
        currentPhase = Phase::Idle;
        return true;
    }
    if (st != capture::CaptureTransport::State::Finished)
        return false;

    const auto& rec = transport.recording();
    if (ph == Phase::LevelCheck)
    {
        // Place the recording at its absolute position in the signal so NAM's
        // v3 indices (background interval, blip positions) apply directly.
        std::vector<float> y (kLevelCheckFrom, 0.0f);
        y.insert (y.end(), rec.begin(), rec.end());
        LevelCheckResult r;
        r.latency = capture::calibrateLatencyV3 (y);
        r.levels = capture::analyseLevels (y);
        levelCheck = std::move (r);
    }
    else
    {
        CaptureResult r;
        r.recording = rec;
        r.latency = capture::calibrateLatencyV3 (r.recording);
        r.levels = capture::analyseLevels (r.recording);
        r.replicate = capture::checkValidationReplicate (r.recording, signal.samples.size());
        r.usable = r.latency.recommended.has_value() && ! r.levels.silent && ! r.latency.matchesLookahead;
        capture = std::move (r);
    }
    transport.reset();
    currentPhase = Phase::Idle;
    return true;
}

// ---------------------------------------------------------------------------
bool CaptureController::startTraining (const train::TrainOptions& options, const ModelDetails& details,
                                       juce::String& error)
{
    if (isTraining())
    {
        error = "Training is already running.";
        return false;
    }
    if (! capture || ! capture->usable)
    {
        error = "Make a successful capture first.";
        return false;
    }
    if (trainingThread.joinable())
        trainingThread.join();

    const int latency = *capture->latency.recommended;
    auto data = capture::prepareV3 (signal.samples, capture->recording, latency);

    {
        std::lock_guard<std::mutex> lk (statusMutex);
        status = {};
        status.message = "Starting...";
    }
    validation.reset();
    pendingNamJson.clear();
    cancelTrainingFlag = false;
    trainingRunning = true;
    trainingThread = std::thread ([this, data = std::move (data), options, details, latency]() mutable
                                  { trainingThreadBody (std::move (data), options, details, latency); });
    return true;
}

void CaptureController::cancelTraining()
{
    cancelTrainingFlag = true;
}

CaptureController::TrainingStatus CaptureController::trainingStatus() const
{
    std::lock_guard<std::mutex> lk (statusMutex);
    return status;
}

void CaptureController::trainingThreadBody (capture::PreparedData data, train::TrainOptions options,
                                            ModelDetails details, int latency)
{
    auto result = train::trainWaveNet (data.trainInput, data.trainOutput, data.validationInput,
                                       data.validationOutput, options,
                                       [this] (const train::TrainProgress& p)
                                       {
                                           std::lock_guard<std::mutex> lk (statusMutex);
                                           status.progress = p;
                                           status.message = "Training";
                                       },
                                       cancelTrainingFlag);

    if (result.cancelled || ! result.success)
    {
        std::lock_guard<std::mutex> lk (statusMutex);
        status.cancelled = result.cancelled;
        status.failed = ! result.cancelled;
        status.message = result.cancelled ? juce::String ("Training cancelled - no model was produced.")
                                          : "Training failed: " + juce::String (result.error);
        trainingRunning = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lk (statusMutex);
        status.message = "Validating the model...";
    }

    // Validate: run the best model over the held-out validation section.
    train::WaveNet net (result.spec);
    net.params() = result.bestParams;
    net.unpack();
    const int R = net.spec().receptiveField();
    Validation v;
    v.model = train::runModel (net, data.validationInput);
    v.reference.assign (data.validationOutput.begin() + (R - 1), data.validationOutput.end());
    v.esr = train::esr (v.model.data(), v.reference.data(), v.reference.size());
    v.parameters = (int) net.params().size();
    v.architecture = juce::String ("WaveNet (") + train::presetName (options.preset) + ")";

    // Metadata in NAM's schema
    nlohmann::json meta = nlohmann::json::object();
    auto put = [&meta] (const char* k, const juce::String& v)
    {
        if (v.isNotEmpty())
            meta[k] = v.toStdString();
    };
    put ("name", details.name);
    put ("modeled_by", details.modeledBy);
    put ("gear_make", details.gearMake);
    put ("gear_model", details.gearModel);
    put ("gear_type", details.gearType);
    put ("tone_type", details.toneType);
    if (details.inputLevelDbu)  meta["input_level_dbu"] = *details.inputLevelDbu;
    if (details.outputLevelDbu) meta["output_level_dbu"] = *details.outputLevelDbu;
    if (! stimulusForLoudness.empty())
    {
        const double loud = train::measureLoudnessDb (net, stimulusForLoudness);
        if (std::isfinite (loud))
            meta["loudness"] = loud;
    }
    const auto now = juce::Time::getCurrentTime();
    meta["date"] = { { "year", now.getYear() }, { "month", now.getMonth() + 1 }, { "day", now.getDayOfMonth() },
                     { "hour", now.getHours() }, { "minute", now.getMinutes() }, { "second", now.getSeconds() } };
    const auto& cal = capture->latency;
    meta["training"] = {
        { "settings", { { "ignore_checks", false } } },
        { "data",
          { { "latency",
              { { "manual", nullptr },
                { "calibration",
                  { { "algorithm_version", 1 },
                    { "delays", cal.detected ? nlohmann::json::array ({ *cal.detected }) : nlohmann::json::array() },
                    { "safety_factor", 1 },
                    { "recommended", latency },
                    { "warnings",
                      { { "matches_lookahead", cal.matchesLookahead },
                        { "disagreement_too_high", false },
                        { "not_detected", cal.notDetected } } } } } } },
            { "checks", { { "version", 3 }, { "passed", capture->replicate.passed } } } } },
        { "validation_esr", v.esr },
    };
    meta["trainer"] = std::string (GAS_PRODUCT_NAME) + " " + GAS_VERSION_STRING + " (C++ port of NAM's WaveNet trainer)";
    if (details.settingsNote.isNotEmpty())
        meta["capture_settings"] = details.settingsNote.toStdString();

    const auto json = net.exportNam (meta.dump(), 48000.0);

    // Level-match reference and model to the same RMS for a fair A/B.
    const float target = std::pow (10.0f, -20.0f / 20.0f);
    const float rRef = rms (v.reference), rModel = rms (v.model);
    audition.setMaterial (v.reference, v.model, rRef > 0 ? target / rRef : 1.0f, rModel > 0 ? target / rModel : 1.0f);

    {
        std::lock_guard<std::mutex> lk (statusMutex);
        validation = std::move (v);
        pendingNamJson = json;
        pendingDetails = details;
        status.finished = true;
        status.message = "Model trained and validated.";
    }
    trainingRunning = false;
}

juce::File CaptureController::capturesFolder()
{
    if (capturesFolderOverride != juce::File())
        return capturesFolderOverride;
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile (GAS_PRODUCT_NAME)
        .getChildFile ("Captures");
}

juce::File CaptureController::saveModel (juce::String& error)
{
    juce::String json, name;
    {
        std::lock_guard<std::mutex> lk (statusMutex);
        json = pendingNamJson;
        name = pendingDetails.name;
    }
    if (json.isEmpty())
    {
        error = "There is no trained model to save.";
        return {};
    }
    if (name.trim().isEmpty())
        name = "Capture " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M");
    const auto safe = juce::File::createLegalFileName (name.trim());
    auto dir = capturesFolder().getChildFile (safe);
    if (dir.exists())
        dir = dir.getNonexistentSibling();
    if (! dir.createDirectory())
    {
        error = "Could not create " + dir.getFullPathName();
        return {};
    }

    const auto namFile = dir.getChildFile (safe + ".nam");
    if (! namFile.replaceWithText (json))
    {
        error = "Could not write " + namFile.getFullPathName();
        return {};
    }

    // Keep the raw return, cropped to the signal's length: it is a valid NAM
    // "output.wav" (pair it with NAM's input file to retrain anywhere).
    if (capture)
    {
        std::vector<float> out (capture->recording.begin(),
                                capture->recording.begin() + (long) std::min (capture->recording.size(), signal.samples.size()));
        wav::writeWavMono24 (dir.getChildFile ("output.wav").getFullPathName().toStdString(), out, 48000.0);
    }
    return namFile;
}

void CaptureController::discardModel()
{
    audition.setMaterial ({}, {}, 1.0f, 1.0f);
    std::lock_guard<std::mutex> lk (statusMutex);
    pendingNamJson.clear();
    validation.reset();
    status = {};
}
