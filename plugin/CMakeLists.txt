# AceForge-Suno plugin — Suno API (Music Generation), AU + VST3 (macOS), VST3 (Windows/Linux)
cmake_minimum_required(VERSION 3.22)

include(FetchContent)
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG        8.0.12
  GIT_SHALLOW    TRUE
)
set(JUCE_ENABLE_GPL_MODE ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(JUCE)

set(SUNO_CLIENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../SunoClient)

# Platform-specific SunoClient implementation
if(APPLE)
  add_library(SunoClient STATIC ${SUNO_CLIENT_DIR}/SunoClientMac.mm)
  target_link_libraries(SunoClient PUBLIC "-framework Foundation" "-framework Security")
elseif(WIN32)
  add_library(SunoClient STATIC ${SUNO_CLIENT_DIR}/SunoClientWin.cpp)
  target_link_libraries(SunoClient PUBLIC winhttp)
elseif(UNIX)
  add_library(SunoClient STATIC ${SUNO_CLIENT_DIR}/SunoClientLinux.cpp)
  find_package(CURL REQUIRED)
  target_link_libraries(SunoClient PUBLIC CURL::libcurl)
endif()

target_include_directories(SunoClient PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(SunoClient PUBLIC cxx_std_17)

# Platform-specific plugin formats
if(APPLE)
  set(PLUGIN_FORMATS AU VST3)
else()
  set(PLUGIN_FORMATS VST3)
endif()

juce_add_plugin(AceForgeSuno
  VERSION 0.1.0
  COMPANY_NAME "AudioHacking"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  EDITOR_WANTS_KEYBOARD_FOCUS FALSE
  PLUGIN_MANUFACTURER_CODE AuHk
  PLUGIN_CODE AfSn
  FORMATS ${PLUGIN_FORMATS}
  PRODUCT_NAME "AceForge-Suno"
  COPY_PLUGIN_AFTER_BUILD FALSE
)

target_sources(AceForgeSuno
  PRIVATE
  PluginProcessor.cpp
  PluginEditor.cpp
)

target_compile_definitions(AceForgeSuno
  PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
)

target_include_directories(AceForgeSuno
  PRIVATE
  ${SUNO_CLIENT_DIR}
)

target_link_libraries(AceForgeSuno
  PRIVATE
  SunoClient
  juce::juce_audio_utils
  juce::juce_audio_formats
  PUBLIC
  juce::juce_recommended_config_flags
  juce::juce_recommended_lto_flags
  juce::juce_recommended_warning_flags
)
