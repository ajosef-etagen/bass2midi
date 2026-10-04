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

    midiEnabledToggle.onClick = [this] { midiSettingsChanged(); };
    midiChannelBox.onChange = [this] { midiSettingsChanged(); };
    gateSlider.onValueChange = [this] { midiSettingsChanged(); };
    midiChannelLabel.setJustificationType (juce::Justification::centredRight);
    gateLabel.setJustificationType (juce::Justification::centredRight);
    for (auto* c : std::initializer_list<juce::Component*> { &midiEnabledToggle, &midiChannelLabel, &midiChannelBox, &gateLabel, &gateSlider })
        addAndMakeVisible (*c);
    midiSettingsChanged();

    testNoteButton.onClick = [this] { sendTestNote(); };
    testNoteButton.setEnabled (midiSender.isOpen());
    resetStatsButton.onClick = [this] { engine.resetCallbackStats(); };
    addAndMakeVisible (testNoteButton);
    addAndMakeVisible (resetStatsButton);

    setSize (680, 800);
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

    engine.setMidiSettings (enabled, currentMidiChannel, gate);

    settings.setValue (midiEnabledKey, enabled);
    settings.setValue (midiChannelKey, currentMidiChannel);
    settings.setValue (gateKey, gate);
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
        pitchReadout.setText ("MIDI  " + juce::String (bass2midi::midi::noteName (s.soundingNote))
                                  + "   vel " + juce::String (s.lastVelocity),
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
         << "Pitch analysis (YIN, 38-420 Hz)\n"
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
             << "  window         " << s.windowSamples << " samples (" << ms (s.windowSamples) << ")\n"
             << "  hop            " << s.hopSamples << " samples (" << ms (s.hopSamples) << ")\n"
             << "Callback cost\n"
             << "  average        " << juce::String (s.averageCallbackMs, 3) << " ms\n"
             << "  worst          " << juce::String (s.worstCallbackMs, 3) << " ms (budget " << ms (s.blockSize) << ")\n";
    }

    text << "MIDI\n"
         << "  output         " << (midiEnabledToggle.getToggleState() ? "on" : "off") << ", channel " << currentMidiChannel << "\n"
         << "  sounding       " << (s.soundingNote >= 0 ? juce::String (bass2midi::midi::noteName (s.soundingNote)) : juce::String ("-")) << "\n"
         << "  note ons       " << s.noteOnCount << " (last velocity " << s.lastVelocity << ")\n"
         << "  sender poll    <= " << MidiOutputSender::senderPollIntervalMs << " ms added\n"
         << "  dropped        " << (int) midiSender.getDroppedMessageCount() << "\n";

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
