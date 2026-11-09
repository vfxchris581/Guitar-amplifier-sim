#pragma once

#include <Eigen/Dense>

#include <cstdint>
#include <string>
#include <vector>

namespace gas::train
{

/** One NAM "layer array" (classic A1 WaveNet). */
struct LayerArraySpec
{
    int inputSize = 1;
    int conditionSize = 1;
    int channels = 16;
    int headSize = 8;
    int kernelSize = 3;
    std::vector<int> dilations;
    bool headBias = false;
};

/** The architectures NAM's classic trainer offers (v0.10, "A1"). */
enum class Preset { Standard = 0, Lite = 1, Feather = 2, Nano = 3 };

const char* presetName (Preset);

/** A WaveNet exactly as NeuralAmpModelerCore's "WaveNet" architecture defines it:
    tanh, ungated, input-mixin from the raw input, residual 1x1, per-array head
    rechannel and a fixed head scale. */
struct WaveNetSpec
{
    std::vector<LayerArraySpec> arrays;
    float headScale = 0.02f;

    static WaveNetSpec fromPreset (Preset);

    int receptiveField() const;   // number of input samples one output sample depends on
    size_t numParams() const;     // trainable parameters (head scale excluded)
    std::string configJson() const; // the "config" object of a .nam file
};

/** Scratch space for one forward/backward pass of one segment. One per thread. */
struct Workspace
{
    struct Layer
    {
        Eigen::MatrixXf input;  // C x T_in
        Eigen::MatrixXf act;    // C x T_out (tanh output)
    };
    struct Array
    {
        Eigen::MatrixXf arrayInput;   // In x T0
        std::vector<Layer> layers;
        Eigen::MatrixXf output;       // C x Tf (input to next array)
        Eigen::MatrixXf headSum;      // C x Tf
        Eigen::MatrixXf head;         // H x Tf
    };
    std::vector<Array> arrays;
    Eigen::RowVectorXf condition;     // 1 x L

    // backward scratch
    Eigen::MatrixXf dX, dXnext, dA, dZ, dHead, dS, dHprev, dArrayOut;
    int length = 0;
};

/** Trainable WaveNet. Parameters live in one flat vector laid out in exactly
    the order NeuralAmpModelerCore reads them from a .nam file. */
class WaveNet
{
public:
    explicit WaveNet (WaveNetSpec spec);

    const WaveNetSpec& spec() const { return netSpec; }

    /** PyTorch-default initialisation (uniform +-1/sqrt(fan_in)), deterministic for a seed. */
    void initialise (std::uint32_t seed);

    std::vector<float>& params() { return flat; }
    const std::vector<float>& params() const { return flat; }

    /** Call after changing params() so the per-tap weight matrices are refreshed. */
    void unpack();

    /** Forward pass over L >= receptiveField() input samples; writes L - R + 1 outputs.
        Keeps intermediates in `ws` for backward(). */
    void forward (const float* x, int length, float* y, Workspace& ws) const;

    /** Back-propagates dLoss/dy (length L - R + 1) from the last forward() on `ws`,
        accumulating parameter gradients into `grad` (same layout as params()). */
    void backward (const float* dy, Workspace& ws, std::vector<float>& grad) const;

    /** Full .nam file (JSON text) for the current parameters. */
    std::string exportNam (const std::string& metadataJson, double sampleRate) const;

private:
    struct ConvParams    // dilated conv: W[out][in][k] then bias[out]
    {
        size_t offset = 0;
        int cin = 0, cout = 0, k = 0, dilation = 1;
        std::vector<Eigen::MatrixXf> taps; // cout x cin per tap (unpacked)
        Eigen::VectorXf bias;
    };
    struct DenseParams   // 1x1: W[out][in] then optional bias[out]
    {
        size_t offset = 0;
        int cin = 0, cout = 0;
        bool hasBias = false;
        Eigen::MatrixXf w;
        Eigen::VectorXf bias;
    };
    struct LayerParams { ConvParams conv; DenseParams mixin, out1x1; };
    struct ArrayParams { DenseParams rechannel; std::vector<LayerParams> layers; DenseParams head; };

    static void unpackDense (DenseParams&, const std::vector<float>&);
    static void accumulateDenseGrad (const DenseParams&, const Eigen::MatrixXf& dOut, const Eigen::MatrixXf& in,
                                     std::vector<float>& grad);

    WaveNetSpec netSpec;
    std::vector<ArrayParams> arrays;
    std::vector<float> flat;
};

} // namespace gas::train
