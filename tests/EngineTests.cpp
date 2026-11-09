// Headless tests for the real-time amp engine.
//
//   parity      engine output == NeuralAmpModelerCore output (same model, same input)
//   resampling  model trained at 48 kHz runs correctly at 44.1 / 96 kHz hosts
//   rt-safety   zero heap allocations on the audio thread, including during model switches
//   switching   no clicks: the crossfade never jumps more than the models themselves do
//   errors      bad/missing files keep the previous model playing and explain why
//   benchmark   CPU cost per model architecture

#include "engine/AmpEngine.h"
#include "engine/NamModelInstance.h"
#include "WavIO.h"

#include "NAM/dsp.h"
#include "NAM/get_dsp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <new>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Allocation tracking: counts every heap allocation made by the thread that is
// currently acting as the audio thread.
// ---------------------------------------------------------------------------
namespace
{
thread_local bool tTrackAllocations = false;
std::atomic<long> gTrackedAllocations { 0 };

// Break on this in a debugger to see where an audio-thread allocation came from.
#if defined(_MSC_VER)
__declspec (noinline) void audioThreadAllocated() { static volatile int sink = 0; sink = sink + 1; }
#else
__attribute__ ((noinline)) void audioThreadAllocated() { asm volatile (""); }
#endif

inline void noteAllocation()
{
    if (tTrackAllocations)
    {
        gTrackedAllocations.fetch_add (1, std::memory_order_relaxed);
        audioThreadAllocated();
    }
}
} // namespace

#if defined(__GLIBC__)
extern "C"
{
void* __libc_malloc (size_t);
void* __libc_calloc (size_t, size_t);
void* __libc_realloc (void*, size_t);
void* __libc_memalign (size_t, size_t);

void* malloc (size_t n) { noteAllocation(); return __libc_malloc (n); }
void* calloc (size_t a, size_t b) { noteAllocation(); return __libc_calloc (a, b); }
void* realloc (void* p, size_t n) { noteAllocation(); return __libc_realloc (p, n); }
void* aligned_alloc (size_t a, size_t n) { noteAllocation(); return __libc_memalign (a, n); }
int posix_memalign (void** out, size_t a, size_t n)
{
    noteAllocation();
    *out = __libc_memalign (a, n);
    return *out != nullptr ? 0 : 12; // ENOMEM
}
}
#define GAS_TRACKS_MALLOC 1
#else
void* operator new (size_t n) { noteAllocation(); if (void* p = std::malloc (n)) return p; throw std::bad_alloc(); }
void* operator new[] (size_t n) { noteAllocation(); if (void* p = std::malloc (n)) return p; throw std::bad_alloc(); }
void operator delete (void* p) noexcept { std::free (p); }
void operator delete[] (void* p) noexcept { std::free (p); }
void operator delete (void* p, size_t) noexcept { std::free (p); }
void operator delete[] (void* p, size_t) noexcept { std::free (p); }
#define GAS_TRACKS_MALLOC 0
#endif

struct TrackAllocations
{
    TrackAllocations()  { gTrackedAllocations = 0; tTrackAllocations = true; }
    ~TrackAllocations() { tTrackAllocations = false; }
    long count() const  { return gTrackedAllocations.load(); }
};

// ---------------------------------------------------------------------------
namespace fs = std::filesystem;
using gas::AmpEngine;

static int gFailures = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (! (cond)) {                                                     \
            ++gFailures;                                                    \
            std::printf ("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
            std::printf (__VA_ARGS__);                                      \
            std::printf ("\n");                                             \
        }                                                                   \
    } while (0)

static fs::path modelPath (const char* name) { return fs::path (NAM_EXAMPLE_MODELS_DIR) / name; }

/** NAM's example input: 1 s of silence followed by a -6 dBFS test signal. */
static std::vector<float> exampleInput (double& rate)
{
    std::vector<float> x;
    if (! gas::test::readWavMono (NAM_EXAMPLE_AUDIO, x, rate))
    {
        std::printf ("  could not read %s\n", NAM_EXAMPLE_AUDIO);
        std::exit (2);
    }
    return x;
}

static std::vector<float> sine (double freq, double rate, double seconds, float amp)
{
    std::vector<float> x ((size_t) (seconds * rate));
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = amp * (float) std::sin (6.283185307179586 * freq * (double) i / rate);
    return x;
}

static void runEngine (AmpEngine& e, std::vector<float>& x, int block)
{
    for (size_t pos = 0; pos < x.size(); pos += (size_t) block)
        e.process (x.data() + pos, (int) std::min<size_t> ((size_t) block, x.size() - pos));
}

/** Loads a model and runs silence until the audio side has switched to it. */
static bool loadAndSettle (AmpEngine& e, const fs::path& p, double rate, int block)
{
    e.loadModel (p);
    e.waitUntilWorkerIdle();
    std::vector<float> silence ((size_t) block, 0.0f);
    for (int i = 0; i < 20000 && ! e.isSettled(); ++i)
        e.process (silence.data(), block);
    // plus one second of silence so stateful models reach steady state
    std::vector<float> s ((size_t) rate, 0.0f);
    runEngine (e, s, block);
    return e.isSettled() && e.getStatus().state == AmpEngine::Status::State::Ready;
}

static double rms (const std::vector<float>& x, size_t from, size_t to)
{
    double s = 0;
    for (size_t i = from; i < to; ++i)
        s += (double) x[i] * x[i];
    return std::sqrt (s / (double) std::max<size_t> (1, to - from));
}

static bool allFinite (const std::vector<float>& x)
{
    for (float v : x)
        if (! std::isfinite (v))
            return false;
    return true;
}

// ---------------------------------------------------------------------------
static void testParity (const char* modelName)
{
    std::printf ("[parity] %s\n", modelName);
    const int block = 128;
    double rate = 48000;
    auto di = exampleInput (rate);

    AmpEngine engine;
    engine.outputMode = (int) gas::OutputMode::Raw;
    engine.prepare (rate, block);
    CHECK (loadAndSettle (engine, modelPath (modelName), rate, block), "model did not load/settle: %s",
           engine.getStatus().message.c_str());

    auto viaEngine = di; // NAM example input
    runEngine (engine, viaEngine, block);

    // Reference: NeuralAmpModelerCore used directly, fed the same silence history.
    auto ref = nam::get_dsp (modelPath (modelName));
    ref->Reset (rate, block);
    std::vector<float> history ((size_t) rate, 0.0f);
    std::vector<float> scratch ((size_t) block);
    auto runRef = [&] (std::vector<float>& x)
    {
        for (size_t pos = 0; pos < x.size(); pos += block)
        {
            int n = (int) std::min<size_t> (block, x.size() - pos);
            float* in = x.data() + pos;
            float* out = scratch.data();
            ref->process (&in, &out, n);
            std::copy (scratch.begin(), scratch.begin() + n, x.begin() + (long) pos);
        }
    };
    runRef (history);
    auto viaRef = di;
    runRef (viaRef);

    double maxErr = 0, peak = 0;
    for (size_t i = 0; i < di.size(); ++i)
    {
        maxErr = std::max (maxErr, (double) std::abs (viaEngine[i] - viaRef[i]));
        peak = std::max (peak, (double) std::abs (viaRef[i]));
    }
    std::printf ("  max |engine - NAM core| = %.3g (output peak %.3f)\n", maxErr, peak);
    CHECK (peak > 1e-3, "reference output is silent");
    CHECK (maxErr <= 1e-5 * std::max (1.0, peak), "engine output differs from NAM core");
}

static void testResampling (double hostRate)
{
    std::printf ("[resampling] 48 kHz model on a %.1f kHz host\n", hostRate / 1000.0);
    const int block = 256;
    AmpEngine engine;
    engine.outputMode = (int) gas::OutputMode::Raw;
    engine.prepare (hostRate, block);
    CHECK (loadAndSettle (engine, modelPath ("wavenet_a1_standard.nam"), hostRate, block), "load failed");
    CHECK (engine.getStatus().resampling, "status should report resampling");
    CHECK (engine.getLatencySamples() > 0, "resampling should report latency");
    std::printf ("  resampler latency: %d samples (%.2f ms)\n", engine.getLatencySamples(),
                 1000.0 * engine.getLatencySamples() / hostRate);

    auto x = sine (220.0, hostRate, 2.0, 0.2f);
    runEngine (engine, x, block);

    auto ref = nam::get_dsp (modelPath ("wavenet_a1_standard.nam"));
    ref->Reset (48000.0, 512);
    auto y = sine (220.0, 48000.0, 2.0, 0.2f);
    std::vector<float> out (512);
    for (size_t pos = 0; pos < y.size(); pos += 512)
    {
        int n = (int) std::min<size_t> (512, y.size() - pos);
        float* in = y.data() + pos;
        float* o = out.data();
        ref->process (&in, &o, n);
        std::copy (out.begin(), out.begin() + n, y.begin() + (long) pos);
    }

    const double a = rms (x, x.size() / 2, x.size()), b = rms (y, y.size() / 2, y.size());
    const double diffDb = 20.0 * std::log10 (a / b);
    std::printf ("  RMS at host rate vs native 48 kHz: %+.3f dB\n", diffDb);
    CHECK (allFinite (x), "non-finite output");
    CHECK (std::abs (diffDb) < 0.25, "level mismatch after resampling: %.3f dB", diffDb);
}

static void testRealtimeSafety()
{
    std::printf ("[rt-safety] heap allocations on the audio thread%s\n",
                 GAS_TRACKS_MALLOC ? " (malloc + operator new)" : " (operator new only)");
    for (double rate : { 48000.0, 44100.0 })
    {
        const int block = 64;
        AmpEngine engine;
        engine.prepare (rate, block);
        CHECK (loadAndSettle (engine, modelPath ("lstm.nam"), rate, block), "load failed");

        auto x = sine (110.0, rate, 0.5, 0.3f);
        long steady = 0, switching = 0;
        {
            TrackAllocations t;
            runEngine (engine, x, block);
            steady = t.count();
        }

        engine.loadModel (modelPath ("wavenet_a1_standard.nam"));
        engine.waitUntilWorkerIdle();
        auto y = x;
        {
            TrackAllocations t;
            for (int i = 0; i < 20000 && ! engine.isSettled(); ++i)
                engine.process (x.data(), block);
            runEngine (engine, y, block);
            switching = t.count();
        }
        std::printf ("  %.1f kHz: steady-state allocations = %ld, during model switch = %ld\n", rate / 1000.0, steady,
                     switching);
        CHECK (steady == 0, "audio thread allocated during steady-state playback");
        CHECK (switching == 0, "audio thread allocated during a model switch");
        CHECK (engine.isSettled(), "switch did not complete");
    }
}

static void testSwitchingIsClickFree()
{
    std::printf ("[switching] crossfade between two models under a continuous signal\n");
    const double rate = 48000;
    const int block = 128;
    AmpEngine engine;
    engine.outputMode = (int) gas::OutputMode::Normalized;
    engine.prepare (rate, block);
    CHECK (loadAndSettle (engine, modelPath ("lstm.nam"), rate, block), "load A failed");

    auto x = sine (82.4, rate, 3.0, 0.25f); // low E
    std::vector<float> y = x;

    const size_t switchAt = (size_t) rate; // 1 s in
    bool requested = false;
    size_t swappedAt = 0;
    for (size_t pos = 0; pos < y.size(); pos += block)
    {
        if (! requested && pos >= switchAt)
        {
            engine.loadModel (modelPath ("wavenet_a1_standard.nam"));
            engine.waitUntilWorkerIdle();
            requested = true;
        }
        engine.process (y.data() + pos, block);
        if (requested && swappedAt == 0 && engine.isSettled())
            swappedAt = pos;
    }

    auto maxStep = [&] (size_t from, size_t to)
    {
        double m = 0;
        for (size_t i = from + 1; i < to; ++i)
            m = std::max (m, (double) std::abs (y[i] - y[i - 1]));
        return m;
    };
    const size_t fade = (size_t) (0.03 * rate);
    const double before = maxStep (switchAt - (size_t) (0.5 * rate), switchAt);
    const double during = maxStep (switchAt, switchAt + fade + 2 * block);
    const double after = maxStep (y.size() - (size_t) (0.5 * rate), y.size());
    std::printf ("  largest sample step: model A %.4f, during switch %.4f, model B %.4f\n", before, during, after);
    CHECK (swappedAt > 0, "never switched");
    CHECK (during <= 1.1 * std::max (before, after) + 1e-4, "switch produced a discontinuity");
    CHECK (allFinite (y), "non-finite output");
}

static void testErrors()
{
    std::printf ("[errors] invalid files keep the current model playing\n");
    const double rate = 48000;
    const int block = 128;
    AmpEngine engine;
    engine.prepare (rate, block);
    CHECK (loadAndSettle (engine, modelPath ("lstm.nam"), rate, block), "load failed");

    const auto bad = fs::temp_directory_path() / "gas_not_a_model.nam";
    {
        std::ofstream f (bad);
        f << "this is not json";
    }
    for (const auto& p : { bad, fs::path ("/definitely/missing/model.nam") })
    {
        engine.loadModel (p);
        engine.waitUntilWorkerIdle();
        auto s = engine.getStatus();
        std::printf ("  %s -> \"%s\"\n", p.filename().string().c_str(), s.message.c_str());
        CHECK (s.state == AmpEngine::Status::State::Error, "expected an error state");
        CHECK (s.info.has_value() && s.info->fileName == "lstm.nam", "previous model info should be kept");
        CHECK (engine.isSettled(), "a failed load must not change what is playing");
    }
    fs::remove (bad);

    engine.unloadModel();
    engine.waitUntilWorkerIdle();
    CHECK (engine.getStatus().state == AmpEngine::Status::State::Empty, "unload should leave the slot empty");
}

static void testSampleRateChangeWhileLoaded()
{
    std::printf ("[prepare] host sample-rate change with a model loaded\n");
    AmpEngine engine;
    engine.prepare (48000, 128);
    CHECK (loadAndSettle (engine, modelPath ("wavenet_a1_standard.nam"), 48000, 128), "load failed");
    CHECK (engine.getLatencySamples() == 0, "no latency expected at the model's own rate");

    engine.prepare (96000, 512);
    CHECK (engine.getStatus().resampling, "96 kHz should resample");
    CHECK (engine.getLatencySamples() > 0, "latency should be reported at 96 kHz");
    auto x = sine (220, 96000, 1.0, 0.2f);
    runEngine (engine, x, 512);
    CHECK (allFinite (x) && rms (x, x.size() / 2, x.size()) > 1e-4, "bad output after rate change");
}

static void benchmark()
{
    std::printf ("[benchmark] CPU per model (48 kHz, 128-sample blocks, one core)\n");
    double rate = 48000;
    auto di = exampleInput (rate);
    for (const char* name : { "wavenet_a1_standard.nam", "A2.nam", "lstm.nam" })
    {
        AmpEngine engine;
        engine.prepare (rate, 128);
        if (! loadAndSettle (engine, modelPath (name), rate, 128))
        {
            CHECK (false, "could not load %s", name);
            continue;
        }
        std::vector<float> x;
        while (x.size() < (size_t) (10 * rate))
            x.insert (x.end(), di.begin(), di.end());
        const auto t0 = std::chrono::steady_clock::now();
        runEngine (engine, x, 128);
        const double secs = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const double audioSecs = (double) x.size() / rate;
        std::printf ("  %-26s %-8s %6zu weights  %6.1fx real-time  (%.1f%% of one core)\n", name,
                     engine.getStatus().info->architecture.c_str(), engine.getStatus().info->numWeights,
                     audioSecs / secs, 100.0 * secs / audioSecs);
    }
}

int main()
{
    testParity ("wavenet_a1_standard.nam");
    testParity ("A2.nam");
    testParity ("lstm.nam");
    testResampling (44100.0);
    testResampling (96000.0);
    testRealtimeSafety();
    testSwitchingIsClickFree();
    testErrors();
    testSampleRateChangeWhileLoaded();
    benchmark();

    std::printf (gFailures == 0 ? "\nALL TESTS PASSED\n" : "\n%d CHECK(S) FAILED\n", gFailures);
    return gFailures == 0 ? 0 : 1;
}
