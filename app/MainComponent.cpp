#include "MainComponent.h"

#include "bass2midi/MidiNoteUtils.h"

#include <cmath>

namespace
{
    constexpr const char* deviceStateKey = "audioDeviceState";
}

MainComponent::MainComponent (juce::PropertiesFile& settingsToUse) : settings (settingsToUse)
{
    // Mono bass input: one or two input channels may be enabled, only the first is analysed.
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

    testNoteButton.onClick = [this] { sendTestNote(); };
    testNoteButton.setEnabled (midiSender.isOpen());
    resetStatsButton.onClick = [this] { engine.resetCallbackStats(); };
    addAndMakeVisible (testNoteButton);
    addAndMakeVisible (resetStatsButton);

    setSize (640, 680);
    startTimerHz (20);
}

MainComponent::~MainComponent()
{
    stopTimer();
    saveDeviceState();

    if (testNoteSounding)
        midiSender.pushFromMessageThread (MidiOutputSender::noteOff (midiChannel, testNote));

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

void MainComponent::sendTestNote()
{
    if (testNoteSounding)
        return;

    testNoteSounding = midiSender.pushFromMessageThread (MidiOutputSender::noteOn (midiChannel, testNote, testVelocity));
    if (! testNoteSounding)
        return;

    juce::Component::SafePointer<MainComponent> safeThis (this);
    juce::Timer::callAfterDelay (testNoteLengthMs, [safeThis]
    {
        if (safeThis != nullptr && safeThis->testNoteSounding)
        {
            // Retry briefly if the queue is full: a lost Note Off would leave a stuck note in MainStage.
            for (int attempt = 0; attempt < 100; ++attempt)
            {
                if (safeThis->midiSender.pushFromMessageThread (MidiOutputSender::noteOff (midiChannel, testNote)))
                    break;
                juce::Thread::sleep (1);
            }
            safeThis->testNoteSounding = false;
        }
    });
}

void MainComponent::timerCallback()
{
    const auto s = engine.getSnapshot();

    if (s.valid && s.frequencyHz > 0.0)
    {
        const auto exact = bass2midi::midi::noteFromFrequencyHz (s.frequencyHz);
        const auto nearest = (int) std::lround (exact);
        pitchReadout.setText (juce::String (bass2midi::midi::noteName (nearest))
                                  + "  " + juce::String (s.frequencyHz, 2) + " Hz  "
                                  + juce::String ((exact - nearest) * 100.0, 0) + " ct",
                              juce::dontSendNotification);
    }
    else
    {
        pitchReadout.setText ("-", juce::dontSendNotification);
    }

    juce::String text;
    text << "Reference analysis (plain YIN, no note decisions yet)\n"
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
    deviceSelector->setBounds (area.removeFromTop (180));
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
