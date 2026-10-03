#include "AudioEngine.h"
#include "MidiOutputSender.h"

#include <cmath>

AudioEngine::AudioEngine (MidiOutputSender& sender) : midiSender (sender) {}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    // Called before callbacks start (not concurrently with them), so allocation is allowed here.
    const auto rate = device->getCurrentSampleRate();
    const auto config = bass2midi::YinPitchEstimator::Config::forFrequencyRange (rate, analysisLowestHz,
                                                                                 analysisHighestHz, yinThreshold);
    const auto hop = juce::jmax (1, (int) std::lround (rate * analysisHopSeconds));

    analysisReady = estimator.prepare (rate, config) && framer.prepare (config.windowSamples, hop);

    sampleRateHz.store (rate);
    blockSize.store (device->getCurrentBufferSizeSamples());
    windowSamples.store (config.windowSamples);
    hopSamples.store (hop);
    lastValid.store (false);
    resetStatsRequested.store (true);
}

void AudioEngine::audioDeviceStopped()
{
    analysisReady = false;
    lastValid.store (false);
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

    if (analysisReady && numInputChannels > 0 && inputChannelData[0] != nullptr)
        framer.push (inputChannelData[0], numSamples, [this] (const float* window, std::int64_t, int)
        {
            handleFrame (window);
        });

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

    const auto ticksPerMs = (double) juce::Time::getHighResolutionTicksPerSecond() / 1000.0;
    const auto count = callbackCount.load();
    s.worstCallbackMs = (double) worstCallbackTicks.load() / ticksPerMs;
    s.averageCallbackMs = count > 0 ? (double) totalCallbackTicks.load() / (double) count / ticksPerMs : 0.0;
    return s;
}
