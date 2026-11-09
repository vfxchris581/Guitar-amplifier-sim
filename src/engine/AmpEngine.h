#pragma once

#include "engine/ModelInfo.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace gas
{

class NamModelInstance;

/** How the amp model's output level is set. Mirrors the official NAM plugin. */
enum class OutputMode : int
{
    Raw = 0,        // the model's output exactly as trained
    Normalized = 1, // uses the model's measured loudness to hit a -18 dB reference
    Calibrated = 2  // uses the model's recorded output level (dBu) and your interface's output level
};

/** The amplifier stage: real-time NAM inference with click-free model switching.

    Threading model
      - A private worker thread loads models, prepares them for the current
        sample rate and frees retired ones. File I/O, parsing, allocation and
        model warm-up happen only there.
      - The audio thread picks up a finished model through a single atomic
        pointer, crossfades from the previous model, and hands the old one back
        through a lock-free queue. process() never allocates, locks or blocks.
      - prepare() must be called while audio is stopped (prepareToPlay).
*/
class AmpEngine
{
public:
    AmpEngine();
    ~AmpEngine();

    AmpEngine (const AmpEngine&) = delete;
    AmpEngine& operator= (const AmpEngine&) = delete;

    // ---- setup (not real-time) -------------------------------------------
    void prepare (double sampleRate, int maxBlockSize);

    // ---- real-time -------------------------------------------------------
    /** Processes a mono buffer in place. */
    void process (float* samples, int numSamples) noexcept;

    // ---- model management (any non-audio thread) --------------------------
    void loadModel (const std::filesystem::path& file);
    void unloadModel();

    /** Path to keep in the plugin state: the model last asked for (even if the
        file is currently missing, so presets keep their reference). */
    std::string requestedModelPath() const;

    struct Status
    {
        enum class State { Empty, Loading, Ready, Error };
        State state = State::Empty;
        std::string message;            // human-readable status / error explanation
        std::optional<ModelInfo> info;  // info for the model that is (or is about to be) playing
        bool resampling = false;
        double hostSampleRate = 0.0;
        std::uint64_t serial = 0;       // increments on every change
    };

    Status getStatus() const;
    std::uint64_t getStatusSerial() const noexcept { return statusSerial.load(); }

    /** Blocks until the worker has nothing left to do (for tests / offline use). */
    void waitUntilWorkerIdle();

    /** True once the audio thread is playing the most recently loaded model and any crossfade has finished. */
    bool isSettled() const noexcept;

    // ---- parameters (written by any thread, read by the audio thread) -------
    std::atomic<float> inputGainDb { 0.0f };
    std::atomic<float> outputGainDb { 0.0f };
    std::atomic<int>   outputMode { (int) OutputMode::Normalized };
    std::atomic<bool>  calibrateInput { false };
    std::atomic<float> interfaceInputDbu { 12.0f };  // level at which your interface input hits 0 dBFS
    std::atomic<float> interfaceOutputDbu { 12.0f }; // level your interface output produces at 0 dBFS

    // ---- monitoring --------------------------------------------------------
    /** Peak since last call (linear). */
    float takeInputPeak() noexcept  { return inputPeak.exchange (0.0f); }
    float takeOutputPeak() noexcept { return outputPeak.exchange (0.0f); }

    /** Latency (samples at host rate) of the model currently playing. */
    int getLatencySamples() const noexcept { return activeLatency.load(); }

    static constexpr float normalizedTargetDb = -18.0f;

private:
    struct Slot
    {
        std::unique_ptr<NamModelInstance> instance; // nullptr => no model (DI passthrough)
        std::uint64_t serial = 0;
        float inGain = 1.0f, outGain = 1.0f;        // audio-thread smoothing state
        bool gainsInitialised = false;
    };

    // Lock-free single-producer (audio) / single-consumer (worker) queue
    template <size_t Capacity>
    struct SpscQueue
    {
        bool push (Slot* s) noexcept;
        Slot* pop() noexcept;
        size_t freeSpace() const noexcept;
        std::array<Slot*, Capacity> items {};
        std::atomic<size_t> head { 0 }, tail { 0 };
    };

    struct Request
    {
        bool unload = false;
        std::filesystem::path file;
    };

    bool prepareLocked (double sampleRate, int maxBlockSize); // returns whether the newest model resamples
    void workerLoop();
    void handleRequest (const Request& r);
    void publish (std::unique_ptr<Slot> slot);
    void freeRetired();
    void setStatus (Status::State, std::string message, std::optional<ModelInfo> info, bool resampling);

    void processSlot (Slot& slot, const float* in, float* out, int n) noexcept;
    void computeTargetGains (const Slot& slot, float& inGain, float& outGain) const noexcept;

    // config shared between prepare() and the worker
    mutable std::mutex configMutex;
    double hostRate = 48000.0;
    int maxBlock = 512;

    // hand-off: worker -> audio
    std::atomic<Slot*> pending { nullptr };
    // audio-thread-owned
    Slot* current = nullptr;
    Slot* outgoing = nullptr;
    int fadePos = 0, fadeLength = 1;
    std::vector<float> dryBuffer, bufferA, bufferB;
    // hand-off: audio -> worker
    SpscQueue<64> retired;

    std::atomic<std::uint64_t> activeSerial { 0 };
    std::atomic<std::uint64_t> publishedSerial { 0 };
    std::atomic<bool> fading { false };
    std::atomic<int> activeLatency { 0 };
    std::atomic<float> inputPeak { 0.0f }, outputPeak { 0.0f };

    // worker
    std::thread worker;
    mutable std::mutex requestMutex;
    std::condition_variable requestCv, idleCv;
    std::optional<Request> nextRequest;
    bool busy = false;
    bool quit = false;
    std::uint64_t nextSerial = 1;
    std::string requestedPath;

    mutable std::mutex statusMutex;
    Status status;
    std::atomic<std::uint64_t> statusSerial { 0 };
};

} // namespace gas
