#include "train/WaveNet.h"

#include "json.hpp"

#include <cmath>
#include <random>
#include <stdexcept>

namespace gas::train
{
using Eigen::MatrixXf;

const char* presetName (Preset p)
{
    switch (p)
    {
        case Preset::Standard: return "Standard";
        case Preset::Lite:     return "Lite";
        case Preset::Feather:  return "Feather";
        case Preset::Nano:     return "Nano";
    }
    return "Standard";
}

// The four architectures of NAM's classic trainer (neural-amp-modeler v0.10.0,
// nam/train/core.py: get_wavenet_config).
WaveNetSpec WaveNetSpec::fromPreset (Preset p)
{
    const std::vector<int> full { 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };
    const std::vector<int> first7 { 1, 2, 4, 8, 16, 32, 64 };
    const std::vector<int> second13 { 128, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 };

    auto make = [] (int c1, int c2, const std::vector<int>& d1, const std::vector<int>& d2)
    {
        WaveNetSpec s;
        s.arrays.push_back ({ 1, 1, c1, c2, 3, d1, false });
        s.arrays.push_back ({ c1, 1, c2, 1, 3, d2, true });
        s.headScale = 0.02f;
        return s;
    };

    switch (p)
    {
        case Preset::Standard: return make (16, 8, full, full);
        case Preset::Lite:     return make (12, 6, first7, second13);
        case Preset::Feather:  return make (8, 4, first7, second13);
        case Preset::Nano:     return make (4, 2, first7, second13);
    }
    return make (16, 8, full, full);
}

int WaveNetSpec::receptiveField() const
{
    int r = 1;
    for (const auto& a : arrays)
        for (int d : a.dilations)
            r += (a.kernelSize - 1) * d;
    return r;
}

size_t WaveNetSpec::numParams() const
{
    size_t n = 0;
    for (const auto& a : arrays)
    {
        n += (size_t) a.channels * a.inputSize; // rechannel (no bias)
        for (size_t l = 0; l < a.dilations.size(); ++l)
        {
            n += (size_t) a.channels * a.channels * a.kernelSize + a.channels; // conv + bias
            n += (size_t) a.channels * a.conditionSize;                       // input mixin
            n += (size_t) a.channels * a.channels + a.channels;               // 1x1 + bias
        }
        n += (size_t) a.headSize * a.channels + (a.headBias ? a.headSize : 0);
    }
    return n;
}

std::string WaveNetSpec::configJson() const
{
    nlohmann::json layers = nlohmann::json::array();
    for (const auto& a : arrays)
        layers.push_back ({ { "input_size", a.inputSize },
                            { "condition_size", a.conditionSize },
                            { "head_size", a.headSize },
                            { "channels", a.channels },
                            { "kernel_size", a.kernelSize },
                            { "dilations", a.dilations },
                            { "activation", "Tanh" },
                            { "gated", false },
                            { "head_bias", a.headBias } });
    nlohmann::json cfg { { "layers", layers }, { "head", nullptr }, { "head_scale", headScale } };
    return cfg.dump();
}

// ---------------------------------------------------------------------------
WaveNet::WaveNet (WaveNetSpec s) : netSpec (std::move (s))
{
    if (netSpec.arrays.empty())
        throw std::invalid_argument ("WaveNet needs at least one layer array");

    size_t off = 0;
    auto dense = [&off] (int cin, int cout, bool bias)
    {
        DenseParams d;
        d.offset = off;
        d.cin = cin;
        d.cout = cout;
        d.hasBias = bias;
        off += (size_t) cin * cout + (bias ? cout : 0);
        return d;
    };

    for (size_t ai = 0; ai < netSpec.arrays.size(); ++ai)
    {
        const auto& a = netSpec.arrays[ai];
        if (ai > 0 && netSpec.arrays[ai - 1].headSize != a.channels)
            throw std::invalid_argument ("head_size of an array must equal channels of the next array");

        ArrayParams ap;
        ap.rechannel = dense (a.inputSize, a.channels, false);
        for (int d : a.dilations)
        {
            LayerParams lp;
            lp.conv.offset = off;
            lp.conv.cin = a.channels;
            lp.conv.cout = a.channels;
            lp.conv.k = a.kernelSize;
            lp.conv.dilation = d;
            off += (size_t) a.channels * a.channels * a.kernelSize + a.channels;
            lp.mixin = dense (a.conditionSize, a.channels, false);
            lp.out1x1 = dense (a.channels, a.channels, true);
            ap.layers.push_back (std::move (lp));
        }
        ap.head = dense (a.channels, a.headSize, a.headBias);
        arrays.push_back (std::move (ap));
    }

    if (off != netSpec.numParams())
        throw std::logic_error ("parameter layout mismatch");
    flat.assign (off, 0.0f);
    unpack();
}

void WaveNet::initialise (std::uint32_t seed)
{
    std::mt19937 rng (seed);
    auto fill = [&] (size_t offset, size_t count, int fanIn)
    {
        const float bound = 1.0f / std::sqrt ((float) fanIn);
        std::uniform_real_distribution<float> dist (-bound, bound);
        for (size_t i = 0; i < count; ++i)
            flat[offset + i] = dist (rng);
    };
    auto fillDense = [&] (const DenseParams& d)
    { fill (d.offset, (size_t) d.cin * d.cout + (d.hasBias ? d.cout : 0), d.cin); };

    for (const auto& ap : arrays)
    {
        fillDense (ap.rechannel);
        for (const auto& lp : ap.layers)
        {
            fill (lp.conv.offset, (size_t) lp.conv.cout * lp.conv.cin * lp.conv.k + lp.conv.cout,
                  lp.conv.cin * lp.conv.k);
            fillDense (lp.mixin);
            fillDense (lp.out1x1);
        }
        fillDense (ap.head);
    }
    unpack();
}

void WaveNet::unpackDense (DenseParams& d, const std::vector<float>& p)
{
    d.w.resize (d.cout, d.cin);
    for (int o = 0; o < d.cout; ++o)
        for (int i = 0; i < d.cin; ++i)
            d.w (o, i) = p[d.offset + (size_t) o * d.cin + i];
    d.bias = Eigen::VectorXf::Zero (d.cout);
    if (d.hasBias)
        for (int o = 0; o < d.cout; ++o)
            d.bias (o) = p[d.offset + (size_t) d.cin * d.cout + o];
}

void WaveNet::unpack()
{
    for (auto& ap : arrays)
    {
        unpackDense (ap.rechannel, flat);
        for (auto& lp : ap.layers)
        {
            auto& c = lp.conv;
            c.taps.assign ((size_t) c.k, MatrixXf (c.cout, c.cin));
            for (int o = 0; o < c.cout; ++o)
                for (int i = 0; i < c.cin; ++i)
                    for (int k = 0; k < c.k; ++k)
                        c.taps[(size_t) k] (o, i) = flat[c.offset + ((size_t) o * c.cin + i) * c.k + k];
            c.bias.resize (c.cout);
            for (int o = 0; o < c.cout; ++o)
                c.bias (o) = flat[c.offset + (size_t) c.cout * c.cin * c.k + o];
            unpackDense (lp.mixin, flat);
            unpackDense (lp.out1x1, flat);
        }
        unpackDense (ap.head, flat);
    }
}

// ---------------------------------------------------------------------------
void WaveNet::forward (const float* x, int length, float* y, Workspace& ws) const
{
    const int R = netSpec.receptiveField();
    if (length < R)
        throw std::invalid_argument ("segment shorter than the receptive field");

    ws.length = length;
    ws.condition = Eigen::Map<const Eigen::RowVectorXf> (x, length);
    ws.arrays.resize (arrays.size());

    for (size_t ai = 0; ai < arrays.size(); ++ai)
    {
        const auto& P = arrays[ai];
        auto& A = ws.arrays[ai];
        if (ai == 0)
            A.arrayInput = ws.condition;
        else
            A.arrayInput = ws.arrays[ai - 1].output;

        A.layers.resize (P.layers.size());
        A.layers[0].input.noalias() = P.rechannel.w * A.arrayInput;

        for (size_t l = 0; l < P.layers.size(); ++l)
        {
            const auto& lp = P.layers[l];
            auto& L = A.layers[l];
            const int tin = (int) L.input.cols();
            const int tout = tin - (lp.conv.k - 1) * lp.conv.dilation;

            L.act.noalias() = lp.mixin.w * ws.condition.rightCols (tout);
            L.act.colwise() += lp.conv.bias;
            for (int k = 0; k < lp.conv.k; ++k)
                L.act.noalias() += lp.conv.taps[(size_t) k] * L.input.middleCols (k * lp.conv.dilation, tout);
            L.act = L.act.array().tanh().matrix();

            MatrixXf& next = (l + 1 < P.layers.size()) ? A.layers[l + 1].input : A.output;
            next = L.input.rightCols (tout);
            next.noalias() += lp.out1x1.w * L.act;
            next.colwise() += lp.out1x1.bias;
        }

        const int tf = (int) A.output.cols();
        A.headSum = A.layers[0].act.rightCols (tf);
        for (size_t l = 1; l < A.layers.size(); ++l)
            A.headSum += A.layers[l].act.rightCols (tf);
        if (ai > 0)
            A.headSum += ws.arrays[ai - 1].head.rightCols (tf);

        A.head.noalias() = P.head.w * A.headSum;
        if (P.head.hasBias)
            A.head.colwise() += P.head.bias;
    }

    const auto& last = ws.arrays.back().head;
    const float s = netSpec.headScale;
    for (int t = 0; t < (int) last.cols(); ++t)
        y[t] = s * last (0, t);
}

void WaveNet::accumulateDenseGrad (const DenseParams& d, const MatrixXf& dOut, const MatrixXf& in,
                                   std::vector<float>& grad)
{
    const MatrixXf g = dOut * in.transpose(); // cout x cin
    for (int o = 0; o < d.cout; ++o)
        for (int i = 0; i < d.cin; ++i)
            grad[d.offset + (size_t) o * d.cin + i] += g (o, i);
    if (d.hasBias)
    {
        const Eigen::VectorXf b = dOut.rowwise().sum();
        for (int o = 0; o < d.cout; ++o)
            grad[d.offset + (size_t) d.cin * d.cout + o] += b (o);
    }
}

void WaveNet::backward (const float* dy, Workspace& ws, std::vector<float>& grad) const
{
    const int tfLast = (int) ws.arrays.back().head.cols();
    ws.dHead = Eigen::Map<const Eigen::RowVectorXf> (dy, tfLast) * netSpec.headScale;

    for (int ai = (int) arrays.size() - 1; ai >= 0; --ai)
    {
        const auto& P = arrays[(size_t) ai];
        auto& A = ws.arrays[(size_t) ai];
        const int tf = (int) A.output.cols();

        // head rechannel
        accumulateDenseGrad (P.head, ws.dHead, A.headSum, grad);
        ws.dS.noalias() = P.head.w.transpose() * ws.dHead; // C x tf

        // gradient arriving at this array's layer output from the next array
        if (ai == (int) arrays.size() - 1)
            ws.dXnext = MatrixXf::Zero (A.output.rows(), tf);
        else
            ws.dXnext = ws.dArrayOut;

        for (int l = (int) P.layers.size() - 1; l >= 0; --l)
        {
            const auto& lp = P.layers[(size_t) l];
            const auto& L = A.layers[(size_t) l];
            const int tin = (int) L.input.cols();
            const int tout = (int) L.act.cols();

            ws.dA.noalias() = lp.out1x1.w.transpose() * ws.dXnext;
            ws.dA.rightCols (tf) += ws.dS;
            accumulateDenseGrad (lp.out1x1, ws.dXnext, L.act, grad);

            ws.dZ = (ws.dA.array() * (1.0f - L.act.array().square())).matrix();

            // conv bias + input mixin
            {
                const Eigen::VectorXf b = ws.dZ.rowwise().sum();
                const size_t bOff = lp.conv.offset + (size_t) lp.conv.cout * lp.conv.cin * lp.conv.k;
                for (int o = 0; o < lp.conv.cout; ++o)
                    grad[bOff + o] += b (o);
            }
            accumulateDenseGrad (lp.mixin, ws.dZ, ws.condition.rightCols (tout), grad);

            ws.dX = MatrixXf::Zero (L.input.rows(), tin);
            ws.dX.rightCols (tout) += ws.dXnext; // residual
            for (int k = 0; k < lp.conv.k; ++k)
            {
                const int off = k * lp.conv.dilation;
                const MatrixXf g = ws.dZ * L.input.middleCols (off, tout).transpose();
                for (int o = 0; o < lp.conv.cout; ++o)
                    for (int i = 0; i < lp.conv.cin; ++i)
                        grad[lp.conv.offset + ((size_t) o * lp.conv.cin + i) * lp.conv.k + k] += g (o, i);
                ws.dX.middleCols (off, tout).noalias() += lp.conv.taps[(size_t) k].transpose() * ws.dZ;
            }
            ws.dXnext.swap (ws.dX);
        }

        // rechannel (dXnext now holds dLoss/d(rechannel output))
        accumulateDenseGrad (P.rechannel, ws.dXnext, A.arrayInput, grad);

        if (ai > 0)
        {
            ws.dArrayOut.noalias() = P.rechannel.w.transpose() * ws.dXnext;
            const auto& prevHead = ws.arrays[(size_t) ai - 1].head;
            ws.dHprev = MatrixXf::Zero (prevHead.rows(), prevHead.cols());
            ws.dHprev.rightCols (tf) = ws.dS;
            ws.dHead.swap (ws.dHprev);
        }
    }
}

std::string WaveNet::exportNam (const std::string& metadataJson, double sampleRate) const
{
    std::vector<float> weights = flat;
    weights.push_back (netSpec.headScale);

    nlohmann::json j;
    j["version"] = "0.5.4";
    j["architecture"] = "WaveNet";
    j["config"] = nlohmann::json::parse (netSpec.configJson());
    j["metadata"] = metadataJson.empty() ? nlohmann::json::object() : nlohmann::json::parse (metadataJson);
    j["weights"] = weights;
    j["sample_rate"] = sampleRate;
    return j.dump();
}

} // namespace gas::train
