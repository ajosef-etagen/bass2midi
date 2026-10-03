#pragma once

#include "AudioEngine.h"
#include "MidiOutputSender.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Phase 0 window: audio input selection, virtual MIDI port status, a test-note button for
// verifying the MainStage route, and live reference-pitch / timing diagnostics.
class MainComponent final : public juce::Component, private juce::Timer
{
public:
    static constexpr const char* virtualPortName = "Bass2MIDI";
    static constexpr int testNote = 48;         // C3
    static constexpr int testVelocity = 100;
    static constexpr int testNoteLengthMs = 400;
    static constexpr int midiChannel = 1;

    explicit MainComponent (juce::PropertiesFile& settings);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void sendTestNote();
    void saveDeviceState();

    juce::PropertiesFile& settings;
    juce::AudioDeviceManager deviceManager;
    MidiOutputSender midiSender;
    AudioEngine engine { midiSender };

    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::Label midiStatus, pitchReadout, diagnostics;
    juce::TextButton testNoteButton { "Send test note (C3)" };
    juce::TextButton resetStatsButton { "Reset timing stats" };
    bool testNoteSounding = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
