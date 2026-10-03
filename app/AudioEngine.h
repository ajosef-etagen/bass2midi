#pragma once

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/YinPitchEstimator.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>

class MidiOutputSender;

// Audio-device callback: conditions the first active input channel and runs pitch analysis.
//
// Phase 0 state: analysis is for monitoring only (reference YIN over the full V1 range). No MIDI
// is generated from audio yet - that waits for the authoritative note state machine (Phase 1).
//
// Real-time rules: audioDeviceIOCallbackWithContext never allocates, locks, logs or touches the
// UI. Everything the UI shows is published through relaxed atomics, read by a message-thread timer.
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    // Reference analysis range: E1 and G4 each with > 1 semitone margin. Same values as the
    // offline "full-range-yin" reference setup so live readings match the baseline report.
    static constexpr double analysisLowestHz = 38.0;
    static constexpr double analysisHighestHz = 420.0;
    static constexpr double analysisHopSeconds = 0.0025;
    static constexpr double yinThreshold = 0.15;

    explicit AudioEngine (MidiOutputSender& sender);

    struct Snapshot
    {
        bool valid = false;
        double frequencyHz = 0.0;
        double clarity = 0.0;
        double windowRmsDbfs = -120.0;
        double sampleRateHz = 0.0;
        int blockSize = 0;
        int windowSamples = 0;
        int hopSamples = 0;
        double worstCallbackMs = 0.0;
        double averageCallbackMs = 0.0;
    };

    // Message thread.
    Snapshot getSnapshot() const noexcept;
    void resetCallbackStats() noexcept { resetStatsRequested.store (true); }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;

private:
    void handleFrame (const float* window) noexcept;

    [[maybe_unused]] MidiOutputSender& midiSender; // used once the note state machine exists

    bass2midi::AnalysisFramer framer;
    bass2midi::YinPitchEstimator estimator;
    bool analysisReady = false;

    std::atomic<bool> lastValid { false };
    std::atomic<double> lastFrequencyHz { 0.0 }, lastClarity { 0.0 }, lastRmsLinear { 0.0 };
    std::atomic<double> sampleRateHz { 0.0 };
    std::atomic<int> blockSize { 0 }, windowSamples { 0 }, hopSamples { 0 };

    // Callback cost, measured with the high-resolution tick counter (lock- and allocation-free).
    std::atomic<std::int64_t> worstCallbackTicks { 0 }, totalCallbackTicks { 0 }, callbackCount { 0 };
    std::atomic<bool> resetStatsRequested { false };
};
