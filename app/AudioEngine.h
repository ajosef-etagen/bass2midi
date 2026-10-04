#pragma once

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/NoteStateMachine.h"
#include "bass2midi/MultiResolutionPitchTracker.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>

class MidiOutputSender;

// Audio-device callback: runs pitch analysis and the note state machine on one selected active
// input channel, and hands the resulting MIDI events to the MidiOutputSender.
//
// Pipeline per analysis frame: MultiResolutionPitchTracker (pitch-adaptive YIN windows over the V1
// range) -> NoteStateMachine -> lock-free MIDI FIFO.
//
// Real-time rules: audioDeviceIOCallbackWithContext never allocates, locks, logs or touches the
// UI. User settings arrive through atomics and are applied at block start; everything the UI shows
// is published through relaxed atomics, read by a message-thread timer.
//
// Cross-thread boundaries:
//  - message thread -> audio thread: setAnalysedChannel, setMidiSettings (atomics + version counter)
//  - audio thread -> sender thread: MidiOutputSender::pushFromAudioThread (SPSC FIFO)
//  - audio thread -> message thread: Snapshot atomics
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    // Analysis: MultiResolutionPitchTracker defaults (38-420 Hz) and a 2.5 ms hop - the same as the
    // offline "multires-yin+nsm" setup, so live behaviour matches the evaluation report.
    static constexpr double analysisHopSeconds = 0.0025;
    static constexpr double onsetSettleWindowFraction = 0.75; // of the analysis window, see NoteStateMachine

    explicit AudioEngine (MidiOutputSender& sender);

    struct Snapshot
    {
        bool valid = false;
        double frequencyHz = 0.0;
        double clarity = 0.0;
        double windowRmsDbfs = -120.0;
        double sampleRateHz = 0.0;
        int blockSize = 0;
        int windowSamples = 0;   // longest analysis window (frame)
        int lastRungWindowSamples = 0; // window of the rung that produced the latest estimate
        int hopSamples = 0;
        double worstCallbackMs = 0.0;
        double averageCallbackMs = 0.0;
        int soundingNote = -1;   // MIDI note currently sent (after transposition), -1 = none
        int playedNote = -1;     // detected note behind it
        int lastVelocity = 0;
        int noteOnCount = 0;     // since start, for the UI
    };

    // Message thread.
    Snapshot getSnapshot() const noexcept;

    // Message thread. Index among the device's *active* input channels (0 = first enabled
    // channel); clamped to the channels actually delivered by the device.
    void setAnalysedChannel (int activeChannelIndex) noexcept { requestedChannel.store (juce::jmax (0, activeChannelIndex)); }

    // Message thread. Applied by the audio thread at the next block. Disabling the output (or
    // changing the channel) ends a sounding note with a matching Note Off; a transposition change
    // applies from the next note (the sounding one keeps its output note until its Note Off).
    void setMidiSettings (bool outputEnabled, int midiChannel, double gateOpenDbfs, int transposeSemitones) noexcept;

    // Message thread. Highest absolute sample value of the analysed channel since the last call.
    float takeInputPeakLinear() noexcept { return inputPeakLinear.exchange (0.0f); }
    void resetCallbackStats() noexcept { resetStatsRequested.store (true); }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;

private:
    void applyPendingSettings() noexcept;
    void handleFrame (const float* window) noexcept;
    void sendEvents (const bass2midi::NoteStateMachine::Output& output) noexcept;

    MidiOutputSender& midiSender;

    bass2midi::AnalysisFramer framer;
    bass2midi::MultiResolutionPitchTracker tracker;
    bass2midi::NoteStateMachine noteMachine;
    bass2midi::NoteStateMachine::Settings noteSettings; // audio thread copy (frame timing set at device start)
    bool analysisReady = false;
    bool outputEnabled = true;
    int appliedSettingsVersion = -1;

    std::atomic<int> requestedChannel { 0 };
    std::atomic<float> inputPeakLinear { 0.0f };

    std::atomic<bool> pendingOutputEnabled { true };
    std::atomic<int> pendingMidiChannel { 1 };
    std::atomic<double> pendingGateOpenDbfs { -45.0 };
    std::atomic<int> pendingTransposeSemitones { 0 };
    std::atomic<int> settingsVersion { 0 };

    std::atomic<bool> lastValid { false };
    std::atomic<double> lastFrequencyHz { 0.0 }, lastClarity { 0.0 }, lastRmsLinear { 0.0 };
    std::atomic<double> sampleRateHz { 0.0 };
    std::atomic<int> blockSize { 0 }, windowSamples { 0 }, hopSamples { 0 }, lastRungWindowSamples { 0 };
    std::atomic<int> soundingNote { -1 }, playedNote { -1 }, lastVelocity { 0 }, noteOnCount { 0 };

    // Callback cost, measured with the high-resolution tick counter (lock- and allocation-free).
    std::atomic<std::int64_t> worstCallbackTicks { 0 }, totalCallbackTicks { 0 }, callbackCount { 0 };
    std::atomic<bool> resetStatsRequested { false };
};
