#pragma once

// AudioDSPTools' ResamplingContainer (the resampler used by the official NAM
// plugin) was extracted from iPlug2 and still expects two iPlug2 constants.
// Supplying them here lets us use the file unmodified.

#ifndef DEFAULT_BLOCK_SIZE
 #define DEFAULT_BLOCK_SIZE 1024
#endif

namespace iplug
{
static constexpr double PI = 3.14159265358979323846;
}

#include "ResamplingContainer/ResamplingContainer.h"
