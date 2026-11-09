# Third-party dependencies, pinned to exact versions.
#
# Any of these can be pointed at a local checkout instead of downloading, e.g.
#   -DFETCHCONTENT_SOURCE_DIR_NAMCORE=/path/to/NeuralAmpModelerCore
#   -DFETCHCONTENT_SOURCE_DIR_JUCE=/path/to/JUCE
# See docs/LICENSING.md for the licence of each dependency.

include(FetchContent)
set(FETCHCONTENT_QUIET ON)

# Genuine Neural Amp Modeler inference library (MIT) — Steven Atkinson.
# Its Eigen/AudioDSPTools git submodules are fetched separately below so that
# the build never depends on gitlab.com being reachable.
FetchContent_Declare(namcore
  GIT_REPOSITORY https://github.com/sdatkinson/NeuralAmpModelerCore.git
  GIT_TAG        v0.6.0
  GIT_SHALLOW    TRUE
  GIT_SUBMODULES ""
  SOURCE_SUBDIR  do-not-add-subdirectory
)

# Eigen (MPL-2.0), at the exact commit NeuralAmpModelerCore v0.6.0 pins.
# Official repo: gitlab.com/libeigen/eigen — this is its GitHub mirror.
FetchContent_Declare(eigen
  URL      https://github.com/eigen-mirror/eigen/archive/bc3b39870ecb690a623a3f49149a358b95c5781d.tar.gz
  SOURCE_SUBDIR do-not-add-subdirectory
)

# AudioDSPTools (MIT, with iPlug2 zlib-style resampler), at the commit NAM pins.
# Provides the Lanczos resampler used by the official NAM plugin.
FetchContent_Declare(audiodsptools
  GIT_REPOSITORY https://github.com/sdatkinson/AudioDSPTools.git
  GIT_TAG        844680d118f0317565132c3c5e3aca5f5c976e7a
  SOURCE_SUBDIR  do-not-add-subdirectory
)

# NAM's trainer (MIT). Not compiled; it is the reference for the C++ trainer and
# provides the loudness stimulus (nam/models/_resources/loudness_input.wav).
FetchContent_Declare(namtrainer
  GIT_REPOSITORY https://github.com/sdatkinson/neural-amp-modeler.git
  GIT_TAG        e680deaf4ed1d28ea260e7267680b9f10fed24b8
  SOURCE_SUBDIR  do-not-add-subdirectory
)

FetchContent_MakeAvailable(namcore eigen audiodsptools namtrainer)
set(NAM_LOUDNESS_STIMULUS "${namtrainer_SOURCE_DIR}/nam/models/_resources/loudness_input.wav")

if(GAS_BUILD_PLUGIN)
  # JUCE (AGPLv3 or commercial JUCE licence — see docs/LICENSING.md).
  FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.5
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(juce)
endif()
