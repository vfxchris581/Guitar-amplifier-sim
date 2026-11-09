#include "train/Trainer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <thread>

namespace gas::train
{

double esr (const float* p, const float* t, size_t n)
{
    double num = 0.0, den = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        const double e = (double) p[i] - t[i];
        num += e * e;
        den += (double) t[i] * t[i];
    }
    return den > 0.0 ? num / den : std::numeric_limits<double>::infinity();
}

std::vector<float> runModel (const WaveNet& net, const std::vector<float>& signal)
{
    const int R = net.spec().receptiveField();
    if ((int) signal.size() < R)
        return {};

    const size_t outputs = signal.size() - (size_t) R + 1;
    std::vector<float> y (outputs);
    Workspace ws;
    constexpr size_t chunk = 16384;
    for (size_t start = 0; start < outputs; start += chunk)
    {
        const size_t n = std::min (chunk, outputs - start);
        net.forward (signal.data() + start, (int) (n + (size_t) R - 1), y.data() + start, ws);
    }
    return y;
}

namespace
{
struct Adam
{
    explicit Adam (size_t n) : m (n, 0.0f), v (n, 0.0f) {}

    void step (std::vector<float>& p, const std::vector<float>& g, float lr)
    {
        ++t;
        const double bc1 = 1.0 - std::pow (beta1, t);
        const double bc2 = 1.0 - std::pow (beta2, t);
        const float stepSize = (float) (lr / bc1);
        const float sqrtBc2 = (float) std::sqrt (bc2);
        for (size_t i = 0; i < p.size(); ++i)
        {
            m[i] = beta1 * m[i] + (1.0f - beta1) * g[i];
            v[i] = beta2 * v[i] + (1.0f - beta2) * g[i] * g[i];
            p[i] -= stepSize * m[i] / (std::sqrt (v[i]) / sqrtBc2 + eps); // PyTorch's formulation
        }
    }

    static constexpr float beta1 = 0.9f, beta2 = 0.999f, eps = 1e-8f;
    std::vector<float> m, v;
    int t = 0;
};
} // namespace

TrainResult trainWaveNet (const std::vector<float>& xTrain, const std::vector<float>& yTrain,
                          const std::vector<float>& xVal, const std::vector<float>& yVal, const TrainOptions& opt,
                          const std::function<void (const TrainProgress&)>& onProgress,
                          const std::atomic<bool>& cancel)
{
    TrainResult result;
    result.spec = WaveNetSpec::fromPreset (opt.preset);

    WaveNet net (result.spec);
    net.initialise (opt.seed);
    const int R = result.spec.receptiveField();
    const int ny = std::max (1, opt.outputSamplesPerItem);

    if (xTrain.size() != yTrain.size() || xVal.size() != yVal.size())
    {
        result.error = "Input and output recordings have different lengths.";
        return result;
    }
    if ((int) xVal.size() < R + 1)
    {
        result.error = "The validation section is shorter than the model's receptive field.";
        return result;
    }

    const size_t items = xTrain.size() >= (size_t) R ? (xTrain.size() - (size_t) R + 1) / (size_t) ny : 0;
    const int batch = std::max (1, opt.batchSize);
    const int stepsPerEpoch = (int) (items / (size_t) batch);
    if (stepsPerEpoch < 1)
    {
        result.error = "Not enough training audio for one batch.";
        return result;
    }

    const int threads = std::max (1, std::min (batch, opt.threads > 0 ? opt.threads
                                                                      : (int) std::max (1u, std::thread::hardware_concurrency())));
    std::vector<Workspace> workspaces ((size_t) threads);
    std::vector<std::vector<float>> grads ((size_t) threads, std::vector<float> (net.params().size()));
    std::vector<std::vector<float>> preds ((size_t) threads, std::vector<float> ((size_t) ny));
    std::vector<std::vector<float>> dys ((size_t) threads, std::vector<float> ((size_t) ny));
    std::vector<double> losses ((size_t) threads);
    std::vector<float> grad (net.params().size());

    std::vector<size_t> order (items);
    std::iota (order.begin(), order.end(), 0);
    std::mt19937 rng (opt.seed);

    Adam adam (net.params().size());
    float lr = opt.learningRate;
    const float gamma = 1.0f - opt.learningRateDecay;

    const std::vector<float> valTarget (yVal.begin() + (R - 1), yVal.end());

    TrainProgress progress;
    progress.epochs = opt.epochs;
    progress.stepsPerEpoch = stepsPerEpoch;
    const auto started = std::chrono::steady_clock::now();
    const int totalSteps = opt.epochs * stepsPerEpoch;

    double best = std::numeric_limits<double>::infinity();

    for (int epoch = 1; epoch <= opt.epochs; ++epoch)
    {
        std::shuffle (order.begin(), order.end(), rng);
        double epochLoss = 0.0;

        for (int step = 0; step < stepsPerEpoch; ++step)
        {
            if (cancel.load())
            {
                result.cancelled = true;
                result.epochsRun = epoch - 1;
                result.bestValidationEsr = best;
                if (! result.bestParams.empty())
                    result.success = false;
                return result;
            }

            const size_t* batchItems = order.data() + (size_t) step * batch;
            const float lossScale = 2.0f / (float) ((size_t) batch * ny);

            auto work = [&] (int ti)
            {
                auto& g = grads[(size_t) ti];
                std::fill (g.begin(), g.end(), 0.0f);
                losses[(size_t) ti] = 0.0;
                for (int b = ti; b < batch; b += threads)
                {
                    const size_t start = batchItems[b] * (size_t) ny;
                    const float* x = xTrain.data() + start;
                    const float* target = yTrain.data() + start + (size_t) R - 1;
                    auto& p = preds[(size_t) ti];
                    auto& dy = dys[(size_t) ti];
                    net.forward (x, R + ny - 1, p.data(), workspaces[(size_t) ti]);
                    for (int i = 0; i < ny; ++i)
                    {
                        const float e = p[(size_t) i] - target[i];
                        losses[(size_t) ti] += (double) e * e;
                        dy[(size_t) i] = lossScale * e;
                    }
                    net.backward (dy.data(), workspaces[(size_t) ti], g);
                }
            };

            if (threads == 1)
                work (0);
            else
            {
                std::vector<std::thread> pool;
                pool.reserve ((size_t) threads - 1);
                for (int ti = 1; ti < threads; ++ti)
                    pool.emplace_back (work, ti);
                work (0);
                for (auto& th : pool)
                    th.join();
            }

            std::fill (grad.begin(), grad.end(), 0.0f);
            double loss = 0.0;
            for (int ti = 0; ti < threads; ++ti)
            {
                for (size_t i = 0; i < grad.size(); ++i)
                    grad[i] += grads[(size_t) ti][i];
                loss += losses[(size_t) ti];
            }
            loss /= (double) ((size_t) batch * ny);

            adam.step (net.params(), grad, lr);
            net.unpack();

            epochLoss += loss;
            progress.epoch = epoch;
            progress.step = step + 1;
            progress.trainLoss = epochLoss / (step + 1);
            const double elapsed = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
            const int done = (epoch - 1) * stepsPerEpoch + step + 1;
            progress.secondsElapsed = elapsed;
            progress.secondsRemaining = elapsed / done * (totalSteps - done);
            if (onProgress)
                onProgress (progress);
        }

        // Validation (also NAM's checkpoint criterion)
        const auto pred = runModel (net, xVal);
        const double valEsr = esr (pred.data(), valTarget.data(), valTarget.size());
        if (! std::isfinite (valEsr))
        {
            result.error = "Training diverged (validation error is not finite).";
            result.epochsRun = epoch;
            return result;
        }
        if (valEsr < best)
        {
            best = valEsr;
            result.bestParams = net.params();
        }

        progress.validationEsr = valEsr;
        progress.bestValidationEsr = best;
        progress.epochTrainLoss.push_back (epochLoss / stepsPerEpoch);
        progress.epochValidationEsr.push_back (valEsr);
        if (onProgress)
            onProgress (progress);

        lr *= gamma;
        result.epochsRun = epoch;

        if (opt.stopAtValidationEsr && best <= *opt.stopAtValidationEsr)
            break;
    }

    result.bestValidationEsr = best;
    result.success = ! result.bestParams.empty();
    return result;
}

// ---------------------------------------------------------------------------
double measureLoudnessDb (const WaveNet& net, const std::vector<float>& stimulus)
{
    // The trainer runs the model with zero padding at the start so the output has
    // one sample per stimulus sample.
    const int R = net.spec().receptiveField();
    std::vector<float> padded ((size_t) R - 1, 0.0f);
    padded.insert (padded.end(), stimulus.begin(), stimulus.end());
    const auto out = runModel (net, padded);

    // ITU-R BS.1770-4 K-weighting at 48 kHz (second-order sections, scipy sosfilt / DF-II transposed)
    static constexpr double sos[2][6] = {
        { 1.53512485958697, -2.69169618940638, 1.19839281085285, 1.0, -1.69065929318241, 0.73248077421585 },
        { 1.0, -2.0, 1.0, 1.0, -1.99004745483398, 0.99007225036621 },
    };
    std::vector<double> y (out.begin(), out.end());
    for (const auto& s : sos)
    {
        double z1 = 0.0, z2 = 0.0;
        for (auto& v : y)
        {
            const double x = v;
            const double o = s[0] * x + z1;
            z1 = s[1] * x - s[4] * o + z2;
            z2 = s[2] * x - s[5] * o;
            v = o;
        }
    }

    constexpr int numBursts = 7;
    constexpr int edgeTrim = (int) (0.030 * 48000);
    constexpr double calibrationDb = 3.0;
    const long n = (long) stimulus.size();
    const long burstLen = n / numBursts;
    const long trim = std::min<long> (edgeTrim, burstLen / 4);
    const long offset = (long) y.size() - n;

    double sumDb = 0.0;
    for (int b = 0; b < numBursts; ++b)
    {
        const long start = std::max<long> (b * burstLen + trim + offset, 0);
        const long stop = std::max<long> ((b + 1) * burstLen - trim + offset, 0);
        double ms = 0.0;
        for (long i = start; i < stop; ++i)
            ms += y[(size_t) i] * y[(size_t) i];
        ms = stop > start ? ms / (double) (stop - start) : 0.0;
        if (ms <= 0.0)
            return -std::numeric_limits<double>::infinity();
        sumDb += -0.691 + 10.0 * std::log10 (ms);
    }
    return sumDb / numBursts + calibrationDb;
}

} // namespace gas::train
