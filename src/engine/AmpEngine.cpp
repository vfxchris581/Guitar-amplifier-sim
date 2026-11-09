#include "engine/AmpEngine.h"
#include "engine/NamModelInstance.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace gas
{
namespace
{
constexpr double kCrossfadeSeconds = 0.030; // model switch crossfade

inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

inline float blockPeak (const float* x, int n) noexcept
{
    float p = 0.0f;
    for (int i = 0; i < n; ++i)
        p = std::max (p, std::abs (x[i]));
    return p;
}

inline void updatePeak (std::atomic<float>& meter, float p) noexcept
{
    if (p > meter.load (std::memory_order_relaxed))
        meter.store (p, std::memory_order_relaxed);
}
} // namespace

// ---------------------------------------------------------------------------
template <size_t C>
bool AmpEngine::SpscQueue<C>::push (Slot* s) noexcept
{
    const auto h = head.load (std::memory_order_relaxed);
    const auto next = (h + 1) % C;
    if (next == tail.load (std::memory_order_acquire))
        return false;
    items[h] = s;
    head.store (next, std::memory_order_release);
    return true;
}

template <size_t C>
AmpEngine::Slot* AmpEngine::SpscQueue<C>::pop() noexcept
{
    const auto t = tail.load (std::memory_order_relaxed);
    if (t == head.load (std::memory_order_acquire))
        return nullptr;
    auto* s = items[t];
    tail.store ((t + 1) % C, std::memory_order_release);
    return s;
}

template <size_t C>
size_t AmpEngine::SpscQueue<C>::freeSpace() const noexcept
{
    const auto h = head.load (std::memory_order_acquire);
    const auto t = tail.load (std::memory_order_acquire);
    return (C - 1) - ((h + C - t) % C);
}

// ---------------------------------------------------------------------------
AmpEngine::AmpEngine()
{
    current = new Slot(); // DI passthrough until a model is loaded
    prepare (hostRate, maxBlock);
    worker = std::thread ([this] { workerLoop(); });
}

AmpEngine::~AmpEngine()
{
    {
        std::lock_guard<std::mutex> lk (requestMutex);
        quit = true;
    }
    requestCv.notify_all();
    if (worker.joinable())
        worker.join();

    freeRetired();
    delete pending.exchange (nullptr);
    delete current;
    delete outgoing;
}

void AmpEngine::prepare (double sampleRate, int maxBlockSize)
{
    const bool resampling = prepareLocked (sampleRate, maxBlockSize);

    // Keep the UI's view of the current model in step with the new rate.
    std::lock_guard<std::mutex> lk (statusMutex);
    status.resampling = resampling;
    status.hostSampleRate = sampleRate;
    status.serial = statusSerial.fetch_add (1) + 1;
}

bool AmpEngine::prepareLocked (double sampleRate, int maxBlockSize)
{
    std::lock_guard<std::mutex> lk (configMutex);

    hostRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    maxBlock = std::max (1, maxBlockSize);

    dryBuffer.assign ((size_t) maxBlock, 0.0f);
    bufferA.assign ((size_t) maxBlock, 0.0f);
    bufferB.assign ((size_t) maxBlock, 0.0f);
    fadeLength = std::max (1, (int) std::lround (kCrossfadeSeconds * hostRate));

    // Audio is stopped while prepare() runs, so the audio-owned slots are ours.
    delete outgoing;
    outgoing = nullptr;
    fading.store (false);

    auto reprepare = [this] (Slot* s)
    {
        if (s == nullptr)
            return;
        if (s->instance != nullptr)
            s->instance->prepare (hostRate, maxBlock);
        s->gainsInitialised = false;
    };

    reprepare (current);
    const Slot* newest = current;

    if (Slot* p = pending.exchange (nullptr))
    {
        reprepare (p);
        pending.store (p);
        newest = p;
    }

    activeLatency.store (current != nullptr && current->instance != nullptr ? current->instance->latencySamples() : 0);
    return newest != nullptr && newest->instance != nullptr && newest->instance->isResampling();
}

// ---------------------------------------------------------------------------
void AmpEngine::computeTargetGains (const Slot& slot, float& inGain, float& outGain) const noexcept
{
    float inDb = inputGainDb.load (std::memory_order_relaxed);
    float outDb = outputGainDb.load (std::memory_order_relaxed);

    if (const auto* inst = slot.instance.get())
    {
        const auto& info = inst->info();

        // Input calibration: make the model see the same analogue level the real amp saw.
        if (calibrateInput.load (std::memory_order_relaxed) && info.inputLevelDbu)
            inDb += interfaceInputDbu.load (std::memory_order_relaxed) - (float) *info.inputLevelDbu;

        switch ((OutputMode) outputMode.load (std::memory_order_relaxed))
        {
            case OutputMode::Calibrated:
                if (info.outputLevelDbu)
                {
                    outDb += (float) *info.outputLevelDbu - interfaceOutputDbu.load (std::memory_order_relaxed);
                    break;
                }
                [[fallthrough]]; // no output level stored: fall back to loudness normalisation
            case OutputMode::Normalized:
                if (info.loudnessDb)
                    outDb += normalizedTargetDb - (float) *info.loudnessDb;
                break;
            case OutputMode::Raw:
            default:
                break;
        }
    }

    inGain = dbToGain (inDb);
    outGain = dbToGain (outDb);
}

void AmpEngine::processSlot (Slot& slot, const float* in, float* out, int n) noexcept
{
    float targetIn, targetOut;
    computeTargetGains (slot, targetIn, targetOut);

    if (! slot.gainsInitialised)
    {
        slot.inGain = targetIn;
        slot.outGain = targetOut;
        slot.gainsInitialised = true;
    }

    // Per-block linear ramps keep gain changes click-free.
    const float inStep = (targetIn - slot.inGain) / (float) n;
    const float outStep = (targetOut - slot.outGain) / (float) n;

    float g = slot.inGain;
    for (int i = 0; i < n; ++i)
    {
        g += inStep;
        out[i] = in[i] * g;
    }

    if (slot.instance != nullptr)
        slot.instance->process (out, out, n);

    g = slot.outGain;
    for (int i = 0; i < n; ++i)
    {
        g += outStep;
        out[i] *= g;
    }

    slot.inGain = targetIn;
    slot.outGain = targetOut;
}

void AmpEngine::process (float* samples, int numSamples) noexcept
{
    // Pick up a newly loaded model (only if we have room to retire the old ones).
    if (retired.freeSpace() >= 2)
    {
        if (Slot* p = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            if (outgoing != nullptr)
                retired.push (outgoing); // a switch arrived mid-fade: drop the oldest
            outgoing = current;
            current = p;
            fadePos = 0;
            fading.store (true);
            activeSerial.store (p->serial);
            activeLatency.store (p->instance != nullptr ? p->instance->latencySamples() : 0);
        }
    }

    updatePeak (inputPeak, blockPeak (samples, numSamples));

    int done = 0;
    while (done < numSamples)
    {
        const int n = std::min (numSamples - done, maxBlock);
        float* io = samples + done;

        if (outgoing == nullptr)
        {
            processSlot (*current, io, io, n);
        }
        else
        {
            std::memcpy (dryBuffer.data(), io, sizeof (float) * (size_t) n);
            processSlot (*current, dryBuffer.data(), bufferA.data(), n);
            processSlot (*outgoing, dryBuffer.data(), bufferB.data(), n);

            // Raised-cosine crossfade (sin^2 / cos^2 always sums to 1).
            constexpr float halfPi = 1.57079632679f;
            for (int i = 0; i < n; ++i)
            {
                const float t = std::min (1.0f, (float) (fadePos + i) / (float) fadeLength);
                const float s = std::sin (halfPi * t);
                const float wIn = s * s;
                io[i] = bufferA[(size_t) i] * wIn + bufferB[(size_t) i] * (1.0f - wIn);
            }

            fadePos += n;
            if (fadePos >= fadeLength)
            {
                retired.push (outgoing);
                outgoing = nullptr;
                fading.store (false);
            }
        }

        done += n;
    }

    updatePeak (outputPeak, blockPeak (samples, numSamples));
}

bool AmpEngine::isSettled() const noexcept
{
    return activeSerial.load() == publishedSerial.load() && ! fading.load() && pending.load() == nullptr;
}

// ---------------------------------------------------------------------------
void AmpEngine::loadModel (const std::filesystem::path& file)
{
    {
        std::lock_guard<std::mutex> lk (requestMutex);
        nextRequest = Request { false, file };
        requestedPath = pathToUtf8 (file);
    }

    std::optional<ModelInfo> info;
    bool resampling = false;
    {
        std::lock_guard<std::mutex> lk (statusMutex);
        info = status.info;
        resampling = status.resampling;
    }
    setStatus (Status::State::Loading, "Loading " + pathToUtf8 (file.filename()) + "...", info, resampling);
    requestCv.notify_all();
}

void AmpEngine::unloadModel()
{
    {
        std::lock_guard<std::mutex> lk (requestMutex);
        nextRequest = Request { true, {} };
        requestedPath.clear();
    }
    requestCv.notify_all();
}

std::string AmpEngine::requestedModelPath() const
{
    std::lock_guard<std::mutex> lk (requestMutex);
    return requestedPath;
}

AmpEngine::Status AmpEngine::getStatus() const
{
    std::lock_guard<std::mutex> lk (statusMutex);
    return status;
}

void AmpEngine::setStatus (Status::State s, std::string message, std::optional<ModelInfo> info, bool resampling)
{
    std::lock_guard<std::mutex> lk (statusMutex);
    status.state = s;
    status.message = std::move (message);
    status.info = std::move (info);
    status.resampling = resampling;
    {
        std::lock_guard<std::mutex> cl (configMutex);
        status.hostSampleRate = hostRate;
    }
    status.serial = statusSerial.fetch_add (1) + 1;
}

void AmpEngine::waitUntilWorkerIdle()
{
    std::unique_lock<std::mutex> lk (requestMutex);
    idleCv.wait (lk, [this] { return quit || (! busy && ! nextRequest.has_value()); });
}

// ---------------------------------------------------------------------------
void AmpEngine::workerLoop()
{
    std::unique_lock<std::mutex> lk (requestMutex);

    while (! quit)
    {
        requestCv.wait_for (lk, std::chrono::milliseconds (40), [this] { return quit || nextRequest.has_value(); });
        if (quit)
            break;

        if (nextRequest.has_value())
        {
            const Request r = *nextRequest;
            nextRequest.reset();
            busy = true;
            lk.unlock();
            handleRequest (r);
            lk.lock();
            busy = false;
        }

        lk.unlock();
        freeRetired();
        lk.lock();
        idleCv.notify_all();
    }

    idleCv.notify_all();
}

void AmpEngine::handleRequest (const Request& r)
{
    if (r.unload)
    {
        publish (std::make_unique<Slot>());
        setStatus (Status::State::Empty, "No amp model loaded - the guitar signal passes through unprocessed.", std::nullopt, false);
        return;
    }

    double rate;
    int block;
    {
        std::lock_guard<std::mutex> lk (configMutex);
        rate = hostRate;
        block = maxBlock;
    }

    std::string error;
    auto instance = NamModelInstance::load (r.file, rate, block, error);

    {
        // A newer request already arrived: skip publishing this one.
        std::lock_guard<std::mutex> lk (requestMutex);
        if (nextRequest.has_value())
            return;
    }

    if (instance == nullptr)
    {
        // Keep the previous model playing; report why the new one failed.
        std::optional<ModelInfo> info;
        bool resampling = false;
        {
            std::lock_guard<std::mutex> lk (statusMutex);
            info = status.info;
            resampling = status.resampling;
        }
        setStatus (Status::State::Error, error, info, resampling);
        return;
    }

    ModelInfo info = instance->info();
    auto slot = std::make_unique<Slot>();
    slot->instance = std::move (instance);
    const bool resampling = slot->instance->isResampling();
    publish (std::move (slot));

    std::string msg = "Ready";
    if (! info.sampleRateFromFile)
        msg += " - file has no sample rate; NAM's 48 kHz default assumed";
    setStatus (Status::State::Ready, msg, std::move (info), resampling);
}

void AmpEngine::publish (std::unique_ptr<Slot> slot)
{
    Slot* old = nullptr;
    {
        std::lock_guard<std::mutex> lk (configMutex);

        // prepare() may have changed the rate while we were loading.
        if (slot->instance != nullptr
            && (slot->instance->preparedSampleRate() != hostRate || slot->instance->preparedBlockSize() != maxBlock))
            slot->instance->prepare (hostRate, maxBlock);

        slot->serial = nextSerial++;
        publishedSerial.store (slot->serial);
        old = pending.exchange (slot.release(), std::memory_order_acq_rel);
    }
    delete old; // never reached the audio thread
}

void AmpEngine::freeRetired()
{
    while (Slot* s = retired.pop())
        delete s;
}

} // namespace gas
