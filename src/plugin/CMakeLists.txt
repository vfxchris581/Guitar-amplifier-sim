# Resources compiled into the plugin
juce_add_binary_data(GasBinaryData SOURCES "${NAM_LOUDNESS_STIMULUS}")

juce_add_plugin(${PRODUCT_TARGET}
  PRODUCT_NAME             "${PRODUCT_NAME}"
  COMPANY_NAME             "${COMPANY_NAME}"
  BUNDLE_ID                "${BUNDLE_ID}"
  PLUGIN_MANUFACTURER_CODE ${PLUGIN_MANUFACTURER_CODE}
  PLUGIN_CODE              ${PLUGIN_CODE}
  VERSION                  ${PROJECT_VERSION}
  FORMATS                  VST3
  IS_SYNTH                 FALSE
  NEEDS_MIDI_INPUT         FALSE
  NEEDS_MIDI_OUTPUT        FALSE
  IS_MIDI_EFFECT           FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  VST3_CATEGORIES          Fx Distortion
  COPY_PLUGIN_AFTER_BUILD  FALSE
)

target_sources(${PRODUCT_TARGET} PRIVATE
  PluginProcessor.cpp
  PluginEditor.cpp
  CaptureController.cpp
  ui/CapturePage.cpp
  ui/Theme.cpp
  ui/LevelMeter.cpp
  ui/AmpPage.cpp
)

target_include_directories(${PRODUCT_TARGET} PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(${PRODUCT_TARGET} PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_DISPLAY_SPLASH_SCREEN=0
  JUCE_REPORT_APP_USAGE=0
  GAS_PRODUCT_NAME="${PRODUCT_NAME}"
  GAS_VERSION_STRING="${PROJECT_VERSION}"
)

if(GAS_LINUX_MINIMAL_X11)
  target_compile_definitions(${PRODUCT_TARGET} PUBLIC
    JUCE_USE_XRANDR=0 JUCE_USE_XINERAMA=0 JUCE_USE_XCURSOR=0)
endif()

target_link_libraries(${PRODUCT_TARGET}
  PRIVATE
    gas_engine
    gas_capture
    gas_train
    GasBinaryData
    juce::juce_audio_processors
    juce::juce_gui_basics
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)
