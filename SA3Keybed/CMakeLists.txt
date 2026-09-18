cmake_minimum_required(VERSION 3.20)
project(SA3Keybed VERSION 0.1.0)

# Dev builds also look for models in the sa3.cpp checkout's staging folder. Release builds turn this
# off so no build-machine path ships in the binary: -DSA3_KEYBED_DEV_MODELS=OFF.
option(SA3_KEYBED_DEV_MODELS "Look for models in ../sa3.cpp's staging folder" ON)
set(SA3_KEYBED_DEV_DEFINES)
if(SA3_KEYBED_DEV_MODELS)
  list(APPEND SA3_KEYBED_DEV_DEFINES
    SA3_KEYBED_DEV_MODELS_DIR="${SA3_CPP_DIR}/models/publication-stage-sat/foundation-1.2-keybeds-GGUF")
endif()

set(SA3_KEYBED_PLATFORM_LIBS)
if(WIN32)
  list(APPEND SA3_KEYBED_PLATFORM_LIBS shell32 ole32)
endif()

iplug_add_plugin(${PROJECT_NAME}
  SOURCES
    SA3Keybed.cpp
    SA3Keybed.h
    KeybedControl.cpp
    KeybedControl.h
    KeybedKit.cpp
    KeybedKit.h
    KeybedRenderService.cpp
    KeybedRenderService.h
    KeybedSampler.h
    ModelDownload.cpp
    ModelDownload.h
    Sa3Runtime.cpp
    Sa3Runtime.h
    config.h
    resources/resource.h
  RESOURCES
    resources/fonts/Roboto-Regular.ttf
  LINK
    iPlug2::Extras::Synth
    sa3_headers
    sa3_ui_theme
    ${SA3_KEYBED_PLATFORM_LIBS}
  DEFINES
    ${SA3_KEYBED_DEV_DEFINES}
  FORMATS APP VST3 CLAP
)

function(sa3_keybed_copy_runtime target_name)
  if(TARGET ${target_name} AND SA3_RUNTIME_LIBS)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SA3_RUNTIME_LIBS} "$<TARGET_FILE_DIR:${target_name}>"
      COMMENT "Copying sa3/ggml runtime libraries for ${target_name}"
    )
  endif()
endfunction()

sa3_keybed_copy_runtime(${PROJECT_NAME}-app)
sa3_keybed_copy_runtime(${PROJECT_NAME}-vst3)
sa3_keybed_copy_runtime(${PROJECT_NAME}-clap)

# Status strings contain UTF-8 punctuation; NanoVG draws UTF-8.
if(MSVC)
  foreach(_target ${PROJECT_NAME}-app ${PROJECT_NAME}-vst3 ${PROJECT_NAME}-clap)
    if(TARGET ${_target})
      target_compile_options(${_target} PRIVATE /utf-8)
    endif()
  endforeach()
endif()

# Headless engine test: render -> kit -> bank -> MIDI -> voices, no host or UI.
option(SA3_KEYBED_BUILD_TESTS "Build the headless SA3Keybed engine test" ON)
if(SA3_KEYBED_BUILD_TESTS)
  add_executable(SA3KeybedEngineTest
    tests/KeybedEngineTest.cpp
    KeybedKit.cpp
    KeybedRenderService.cpp
    Sa3Runtime.cpp
    ${IPLUG2_DIR}/IPlug/Extras/Synth/MidiSynth.cpp
    ${IPLUG2_DIR}/IPlug/Extras/Synth/VoiceAllocator.cpp
  )
  target_include_directories(SA3KeybedEngineTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IPLUG2_DIR}/IPlug
    ${IPLUG2_DIR}/IPlug/Extras
    ${IPLUG2_DIR}/IPlug/Extras/Synth
    ${IPLUG2_DIR}/WDL
  )
  target_compile_definitions(SA3KeybedEngineTest PRIVATE NOMINMAX _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(SA3KeybedEngineTest PRIVATE /wd4068 /utf-8)
  endif()
  target_link_libraries(SA3KeybedEngineTest PRIVATE sa3_headers ${SA3_KEYBED_PLATFORM_LIBS})
  set_target_properties(SA3KeybedEngineTest PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out-test")
  sa3_keybed_copy_runtime(SA3KeybedEngineTest)
endif()
