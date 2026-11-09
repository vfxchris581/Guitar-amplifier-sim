cmake_minimum_required(VERSION 3.24)

# ---------------------------------------------------------------------------
# Product identity (rename the product here; nothing else needs to change)
# ---------------------------------------------------------------------------
set(PRODUCT_NAME        "Guitar Amplifier Sim")
set(PRODUCT_TARGET      GuitarAmpSim)
set(COMPANY_NAME        "vfxchris581")
set(BUNDLE_ID           "com.vfxchris581.guitaramplifiersim")
set(PLUGIN_MANUFACTURER_CODE Vfxc)
set(PLUGIN_CODE              Gasm)

project(${PRODUCT_TARGET} VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(GAS_BUILD_PLUGIN "Build the VST3 plugin (needs JUCE + GUI system libraries)" ON)
option(GAS_BUILD_TESTS  "Build the headless engine tests"                       ON)
option(GAS_LINUX_MINIMAL_X11
  "Linux only: build without Xrandr/Xinerama/Xcursor headers (for minimal build machines; not for release builds)" OFF)

include(cmake/Dependencies.cmake)   # fetches NAM core, Eigen, AudioDSPTools, JUCE
include(cmake/NamCore.cmake)        # builds the genuine NAM inference library

# ---------------------------------------------------------------------------
# Real-time engine: pure C++ (no JUCE), so it can be unit-tested headlessly
# ---------------------------------------------------------------------------
add_library(gas_engine STATIC
  src/engine/ModelInfo.cpp
  src/engine/NamModelInstance.cpp
  src/engine/AmpEngine.cpp
)
target_include_directories(gas_engine PUBLIC src)
# NAM registers architectures via static initialisers, so every object must be linked.
target_link_libraries(gas_engine PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")
if(NOT MSVC)
  target_compile_options(gas_engine PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

# Model training (pure C++, background threads only)
add_library(gas_train STATIC
  src/train/WaveNet.cpp
  src/train/Trainer.cpp
)
target_include_directories(gas_train PUBLIC src)
target_link_libraries(gas_train PUBLIC "$<LINK_LIBRARY:WHOLE_ARCHIVE,nam_core>")
find_package(Threads REQUIRED)
target_link_libraries(gas_train PUBLIC Threads::Threads)
if(MSVC)
  target_compile_options(gas_train PRIVATE /O2)
else()
  target_compile_options(gas_train PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
endif()

if(GAS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(GAS_BUILD_PLUGIN)
  add_subdirectory(src/plugin)
endif()
