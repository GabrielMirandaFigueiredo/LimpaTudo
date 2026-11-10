cmake_minimum_required(VERSION 3.22)
project(LimpaTudo VERSION 1.0.0 LANGUAGES C CXX)

# Runtime do Visual C++ embutido: o plugin funciona sem instalar o "VC++ Redistributable".
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW ON)
FetchContent_MakeAvailable(JUCE)

# --- RNNoise (rede neural de supressão de ruído, licença BSD) ---
file(GLOB RNNOISE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src/*.c)
add_library(rnnoise STATIC ${RNNOISE_SOURCES})
target_include_directories(rnnoise
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise/src)
if(MSVC)
    # AVX2 é necessário para o caminho rápido do RNNoise (qualquer CPU de ~2015 em diante tem).
    target_compile_options(rnnoise PRIVATE /arch:AVX2 /O2 /wd4244 /wd4267 /wd4305 /wd4996)
    target_compile_definitions(rnnoise PRIVATE _CRT_SECURE_NO_WARNINGS OPUS_X86_MAY_HAVE_SSE2)
else()
    target_compile_options(rnnoise PRIVATE -O3 -mavx2 -mfma -w)
endif()

# --- Plugin ---
juce_add_plugin(LimpaTudo
    COMPANY_NAME "Gabriel"
    PLUGIN_MANUFACTURER_CODE Gabr
    PLUGIN_CODE Ltdo
    FORMATS VST3
    PRODUCT_NAME "LimpaTudo"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    VST3_CATEGORIES Fx
    COPY_PLUGIN_AFTER_BUILD FALSE)

juce_generate_juce_header(LimpaTudo)

target_sources(LimpaTudo PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginProcessor.h
    Source/PluginEditor.cpp
    Source/PluginEditor.h
    Source/Engine.h)

target_compile_definitions(LimpaTudo PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(LimpaTudo
    PRIVATE
        rnnoise
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
