# Real-time DSP core: plain C++20, no JUCE, no allocation after prepare().
add_library(bass2midi_dsp STATIC
    src/AnalysisFramer.cpp
    src/BassToMidiProcessor.cpp
    src/AttackDetector.cpp
    src/BarPlayer.cpp
    src/SongFollower.cpp
    src/MultiResolutionPitchTracker.cpp
    src/NoteStateMachine.cpp
    src/Fft.cpp
    src/YinPitchEstimator.cpp)

target_include_directories(bass2midi_dsp PUBLIC include)
target_link_libraries(bass2midi_dsp PRIVATE bass2midi_warnings)
set_target_properties(bass2midi_dsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
