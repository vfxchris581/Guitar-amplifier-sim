# Builds NeuralAmpModelerCore's sources (unmodified) as a static library.

file(GLOB NAM_CORE_SOURCES CONFIGURE_DEPENDS
  "${namcore_SOURCE_DIR}/NAM/*.cpp"
  "${namcore_SOURCE_DIR}/NAM/*/*.cpp"
)

add_library(nam_core STATIC ${NAM_CORE_SOURCES})

target_include_directories(nam_core SYSTEM PUBLIC
  "${namcore_SOURCE_DIR}"                           # #include "NAM/dsp.h"
  "${namcore_SOURCE_DIR}/Dependencies/nlohmann"     # json.hpp
  "${eigen_SOURCE_DIR}"                             # Eigen/Dense
  "${audiodsptools_SOURCE_DIR}/dsp"                 # ResamplingContainer
)

target_compile_definitions(nam_core PUBLIC
  NAM_SAMPLE_FLOAT          # 32-bit float processing path, matching the host buffers
  NAM_ENABLE_A2_FAST        # NAM's hand-optimised path for A2 WaveNet models
)

if(WIN32)
  target_compile_definitions(nam_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES)
endif()

if(MSVC)
  target_compile_options(nam_core PRIVATE /O2 /bigobj /W0)
else()
  target_compile_options(nam_core PRIVATE -O3 -w)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" AND NOT APPLE)
    # Baseline x86-64-v2 (SSE4.2) keeps the plugin loadable on any machine from
    # the last ~15 years while giving Eigen SIMD. PUBLIC so every translation
    # unit that sees Eigen types agrees on vectorisation/alignment.
    target_compile_options(nam_core PUBLIC -msse4.2)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
  target_link_libraries(nam_core PUBLIC stdc++fs)
endif()
