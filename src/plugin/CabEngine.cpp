#include "plugin/CabEngine.h"

#include <cmath>

void Biquad::set (Type type, double freq, double fs) noexcept
{
    const double w0 = 2.0 * juce::MathConstants<double>::pi * juce::jlimit (1.0, fs * 0.49, freq) / fs;
    const double cosw = std::cos (w0), alpha = std::sin (w0) / (2.0 * std::sqrt (0.5)); // Q = 1/sqrt(2)
    const double a0 = 1.0 + alpha;
    if (type == Type::HighPass)
    {
        b0 = (1.0 + cosw) * 0.5 / a0;
        b1 = -(1.0 + cosw) / a0;
        b2 = b0;
    }
    else
    {
        b0 = (1.0 - cosw) * 0.5 / a0;
        b1 = (1.0 - cosw) / a0;
        b2 = b0;
    }
    a1 = -2.0 * cosw / a0;
    a2 = (1.0 - alpha) / a0;
}

CabEngine::CabEngine()
{
    formats.registerBasicFormats();
    for (auto& f : irLoaded)
        f = false;
    for (auto& f : irStereo)
        f = false;
}

void CabEngine::prepare (double sr, int block)
{
    sampleRate = sr > 0 ? sr : 48000.0;
    maxBlock = juce::jmax (1, block);
    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, 2 };
    for (auto& m : micDsp)
    {
        m.convolution.prepare (spec);
        m.delay.prepare (spec);
        m.delay.setMaximumDelayInSamples ((int) std::ceil (maxDelayMs * 0.001 * sampleRate) + 8);
        m.lastHpf = m.lastLpf = -1.0f;
        m.gainsInitialised = false;
        for (auto& f : m.hpf) f.reset();
        for (auto& f : m.lpf) f.reset();
    }
    scratch.setSize (2, maxBlock, false, true, false);
    micOut.setSize (2, maxBlock, false, true, false);
}

void CabEngine::updateFilters (Mic& m, const MicParams& p) noexcept
{
    const float hp = p.hpfHz.load(), lp = p.lpfHz.load();
    if (std::abs (hp - m.lastHpf) > 1e-3f)
    {
        for (auto& f : m.hpf)
            hp <= 20.0f ? f.bypass() : f.set (Biquad::Type::HighPass, hp, sampleRate);
        m.lastHpf = hp;
    }
    if (std::abs (lp - m.lastLpf) > 1e-3f)
    {
        for (auto& f : m.lpf)
            lp >= 20000.0f || lp >= sampleRate * 0.49 ? f.bypass() : f.set (Biquad::Type::LowPass, lp, sampleRate);
        m.lastLpf = lp;
    }
}

bool CabEngine::process (const float* in, float* outL, float* outR, int numSamples) noexcept
{
    bool any = false;
    for (int i = 0; i < numMics; ++i)
        any = any || (mics[(size_t) i].enabled.load() && irLoaded[(size_t) i].load());
    if (! any)
        return false;

    for (int done = 0; done < numSamples;)
    {
        const int n = juce::jmin (maxBlock, numSamples - done);
        std::fill (outL + done, outL + done + n, 0.0f);
        std::fill (outR + done, outR + done + n, 0.0f);

        for (int mi = 0; mi < numMics; ++mi)
        {
            auto& p = mics[(size_t) mi];
            auto& m = micDsp[(size_t) mi];
            if (! p.enabled.load() || ! irLoaded[(size_t) mi].load())
                continue;

            for (int ch = 0; ch < 2; ++ch)
                micOut.copyFrom (ch, 0, in + done, n);

            juce::dsp::AudioBlock<float> block (micOut);
            auto sub = block.getSubBlock (0, (size_t) n);
            m.convolution.process (juce::dsp::ProcessContextReplacing<float> (sub));

            updateFilters (m, p);
            const float delaySamples = juce::jlimit (0.0f, maxDelayMs, p.delayMs.load()) * 0.001f * (float) sampleRate;
            m.delay.setDelay (delaySamples);
            const float polarity = p.invert.load() ? -1.0f : 1.0f;

            for (int ch = 0; ch < 2; ++ch)
            {
                float* x = micOut.getWritePointer (ch);
                for (int s = 0; s < n; ++s)
                {
                    m.delay.pushSample (ch, x[s]);
                    float y = m.delay.popSample (ch);
                    y = m.lpf[(size_t) ch].process (m.hpf[(size_t) ch].process (y));
                    x[s] = polarity * y;
                }
            }

            // Pan: equal-power for mono IRs (unity in the centre), balance for stereo IRs.
            const float level = std::pow (10.0f, p.levelDb.load() / 20.0f);
            const float pan = juce::jlimit (-1.0f, 1.0f, p.pan.load());
            float gl, gr;
            if (irStereo[(size_t) mi].load())
            {
                gl = level * juce::jmin (1.0f, 1.0f - pan);
                gr = level * juce::jmin (1.0f, 1.0f + pan);
            }
            else
            {
                const float theta = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
                gl = level * std::sqrt (2.0f) * std::cos (theta);
                gr = level * std::sqrt (2.0f) * std::sin (theta);
            }
            if (! m.gainsInitialised)
            {
                m.gainL = gl;
                m.gainR = gr;
                m.gainsInitialised = true;
            }
            const float stepL = (gl - m.gainL) / (float) n, stepR = (gr - m.gainR) / (float) n;
            const float* xl = micOut.getReadPointer (0);
            const float* xr = micOut.getReadPointer (1);
            float cl = m.gainL, cr = m.gainR;
            for (int s = 0; s < n; ++s)
            {
                cl += stepL;
                cr += stepR;
                outL[done + s] += xl[s] * cl;
                outR[done + s] += xr[s] * cr;
            }
            m.gainL = gl;
            m.gainR = gr;
        }
        done += n;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool CabEngine::loadIR (int mic, const juce::File& file, double maxLengthMs, bool normalise, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Could not read " + file.getFileName() + " as an audio file.";
        return false;
    }
    if (reader->lengthInSamples < 1 || reader->lengthInSamples > (juce::int64) (reader->sampleRate * 20.0))
    {
        error = "This doesn't look like an impulse response (length " + juce::String (reader->lengthInSamples / reader->sampleRate, 2) + " s).";
        return false;
    }
    const int channels = (int) juce::jmin (2u, reader->numChannels);
    juce::AudioBuffer<float> buf (channels, (int) reader->lengthInSamples);
    reader->read (&buf, 0, buf.getNumSamples(), 0, true, channels > 1);

    {
        std::lock_guard<std::mutex> lk (infoMutex);
        originals[(size_t) mic] = std::move (buf);
        originalRates[(size_t) mic] = reader->sampleRate;
        auto& info = infos[(size_t) mic];
        info.name = file.getFileNameWithoutExtension();
        info.path = file.getFullPathName();
        info.channels = channels;
        info.sampleRate = reader->sampleRate;
        info.fileLengthMs = 1000.0 * (double) reader->lengthInSamples / reader->sampleRate;
    }
    return reloadIR (mic, maxLengthMs, normalise);
}

bool CabEngine::reloadIR (int mic, double maxLengthMs, bool normalise)
{
    juce::AudioBuffer<float> buf;
    double rate;
    {
        std::lock_guard<std::mutex> lk (infoMutex);
        if (originals[(size_t) mic].getNumSamples() == 0)
            return false;
        buf.makeCopyOf (originals[(size_t) mic]);
        rate = originalRates[(size_t) mic];
    }

    // Adjustable length: truncate with a 5 ms raised-cosine fade so the cut is inaudible.
    const int maxLen = juce::jmax (16, (int) std::lround (maxLengthMs * 0.001 * rate));
    if (buf.getNumSamples() > maxLen)
    {
        buf.setSize (buf.getNumChannels(), maxLen, true, false, false);
        const int fade = juce::jmin (maxLen / 2, (int) (0.005 * rate));
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < fade; ++i)
            {
                const float w = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * (float) (i + 1) / (float) fade);
                buf.setSample (ch, maxLen - fade + i, buf.getSample (ch, maxLen - fade + i) * w);
            }
    }
    const double usedMs = 1000.0 * buf.getNumSamples() / rate;
    const bool stereo = buf.getNumChannels() > 1;

    micDsp[(size_t) mic].convolution.loadImpulseResponse (std::move (buf), rate,
                                                          stereo ? juce::dsp::Convolution::Stereo::yes : juce::dsp::Convolution::Stereo::no,
                                                          juce::dsp::Convolution::Trim::no,
                                                          normalise ? juce::dsp::Convolution::Normalise::yes : juce::dsp::Convolution::Normalise::no);
    irStereo[(size_t) mic] = stereo;
    irLoaded[(size_t) mic] = true;

    std::lock_guard<std::mutex> lk (infoMutex);
    infos[(size_t) mic].usedLengthMs = usedMs;
    infos[(size_t) mic].normalised = normalise;
    return true;
}

void CabEngine::clearIR (int mic)
{
    irLoaded[(size_t) mic] = false;
    std::lock_guard<std::mutex> lk (infoMutex);
    infos[(size_t) mic] = {};
    originals[(size_t) mic].setSize (0, 0);
}

CabEngine::IRInfo CabEngine::getIRInfo (int mic) const
{
    std::lock_guard<std::mutex> lk (infoMutex);
    return infos[(size_t) mic];
}
