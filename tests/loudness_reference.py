"""Cross-check of the C++ loudness measurement against NAM's own Python code.

Usage: python3 loudness_reference.py <neural-amp-modeler source dir> <model_output.wav> <stimulus.wav>
Prints NAM's loudness (dB) for a model output. Only the measurement function is
taken from the trainer (no PyTorch needed).
"""
import ast
import math
import sys

import numpy as np
from scipy.io import wavfile
from scipy.signal import sosfilt

src_dir, out_wav, stim_wav = sys.argv[1:4]
source = open(f"{src_dir}/nam/models/base.py").read()
tree = ast.parse(source)
wanted = {"_burst_train_loudness_db"}
names = {"_LOUDNESS_SAMPLE_RATE", "_LOUDNESS_NUM_BURSTS", "_LOUDNESS_BURST_EDGE_TRIM_SAMPLES",
         "_LOUDNESS_CALIBRATION_DB", "_LOUDNESS_K_WEIGHTING_SOS"}
nodes = [n for n in tree.body
         if (isinstance(n, ast.FunctionDef) and n.name in wanted)
         or (isinstance(n, ast.Assign) and any(getattr(t, "id", None) in names for t in n.targets))]
ns = {"_np": np, "_sosfilt": sosfilt, "_math": math}
exec(compile(ast.Module(body=nodes, type_ignores=[]), "base.py", "exec"), ns)


def read(path):
    _, data = wavfile.read(path)
    if data.dtype == np.int32 and data.max() < (1 << 23) and data.min() >= -(1 << 23):
        return data.astype(np.float64) / float(1 << 23)  # 24-bit packed in int32
    if data.dtype.kind == "i":
        return data.astype(np.float64) / float(np.iinfo(data.dtype).max + 1)
    return data.astype(np.float64)


y = read(out_wav)
n = len(read(stim_wav))
print(f"{ns['_burst_train_loudness_db'](y, num_input_samples=n):.6f}")
