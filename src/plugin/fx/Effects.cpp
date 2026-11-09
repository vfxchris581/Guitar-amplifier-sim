#include "plugin/fx/Effects.h"
#include "plugin/CabEngine.h" // Biquad

#include <cmath>
#include <complex>

namespace fx
{
using namespace juce;

namespace
{
constexpr float pi = MathConstants<float>::pi;
inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept { return 20.0f * std::log10 (std::max (g, 1e-9f)); }
inline float onePoleCoef (float ms, double fs) noexcept { return std::exp (-1.0f / (std::max (ms, 0.01f) * 0.001f * (float) fs)); }

/** RBJ peaking / shelving biquads for the EQ (same structure as the cab filters). */
struct EqBiquad : Biquad
{
    void peak (double f, double q, double gainDb, double fs) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * MathConstants<double>::pi * jlimit (10.0, fs * 0.49, f) / fs;
        const double alpha = std::sin (w0) / (2.0 * q), c = std::cos (w0), a0 = 1.0 + alpha / A;
        b0 = (1.0 + alpha * A) / a0; b1 = -2.0 * c / a0; b2 = (1.0 - alpha * A) / a0; a1 = -2.0 * c / a0; a2 = (1.0 - alpha / A) / a0;
    }
    void shelf (bool high, double f, double gainDb, double fs) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0), w0 = 2.0 * MathConstants<double>::pi * jlimit (10.0, fs * 0.49, f) / fs;
        const double c = std::cos (w0), alpha = std::sin (w0) / 2.0 * std::sqrt (2.0), sA = 2.0 * std::sqrt (A) * alpha;
        const double sign = high ? -1.0 : 1.0;
        const double a0 = (A + 1) + sign * (A - 1) * c + sA;
        b0 = A * ((A + 1) - sign * (A - 1) * c + sA) / a0;
        b1 = sign * 2 * A * ((A - 1) - sign * (A + 1) * c) / a0;
        b2 = A * ((A + 1) - sign * (A - 1) * c - sA) / a0;
        a1 = -sign * 2 * ((A - 1) + sign * (A + 1) * c) / a0;
        a2 = ((A + 1) + sign * (A - 1) * c - sA) / a0;
    }
};

/** One-pole low-pass / high-pass. */
struct OnePole
{
    float a = 0, z = 0;
    void setLowPass (float hz, double fs) noexcept { a = std::exp (-2.0f * pi * hz / (float) fs); }
    float lp (float x) noexcept { z = x + a * (z - x); return z; }
    float hp (float x) noexcept { return x - lp (x); }
};

/** Mono stomp with 4x polyphase-IIR oversampling around its non-linearity. */
class OversampledStomp : public Module
{
public:
    void prepare (double sampleRate, int maxBlock) override
    {
        fs = sampleRate;
        os = std::make_unique<dsp::Oversampling<float>> (1, 2, dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true);
        os->initProcessing ((size_t) maxBlock);
        osRate = sampleRate * 4.0;
        maxN = maxBlock;
        prepareStomp();
    }
    void reset() override { if (os) os->reset(); }

    void process (float* l, float*, int n) noexcept override
    {
        for (int done = 0; done < n;)
        {
            const int m = std::min (maxN, n - done);
            float* chans[] = { l + done };
            dsp::AudioBlock<float> block (chans, 1, (size_t) m);
            auto up = os->processSamplesUp (block);
            shape (up.getChannelPointer (0), (int) up.getNumSamples());
            os->processSamplesDown (block);
            post (l + done, m);
            done += m;
        }
    }

protected:
    virtual void prepareStomp() {}
    virtual void shape (float* x, int n) noexcept = 0;   // at 4x rate
    virtual void post (float* x, int n) noexcept = 0;    // tone / level at base rate
    std::unique_ptr<dsp::Oversampling<float>> os;
    double osRate = 192000.0;
    int maxN = 512;
};

// ---------------------------------------------------------------------------
class NoiseGate : public Module
{
    std::vector<const char*> paramNames() const override { return { "threshold", "release" }; }
    float env = 0, gain = 1, holdLeft = 0;
    bool open = false;

public:
    void prepare (double sr, int) override { fs = sr; reset(); }
    void reset() override { env = 0; gain = 1; open = false; holdLeft = 0; }
    void process (float* x, float*, int n) noexcept override
    {
        const float openAt = dbToGain (p (0)), closeAt = openAt * 0.5f; // 6 dB hysteresis
        const float envRel = onePoleCoef (20.0f, fs);
        const float atk = onePoleCoef (0.5f, fs), rel = onePoleCoef (p (1), fs);
        const float holdSamples = 0.05f * (float) fs;
        for (int i = 0; i < n; ++i)
        {
            const float a = std::abs (x[i]);
            env = a > env ? a : a + envRel * (env - a);
            if (env > openAt) { open = true; holdLeft = holdSamples; }
            else if (env < closeAt && --holdLeft <= 0) open = false;
            const float target = open ? 1.0f : 0.0f;
            gain = target + (open ? atk : rel) * (gain - target);
            x[i] *= gain;
        }
    }
};

class Compressor : public Module
{
    std::vector<const char*> paramNames() const override { return { "threshold", "ratio", "attack", "release", "makeup", "mix" }; }
    float envDb = -120.0f;

public:
    float lastReductionDb = 0.0f;
    void prepare (double sr, int) override { fs = sr; reset(); }
    void reset() override { envDb = -120.0f; }
    void process (float* x, float*, int n) noexcept override
    {
        const float thr = p (0), ratio = std::max (1.0f, p (1)), knee = 6.0f;
        const float atk = onePoleCoef (p (2), fs), rel = onePoleCoef (p (3), fs);
        const float makeup = dbToGain (p (4)), mix = p (5) / 100.0f;
        float maxRed = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float in = gainToDb (std::abs (x[i]));
            // soft-knee gain computer
            const float over = in - thr;
            float outDb = in;
            if (2.0f * over > knee)
                outDb = thr + over / ratio;
            else if (2.0f * std::abs (over) <= knee)
                outDb = in + (1.0f / ratio - 1.0f) * (over + knee / 2) * (over + knee / 2) / (2.0f * knee);
            const float red = in - outDb; // >= 0
            envDb = red > envDb ? red + atk * (envDb - red) : red + rel * (envDb - red);
            maxRed = std::max (maxRed, envDb);
            const float wet = x[i] * dbToGain (-envDb) * makeup;
            x[i] = mix * wet + (1.0f - mix) * x[i];
        }
        lastReductionDb = maxRed;
    }
};

class CleanBoost : public Module
{
    std::vector<const char*> paramNames() const override { return { "gain", "output" }; }
    float g = 1.0f;

public:
    void prepare (double sr, int) override { fs = sr; g = dbToGain (p (0) + p (1)); }
    void process (float* x, float*, int n) noexcept override
    {
        const float target = dbToGain (p (0) + p (1)), step = (target - g) / (float) n;
        for (int i = 0; i < n; ++i)
        {
            g += step;
            x[i] *= g;
        }
        g = target;
    }
};

/** Tube-Screamer-style: mid-focused clipping in the feedback path + clean low end, tone, level. */
class Overdrive : public OversampledStomp
{
    std::vector<const char*> paramNames() const override { return { "drive", "tone", "level" }; }
    OnePole hp720, tone;
    void prepareStomp() override { hp720.setLowPass (720.0f, osRate); }
    void shape (float* x, int n) noexcept override
    {
        const float gain = 1.0f + 120.0f * p (0) * p (0); // op-amp gain 1 .. ~120
        for (int i = 0; i < n; ++i)
        {
            const float mid = hp720.hp (x[i]);
            const float clipped = std::tanh (gain * mid) * 0.6f; // diode pair in the feedback loop
            x[i] = x[i] + clipped;
        }
    }
    void post (float* x, int n) noexcept override
    {
        tone.setLowPass (700.0f * std::pow (8.0f, p (1)), fs); // 700 Hz .. 5.6 kHz
        const float lvl = dbToGain (p (2));
        for (int i = 0; i < n; ++i)
            x[i] = tone.lp (x[i]) * lvl;
    }
};

/** RAT-style: high-gain op-amp stage with gain-bandwidth roll-off into hard silicon clipping, filter, output. */
class Distortion : public OversampledStomp
{
    std::vector<const char*> paramNames() const override { return { "drive", "tone", "output" }; }
    OnePole hp60, gbw, filter;
    void prepareStomp() override { hp60.setLowPass (60.0f, osRate); }
    void shape (float* x, int n) noexcept override
    {
        const float gain = 2.0f + 1000.0f * p (0) * p (0) * p (0);
        gbw.setLowPass (std::min (20000.0f, 1.0e6f / gain), osRate); // op-amp gain-bandwidth limit
        for (int i = 0; i < n; ++i)
        {
            const float v = gbw.lp (gain * hp60.hp (x[i]));
            x[i] = v / std::pow (1.0f + std::pow (std::abs (v), 4.0f), 0.25f) * 0.7f; // hard-knee diode clip
        }
    }
    void post (float* x, int n) noexcept override
    {
        filter.setLowPass (475.0f * std::pow (32.0f, 1.0f - p (1)) , fs); // "filter": 15 kHz (open) .. 475 Hz
        const float out = dbToGain (p (2));
        for (int i = 0; i < n; ++i)
            x[i] = filter.lp (x[i]) * out;
    }
};

/** Fuzz-Face-style: very high gain, asymmetric transistor clipping that cleans up at low input. */
class Fuzz : public OversampledStomp
{
    std::vector<const char*> paramNames() const override { return { "fuzz", "tone", "output" }; }
    OnePole dcBlock, toneLp, toneHp, sag;
    void prepareStomp() override { dcBlock.setLowPass (15.0f, osRate); sag.setLowPass (8.0f, osRate); }
    void shape (float* x, int n) noexcept override
    {
        const float gain = 1.0f + 500.0f * p (0) * p (0) * p (0); // ~1.5x at 10% .. ~360x at 90%
        for (int i = 0; i < n; ++i)
        {
            const float level = sag.lp (std::abs (x[i]));
            const float bias = 0.35f - 0.25f * std::min (1.0f, level * 8.0f); // bias shifts with level (sputter)
            const float v = gain * x[i];
            const float y = std::tanh (v + bias) - std::tanh (bias);
            x[i] = dcBlock.hp (y);
        }
    }
    void post (float* x, int n) noexcept override
    {
        const float t = p (1); // dark <-> bright tilt
        toneLp.setLowPass (6000.0f, fs);
        toneHp.setLowPass (400.0f, fs);
        const float out = dbToGain (p (2));
        for (int i = 0; i < n; ++i)
        {
            const float lowPart = toneLp.lp (x[i]);
            const float highPart = toneHp.hp (x[i]);
            x[i] = ((1.0f - t) * lowPart + t * highPart * 1.6f) * out;
        }
    }
};

// ---------------------------------------------------------------------------
class ParametricEq : public Module
{
    std::vector<const char*> paramNames() const override
    {
        return { "hpf", "lpf", "lowFreq", "lowGain", "b1Freq", "b1Gain", "b1Q", "b2Freq", "b2Gain", "b2Q",
                 "b3Freq", "b3Gain", "b3Q", "b4Freq", "b4Gain", "b4Q", "highFreq", "highGain" };
    }
    static constexpr int numFilters = 8;
    std::array<std::array<EqBiquad, numFilters>, 2> f;
    std::array<float, 18> last {};

    void design() noexcept
    {
        for (auto& ch : f)
        {
            p (0) <= 20.5f ? ch[0].bypass() : ch[0].set (Biquad::Type::HighPass, p (0), fs);
            p (1) >= 19999.0f ? ch[1].bypass() : ch[1].set (Biquad::Type::LowPass, p (1), fs);
            ch[2].shelf (false, p (2), p (3), fs);
            for (int b = 0; b < 4; ++b)
                ch[(size_t) (3 + b)].peak (p (4 + b * 3), std::max (0.1f, p (6 + b * 3)), p (5 + b * 3), fs);
            ch[7].shelf (true, p (16), p (17), fs);
        }
    }

public:
    void prepare (double sr, int) override { fs = sr; reset(); design(); for (int i = 0; i < 18; ++i) last[(size_t) i] = p (i); }
    void reset() override { for (auto& ch : f) for (auto& b : ch) b.reset(); }
    void process (float* l, float* r, int n) noexcept override
    {
        bool changed = false;
        for (int i = 0; i < 18; ++i)
            if (last[(size_t) i] != p (i)) { last[(size_t) i] = p (i); changed = true; }
        if (changed)
            design();
        float* chans[] = { l, r };
        for (int c = 0; c < 2; ++c)
            if (chans[c] != nullptr)
                for (int i = 0; i < n; ++i)
                {
                    float v = chans[c][i];
                    for (auto& b : f[(size_t) c])
                        v = b.process (v);
                    chans[c][i] = v;
                }
    }
};

class Modulation : public Module
{
    std::vector<const char*> paramNames() const override { return { "type", "rate", "depth", "feedback", "mix" }; }
    std::array<std::vector<float>, 2> buf;
    int writePos = 0, size = 1;
    double phase = 0;
    std::array<std::array<float, 6>, 2> apState {};
    std::array<float, 2> fbState {};

    float readDelay (int ch, float delaySamples) const noexcept
    {
        float rp = (float) writePos - delaySamples;
        while (rp < 0) rp += (float) size;
        const int i0 = (int) rp, i1 = (i0 + 1) % size;
        const float fr = rp - (float) i0;
        return buf[(size_t) ch][(size_t) i0] * (1 - fr) + buf[(size_t) ch][(size_t) i1] * fr;
    }

public:
    void prepare (double sr, int) override
    {
        fs = sr;
        size = (int) (0.05 * sr) + 4;
        for (auto& b : buf) b.assign ((size_t) size, 0.0f);
        reset();
    }
    void reset() override { for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f); apState = {}; fbState = {}; phase = 0; }

    void process (float* l, float* r, int n) noexcept override
    {
        const int type = (int) p (0); // 0 chorus, 1 phaser, 2 flanger
        const double inc = p (1) / fs;
        const float depth = p (2), fb = p (3), mix = p (4);
        float* chans[] = { l, r };
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                float* x = chans[c] != nullptr ? chans[c] : chans[0];
                if (chans[c] == nullptr) continue;
                const float lfo = (float) std::sin (2.0 * MathConstants<double>::pi * (phase + (c == 0 ? 0.0 : 0.25)));
                const float dry = x[i];
                float wet;
                if (type == 1) // phaser: 6 first-order all-passes swept 200 Hz .. 2 kHz
                {
                    const float fc = 200.0f * std::pow (10.0f, depth * (0.5f + 0.5f * lfo));
                    const float t = std::tan (pi * fc / (float) fs), a = (t - 1) / (t + 1);
                    float v = dry + fb * fbState[(size_t) c];
                    for (auto& s : apState[(size_t) c])
                    {
                        const float y = a * v + s;
                        s = v - a * y;
                        v = y;
                    }
                    fbState[(size_t) c] = v;
                    wet = v;
                }
                else
                {
                    const float base = type == 0 ? 0.007f : 0.001f, swing = type == 0 ? 0.005f : 0.004f;
                    const float d = (base + swing * depth * (0.5f + 0.5f * lfo)) * (float) fs;
                    wet = readDelay (c, d);
                    buf[(size_t) c][(size_t) writePos] = dry + (type == 2 ? fb * wet : 0.0f);
                }
                x[i] = type == 1 ? dry * (1 - mix * 0.5f) + wet * mix * 0.5f // phaser: notches need equal blend
                                 : dry * (1 - mix) + wet * mix;
            }
            if (++writePos >= size) writePos = 0;
            phase += inc;
            if (phase >= 1.0) phase -= 1.0;
        }
    }
};

class Delay : public Module
{
    std::vector<const char*> paramNames() const override
    {
        return { "type", "time", "sync", "feedback", "mix", "lowcut", "highcut", "width" };
    }
    std::array<std::vector<float>, 2> buf;
    int writePos = 0, size = 1;
    float smoothedDelay = -1;
    std::array<OnePole, 2> lc, hc;
    double lfo = 0;

public:
    static constexpr const char* syncNames[] = { "Off", "1/1", "1/2", "1/4", "1/4 dotted", "1/8", "1/8 dotted", "1/8 triplet", "1/16" };
    static float syncBeats (int i)
    {
        static const float beats[] = { 0, 4, 2, 1, 1.5f, 0.5f, 0.75f, 1.0f / 3.0f, 0.25f };
        return beats[jlimit (0, 8, i)];
    }

    void prepare (double sr, int) override
    {
        fs = sr;
        size = (int) (2.1 * sr) + 4;
        for (auto& b : buf) b.assign ((size_t) size, 0.0f);
        reset();
    }
    void reset() override { for (auto& b : buf) std::fill (b.begin(), b.end(), 0.0f); smoothedDelay = -1; }

    void process (float* l, float* r, int n) noexcept override
    {
        const int type = (int) p (0); // 0 digital, 1 analog, 2 ping-pong
        const int sync = (int) p (2);
        float ms = p (1);
        if (sync > 0 && tempo != nullptr)
            ms = (float) (60000.0 / std::max (20.0, tempo->load()) * syncBeats (sync));
        const float target = jlimit (1.0f, 2000.0f, ms) * 0.001f * (float) fs;
        if (smoothedDelay < 0) smoothedDelay = target;
        const float fb = std::min (0.95f, p (3)), mix = p (4), width = p (7);
        for (auto& f : lc) f.setLowPass (p (5), fs);
        for (auto& f : hc) f.setLowPass (type == 1 ? std::min (p (6), 2800.0f) : p (6), fs);

        for (int i = 0; i < n; ++i)
        {
            smoothedDelay += 0.0005f * (target - smoothedDelay); // glide on time changes, no zipper noise
            float d = smoothedDelay;
            if (type == 1) // analog: gentle wow
            {
                d += (float) (0.0003 * fs * std::sin (2.0 * MathConstants<double>::pi * lfo));
                lfo += 0.4 / fs;
                if (lfo >= 1) lfo -= 1;
            }
            float rp = (float) writePos - d;
            while (rp < 0) rp += (float) size;
            const int i0 = (int) rp, i1 = (i0 + 1) % size;
            const float fr = rp - (float) i0;
            float tapL = buf[0][(size_t) i0] * (1 - fr) + buf[0][(size_t) i1] * fr;
            float tapR = buf[1][(size_t) i0] * (1 - fr) + buf[1][(size_t) i1] * fr;

            const float inL = l[i], inR = r != nullptr ? r[i] : l[i];
            // filtering in the feedback path
            float fbL = hc[0].lp (lc[0].hp (tapL)) * fb, fbR = hc[1].lp (lc[1].hp (tapR)) * fb;
            if (type == 1) { fbL = std::tanh (fbL); fbR = std::tanh (fbR); }
            if (type == 2) // ping-pong: input enters left, repeats alternate sides
            {
                buf[0][(size_t) writePos] = 0.5f * (inL + inR) + fbR;
                buf[1][(size_t) writePos] = fbL;
            }
            else
            {
                buf[0][(size_t) writePos] = inL + fbL;
                buf[1][(size_t) writePos] = inR + fbR;
            }
            // stereo width of the repeats (mid/side)
            const float mid = 0.5f * (tapL + tapR), side = 0.5f * (tapL - tapR) * width;
            tapL = mid + side;
            tapR = mid - side;
            l[i] = inL + mix * tapL;
            if (r != nullptr) r[i] = inR + mix * tapR;
            if (++writePos >= size) writePos = 0;
        }
    }
};

/** Reverb: an 8-line feedback delay network with input diffusion (room / hall / plate)
    and a dispersive spring model (allpass "chirp" chain in a feedback loop). */
class Reverb : public Module
{
    std::vector<const char*> paramNames() const override { return { "type", "decay", "predelay", "mix", "size", "damping", "width" }; }

    struct Line { std::vector<float> d; int pos = 0; float read (int len) const noexcept { int i = pos - len; if (i < 0) i += (int) d.size(); return d[(size_t) i]; } void write (float v) noexcept { d[(size_t) pos] = v; if (++pos >= (int) d.size()) pos = 0; } };
    struct Allpass { Line line; int len = 1; float g = 0.6f; float process (float x) noexcept { const float z = line.read (len); const float v = x + g * z; line.write (v); return z - g * v; } };

    std::array<Line, 8> lines;
    std::array<int, 8> lens {};
    std::array<float, 8> damp {};
    std::array<Allpass, 4> diffuse;
    std::array<std::array<Allpass, 12>, 2> spring; // dispersion chains
    std::array<Line, 2> springLoop;
    std::array<float, 2> springLp {};
    Line pre[2];
    double modPhase = 0;

public:
    void prepare (double sr, int) override
    {
        fs = sr;
        const double s = sr / 48000.0;
        for (auto& l : lines) l.d.assign ((size_t) (0.25 * sr) + 8, 0.0f);
        for (auto& a : diffuse) a.line.d.assign ((size_t) (0.02 * sr) + 8, 0.0f);
        for (auto& ch : spring) for (auto& a : ch) { a.line.d.assign (8, 0.0f); a.len = 1; a.g = -0.6f; }
        for (auto& l : springLoop) l.d.assign ((size_t) (0.1 * sr) + 8, 0.0f);
        for (auto& l : pre) l.d.assign ((size_t) (0.21 * sr) + 8, 0.0f);
        const int dl[4] = { 142, 107, 379, 277 };
        for (int i = 0; i < 4; ++i) diffuse[(size_t) i].len = (int) (dl[i] * s);
        reset();
    }
    void reset() override
    {
        for (auto& l : lines) std::fill (l.d.begin(), l.d.end(), 0.0f);
        for (auto& a : diffuse) std::fill (a.line.d.begin(), a.line.d.end(), 0.0f);
        for (auto& l : springLoop) std::fill (l.d.begin(), l.d.end(), 0.0f);
        for (auto& ch : spring) for (auto& a : ch) std::fill (a.line.d.begin(), a.line.d.end(), 0.0f);
        for (auto& l : pre) std::fill (l.d.begin(), l.d.end(), 0.0f);
        damp = {};
        springLp = {};
    }

    void process (float* l, float* r, int n) noexcept override
    {
        const int type = (int) p (0); // 0 room, 1 hall, 2 plate, 3 spring
        const float decay = std::max (0.1f, p (1)), mix = p (3), size = p (4), damping = p (5), width = p (6);
        const int preLen = std::max (1, (int) (p (2) * 0.001f * (float) fs));

        // delay lengths per type (prime-ish, in ms at 48 kHz), scaled by size
        static const float roomMs[8] = { 11.3f, 13.7f, 17.1f, 19.9f, 23.3f, 27.1f, 29.7f, 31.9f };
        static const float hallMs[8] = { 37.1f, 41.3f, 47.9f, 53.3f, 61.1f, 67.7f, 73.1f, 79.3f };
        static const float plateMs[8] = { 19.3f, 23.9f, 29.3f, 31.7f, 37.9f, 41.1f, 43.7f, 47.3f };
        const float* ms = type == 0 ? roomMs : (type == 1 ? hallMs : plateMs);
        const float scale = 0.5f + size;
        float g[8];
        for (int i = 0; i < 8; ++i)
        {
            lens[(size_t) i] = std::min ((int) lines[(size_t) i].d.size() - 2, (int) (ms[i] * scale * 0.001f * (float) fs));
            g[i] = std::pow (10.0f, -3.0f * (float) lens[(size_t) i] / (decay * (float) fs)); // T60
        }
        const float dampCoef = type == 2 ? 0.05f + 0.4f * damping : 0.1f + 0.7f * damping;
        for (auto& a : diffuse) a.g = type == 2 ? 0.75f : 0.6f;

        // spring parameters
        const int loopLen = (int) ((25.0f + 40.0f * size) * 0.001f * (float) fs);
        const float springG = std::pow (10.0f, -3.0f * (float) loopLen / (decay * (float) fs));
        for (auto& ch : spring) for (auto& a : ch) a.g = -0.55f - 0.1f * size;

        for (int i = 0; i < n; ++i)
        {
            const float inL = l[i], inR = r != nullptr ? r[i] : l[i];
            pre[0].write (0.5f * (inL + inR));
            const float x = pre[0].read (preLen);
            float wetL, wetR;

            if (type == 3)
            {
                float out[2];
                for (int c = 0; c < 2; ++c)
                {
                    float v = x + springG * springLoop[(size_t) c].read (loopLen + c * 37);
                    for (auto& a : spring[(size_t) c]) v = a.process (v); // dispersion: the "boing" chirp
                    springLp[(size_t) c] += (1.0f - (0.3f + 0.5f * damping)) * (v - springLp[(size_t) c]); // ~4 kHz band limit
                    springLoop[(size_t) c].write (springLp[(size_t) c]);
                    out[c] = springLp[(size_t) c];
                }
                wetL = out[0];
                wetR = out[1];
            }
            else
            {
                float v = x;
                for (auto& a : diffuse) v = a.process (v);
                // read, damp, mix (Hadamard), write
                float o[8];
                for (int k = 0; k < 8; ++k)
                {
                    int len = lens[(size_t) k];
                    if (type == 1 && k < 2) // hall: slow modulation of two lines to avoid metallic ringing
                        len += (int) (8.0 * std::sin (2.0 * MathConstants<double>::pi * (modPhase + k * 0.5)));
                    float y = lines[(size_t) k].read (len);
                    damp[(size_t) k] += (1.0f - dampCoef) * (y - damp[(size_t) k]);
                    o[k] = damp[(size_t) k] * g[k];
                }
                float h[8];
                for (int k = 0; k < 8; ++k) h[k] = o[k];
                for (int len = 1; len < 8; len <<= 1)
                    for (int a = 0; a < 8; a += len << 1)
                        for (int b = a; b < a + len; ++b)
                        {
                            const float u = h[b], w = h[b + len];
                            h[b] = u + w;
                            h[b + len] = u - w;
                        }
                for (int k = 0; k < 8; ++k)
                    lines[(size_t) k].write (h[k] * 0.35355339f + (k % 2 == 0 ? v : -v) * 0.5f);
                wetL = o[0] + o[2] + o[4] + o[6];
                wetR = o[1] + o[3] + o[5] + o[7];
                wetL *= 0.5f;
                wetR *= 0.5f;
            }
            modPhase += 0.5 / fs;
            if (modPhase >= 1) modPhase -= 1;

            const float mid = 0.5f * (wetL + wetR), side = 0.5f * (wetL - wetR) * width;
            l[i] = inL * (1 - mix * 0.5f) + mix * (mid + side);
            if (r != nullptr) r[i] = inR * (1 - mix * 0.5f) + mix * (mid - side);
        }
    }
};
} // namespace

// ===========================================================================
void Module::bind (AudioProcessorValueTreeState& apvts, const String& id)
{
    params.clear();
    for (auto* n : paramNames())
    {
        auto* a = apvts.getRawParameterValue (id + "_" + n);
        jassert (a != nullptr);
        params.push_back (a);
    }
    onParam = apvts.getRawParameterValue (id + "_on");
}

const std::vector<ModuleInfo>& allModules()
{
    static const std::vector<ModuleInfo> m {
        { "gate", "Noise Gate", true }, { "comp", "Compressor", true }, { "od", "Overdrive", true },
        { "boost", "Clean Boost", true }, { "dist", "Distortion", true }, { "fuzz", "Fuzz", true },
        { "eq", "Parametric EQ", false }, { "mod", "Modulation", false }, { "delay", "Delay", false },
        { "reverb", "Reverb", false },
    };
    return m;
}

int moduleIndex (const String& id)
{
    const auto& m = allModules();
    for (size_t i = 0; i < m.size(); ++i)
        if (id == m[i].id)
            return (int) i;
    return -1;
}

std::unique_ptr<Module> createModule (const String& id)
{
    if (id == "gate")   return std::make_unique<NoiseGate>();
    if (id == "comp")   return std::make_unique<Compressor>();
    if (id == "od")     return std::make_unique<Overdrive>();
    if (id == "boost")  return std::make_unique<CleanBoost>();
    if (id == "dist")   return std::make_unique<Distortion>();
    if (id == "fuzz")   return std::make_unique<Fuzz>();
    if (id == "eq")     return std::make_unique<ParametricEq>();
    if (id == "mod")    return std::make_unique<Modulation>();
    if (id == "delay")  return std::make_unique<Delay>();
    if (id == "reverb") return std::make_unique<Reverb>();
    return nullptr;
}

std::vector<float> eqResponseDb (AudioProcessorValueTreeState& apvts, const std::vector<float>& freqs, double rate)
{
    auto v = [&apvts] (const char* n) { return apvts.getRawParameterValue (String ("eq_") + n)->load(); };
    std::vector<EqBiquad> f (8);
    v ("hpf") <= 20.5f ? f[0].bypass() : f[0].set (Biquad::Type::HighPass, v ("hpf"), rate);
    v ("lpf") >= 19999.0f ? f[1].bypass() : f[1].set (Biquad::Type::LowPass, v ("lpf"), rate);
    f[2].shelf (false, v ("lowFreq"), v ("lowGain"), rate);
    for (int b = 0; b < 4; ++b)
    {
        const String id = "b" + String (b + 1);
        f[(size_t) (3 + b)].peak (v ((id + "Freq").toRawUTF8()), std::max (0.1f, v ((id + "Q").toRawUTF8())), v ((id + "Gain").toRawUTF8()), rate);
    }
    f[7].shelf (true, v ("highFreq"), v ("highGain"), rate);

    std::vector<float> out;
    for (float hz : freqs)
    {
        const double w = 2.0 * MathConstants<double>::pi * hz / rate;
        const std::complex<double> z1 = std::polar (1.0, -w), z2 = z1 * z1;
        double db = 0;
        for (auto& b : f)
            db += 20.0 * std::log10 (std::abs ((b.b0 + b.b1 * z1 + b.b2 * z2) / (1.0 + b.a1 * z1 + b.a2 * z2)) + 1e-12);
        out.push_back ((float) db);
    }
    return out;
}

std::vector<String> parameterNames (const String& id)
{
    auto m = createModule (id);
    std::vector<String> names;
    for (auto* n : m->paramNames())
        names.push_back (n);
    return names;
}

void addParameters (AudioProcessorValueTreeState::ParameterLayout& layout)
{
    auto unit = [] (const String& u, int decimals)
    {
        return AudioParameterFloatAttributes().withLabel (u).withStringFromValueFunction (
            [u, decimals] (float v, int) { return String (v, decimals) + (u.isEmpty() ? "" : " " + u); });
    };
    auto hz = AudioParameterFloatAttributes().withLabel ("Hz").withStringFromValueFunction (
        [] (float v, int) { return v >= 1000.0f ? String (v / 1000.0f, 2) + " kHz" : String ((int) std::lround (v)) + " Hz"; });
    auto pct = AudioParameterFloatAttributes().withStringFromValueFunction ([] (float v, int) { return String ((int) std::lround (v * 100)) + "%"; });
    auto add = [&layout] (const String& id, const String& name, NormalisableRange<float> r, float def, AudioParameterFloatAttributes a)
    { layout.add (std::make_unique<AudioParameterFloat> (ParameterID { id, 1 }, name, r, def, a)); };
    auto on = [&layout] (const String& id, const String& name, bool def)
    { layout.add (std::make_unique<AudioParameterBool> (ParameterID { id + "_on", 1 }, name + " On", def)); };
    auto choice = [&layout] (const String& id, const String& name, StringArray items, int def)
    { layout.add (std::make_unique<AudioParameterChoice> (ParameterID { id, 1 }, name, items, def)); };
    const auto skewed = [] (float lo, float hi, float centre) { NormalisableRange<float> r (lo, hi); r.setSkewForCentre (centre); return r; };

    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "stomps_on", 1 }, "Stomps Section On", true));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "fx_on", 1 }, "Effects Section On", true));

    on ("gate", "Gate", false);
    add ("gate_threshold", "Gate Threshold", { -90.0f, 0.0f, 0.1f }, -60.0f, unit ("dB", 1));
    add ("gate_release", "Gate Release", skewed (10.0f, 500.0f, 80.0f), 100.0f, unit ("ms", 0));

    on ("comp", "Compressor", false);
    add ("comp_threshold", "Comp Threshold", { -60.0f, 0.0f, 0.1f }, -20.0f, unit ("dB", 1));
    add ("comp_ratio", "Comp Ratio", skewed (1.0f, 20.0f, 4.0f), 4.0f, unit (": 1", 1));
    add ("comp_attack", "Comp Attack", skewed (0.1f, 100.0f, 10.0f), 5.0f, unit ("ms", 1));
    add ("comp_release", "Comp Release", skewed (10.0f, 1000.0f, 150.0f), 120.0f, unit ("ms", 0));
    add ("comp_makeup", "Comp Makeup", { 0.0f, 24.0f, 0.1f }, 0.0f, unit ("dB", 1));
    add ("comp_mix", "Comp Mix", { 0.0f, 100.0f, 1.0f }, 100.0f, unit ("%", 0));

    on ("od", "Overdrive", false);
    add ("od_drive", "Overdrive Drive", { 0.0f, 1.0f }, 0.5f, pct);
    add ("od_tone", "Overdrive Tone", { 0.0f, 1.0f }, 0.5f, pct);
    add ("od_level", "Overdrive Level", { -24.0f, 12.0f, 0.1f }, 0.0f, unit ("dB", 1));

    on ("boost", "Clean Boost", false);
    add ("boost_gain", "Boost Gain", { 0.0f, 24.0f, 0.1f }, 6.0f, unit ("dB", 1));
    add ("boost_output", "Boost Output", { -24.0f, 12.0f, 0.1f }, 0.0f, unit ("dB", 1));

    on ("dist", "Distortion", false);
    add ("dist_drive", "Distortion Drive", { 0.0f, 1.0f }, 0.5f, pct);
    add ("dist_tone", "Distortion Tone", { 0.0f, 1.0f }, 0.5f, pct);
    add ("dist_output", "Distortion Output", { -24.0f, 12.0f, 0.1f }, -6.0f, unit ("dB", 1));

    on ("fuzz", "Fuzz", false);
    add ("fuzz_fuzz", "Fuzz Amount", { 0.0f, 1.0f }, 0.7f, pct);
    add ("fuzz_tone", "Fuzz Tone", { 0.0f, 1.0f }, 0.5f, pct);
    add ("fuzz_output", "Fuzz Output", { -24.0f, 12.0f, 0.1f }, -6.0f, unit ("dB", 1));

    on ("eq", "EQ", false);
    add ("eq_hpf", "EQ High-pass", skewed (20.0f, 500.0f, 80.0f), 20.0f, hz);
    add ("eq_lpf", "EQ Low-pass", skewed (2000.0f, 20000.0f, 8000.0f), 20000.0f, hz);
    add ("eq_lowFreq", "EQ Low Shelf Freq", skewed (40.0f, 400.0f, 120.0f), 100.0f, hz);
    add ("eq_lowGain", "EQ Low Shelf Gain", { -15.0f, 15.0f, 0.1f }, 0.0f, unit ("dB", 1));
    const float bandFreq[4] = { 250.0f, 800.0f, 2000.0f, 5000.0f };
    for (int b = 1; b <= 4; ++b)
    {
        const String id = "eq_b" + String (b);
        add (id + "Freq", "EQ Band " + String (b) + " Freq", skewed (40.0f, 16000.0f, 1000.0f), bandFreq[b - 1], hz);
        add (id + "Gain", "EQ Band " + String (b) + " Gain", { -15.0f, 15.0f, 0.1f }, 0.0f, unit ("dB", 1));
        add (id + "Q", "EQ Band " + String (b) + " Q", skewed (0.2f, 10.0f, 1.0f), 1.0f, unit ("", 2));
    }
    add ("eq_highFreq", "EQ High Shelf Freq", skewed (2000.0f, 12000.0f, 5000.0f), 6000.0f, hz);
    add ("eq_highGain", "EQ High Shelf Gain", { -15.0f, 15.0f, 0.1f }, 0.0f, unit ("dB", 1));

    on ("mod", "Modulation", false);
    choice ("mod_type", "Modulation Type", { "Chorus", "Phaser", "Flanger" }, 0);
    add ("mod_rate", "Modulation Rate", skewed (0.05f, 10.0f, 1.0f), 0.8f, unit ("Hz", 2));
    add ("mod_depth", "Modulation Depth", { 0.0f, 1.0f }, 0.5f, pct);
    add ("mod_feedback", "Modulation Feedback", { -0.95f, 0.95f, 0.01f }, 0.3f, pct);
    add ("mod_mix", "Modulation Mix", { 0.0f, 1.0f }, 0.5f, pct);

    on ("delay", "Delay", false);
    choice ("delay_type", "Delay Type", { "Digital", "Analog", "Ping-pong" }, 0);
    add ("delay_time", "Delay Time", skewed (1.0f, 2000.0f, 350.0f), 380.0f, unit ("ms", 0));
    choice ("delay_sync", "Delay Sync", { "Off", "1/1", "1/2", "1/4", "1/4 dotted", "1/8", "1/8 dotted", "1/8 triplet", "1/16" }, 0);
    add ("delay_feedback", "Delay Feedback", { 0.0f, 0.95f }, 0.35f, pct);
    add ("delay_mix", "Delay Mix", { 0.0f, 1.0f }, 0.3f, pct);
    add ("delay_lowcut", "Delay Low Cut", skewed (20.0f, 1000.0f, 150.0f), 80.0f, hz);
    add ("delay_highcut", "Delay High Cut", skewed (1000.0f, 20000.0f, 5000.0f), 6000.0f, hz);
    add ("delay_width", "Delay Width", { 0.0f, 1.0f }, 0.5f, pct);

    on ("reverb", "Reverb", false);
    choice ("reverb_type", "Reverb Type", { "Room", "Hall", "Plate", "Spring" }, 0);
    add ("reverb_decay", "Reverb Decay", skewed (0.2f, 10.0f, 2.0f), 1.5f, unit ("s", 2));
    add ("reverb_predelay", "Reverb Pre-delay", { 0.0f, 200.0f, 0.1f }, 10.0f, unit ("ms", 1));
    add ("reverb_mix", "Reverb Mix", { 0.0f, 1.0f }, 0.25f, pct);
    add ("reverb_size", "Reverb Size", { 0.0f, 1.0f }, 0.5f, pct);
    add ("reverb_damping", "Reverb Damping", { 0.0f, 1.0f }, 0.5f, pct);
    add ("reverb_width", "Reverb Width", { 0.0f, 1.0f }, 0.8f, pct);
}

// ===========================================================================
namespace
{
std::uint64_t pack (const std::vector<int>& idx)
{
    std::uint64_t v = 0;
    for (size_t i = 0; i < idx.size(); ++i)
        v |= (std::uint64_t) (idx[i] + 1) << (8 * i);
    return v;
}
} // namespace

Chain::Chain()
{
    for (const auto& m : allModules())
    {
        modules.push_back (createModule (m.id));
        modules.back()->tempo = &bpm;
    }
    setOrder (true, defaultOrder (true));
    setOrder (false, defaultOrder (false));
}

void Chain::bind (AudioProcessorValueTreeState& apvts)
{
    const auto& info = allModules();
    for (size_t i = 0; i < modules.size(); ++i)
        modules[i]->bind (apvts, info[i].id);
    stompsOn = apvts.getRawParameterValue ("stomps_on");
    fxOn = apvts.getRawParameterValue ("fx_on");
}

void Chain::prepare (double sampleRate, int maxBlock)
{
    for (auto& m : modules)
        m->prepare (sampleRate, maxBlock);
}

String Chain::defaultOrder (bool pre)
{
    StringArray ids;
    for (const auto& m : allModules())
        if (m.preAmp == pre)
            ids.add (m.id);
    return ids.joinIntoString (",");
}

String Chain::getOrder (bool pre) const
{
    StringArray ids;
    auto v = (pre ? preOrder : postOrder).load();
    for (int i = 0; i < 8 && (v & 0xff) != 0; ++i, v >>= 8)
        ids.add (allModules()[(size_t) ((v & 0xff) - 1)].id);
    return ids.joinIntoString (",");
}

bool Chain::setOrder (bool pre, const String& ids)
{
    StringArray list;
    list.addTokens (ids, ",", {});
    list.trim();
    list.removeEmptyStrings();
    StringArray expected;
    expected.addTokens (defaultOrder (pre), ",", {});
    if (list.size() != expected.size())
        return false;
    std::vector<int> idx;
    for (const auto& id : list)
    {
        const int i = moduleIndex (id);
        if (i < 0 || ! expected.contains (id) || std::find (idx.begin(), idx.end(), i) != idx.end())
            return false;
        idx.push_back (i);
    }
    (pre ? preOrder : postOrder).store (pack (idx));
    return true;
}

void Chain::processPre (float* mono, int n) noexcept
{
    if (stompsOn != nullptr && stompsOn->load() < 0.5f)
        return;
    for (auto v = preOrder.load(); (v & 0xff) != 0; v >>= 8)
    {
        auto& m = modules[(size_t) ((v & 0xff) - 1)];
        if (m->enabled())
            m->process (mono, nullptr, n);
    }
}

void Chain::processPost (float* l, float* r, int n) noexcept
{
    const bool on = fxOn == nullptr || fxOn->load() > 0.5f;
    for (auto v = postOrder.load(); (v & 0xff) != 0; v >>= 8)
    {
        const int index = (int) (v & 0xff) - 1;
        auto& m = modules[(size_t) index];
        if (on && m->enabled())
            m->process (l, r, n);
        if (std::string_view (allModules()[(size_t) index].id) == "eq")
            analyser.push (l, r, n);
    }
}

void AnalyserTap::push (const float* l, const float* r, int n) noexcept
{
    int s1, n1, s2, n2;
    fifo.prepareToWrite (n, s1, n1, s2, n2);
    auto put = [&] (int start, int count, int offset)
    {
        for (int i = 0; i < count; ++i)
            buffer[(size_t) (start + i)] = r != nullptr ? 0.5f * (l[offset + i] + r[offset + i]) : l[offset + i];
    };
    put (s1, n1, 0);
    put (s2, n2, n1);
    fifo.finishedWrite (n1 + n2); // drops samples if the UI isn't reading (bounded, never blocks)
}

int AnalyserTap::pull (float* dest, int maxSamples) noexcept
{
    int s1, n1, s2, n2;
    fifo.prepareToRead (maxSamples, s1, n1, s2, n2);
    std::copy (buffer.begin() + s1, buffer.begin() + s1 + n1, dest);
    std::copy (buffer.begin() + s2, buffer.begin() + s2 + n2, dest + n1);
    fifo.finishedRead (n1 + n2);
    return n1 + n2;
}
} // namespace fx
