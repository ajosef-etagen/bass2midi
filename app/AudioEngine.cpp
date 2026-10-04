#include "AudioEngine.h"
#include "MidiOutputSender.h"

#include <cmath>

AudioEngine::AudioEngine (MidiOutputSender& sender) : midiSender (sender) {}

void AudioEngine::setMidiSettings (bool enabled, int midiChannel, double gateOpenDbfs, int transposeSemitones) noexcept
{
    pendingTransposeSemitones.store (juce::jlimit (-48, 48, transposeSemitones));
    pendingOutputEnabled.store (enabled);
    pendingMidiChannel.store (juce::jlimit (1, 16, midiChannel));
    pendingGateOpenDbfs.store (juce::jlimit (-90.0, 0.0, gateOpenDbfs));
    settingsVersion.fetch_add (1);
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Called before callbacks start (not concurrently with them), so allocation is allowed here.
    const auto rate = device->getCurrentSampleRate();
    const auto config = bass2midi::YinPitchEstimator::Config::forFrequencyRange (rate, analysisLowestHz,
                                                                                 analysisHighestHz, yinThreshold);
    const auto hop = juce::jmax (1, (int) std::lround (rate * analysisHopSeconds));

    analysisReady = estimator.prepare (rate, config) && framer.prepare (config.windowSamples, hop);

    // Frame timing of the state machine follows the analysis framing at this sample rate.
    noteSettings.frameIntervalSeconds = (double) hop / rate;
    noteSettings.onsetSettleMs = onsetSettleWindowFraction * 1000.0 * (double) config.windowSamples / rate;
    noteMachine.reset();
    appliedSettingsVersion = -1; // re-apply user settings on the first block
    soundingNote.store (-1);

    sampleRateHz.store (rate);
    blockSize.store (device->getCurrentBufferSizeSamples());
    windowSamples.store (config.windowSamples);
    hopSamples.store (hop);
    lastValid.store (false);
    resetStatsRequested.store (true);
}

void AudioEngine::audioDeviceStopped()
{
    // Callbacks have stopped, so this thread is the only producer for the audio FIFO now:
    // end a sounding note so MainStage never keeps a stuck note.
    sendEvents (noteMachine.allNotesOff());
    analysisReady = false;
    lastValid.store (false);
}

void AudioEngine::applyPendingSettings() noexcept
{
    const auto version = settingsVersion.load();
    if (version == appliedSettingsVersion)
        return;

    appliedSettingsVersion = version;

    const bool enabled = pendingOutputEnabled.load();
    if (! enabled && outputEnabled)
        sendEvents (noteMachine.allNotesOff());
    outputEnabled = enabled;

    auto updated = noteSettings;
    updated.midiChannel = pendingMidiChannel.load();
    updated.gateOpenDbfs = pendingGateOpenDbfs.load();
    updated.transposeSemitones = pendingTransposeSemitones.load();

    // A channel change ends the sounding note on its old channel instead of letting it hang.
    if (updated.midiChannel != noteSettings.midiChannel && noteMachine.getSoundingNote() >= 0)
        sendEvents (noteMachine.allNotesOff());

    if (noteMachine.setSettings (updated))
        noteSettings = updated;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                                    float* const* outputChannelData, int numOutputChannels,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    const auto startTicks = juce::Time::getHighResolutionTicks();

    // Bass2MIDI produces MIDI only; never pass audio through.
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    applyPendingSettings();

    const int channel = juce::jmin (requestedChannel.load (std::memory_order_relaxed), numInputChannels - 1);
    const float* input = channel >= 0 ? inputChannelData[channel] : nullptr;

    if (input != nullptr)
    {
        const auto range = juce::FloatVectorOperations::findMinAndMax (input, numSamples);
        const auto blockPeak = juce::jmax (-range.getStart(), range.getEnd());
        if (blockPeak > inputPeakLinear.load (std::memory_order_relaxed))
            inputPeakLinear.store (blockPeak, std::memory_order_relaxed);

        if (analysisReady)
            framer.push (input, numSamples, [this] (const float* window, std::int64_t, int)
            {
                handleFrame (window);
            });
    }

    const auto elapsed = juce::Time::getHighResolutionTicks() - startTicks;

    if (resetStatsRequested.exchange (false))
    {
        worstCallbackTicks.store (0);
        totalCallbackTicks.store (0);
        callbackCount.store (0);
    }

    if (elapsed > worstCallbackTicks.load (std::memory_order_relaxed))
        worstCallbackTicks.store (elapsed, std::memory_order_relaxed);
    totalCallbackTicks.fetch_add (elapsed, std::memory_order_relaxed);
    callbackCount.fetch_add (1, std::memory_order_relaxed);
}

void AudioEngine::handleFrame (const float* window) noexcept
{
    const auto candidate = estimator.estimate (window);
    lastValid.store (candidate.valid, std::memory_order_relaxed);
    lastFrequencyHz.store (candidate.frequencyHz, std::memory_order_relaxed);
    lastClarity.store (candidate.clarity, std::memory_order_relaxed);
    lastRmsLinear.store (candidate.windowRmsLinear, std::memory_order_relaxed);

    if (! outputEnabled)
        return;

    // Level of the samples that are new since the previous frame (the newest hop).
    const int windowLength = framer.getWindowSamples();
    const int hopLength = framer.getHopSamples();
    const auto newest = juce::FloatVectorOperations::findMinAndMax (window + (windowLength - hopLength), hopLength);
    const auto hopPeak = juce::jmax (-newest.getStart(), newest.getEnd());

    sendEvents (noteMachine.processFrame ({ candidate, (double) hopPeak }));
}

void AudioEngine::sendEvents (const bass2midi::NoteStateMachine::Output& output) noexcept
{
    for (int i = 0; i < output.count; ++i)
    {
        const auto& e = output.events[(size_t) i];
        if (e.type == bass2midi::NoteEvent::Type::noteOn)
        {
            midiSender.pushFromAudioThread (MidiOutputSender::noteOn (e.channel, e.note, e.velocity));
            lastVelocity.store (e.velocity, std::memory_order_relaxed);
            noteOnCount.fetch_add (1, std::memory_order_relaxed);
        }
        else
        {
            midiSender.pushFromAudioThread (MidiOutputSender::noteOff (e.channel, e.note));
        }
    }

    soundingNote.store (noteMachine.getSoundingOutputNote(), std::memory_order_relaxed);
    playedNote.store (noteMachine.getSoundingNote(), std::memory_order_relaxed);
}

AudioEngine::Snapshot AudioEngine::getSnapshot() const noexcept
{
    Snapshot s;
    s.valid = lastValid.load();
    s.frequencyHz = lastFrequencyHz.load();
    s.clarity = lastClarity.load();
    const auto rms = lastRmsLinear.load();
    s.windowRmsDbfs = rms > 1.0e-6 ? 20.0 * std::log10 (rms) : -120.0;
    s.sampleRateHz = sampleRateHz.load();
    s.blockSize = blockSize.load();
    s.windowSamples = windowSamples.load();
    s.hopSamples = hopSamples.load();
    s.soundingNote = soundingNote.load();
    s.playedNote = playedNote.load();
    s.lastVelocity = lastVelocity.load();
    s.noteOnCount = noteOnCount.load();

    const auto ticksPerMs = (double) juce::Time::getHighResolutionTicksPerSecond() / 1000.0;
    const auto count = callbackCount.load();
    s.worstCallbackMs = (double) worstCallbackTicks.load() / ticksPerMs;
    s.averageCallbackMs = count > 0 ? (double) totalCallbackTicks.load() / (double) count / ticksPerMs : 0.0;
    return s;
}
