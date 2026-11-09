// Capture pipeline tests, using a simulated reamp chain:
//   plugin output -> "amplifier" (a real NAM model) -> latency + noise -> plugin input
//
// The stimulus has the exact layout of NAM's v3 capture signal (validation passes,
// blips, training section) but synthetic content, since NAM's file is not bundled.

#include "capture/CaptureAnalysis.h"
#include "capture/CaptureSignal.h"
#include "capture/CaptureTransport.h"
#include "capture/Md5.h"
#include "train/Trainer.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"
#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <random>

using namespace gas::capture;
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

static void fillGuitarLike (std::vector<float>& x, size_t from, size_t to, std::mt19937& rng)
{
    std::uniform_real_distribution<float> u (0.0f, 1.0f);
    size_t pos = from;
    while (pos < to)
    {
        const size_t len = 4800 + (size_t) (u (rng) * 24000);
        const float f0 = 82.0f * std::pow (2.0f, u (rng) * 3.0f);
        const float amp = 0.05f + 0.6f * u (rng);
        const float decay = 2.0f + 6.0f * u (rng);
        for (size_t i = 0; i < len && pos + i < to; ++i)
        {
            const float t = (float) i / 48000.0f;
            float s = 0.0f;
            for (int h = 1; h <= 5; ++h)
                s += std::sin (6.2831853f * f0 * h * t) / (float) h;
            x[pos + i] = amp * std::exp (-t * decay) * s * 0.6f;
        }
        pos += len;
    }
}

/** A stimulus with NAM v3's layout (190 s at 48 kHz). */
static std::vector<float> syntheticV3()
{
    using L = V3Layout;
    const size_t n = 190 * 48000;
    std::vector<float> x (n, 0.0f);
    std::mt19937 rng (1);
    fillGuitarLike (x, 0, L::validationLength, rng);                         // validation 1
    x[L::blip1] = 0.5f;                                                      // blips
    x[L::blip2] = 0.5f;
    for (size_t i = 0; i < 144000; ++i)                                      // chirp 12-15 s
    {
        const double t = (double) i / 48000.0;
        x[576000 + i] = 0.3f * (float) std::sin (6.283185307 * (20.0 * t + 1000.0 * t * t));
    }
    std::normal_distribution<float> nd (0.0f, 0.1f);                         // noise 15-17 s
    for (size_t i = 720000; i < 816000; ++i)
        x[i] = nd (rng);
    fillGuitarLike (x, 816000, n - L::validationLength - 24000, rng);        // training
    std::copy (x.begin(), x.begin() + L::validationLength, x.end() - L::validationLength); // validation 2
    return x;
}

struct SimulatedRig
{
    std::unique_ptr<nam::DSP> amp;
    int latency = 157;
    float noiseRms = 1e-5f;
    float gainChangeAfter = -1.0f; // seconds; simulates the amp drifting mid-capture
    float gainChange = 1.0f;

    std::vector<float> run (const std::vector<float>& sent)
    {
        amp->Reset (48000.0, 512);
        std::vector<float> y (sent.size() + (size_t) latency, 0.0f), buf (512), out (512);
        for (size_t pos = 0; pos < sent.size(); pos += 512)
        {
            const int n = (int) std::min<size_t> (512, sent.size() - pos);
            std::copy (sent.begin() + (long) pos, sent.begin() + (long) pos + n, buf.begin());
            float* i = buf.data();
            float* o = out.data();
            amp->process (&i, &o, n);
            for (int k = 0; k < n; ++k)
            {
                float g = 1.0f;
                if (gainChangeAfter >= 0.0f && (float) (pos + (size_t) k) / 48000.0f > gainChangeAfter)
                    g = gainChange;
                y[pos + (size_t) k + (size_t) latency] = g * out[(size_t) k];
            }
        }
        std::mt19937 rng (9);
        std::normal_distribution<float> nd (0.0f, noiseRms);
        for (auto& v : y)
            v += nd (rng);
        return y;
    }
};

/** Runs the transport like a host would: blocks of `block` samples, return fed back in. */
static std::vector<float> captureThroughTransport (CaptureTransport& t, const std::vector<float>& signal,
                                                   SimulatedRig& rig, int block, std::vector<float>* sentOut)
{
    // The rig's response only depends on what was sent, so compute it for the whole
    // playback, then feed it back block by block exactly as the host would deliver it.
    const auto returned = rig.run (signal);
    t.start();
    std::vector<float> in ((size_t) block), out ((size_t) block);
    size_t pos = 0;
    while (t.state() == CaptureTransport::State::Running)
    {
        for (int i = 0; i < block; ++i)
            in[(size_t) i] = pos + (size_t) i < returned.size() ? returned[pos + (size_t) i] : 0.0f;
        t.process (in.data(), out.data(), block);
        if (sentOut)
            for (int i = 0; i < block; ++i)
                sentOut->push_back (out[(size_t) i]);
        pos += (size_t) block;
    }
    return returned;
}

// ---------------------------------------------------------------------------
static void testMd5()
{
    std::printf ("[md5] RFC 1321 test vectors\n");
    CHECK (Md5::ofString ("") == "d41d8cd98f00b204e9800998ecf8427e", "empty");
    CHECK (Md5::ofString ("abc") == "900150983cd24fb0d6963f7d28e17f72", "abc");
    CHECK (Md5::ofString ("The quick brown fox jumps over the lazy dog") == "9e107d9d372bb6826bd81d3542a419d6", "fox");
    std::string million (1000000, 'a');
    CHECK (Md5::ofString (million) == "7707d6ae4e027c70eea2a935c2296f21", "million a");
}

static void testCaptureEndToEnd()
{
    std::printf ("[capture] play, record, calibrate, check and prepare a v3-layout capture\n");
    const auto signal = syntheticV3();

    SimulatedRig rig;
    rig.amp = nam::get_dsp (fs::path (NAM_EXAMPLE_MODELS_DIR) / "lstm.nam");

    CaptureTransport t;
    CHECK (t.arm (signal, 0, signal.size(), 24000), "arm failed");
    std::vector<float> sent;
    const auto returned = captureThroughTransport (t, signal, rig, 480, &sent);
    CHECK (t.state() == CaptureTransport::State::Finished, "transport did not finish");

    // Sample-exact playback and recording
    bool playbackExact = true;
    for (size_t i = 0; i < signal.size(); ++i)
        playbackExact = playbackExact && sent[i] == signal[i];
    CHECK (playbackExact, "played signal differs from the capture signal");
    const auto& rec = t.recording();
    CHECK (rec.size() == signal.size() + 24000, "recording length");
    bool recExact = true;
    for (size_t i = 0; i < rec.size(); ++i)
        recExact = recExact && rec[i] == (i < returned.size() ? returned[i] : 0.0f);
    CHECK (recExact, "recording is not sample-aligned with what came back");

    const auto lat = calibrateLatencyV3 (rec);
    std::printf ("  %s (simulated %d)\n", lat.message.c_str(), rig.latency);
    CHECK (lat.detected && *lat.detected == rig.latency, "latency detection wrong");
    CHECK (lat.recommended && *lat.recommended == rig.latency - 1, "NAM safety factor not applied");

    const auto levels = analyseLevels (rec);
    std::printf ("  peak %.1f dBFS, noise floor %.1f dBFS, %d clipped, %zu warnings\n", levels.peakDbfs,
                 levels.noiseFloorDbfs, levels.clippedSamples, levels.warnings.size());
    CHECK (levels.clippedSamples == 0 && ! levels.silent, "level report");

    const auto rep = checkValidationReplicate (rec, signal.size());
    std::printf ("  validation replicate ESR %.2e -> %s\n", rep.esr, rep.passed ? "passed" : "FAILED");
    CHECK (rep.passed, "replicate check should pass for a stable rig");

    // Alignment as NAM's trainer does it: y[t + latency] pairs with x[t]
    const auto data = prepareV3 (signal, rec, *lat.recommended);
    const size_t expectTrain = (signal.size() - V3Layout::validationLength - V3Layout::trainStart) - (size_t) *lat.recommended;
    CHECK (data.trainInput.size() == expectTrain && data.trainOutput.size() == expectTrain, "train split size");
    CHECK (data.validationInput.size() == V3Layout::validationLength - (size_t) *lat.recommended, "validation size");
    // With NAM's 1-sample safety factor the aligned output is the amp's response one sample late.
    double err = 0.0;
    const auto ref = rig.run (signal); // rig output (deterministic)
    for (size_t i = 1000; i < 2000; ++i)
        err = std::max (err, (double) std::abs (data.trainOutput[i] - ref[V3Layout::trainStart + i + (size_t) *lat.recommended]));
    CHECK (err == 0.0, "alignment mismatch %g", err);

    // A short training run on the captured data must improve and export a valid NAM model.
    gas::train::TrainOptions opt;
    opt.preset = gas::train::Preset::Nano;
    opt.epochs = 3;
    std::atomic<bool> cancel { false };
    std::vector<double> esrs;
    auto result = gas::train::trainWaveNet (data.trainInput, data.trainOutput, data.validationInput,
                                            data.validationOutput, opt,
                                            [&] (const gas::train::TrainProgress& p)
                                            {
                                                if (p.epochValidationEsr.size() > esrs.size())
                                                    esrs.push_back (p.epochValidationEsr.back());
                                            },
                                            cancel);
    CHECK (result.success, "training on captured data failed: %s", result.error.c_str());
    if (result.success)
    {
        std::printf ("  Nano model trained on the capture: validation ESR by epoch");
        for (double e : esrs)
            std::printf (" %.3f", e);
        std::printf ("\n");
        CHECK (esrs.back() < esrs.front(), "no improvement");
        gas::train::WaveNet net (result.spec);
        net.params() = result.bestParams;
        net.unpack();
        auto dsp = nam::get_dsp (nlohmann::json::parse (net.exportNam ("{}", 48000.0)));
        CHECK (dsp != nullptr, "export did not load in NAM core");
    }
}

static void testChecksCatchProblems()
{
    std::printf ("[checks] problems a capture must catch\n");
    const auto signal = syntheticV3();

    {   // gear changed during the capture
        SimulatedRig rig;
        rig.amp = nam::get_dsp (fs::path (NAM_EXAMPLE_MODELS_DIR) / "lstm.nam");
        rig.gainChangeAfter = 100.0f;
        rig.gainChange = 1.25f;
        const auto rec = rig.run (signal);
        const auto rep = checkValidationReplicate (rec, signal.size());
        std::printf ("  amp level drifted 25%% mid-capture: replicate ESR %.4f -> %s\n", rep.esr,
                     rep.passed ? "passed" : "flagged");
        CHECK (! rep.passed, "drift not detected");
    }
    {   // nothing connected
        std::vector<float> silence (signal.size(), 0.0f);
        const auto lat = calibrateLatencyV3 (silence);
        CHECK (lat.notDetected, "silence must not yield a latency");
        const auto lv = analyseLevels (silence);
        CHECK (lv.silent && ! lv.warnings.empty(), "silence must be reported");
    }
    {   // clipping
        SimulatedRig rig;
        rig.amp = nam::get_dsp (fs::path (NAM_EXAMPLE_MODELS_DIR) / "lstm.nam");
        auto rec = rig.run (signal);
        for (auto& v : rec)
            v = std::max (-1.0f, std::min (1.0f, v * 40.0f));
        const auto lv = analyseLevels (rec);
        std::printf ("  overdriven return: %d clipped samples, %zu warnings\n", lv.clippedSamples, lv.warnings.size());
        CHECK (lv.clippedSamples > 0, "clipping not detected");
    }
    {   // cancellation
        CaptureTransport t;
        t.arm (signal, 0, 48000, 1000);
        t.start();
        std::vector<float> in (256, 0.0f), out (256);
        t.process (in.data(), out.data(), 256);
        t.cancel();
        CHECK (! t.process (in.data(), out.data(), 256), "cancelled transport must stop");
        CHECK (t.state() == CaptureTransport::State::Cancelled, "state after cancel");
    }
}

int main()
{
    testMd5();
    testCaptureEndToEnd();
    testChecksCatchProblems();
    std::printf (gFailures == 0 ? "\nALL CAPTURE TESTS PASSED\n" : "\n%d CHECK(S) FAILED\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
