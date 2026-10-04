#pragma once

#include "AudioEngine.h"
#include "MidiOutputSender.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Main window: audio input selection, MIDI output settings (enable, channel, gate), virtual MIDI
// port status, a test-note button for verifying the MainStage route, and live pitch / note /
// timing diagnostics.
class MainComponent final : public juce::Component, private juce::Timer
{
public:
    static constexpr const char* virtualPortName = "Bass2MIDI";
    static constexpr int testNote = 48;         // C3
    static constexpr int testVelocity = 100;
    static constexpr int testNoteLengthMs = 400;
    static constexpr int defaultMidiChannel = 1;
    static constexpr double defaultGateDbfs = -45.0;   // NoteStateMachine::Settings default
    static constexpr double minGateDbfs = -80.0, maxGateDbfs = -20.0;
    static constexpr int maxTransposeSemitones = 24;

    explicit MainComponent (juce::PropertiesFile& settings);
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void sendTestNote();
    void saveDeviceState();
    void refreshInputChannelChoices();
    void inputChannelChosen();
    void midiSettingsChanged();

    juce::PropertiesFile& settings;
    juce::AudioDeviceManager deviceManager;
    MidiOutputSender midiSender;
    AudioEngine engine { midiSender };

    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::Label midiStatus, pitchReadout, diagnostics;
    juce::Label inputChannelLabel { {}, "Analysed input:" };
    juce::ComboBox inputChannelBox;
    juce::StringArray activeInputNames;   // active input channels, in the order the device delivers them
    float displayedPeakLinear = 0.0f;     // decaying peak hold for the readout
    juce::ToggleButton midiEnabledToggle { "MIDI output" };
    juce::Label midiChannelLabel { {}, "Channel:" };
    juce::ComboBox midiChannelBox;
    juce::Label gateLabel { {}, "Gate (dBFS):" };
    juce::Slider gateSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Label transposeLabel { {}, "Transpose:" };
    juce::Slider transposeSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    int currentMidiChannel = defaultMidiChannel;
    juce::TextButton testNoteButton { "Test note (MIDI 48)" };
    juce::TextButton resetStatsButton { "Reset timing stats" };
    bool testNoteSounding = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
