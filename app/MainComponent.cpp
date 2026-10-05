#include "MainComponent.h"

#include "bass2midi/MidiNoteUtils.h"

#include <cmath>

namespace
{
    constexpr const char* deviceStateKey = "audioDeviceState";
    constexpr const char* analysedInputKey = "analysedInputChannel";
    constexpr const char* midiEnabledKey = "midiOutputEnabled";
    constexpr const char* midiChannelKey = "midiChannel";
    constexpr const char* gateKey = "gateOpenDbfs";
    constexpr const char* transposeKey = "transposeSemitones";
    constexpr const char* songLibraryKey = "songLibrary";
    constexpr const char* selectedSongKey = "selectedSong";
    constexpr const char* songModeKey = "songMode";
    constexpr const char* repluckKey = "repluckDetection";
}

MainComponent::MainComponent (juce::PropertiesFile& settingsToUse) : settings (settingsToUse)
{
    // Mono bass input: one or two input channels may be enabled; "Analysed input" picks the one
    // that is analysed (the device selector's level meter shows all enabled channels together).
    // No outputs: Bass2MIDI never plays audio, the interface stays free for MainStage.
    const auto savedState = settings.getXmlValue (deviceStateKey);
    const auto error = deviceManager.initialise (1, 0, savedState.get(), true);
    if (error.isNotEmpty())
    {
        DBG ("Audio device init: " << error);
    }

    deviceManager.addAudioCallback (&engine);

    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent> (deviceManager, 1, 2, 0, 0,
                                                                            false, false, false, false);
    addAndMakeVisible (*deviceSelector);

    if (midiSender.openVirtualPort (virtualPortName))
        midiStatus.setText (juce::String ("Virtual MIDI output: \"") + virtualPortName
                                + "\" - select it as MIDI input in MainStage.", juce::dontSendNotification);
    else
        midiStatus.setText ("Virtual MIDI output unavailable on this OS.", juce::dontSendNotification);

    for (auto* label : { &midiStatus, &pitchReadout, &diagnostics })
        addAndMakeVisible (*label);

    pitchReadout.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    diagnostics.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
    diagnostics.setJustificationType (juce::Justification::topLeft);

    inputChannelLabel.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (inputChannelLabel);
    inputChannelBox.onChange = [this] { inputChannelChosen(); };
    addAndMakeVisible (inputChannelBox);
    refreshInputChannelChoices();

    // MIDI settings (persisted). Gate: the envelope level a pluck must reach to start a note -
    // set it a few dB above the noise shown under "Analysed input" when the strings are muted.
    midiEnabledToggle.setToggleState (settings.getBoolValue (midiEnabledKey, true), juce::dontSendNotification);
    for (int ch = 1; ch <= 16; ++ch)
        midiChannelBox.addItem (juce::String (ch), ch);
    midiChannelBox.setSelectedId (juce::jlimit (1, 16, settings.getIntValue (midiChannelKey, defaultMidiChannel)), juce::dontSendNotification);
    gateSlider.setRange (minGateDbfs, maxGateDbfs, 1.0);
    gateSlider.setValue (juce::jlimit (minGateDbfs, maxGateDbfs, settings.getDoubleValue (gateKey, defaultGateDbfs)), juce::dontSendNotification);
    gateSlider.setTextValueSuffix (" dBFS");
    transposeSlider.setRange (-maxTransposeSemitones, maxTransposeSemitones, 1.0);
    transposeSlider.setValue (juce::jlimit (-maxTransposeSemitones, maxTransposeSemitones, settings.getIntValue (transposeKey, 0)), juce::dontSendNotification);
    transposeSlider.setTextValueSuffix (" st");
    transposeSlider.setDoubleClickReturnValue (true, 0.0);
    transposeSlider.onValueChange = [this] { midiSettingsChanged(); };
    transposeLabel.setJustificationType (juce::Justification::centredRight);

    midiEnabledToggle.onClick = [this] { midiSettingsChanged(); };
    midiChannelBox.onChange = [this] { midiSettingsChanged(); };
    gateSlider.onValueChange = [this] { midiSettingsChanged(); };
    midiChannelLabel.setJustificationType (juce::Justification::centredRight);
    gateLabel.setJustificationType (juce::Justification::centredRight);
    for (auto* c : std::initializer_list<juce::Component*> { &midiEnabledToggle, &midiChannelLabel, &midiChannelBox, &gateLabel, &gateSlider,
                                                             &transposeLabel, &transposeSlider })
        addAndMakeVisible (*c);
    midiSettingsChanged();

    // Song palette: the song list is restored from the settings; a missing or unreadable file stays
    // listed with its error and selecting it means Free mode, so a song never blocks normal use.
    songs.restoreState (settings.getValue (songLibraryKey));
    for (auto* label : { &songLabel, &trackLabel })
        label->setJustificationType (juce::Justification::centredRight);
    songBox.onChange = [this] { songChosen(); };
    trackBox.onChange = [this] { trackChosen(); };
    previousSongButton.onClick = [this] { stepSong (-1); };
    nextSongButton.onClick = [this] { stepSong (1); };
    addSongsButton.onClick = [this] { addSongs(); };
    removeSongButton.onClick = [this] { removeSelectedSong(); };
    previousSongButton.setTooltip ("Previous song");
    nextSongButton.setTooltip ("Next song");
    for (auto* c : std::initializer_list<juce::Component*> { &songLabel, &songBox, &previousSongButton, &nextSongButton,
                                                             &addSongsButton, &removeSongButton, &trackLabel, &trackBox, &paletteLabel })
        addAndMakeVisible (*c);
    songModeToggle.setToggleState (settings.getBoolValue (songModeKey, false), juce::dontSendNotification);
    songModeToggle.onClick = [this] { songModeChanged(); };
    repluckToggle.setToggleState (settings.getBoolValue (repluckKey, false), juce::dontSendNotification);
    repluckToggle.onClick = [this] { songModeChanged(); };
    repluckToggle.setTooltip ("Finds plucks of a still-ringing string (repeated notes) from the break in periodicity. "
                              "Clearly better on synthetic tests, but more false triggers on the first real recordings.");
    startBarSlider.setRange (1.0, 1.0, 1.0);
    startBarSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 24);
    startBarLabel.setJustificationType (juce::Justification::centredRight);
    restartButton.onClick = [this] { restartAtBar(); };
    for (auto* c : std::initializer_list<juce::Component*> { &songModeToggle, &repluckToggle, &startBarLabel, &startBarSlider, &restartButton })
        addAndMakeVisible (*c);
    refreshSongControls();
    {
        const juce::File selected (settings.getValue (selectedSongKey));
        for (int i = 0; i < songs.size(); ++i)
            if (selected != juce::File() && songs[i].file == selected)
                songBox.setSelectedId (songIdOffset + i, juce::dontSendNotification);
    }
    songChosen();
    songModeChanged();

    testNoteButton.onClick = [this] { sendTestNote(); };
    testNoteButton.setEnabled (midiSender.isOpen());
    resetStatsButton.onClick = [this] { engine.resetCallbackStats(); };
    addAndMakeVisible (testNoteButton);
    addAndMakeVisible (resetStatsButton);

    setSize (720, 1120);
    startTimerHz (20);
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveDeviceState();

    if (testNoteSounding)
        midiSender.pushFromMessageThread (MidiOutputSender::noteOff (currentMidiChannel, testNote));

    deviceManager.removeAudioCallback (&engine);
    deviceManager.closeAudioDevice();
    midiSender.close(); // drains queued messages, including the Note Off above
}

void MainComponent::saveDeviceState()
{
    if (const auto state = deviceManager.createStateXml())
        settings.setValue (deviceStateKey, state.get());
    settings.saveIfNeeded();
}

void MainComponent::refreshInputChannelChoices()
{
    juce::StringArray names;
    if (auto* device = deviceManager.getCurrentAudioDevice())
    {
        const auto allNames = device->getInputChannelNames();
        const auto active = device->getActiveInputChannels();
        for (int i = 0; i < allNames.size(); ++i)
            if (active[i])
                names.add (allNames[i]);
    }

    if (names == activeInputNames)
        return;

    activeInputNames = names;
    inputChannelBox.clear (juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i)
        inputChannelBox.addItem (names[i], i + 1);

    const auto saved = settings.getValue (analysedInputKey);
    const auto savedIndex = names.indexOf (saved);
    inputChannelBox.setSelectedId (savedIndex >= 0 ? savedIndex + 1 : 1, juce::dontSendNotification);
    engine.setAnalysedChannel (juce::jmax (0, inputChannelBox.getSelectedId() - 1));
}

void MainComponent::inputChannelChosen()
{
    const auto index = inputChannelBox.getSelectedId() - 1;
    if (index < 0)
        return;

    engine.setAnalysedChannel (index);
    settings.setValue (analysedInputKey, activeInputNames[index]);
}

void MainComponent::midiSettingsChanged()
{
    currentMidiChannel = juce::jmax (1, midiChannelBox.getSelectedId());
    const bool enabled = midiEnabledToggle.getToggleState();
    const double gate = gateSlider.getValue();
    const int transpose = (int) transposeSlider.getValue();

    engine.setMidiSettings (enabled, currentMidiChannel, gate, transpose);
    settings.setValue (transposeKey, transpose);

    settings.setValue (midiEnabledKey, enabled);
    settings.setValue (midiChannelKey, currentMidiChannel);
    settings.setValue (gateKey, gate);
}

void MainComponent::refreshSongControls()
{
    const auto previous = songBox.getSelectedId();
    songBox.clear (juce::dontSendNotification);
    songBox.addItem ("Free (no song guidance)", freeModeId);
    for (int i = 0; i < songs.size(); ++i)
        songBox.addItem (juce::String (i + 1) + ". " + songs[i].displayName(), songIdOffset + i);
    songBox.setSelectedId (previous >= freeModeId && previous < songIdOffset + songs.size() ? previous : freeModeId,
                           juce::dontSendNotification);
}

void MainComponent::songChosen()
{
    const int index = selectedSongIndex();
    trackBox.clear (juce::dontSendNotification);
    if (index >= 0)
    {
        const auto& entry = songs[index];
        for (size_t t = 0; t < entry.song.tracks.size(); ++t)
        {
            const auto& track = entry.song.tracks[t];
            trackBox.addItem (juce::String::fromUTF8 (track.name.c_str()) + "  (" + juce::String ((int) track.notes.size()) + " notes)",
                              (int) t + 1);
            trackBox.setItemEnabled ((int) t + 1, ! track.percussion && ! track.notes.empty());
        }
        trackBox.setSelectedId (entry.track + 1, juce::dontSendNotification);
    }
    trackBox.setEnabled (index >= 0 && songs[index].usable());
    removeSongButton.setEnabled (index >= 0);
    previousSongButton.setEnabled (songs.size() > 0);
    nextSongButton.setEnabled (songs.size() > 0);
    applySongPalette();
}

void MainComponent::trackChosen()
{
    const int index = selectedSongIndex();
    if (index < 0)
        return;
    songs.setTrack (index, trackBox.getSelectedId() - 1);
    applySongPalette();
}

void MainComponent::applySongPalette()
{
    const int index = selectedSongIndex();
    bass2midi::NotePalette palette;
    juce::String text;
    if (index < 0)
    {
        text = "Free mode: every plausible bass note is accepted equally.";
    }
    else if (! songs[index].usable())
    {
        text = "Cannot use this song (" + (songs[index].error.isNotEmpty() ? songs[index].error : juce::String ("no track"))
             + ") - running in Free mode.";
    }
    else
    {
        palette = songs[index].palette();
        text = "Palette: " + SongLibrary::describe (palette);
    }

    engine.setPalette (palette);

    // Song Mode timeline (playing order, repeats unrolled). Free mode or an unusable song: empty.
    songTimeline.clear();
    if (index >= 0 && songs[index].usable())
        songTimeline = bass2midi::song::expectedNotes (songs[index].song, songs[index].track);
    engine.setSongTimeline (songTimeline);
    const int bars = index >= 0 ? juce::jmax (1, songs[index].song.barCount) : 1;
    startBarSlider.setRange (1.0, (double) bars, 1.0);
    startBarSlider.setValue (1.0, juce::dontSendNotification);
    songModeToggle.setEnabled (! songTimeline.empty());
    repluckToggle.setEnabled (! songTimeline.empty());
    startBarSlider.setEnabled (! songTimeline.empty());
    restartButton.setEnabled (! songTimeline.empty());

    paletteLabel.setText (text, juce::dontSendNotification);
    paletteLabel.setTooltip (text);
    saveSongState();
}

void MainComponent::songModeChanged()
{
    engine.setSongMode (songModeToggle.getToggleState());
    engine.setRepluckDetection (repluckToggle.getToggleState());
    settings.setValue (songModeKey, songModeToggle.getToggleState());
    settings.setValue (repluckKey, repluckToggle.getToggleState());
}

void MainComponent::restartAtBar()
{
    // First expected note at or after the chosen written bar (its first occurrence in playing order).
    const int bar = (int) startBarSlider.getValue() - 1;
    int index = 0;
    for (int i = 0; i < (int) songTimeline.size(); ++i)
        if (songTimeline[(size_t) i].bar >= bar)
        {
            index = i;
            break;
        }
    engine.setSongPosition (index);
}

juce::String MainComponent::songModeText (const AudioEngine::Snapshot& s) const
{
    juce::String text;
    const auto name = [] (int note) { return note >= 0 ? juce::String (bass2midi::midi::noteName (note)) : juce::String ("-"); };
    text << "Song Mode\n";
    if (songTimeline.empty())
        return text << "  off            no song selected (Free mode)\n";
    if (! songModeToggle.getToggleState())
        text << "  off            Free mode (" << (int) songTimeline.size() << " notes ready)\n";
    else
        text << "  on             " << (! s.songActive ? juce::String ("waiting for the audio device")
                                      : s.songConfidence >= 0.5 ? juce::String ("following, predicting")
                                                                : juce::String ("following, confidence too low: Free fallback")) << "\n";

    const int position = juce::jlimit (0, (int) songTimeline.size(), s.songPosition);
    if (position < (int) songTimeline.size())
    {
        const auto& next = songTimeline[(size_t) position];
        text << "  position       note " << position + 1 << " of " << (int) songTimeline.size() << ", bar " << next.bar + 1
             << " beat " << juce::String (next.beat, 2) << "\n";
        text << "  upcoming      ";
        const double tempo = s.songTempoRatio > 0.0 ? s.songTempoRatio : 1.0;
        for (int i = position; i < juce::jmin ((int) songTimeline.size(), position + 4); ++i)
        {
            const double aheadMs = 1000.0 * (songTimeline[(size_t) i].startSeconds - next.startSeconds) / tempo;
            text << " +" << juce::roundToInt (aheadMs) << " ms " << name (songTimeline[(size_t) i].midiNote);
        }
        text << "\n";
    }
    else
        text << "  position       end of song\n";
    text << "  confidence     " << juce::String (s.songConfidence, 2) << "   tempo x" << juce::String (s.songTempoRatio, 2) << " of the score\n"
         << "  last attack    velocity " << s.lastAttackVelocity << " (" << s.attackCount << " attacks)\n"
         << "  predicted      " << name (s.lastPredictedNote) << "   pitch path heard " << name (s.lastValidatedNote)
         << (s.lastValidationMatch == 1 ? "  (match)" : s.lastValidationMatch == 0 ? "  (MISMATCH)" : "") << "\n"
         << "  trigger delay  " << (s.lastTriggerLatencyMs >= 0.0 ? juce::String (s.lastTriggerLatencyMs, 1) + " ms" : juce::String ("-"))
         << " attack detected -> MIDI out\n"
         << "  counts         " << s.predictions << " predicted, " << s.corrections << " corrected, " << s.resyncs << " re-aligned, "
         << s.extraAttacks << " extra, " << s.ghostAttacks << " ghost, " << s.unpitchedAttacks << " unpitched\n";
    return text;
}

void MainComponent::addSongs()
{
    songChooser = std::make_unique<juce::FileChooser> ("Add Guitar Pro songs (.gp, .gp5)", juce::File(), SongLibrary::fileWildcard);
    songChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& chooser)
    {
        int last = -1;
        for (const auto& file : chooser.getResults())
            last = songs.add (file);
        if (last < 0)
            return;
        refreshSongControls();
        songBox.setSelectedId (songIdOffset + last, juce::dontSendNotification);
        songChosen();
    });
}

void MainComponent::removeSelectedSong()
{
    const int index = selectedSongIndex();
    if (index < 0)
        return;
    songs.remove (index);
    songBox.setSelectedId (freeModeId, juce::dontSendNotification);
    refreshSongControls();
    songChosen();
}

void MainComponent::stepSong (int delta)
{
    // Cycles through Free mode and all songs, so Free is always one step away.
    const int count = songs.size() + 1;
    const int position = juce::jmax (0, songBox.getSelectedId() - freeModeId);
    const int next = ((position + delta) % count + count) % count;
    songBox.setSelectedId (freeModeId + next, juce::dontSendNotification);
    songChosen();
}

void MainComponent::saveSongState()
{
    settings.setValue (songLibraryKey, songs.toState());
    const int index = selectedSongIndex();
    settings.setValue (selectedSongKey, index >= 0 ? songs[index].file.getFullPathName() : juce::String());
}

void MainComponent::sendTestNote()
{
    if (testNoteSounding)
        return;

    const int channel = currentMidiChannel;
    testNoteSounding = midiSender.pushFromMessageThread (MidiOutputSender::noteOn (channel, testNote, testVelocity));
    if (! testNoteSounding)
        return;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::Timer::callAfterDelay (testNoteLengthMs, [safeThis, channel]
    {
        if (safeThis != nullptr && safeThis->testNoteSounding)
        {
            // Retry briefly if the queue is full: a lost Note Off would leave a stuck note in MainStage.
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                if (safeThis->midiSender.pushFromMessageThread (MidiOutputSender::noteOff (channel, testNote)))
                    break;
                juce::Thread::sleep (1);
            }
            safeThis->testNoteSounding = false;
        }
    });
}

void MainComponent::timerCallback()
{
    refreshInputChannelChoices();

    const auto s = engine.getSnapshot();

    // Peak hold with ~1.5 s decay at 20 Hz refresh.
    displayedPeakLinear = juce::jmax (engine.takeInputPeakLinear(), displayedPeakLinear * 0.85f);
    const auto peakDbfs = displayedPeakLinear > 1.0e-6f ? 20.0 * std::log10 ((double) displayedPeakLinear) : -120.0;

    // Big readout: the MIDI note currently on (what MainStage hears), else the raw pitch estimate.
    if (s.soundingNote >= 0)
    {
        // Names use middle C = C4 (Logic/MainStage default to "C3" for MIDI 60 unless set to the
        // Roland convention); the MIDI number is shown so both conventions can be matched.
        pitchReadout.setText ("MIDI " + juce::String (s.soundingNote) + "  "
                                  + juce::String (bass2midi::midi::noteName (s.soundingNote))
                                  + "   vel " + juce::String (s.lastVelocity)
                                  + (s.playedNote >= 0 && s.playedNote != s.soundingNote
                                         ? "   (played " + juce::String (bass2midi::midi::noteName (s.playedNote)) + ")"
                                         : juce::String()),
                              juce::dontSendNotification);
    }
    else if (s.valid && s.frequencyHz > 0.0)
    {
        const auto exact = bass2midi::midi::noteFromFrequencyHz (s.frequencyHz);
        const auto nearest = (int) std::lround (exact);
        pitchReadout.setText ("(" + juce::String (bass2midi::midi::noteName (nearest))
                                  + "  " + juce::String (s.frequencyHz, 2) + " Hz  "
                                  + juce::String ((exact - nearest) * 100.0, 0) + " ct)",
                              juce::dontSendNotification);
    }
    else
    {
        pitchReadout.setText ("-", juce::dontSendNotification);
    }

    juce::String text;
    text << "Analysed input\n"
         << "  channel        " << (activeInputNames.isEmpty() ? juce::String ("none enabled") : inputChannelBox.getText()) << "\n"
         << "  peak           " << juce::String (peakDbfs, 1) << " dBFS" << (displayedPeakLinear >= 0.99f ? "  CLIPPING" : "") << "\n"
         << "Pitch analysis (multi-resolution YIN, 38-420 Hz)\n"
         << "  clarity        " << juce::String (s.clarity, 3) << "\n"
         << "  window level   " << juce::String (s.windowRmsDbfs, 1) << " dBFS RMS\n";

    if (s.sampleRateHz > 0.0)
    {
        const auto ms = [&] (double samples) { return juce::String (1000.0 * samples / s.sampleRateHz, 2) + " ms"; };
        text << "Device\n"
             << "  sample rate    " << juce::String (s.sampleRateHz, 0) << " Hz\n"
             << "  block size     " << s.blockSize << " samples (" << ms (s.blockSize) << ")\n";

        if (auto* device = deviceManager.getCurrentAudioDevice())
            text << "  input latency  " << device->getInputLatencyInSamples() << " samples ("
                 << ms (device->getInputLatencyInSamples()) << ", driver-reported)\n";

        text << "Analysis\n"
             << "  window         " << s.lastRungWindowSamples << " samples (" << ms (s.lastRungWindowSamples)
             << ") of max " << s.windowSamples << " (" << ms (s.windowSamples) << ")\n"
             << "  hop            " << s.hopSamples << " samples (" << ms (s.hopSamples) << ")\n"
             << "Callback cost\n"
             << "  average        " << juce::String (s.averageCallbackMs, 3) << " ms\n"
             << "  worst          " << juce::String (s.worstCallbackMs, 3) << " ms (budget " << ms (s.blockSize) << ")\n";
    }

    text << "MIDI\n"
         << "  output         " << (midiEnabledToggle.getToggleState() ? "on" : "off") << ", channel " << currentMidiChannel
         << ", transpose " << juce::String ((int) transposeSlider.getValue()) << " st\n"
         << "  sounding       " << (s.soundingNote >= 0 ? juce::String (bass2midi::midi::noteName (s.soundingNote)) : juce::String ("-")) << "\n"
         << "  note ons       " << s.noteOnCount << " (last velocity " << s.lastVelocity << ")\n"
         << "  sender poll    <= " << MidiOutputSender::senderPollIntervalMs << " ms added\n"
         << "  dropped        " << (int) midiSender.getDroppedMessageCount() << "\n"
         << "Song guidance\n"
         << "  palette        " << (s.sampleRateHz <= 0.0 ? juce::String ("not applied yet (no audio device running)")
                                       : s.paletteSize > 0 ? juce::String (s.paletteSize) + " notes in use"
                                                           : juce::String ("none (Free mode)")) << "\n"
         << "  early frames   " << s.paletteRelievedFrames << " (decided sooner thanks to the palette)\n"
         << "  delayed notes  " << s.paletteDelayed << " (off-palette, needed extra confirmation)\n"
         << songModeText (s);
    engine.releaseRetiredTimelines();

    diagnostics.setText (text, juce::dontSendNotification);
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced (12);
    deviceSelector->setBounds (area.removeFromTop (200));
    area.removeFromTop (8);
    auto channelRow = area.removeFromTop (28);
    inputChannelLabel.setBounds (channelRow.removeFromLeft (140));
    inputChannelBox.setBounds (channelRow.removeFromLeft (260));
    area.removeFromTop (8);
    auto midiRow = area.removeFromTop (28);
    midiEnabledToggle.setBounds (midiRow.removeFromLeft (120));
    midiChannelLabel.setBounds (midiRow.removeFromLeft (70));
    midiChannelBox.setBounds (midiRow.removeFromLeft (70));
    gateLabel.setBounds (midiRow.removeFromLeft (100));
    gateSlider.setBounds (midiRow.removeFromLeft (280));
    area.removeFromTop (4);
    auto transposeRow = area.removeFromTop (28);
    transposeRow.removeFromLeft (120);
    transposeLabel.setBounds (transposeRow.removeFromLeft (140));
    transposeSlider.setBounds (transposeRow.removeFromLeft (380));
    area.removeFromTop (8);
    area.removeFromTop (8);
    auto songRow = area.removeFromTop (28);
    songLabel.setBounds (songRow.removeFromLeft (60));
    previousSongButton.setBounds (songRow.removeFromLeft (28));
    songRow.removeFromLeft (4);
    songBox.setBounds (songRow.removeFromLeft (300));
    songRow.removeFromLeft (4);
    nextSongButton.setBounds (songRow.removeFromLeft (28));
    songRow.removeFromLeft (8);
    addSongsButton.setBounds (songRow.removeFromLeft (110));
    songRow.removeFromLeft (4);
    removeSongButton.setBounds (songRow.removeFromLeft (80));
    area.removeFromTop (4);
    auto trackRow = area.removeFromTop (28);
    trackLabel.setBounds (trackRow.removeFromLeft (92));
    trackBox.setBounds (trackRow.removeFromLeft (300));
    area.removeFromTop (2);
    paletteLabel.setBounds (area.removeFromTop (24));
    area.removeFromTop (4);
    songModeToggle.setBounds (area.removeFromTop (26));
    repluckToggle.setBounds (area.removeFromTop (26).withTrimmedLeft (24));
    auto startRow = area.removeFromTop (28);
    startBarLabel.setBounds (startRow.removeFromLeft (100));
    startBarSlider.setBounds (startRow.removeFromLeft (140));
    startRow.removeFromLeft (8);
    restartButton.setBounds (startRow.removeFromLeft (120));
    area.removeFromTop (8);
    midiStatus.setBounds (area.removeFromTop (24));

    auto buttons = area.removeFromTop (32);
    testNoteButton.setBounds (buttons.removeFromLeft (200));
    buttons.removeFromLeft (8);
    resetStatsButton.setBounds (buttons.removeFromLeft (180));

    area.removeFromTop (8);
    pitchReadout.setBounds (area.removeFromTop (44));
    diagnostics.setBounds (area);
}
