#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace fx
{
/** A processing module in the rig. Pre-amp modules are mono (right == nullptr);
    post-amp modules are stereo. process() is real-time safe. */
class Module
{
public:
    virtual ~Module() = default;
    virtual void prepare (double sampleRate, int maxBlock) = 0;
    virtual void reset() {}
    virtual void process (float* left, float* right, int n) noexcept = 0;

    /** Parameter access by "<module>_<name>". */
    void bind (juce::AudioProcessorValueTreeState&, const juce::String& moduleId);
    float p (int index) const noexcept { return params[(size_t) index]->load (std::memory_order_relaxed); }
    bool enabled() const noexcept { return onParam != nullptr && onParam->load() > 0.5f; }

    virtual std::vector<const char*> paramNames() const = 0;

protected:
    std::vector<std::atomic<float>*> params;
    std::atomic<float>* onParam = nullptr;
    double fs = 48000.0;

public:
    const std::atomic<double>* tempo = nullptr; // host BPM (for tempo-synced effects)
};

/** Identifiers, display names and order of every module. */
struct ModuleInfo
{
    const char* id;
    const char* name;
    bool preAmp; // mono, before the amp
};
const std::vector<ModuleInfo>& allModules();
int moduleIndex (const juce::String& id);

/** Adds every effect parameter ("<module>_<name>") to the plugin's layout. */
void addParameters (juce::AudioProcessorValueTreeState::ParameterLayout&);

std::unique_ptr<Module> createModule (const juce::String& id);

/** Magnitude response (dB) of the EQ's current settings at the given frequencies. */
std::vector<float> eqResponseDb (juce::AudioProcessorValueTreeState&, const std::vector<float>& freqs, double sampleRate);

/** The parameter names (after "<module>_") a module exposes, in display order. */
std::vector<juce::String> parameterNames (const juce::String& moduleId);

/** Post-EQ analyser tap: audio thread writes, UI reads. */
struct AnalyserTap
{
    void push (const float* l, const float* r, int n) noexcept;
    int pull (float* dest, int maxSamples) noexcept;
    juce::AbstractFifo fifo { 8192 };
    std::array<float, 8192> buffer {};
};

/** The ordered pre-amp (stomp) and post-amp (effects) chains. Orders are
    changed on the message thread and read lock-free by the audio thread. */
class Chain
{
public:
    Chain();
    void bind (juce::AudioProcessorValueTreeState&);
    void prepare (double sampleRate, int maxBlock);

    void processPre (float* mono, int n) noexcept;                      // before the amp
    void processPost (float* left, float* right, int n) noexcept;       // after the cab

    /** Comma-separated module ids, e.g. "gate,comp,od,boost,dist,fuzz". */
    juce::String getOrder (bool pre) const;
    bool setOrder (bool pre, const juce::String& ids); // must be a permutation of that section's modules
    static juce::String defaultOrder (bool pre);

    Module* get (const juce::String& id) { const int i = moduleIndex (id); return i < 0 ? nullptr : modules[(size_t) i].get(); }
    std::atomic<double> bpm { 120.0 };
    AnalyserTap analyser; // signal right after the EQ

private:
    std::vector<std::unique_ptr<Module>> modules;
    std::atomic<std::uint64_t> preOrder { 0 }, postOrder { 0 }; // packed 6 x 8-bit module indices
    std::atomic<float>* stompsOn = nullptr;
    std::atomic<float>* fxOn = nullptr;
};
} // namespace fx
