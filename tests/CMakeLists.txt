add_executable(engine_tests EngineTests.cpp WavIO.cpp)
target_link_libraries(engine_tests PRIVATE gas_engine)
target_compile_definitions(engine_tests PRIVATE
  NAM_EXAMPLE_MODELS_DIR="${namcore_SOURCE_DIR}/example_models"
  NAM_EXAMPLE_AUDIO="${namcore_SOURCE_DIR}/example_audio/input.wav"
)
find_package(Threads REQUIRED)
target_link_libraries(engine_tests PRIVATE Threads::Threads)

add_test(NAME engine_tests COMMAND engine_tests)

# Plugin-level tests link the plugin's shared code (processor + editor).
if(GAS_BUILD_PLUGIN)
  add_executable(plugin_tests PluginTests.cpp WavIO.cpp)
  target_link_libraries(plugin_tests PRIVATE ${PRODUCT_TARGET})
  # Compile exactly like the plugin's own sources (JUCE module config, includes).
  target_include_directories(plugin_tests PRIVATE $<TARGET_PROPERTY:${PRODUCT_TARGET},INCLUDE_DIRECTORIES>)
  target_compile_definitions(plugin_tests PRIVATE $<TARGET_PROPERTY:${PRODUCT_TARGET},COMPILE_DEFINITIONS>)
  target_compile_definitions(plugin_tests PRIVATE
    NAM_EXAMPLE_MODELS_DIR="${namcore_SOURCE_DIR}/example_models"
    NAM_EXAMPLE_AUDIO="${namcore_SOURCE_DIR}/example_audio/input.wav"
  )
  add_test(NAME plugin_tests COMMAND plugin_tests)
endif()
