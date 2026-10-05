cmake_minimum_required(VERSION 3.22)

project(Bass2MIDI VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# The JUCE app is the only part that needs JUCE (and, on Linux, its system packages). The DSP
# library, evaluation tools and tests are plain C++ and build anywhere.
option(BASS2MIDI_BUILD_APP "Build the JUCE standalone app" ON)
option(BASS2MIDI_BUILD_TESTS "Build unit tests and evaluation tools" ON)

add_library(bass2midi_warnings INTERFACE)
if(MSVC)
    target_compile_options(bass2midi_warnings INTERFACE /W4)
else()
    target_compile_options(bass2midi_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
endif()

add_subdirectory(dsp)
add_subdirectory(song)

if(BASS2MIDI_BUILD_TESTS)
    include(FetchContent)
    FetchContent_Declare(doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.5.3
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(doctest)

    # Offline evaluation support: synthetic signals and reference setups. Never linked into the app.
    add_library(bass2midi_eval STATIC
        eval/SyntheticBass.cpp
        eval/ReferenceSetups.cpp
        eval/WavFile.cpp)
    target_include_directories(bass2midi_eval PUBLIC eval)
    target_link_libraries(bass2midi_eval PUBLIC bass2midi_dsp PRIVATE bass2midi_warnings)

    add_executable(bass2midi_baseline eval/BaselineEval.cpp)
    target_link_libraries(bass2midi_baseline PRIVATE bass2midi_eval bass2midi_warnings)

    add_executable(bass2midi_analyze eval/AnalyzeRecording.cpp)
    target_link_libraries(bass2midi_analyze PRIVATE bass2midi_eval bass2midi_warnings)

    add_executable(bass2midi_songinfo eval/SongInfo.cpp)
    target_link_libraries(bass2midi_songinfo PRIVATE bass2midi_song bass2midi_warnings)

    enable_testing()
    add_executable(bass2midi_tests tests/DspTests.cpp tests/NoteStateMachineTests.cpp tests/MultiResolutionTests.cpp tests/SongTests.cpp tests/AttackDetectorTests.cpp)
    target_link_libraries(bass2midi_tests PRIVATE bass2midi_eval bass2midi_song doctest::doctest bass2midi_warnings)
    target_compile_definitions(bass2midi_tests PRIVATE BASS2MIDI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    add_test(NAME bass2midi_tests COMMAND bass2midi_tests)
endif()

if(BASS2MIDI_BUILD_APP)
    add_subdirectory(app)
endif()
