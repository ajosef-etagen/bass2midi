#pragma once

#include "AudioEngine.h"
#include "MidiOutputSender.h"
#include "SongLibrary.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

// Main window: audio input selection, MIDI output settings (enable, channel, gate, transpose), the
// song list for the optional song palette (Free mode by default), virtual MIDI port status, a
// test-note button for verifying the MainStage route, and live pitch / note / timing diagnostics.
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

    // Song palette (message thread). Item ids in songBox: freeModeId, then songIdOffset + index.
    static constexpr int freeModeId = 1, songIdOffset = 2;
    void refreshSongControls();
    void songChosen();
    void trackChosen();
    void applySongPalette();
    void addSongs();
    void removeSelectedSong();
    void stepSong (int delta);
    void saveSongState();
    void songModeChanged();
    void restartAtBar();
    juce::String songModeText (const AudioEngine::Snapshot& s) const;
    int selectedSongIndex() const { return songBox.getSelectedId() - songIdOffset; } // -1 = Free mode

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
    SongLibrary songs;
    juce::Label songLabel { {}, "Song:" };
    juce::ComboBox songBox;
    juce::TextButton previousSongButton { "<" }, nextSongButton { ">" };
    juce::TextButton addSongsButton { "Add songs..." }, removeSongButton { "Remove" };
    juce::Label trackLabel { {}, "Bass track:" };
    juce::ComboBox trackBox;
    juce::Label paletteLabel;
    juce::ToggleButton songModeToggle { "Song Mode: a bass attack plays the song's next note at once" };
    juce::ToggleButton repluckToggle { "Detect re-plucks of ringing notes (experimental)" };
    juce::Label startBarLabel { {}, "Start at bar:" };
    juce::Slider startBarSlider { juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft };
    juce::TextButton restartButton { "Restart here" };
    std::vector<bass2midi::ExpectedNote> songTimeline; // message-thread copy of what the engine follows
    std::unique_ptr<juce::FileChooser> songChooser;

    juce::TextButton testNoteButton { "Test note (MIDI 48)" };
    juce::TextButton resetStatsButton { "Reset timing stats" };
    bool testNoteSounding = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
