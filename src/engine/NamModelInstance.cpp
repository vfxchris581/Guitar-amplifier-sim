#include "engine/NamModelInstance.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "engine/ResamplerShim.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace gas
{
namespace
{
constexpr double kNamDefaultSampleRate = 48000.0; // NAM convention for files without "sample_rate"

std::string readString (const nlohmann::json& j, const char* key)
{
    auto it = j.find (key);
    if (it != j.end() && it->is_string())
        return it->get<std::string>();
    return {};
}

std::optional<double> readNumber (const nlohmann::json& j, const char* key)
{
    auto it = j.find (key);
    if (it != j.end() && it->is_number())
        return it->get<double>();
    return std::nullopt;
}

void fillMetadata (ModelInfo& info, const nlohmann::json& meta)
{
    if (! meta.is_object())
        return;

    info.name      = readString (meta, "name");
    info.modeledBy = readString (meta, "modeled_by");
    info.gearMake  = readString (meta, "gear_make");
    info.gearModel = readString (meta, "gear_model");
    info.gearType  = readString (meta, "gear_type");
    info.toneType  = readString (meta, "tone_type");

    info.loudnessDb     = readNumber (meta, "loudness");
    info.inputLevelDbu  = readNumber (meta, "input_level_dbu");
    info.outputLevelDbu = readNumber (meta, "output_level_dbu");

    if (auto d = meta.find ("date"); d != meta.end() && d->is_object())
    {
        auto y = readNumber (*d, "year"), m = readNumber (*d, "month"), day = readNumber (*d, "day");
        if (y && m && day)
        {
            char buf[16];
            std::snprintf (buf, sizeof (buf), "%04d-%02d-%02d", (int) *y, (int) *m, (int) *day);
            info.date = buf;
        }
    }

    if (auto t = meta.find ("training"); t != meta.end() && t->is_object())
        info.validationEsr = readNumber (*t, "validation_esr");
}
} // namespace

NamModelInstance::~NamModelInstance() = default;

std::unique_ptr<NamModelInstance> NamModelInstance::load (const std::filesystem::path& file,
                                                          double hostSampleRate,
                                                          int maxBlockSize,
                                                          std::string& error)
{
    error.clear();

    std::error_code ec;
    if (! std::filesystem::is_regular_file (file, ec))
    {
        error = "File not found: " + pathToUtf8 (file);
        return nullptr;
    }

    nlohmann::json j;
    try
    {
        std::ifstream stream (file, std::ios::binary);
        if (! stream)
        {
            error = "Could not open the file for reading.";
            return nullptr;
        }
        j = nlohmann::json::parse (stream);
    }
    catch (const std::exception&)
    {
        error = "This is not a valid .nam model file (it could not be read as NAM JSON).";
        return nullptr;
    }

    if (! j.is_object() || ! j.contains ("architecture") || ! j.contains ("weights"))
    {
        error = "This file is missing the model architecture or weights, so it is not a NAM model.";
        return nullptr;
    }

    std::unique_ptr<NamModelInstance> inst (new NamModelInstance());
    nam::dspData data;

    try
    {
        nam::DspLoadOptions options;
        options.prewarm = false; // we prewarm in prepare(), once the real sample rate is known
        inst->model = nam::get_dsp (j, data, options);
    }
    catch (const std::exception& e)
    {
        error = std::string ("The NAM engine could not load this model: ") + e.what();
        return nullptr;
    }

    if (inst->model == nullptr)
    {
        error = "The NAM engine did not recognise this model architecture.";
        return nullptr;
    }

    if (inst->model->NumInputChannels() != 1 || inst->model->NumOutputChannels() != 1)
    {
        error = "Only mono-in / mono-out amp models are supported in the AMP slot (this model has "
                + std::to_string (inst->model->NumInputChannels()) + " in / "
                + std::to_string (inst->model->NumOutputChannels()) + " out).";
        return nullptr;
    }

    auto& info = inst->modelInfo;
    info.filePath       = pathToUtf8 (file);
    info.fileName       = pathToUtf8 (file.filename());
    info.architecture   = data.architecture;
    info.namFileVersion = data.version;
    info.numWeights     = data.weights.size();

    if (data.expected_sample_rate > 0.0)
    {
        info.sampleRate = data.expected_sample_rate;
        info.sampleRateFromFile = true;
    }
    else
    {
        info.sampleRate = kNamDefaultSampleRate;
        info.sampleRateFromFile = false;
    }

    fillMetadata (info, data.metadata);

    // The DSP object is the authority on calibration values actually applied.
    if (inst->model->HasLoudness())    info.loudnessDb     = inst->model->GetLoudness();
    if (inst->model->HasInputLevel())  info.inputLevelDbu  = inst->model->GetInputLevel();
    if (inst->model->HasOutputLevel()) info.outputLevelDbu = inst->model->GetOutputLevel();

    try
    {
        inst->prepare (hostSampleRate, maxBlockSize);
    }
    catch (const std::exception& e)
    {
        error = std::string ("The model loaded but could not be prepared for playback: ") + e.what();
        return nullptr;
    }

    return inst;
}

void NamModelInstance::prepare (double hostSampleRate, int maxBlockSize)
{
    hostRate = hostSampleRate > 0.0 ? hostSampleRate : modelInfo.sampleRate;
    maxBlock = std::max (1, maxBlockSize);

    inScratch.assign ((size_t) maxBlock, 0.0f);
    outScratch.assign ((size_t) maxBlock, 0.0f);

    const double modelRate = modelInfo.sampleRate;
    const bool rateMatches = std::abs (hostRate - modelRate) < 0.5;

    if (rateMatches || model->SupportsArbitrarySampleRate())
    {
        resampler.reset();
        maxModelBlock = maxBlock;
        model->Reset (rateMatches ? modelRate : hostRate, maxModelBlock);
    }
    else
    {
        // Same approach as the official NAM plugin: run the network at its
        // training rate inside a Lanczos (A = 12) resampling container.
        resampler = std::make_unique<Resampler> (modelRate);
        resampler->Reset (hostRate, maxBlock);
        maxModelBlock = (int) std::ceil ((double) maxBlock * modelRate / hostRate) + 2;
        model->Reset (modelRate, maxModelBlock);
    }
}

void NamModelInstance::reset()
{
    prepare (hostRate, maxBlock);
}

int NamModelInstance::latencySamples() const noexcept
{
    return resampler != nullptr ? resampler->GetLatency() : 0;
}

void NamModelInstance::processAtModelRate (float** in, float** out, int n) noexcept
{
    int done = 0;
    while (done < n)
    {
        const int chunk = std::min (n - done, maxModelBlock);
        float* i = in[0] + done;
        float* o = out[0] + done;
        model->process (&i, &o, chunk);
        done += chunk;
    }
}

void NamModelInstance::process (const float* input, float* output, int numSamples) noexcept
{
    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, maxBlock);
        std::memcpy (inScratch.data(), input + done, sizeof (float) * (size_t) n);

        float* in = inScratch.data();
        float* out = outScratch.data();

        if (resampler != nullptr)
            resampler->ProcessBlock (&in, &out, n, [this] (float** i, float** o, int m) { processAtModelRate (i, o, m); });
        else
            processAtModelRate (&in, &out, n);

        std::memcpy (output + done, outScratch.data(), sizeof (float) * (size_t) n);
        done += n;
    }
}

} // namespace gas
