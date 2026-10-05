include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

# Standalone macOS app: reads the bass from the audio interface and publishes a virtual CoreMIDI
# source ("Bass2MIDI") that MainStage can select as a MIDI input. MainStage cannot host VST3, so
# this replaces a plugin as the V1 delivery target.
juce_add_gui_app(Bass2MIDIApp
    PRODUCT_NAME "Bass2MIDI"
    COMPANY_NAME "Bass2MIDI"
    BUNDLE_ID "io.github.ajosef-etagen.bass2midi"
    VERSION ${PROJECT_VERSION}
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Bass2MIDI listens to your bass on the audio interface input to convert it to MIDI.")

target_sources(Bass2MIDIApp PRIVATE
    Main.cpp
    MainComponent.cpp
    AudioEngine.cpp
    MidiOutputSender.cpp
    SongLibrary.cpp
    TabView.cpp)

target_compile_definitions(Bass2MIDIApp PRIVATE
    BASS2MIDI_VERSION="${PROJECT_VERSION}"
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1)

target_link_libraries(Bass2MIDIApp
    PRIVATE
        bass2midi_dsp
        bass2midi_song
        juce::juce_audio_devices
        juce::juce_audio_utils
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
