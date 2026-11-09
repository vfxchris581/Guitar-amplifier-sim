// Tests for the C++ NAM trainer.
//
//   gradients   analytic backprop vs. finite differences
//   export      exported .nam loads in NeuralAmpModelerCore and plays identically (all presets)
//   training    learns a real NAM model's behaviour; exported result validated in NAM core
//   loudness    loudness metadata vs. a reference value (pass --loudness-reference <dB>)

#include "train/Trainer.h"
#include "train/WaveNet.h"
#include "WavIO.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "json.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using namespace gas::train;
namespace fs = std::filesystem;

static int gFailures = 0;
#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        if (! (cond)) {                                                                          \
            ++gFailures;                                                                         \
            std::printf ("  FAIL line %d: %s -- ", __LINE__, #cond);                             \
            std::printf (__VA_ARGS__);                                                           \
            std::printf ("\n");                                                                  \
        }                                                                                        \
    } while (0)

/** Guitar-like test material: plucked decaying harmonics at varied pitch/level plus noise bursts. */
static std::vector<float> testSignal (size_t n, std::uint32_t seed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    std::vector<float> x (n, 0.0f);
    size_t pos = 0;
    while (pos < n)
    {
        const size_t len = 4800 + (size_t) (u (rng) * 24000);
        const float f0 = 82.0f * std::pow (2.0f, u (rng) * 3.0f);
        const float amp = 0.05f + 0.6f * u (rng);
        const bool noise = u (rng) < 0.2f;
        const float decay = 2.0f + 6.0f * u (rng);
        for (size_t i = 0; i < len && pos + i < n; ++i)
        {
            const float t = (float) i / 48000.0f;
            const float env = std::exp (-t * decay);
            float s = 0.0f;
            if (noise)
                s = (u (rng) * 2.0f - 1.0f) * 0.5f;
            else
                for (int h = 1; h <= 6; ++h)
                    s += std::sin (6.2831853f * f0 * h * t) / (float) h;
            x[pos + i] += amp * env * s * 0.6f;
        }
        pos += len;
    }
    return x;
}

static std::vector<float> runNamCore (nam::DSP& dsp, const std::vector<float>& x)
{
    std::vector<float> y (x.size());
    const int block = 512;
    dsp.Reset (48000.0, block);
    std::vector<float> in (block), out (block);
    for (size_t pos = 0; pos < x.size(); pos += block)
    {
        const int n = (int) std::min<size_t> (block, x.size() - pos);
        std::copy (x.begin() + (long) pos, x.begin() + (long) pos + n, in.begin());
        float* i = in.data();
        float* o = out.data();
        dsp.process (&i, &o, n);
        std::copy (out.begin(), out.begin() + n, y.begin() + (long) pos);
    }
    return y;
}

/** Our model on a causal stream (zero history), like a plugin would run it. */
static std::vector<float> runCausal (const WaveNet& net, const std::vector<float>& x)
{
    std::vector<float> padded ((size_t) net.spec().receptiveField() - 1, 0.0f);
    padded.insert (padded.end(), x.begin(), x.end());
    return runModel (net, padded);
}

// ---------------------------------------------------------------------------
static void testGradients()
{
    std::printf ("[gradients] backprop vs finite differences\n");
    WaveNetSpec spec;
    spec.arrays.push_back ({ 1, 1, 3, 2, 3, { 1, 2, 4 }, false });
    spec.arrays.push_back ({ 3, 1, 2, 1, 2, { 1, 3 }, true });
    spec.headScale = 0.5f;

    WaveNet net (spec);
    net.initialise (7);
    const int R = spec.receptiveField();
    const int L = R + 23;
    const auto x = testSignal ((size_t) L, 3);
    std::mt19937 rng (11);
    std::normal_distribution<float> nd;
    std::vector<float> r ((size_t) (L - R + 1));
    for (auto& v : r)
        v = nd (rng);

    // loss = sum(y * r)  =>  dL/dy = r
    auto loss = [&] (WaveNet& n)
    {
        Workspace ws;
        std::vector<float> y (r.size());
        n.forward (x.data(), L, y.data(), ws);
        double s = 0;
        for (size_t i = 0; i < y.size(); ++i)
            s += (double) y[i] * r[i];
        return s;
    };

    Workspace ws;
    std::vector<float> y (r.size()), grad (net.params().size(), 0.0f);
    net.forward (x.data(), L, y.data(), ws);
    net.backward (r.data(), ws, grad);

    double worst = 0;
    const float h = 1e-3f;
    for (size_t i = 0; i < net.params().size(); ++i)
    {
        const float orig = net.params()[i];
        net.params()[i] = orig + h; net.unpack();
        const double lp = loss (net);
        net.params()[i] = orig - h; net.unpack();
        const double lm = loss (net);
        net.params()[i] = orig; net.unpack();
        const double numeric = (lp - lm) / (2.0 * h);
        const double err = std::abs (numeric - grad[i]) / std::max (1e-2, std::abs (numeric) + std::abs (grad[i]));
        worst = std::max (worst, err);
    }
    std::printf ("  %zu parameters, worst relative gradient error %.2e\n", net.params().size(), worst);
    CHECK (worst < 2e-2, "gradient mismatch %.3g", worst);
}

static void testExportMatchesNamCore()
{
    std::printf ("[export] trained-format .nam plays identically in NeuralAmpModelerCore\n");
    const auto x = testSignal (48000, 5);
    for (auto p : { Preset::Standard, Preset::Lite, Preset::Feather, Preset::Nano })
    {
        WaveNet net (WaveNetSpec::fromPreset (p));
        net.initialise (42);
        const auto mine = runCausal (net, x);

        const auto text = net.exportNam (R"({"name":"export test"})", 48000.0);
        auto dsp = nam::get_dsp (nlohmann::json::parse (text));
        const auto theirs = runNamCore (*dsp, x);

        double maxErr = 0, peak = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            maxErr = std::max (maxErr, (double) std::abs (mine[i] - theirs[i]));
            peak = std::max (peak, (double) std::abs (theirs[i]));
        }
        std::printf ("  %-8s %6zu params, receptive field %4d: max |ours - NAM core| = %.2e (peak %.3f)\n",
                     presetName (p), net.params().size(), net.spec().receptiveField(), maxErr, peak);
        CHECK (peak > 1e-4, "silent output");
        CHECK (maxErr < 1e-5 + 1e-4 * peak, "%s export mismatch", presetName (p));
    }
}

static void testTraining()
{
    std::printf ("[training] learn a real NAM capture's behaviour from audio\n");
    // The "amplifier" is NAM's example WaveNet (A1 standard) - a genuine nonlinear model.
    auto amp = nam::get_dsp (fs::path (NAM_EXAMPLE_MODELS_DIR) / "wavenet_a1_standard.nam");
    const auto xTrain = testSignal (48000 * 12, 101);
    const auto xVal = testSignal (48000 * 3, 202);
    const auto yTrain = runNamCore (*amp, xTrain);
    const auto yVal = runNamCore (*amp, xVal);

    TrainOptions opt;
    opt.preset = Preset::Feather;
    opt.epochs = 40;
    opt.batchSize = 16;
    opt.outputSamplesPerItem = 2048; // shorter windows -> enough optimizer steps on 12 s of audio

    std::atomic<bool> cancel { false };
    double firstEsr = -1;
    const auto t0 = std::chrono::steady_clock::now();
    auto result = trainWaveNet (xTrain, yTrain, xVal, yVal, opt,
                                [&] (const TrainProgress& p)
                                {
                                    if (p.step == p.stepsPerEpoch && p.validationEsr
                                        && (int) p.epochValidationEsr.size() == p.epoch)
                                    {
                                        if (firstEsr < 0)
                                            firstEsr = *p.validationEsr;
                                        if (p.epoch % 5 == 0 || p.epoch == 1)
                                            std::printf ("  epoch %2d  train MSE %.3e  val ESR %.4f\n", p.epoch,
                                                         p.trainLoss, *p.validationEsr);
                                    }
                                },
                                cancel);
    const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("  %.1f s for %d epochs (%s)\n", secs, result.epochsRun, presetName (opt.preset));
    CHECK (result.success, "training failed: %s", result.error.c_str());
    if (! result.success)
        return;
    CHECK (result.bestValidationEsr < 0.25 * firstEsr, "validation ESR did not improve enough (%.4f -> %.4f)",
           firstEsr, result.bestValidationEsr);
    CHECK (result.bestValidationEsr < 0.3, "final ESR too high: %.4f", result.bestValidationEsr);

    // Export and check NAM core reproduces the trainer's validation ESR.
    WaveNet net (result.spec);
    net.params() = result.bestParams;
    net.unpack();
    auto dsp = nam::get_dsp (nlohmann::json::parse (net.exportNam ("{}", 48000.0)));
    const auto yModel = runNamCore (*dsp, xVal);
    const int R = net.spec().receptiveField();
    const double esrCore = esr (yModel.data() + R - 1, yVal.data() + R - 1, yVal.size() - (size_t) R + 1);
    std::printf ("  exported model in NAM core: validation ESR %.4f (trainer reported %.4f)\n", esrCore,
                 result.bestValidationEsr);
    CHECK (std::abs (esrCore - result.bestValidationEsr) < 1e-3 + 1e-2 * result.bestValidationEsr,
           "NAM core disagrees with trainer ESR");

    // Cancellation is honoured promptly.
    std::atomic<bool> stop { false };
    int steps = 0;
    auto cancelled = trainWaveNet (xTrain, yTrain, xVal, yVal, opt,
                                   [&] (const TrainProgress&)
                                   {
                                       if (++steps == 3)
                                           stop = true;
                                   },
                                   stop);
    CHECK (cancelled.cancelled && ! cancelled.success, "cancel not honoured");
}

static void testLoudness (const char* outDir, double reference)
{
    std::printf ("[loudness] NAM loudness metadata\n");
    std::vector<float> stim;
    double rate = 0;
    if (! gas::test::readWavMono (NAM_LOUDNESS_STIMULUS, stim, rate))
    {
        CHECK (false, "could not read %s", NAM_LOUDNESS_STIMULUS);
        return;
    }
    CHECK (rate == 48000.0 && stim.size() == 84000, "unexpected stimulus format");
    WaveNet net (WaveNetSpec::fromPreset (Preset::Standard));
    net.initialise (42);
    const double db = measureLoudnessDb (net, stim);
    std::printf ("  loudness of seeded Standard model: %.6f dB\n", db);
    CHECK (std::isfinite (db), "loudness not finite");

    if (outDir != nullptr)
    {
        // For the Python cross-check (tests/loudness_reference.py)
        const auto y = runCausal (net, stim);
        gas::test::writeWavMono ((fs::path (outDir) / "loudness_model_output.wav").string(), y, 48000.0);
    }
    if (! std::isnan (reference))
    {
        std::printf ("  Python reference: %.6f dB\n", reference);
        CHECK (std::abs (db - reference) < 1e-3, "loudness differs from NAM's Python implementation");
    }
}

int main (int argc, char** argv)
{
    const char* outDir = nullptr;
    double reference = std::nan ("");
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--write-loudness-output") == 0 && i + 1 < argc)
            outDir = argv[++i];
        else if (std::strcmp (argv[i], "--loudness-reference") == 0 && i + 1 < argc)
            reference = std::atof (argv[++i]);
    }

    testGradients();
    testExportMatchesNamCore();
    testLoudness (outDir, reference);
    testTraining();

    std::printf (gFailures == 0 ? "\nALL TRAINER TESTS PASSED\n" : "\n%d CHECK(S) FAILED\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
