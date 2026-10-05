#pragma once

#include "bass2midi/BassToMidiProcessor.h"
#include "bass2midi/ExpectedNote.h"
#include "bass2midi/NotePalette.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

class MidiOutputSender;

// Audio-device callback: runs the BassToMidiProcessor (the same chain as the offline evaluation)
// on one selected active input channel and hands the resulting MIDI events to the MidiOutputSender.
//
// Free mode: MultiResolutionPitchTracker -> NoteStateMachine. Song Mode (a song timeline is set
// and enabled): additionally, bass attacks trigger the expected note of the song at once (see
// BassToMidiProcessor / SongFollower); the pitch path validates and corrects.
//
// Real-time rules: audioDeviceIOCallbackWithContext never allocates, locks, logs or touches the
// UI. User settings arrive through atomics and are applied at block start; everything the UI shows
// is published through relaxed atomics, read by a message-thread timer.
//
// Cross-thread boundaries:
//  - message thread -> audio thread: setAnalysedChannel, setMidiSettings (atomics + version
//    counter), setPalette (two 64-bit words under a sequence counter; a torn read is skipped and
//    retried at the next block, never waited for), setSongTimeline (atomic pointer to an immutable
//    timeline; the old one is freed on the message thread only after the audio thread has
//    acknowledged the new one), setMode, setSongPosition, setBarStart, score-follower commands
//    (atomics; a command counter makes each request apply exactly once)
//  - audio thread -> sender thread: MidiOutputSender::pushFromAudioThread (SPSC FIFO)
//  - audio thread -> message thread: Snapshot atomics
class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    explicit AudioEngine (MidiOutputSender& sender);
    ~AudioEngine() override;

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
        int paletteSize = 0;     // notes in the applied song palette, 0 = Free mode
        int paletteRelievedFrames = 0; // frames decided early thanks to the palette, since start
        int paletteDelayed = 0;  // decisions the palette postponed (off-palette notes), since start

        // Song Mode
        bool songActive = false;
        int songPosition = 0;            // index of the next expected note
        int songNoteCount = 0;
        double songConfidence = 0.0, songTempoRatio = 1.0;
        int attackCount = 0, lastAttackVelocity = 0;
        int lastPredictedNote = -1, lastValidatedNote = -1, lastValidationMatch = -1;
        double lastTriggerLatencyMs = -1.0; // attack detection -> MIDI queued (block end)
        int predictions = 0, corrections = 0, resyncs = 0, extraAttacks = 0, ghostAttacks = 0, unpitchedAttacks = 0;

        // Bar playback
        bool barActive = false, barOutputting = false;
        int barWritten = -1, barsStarted = 0, barStops = 0, barFallbacks = 0, rootMatches = 0, rootMismatches = 0;
        double barTempoRatio = 1.0;

        // Score follower
        bool scoreRunning = false, scoreArmed = false;
        double scoreSeconds = 0.0, scoreBeat = 1.0, scoreBpm = 0.0, scoreConfidence = 0.0;
        int scorePlayedBar = -1, scoreWrittenBar = -1, scoreNextNote = -1;
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

    // Message thread. Song palette in played-note space (before transposition); empty = Free mode.
    // Applied by the audio thread at the next block; a sounding note is never ended by it.
    void setPalette (const bass2midi::NotePalette& palette) noexcept;

    // Message thread. The song's expected-note timeline for Song Mode (empty = none). Copied; the
    // audio thread switches at the next block and restarts at note 0.
    void setSongTimeline (std::vector<bass2midi::ExpectedNote> notes, std::vector<bass2midi::TimelineBar> bars);
    // Message thread. Song Mode on/off (needs a timeline to have an effect).
    enum class Mode { free = 0, song = 1, bar = 2 };
    void setMode (Mode mode) noexcept { pendingMode.store ((int) mode); }
    // Message thread. Bar playback waits for the downbeat of played bar `bar`.
    void setBarStart (int bar) noexcept { pendingBarStart.store (juce::jmax (0, bar)); }
    // Message thread. Score follower: arm at played bar `bar` with the expected BPM (the clock starts
    // with the first played note), start the clock now, or stop it.
    void armScoreFollower (int bar, double bpm) noexcept
    {
        pendingScoreBar.store (juce::jmax (0, bar));
        pendingScoreBpm.store (bpm);
        pendingScoreCommand.store (1);
        scoreCommandCounter.fetch_add (1);
    }
    void startScoreClock() noexcept { pendingScoreCommand.store (2); scoreCommandCounter.fetch_add (1); }
    void stopScoreFollower() noexcept { pendingScoreCommand.store (3); scoreCommandCounter.fetch_add (1); }
    // Message thread. Experimental re-pluck detection for Song Mode (periodicity break).
    void setRepluckDetection (bool enabled) noexcept { pendingRepluck.store (enabled); }
    // Message thread. The next attack is expected to be timeline note `index`.
    void setSongPosition (int index) noexcept { pendingSongPosition.store (juce::jmax (0, index)); }
    // Message thread. Frees timelines the audio thread no longer uses (call from a timer).
    void releaseRetiredTimelines();

    // Message thread. Highest absolute sample value of the analysed channel since the last call.
    float takeInputPeakLinear() noexcept { return inputPeakLinear.exchange (0.0f); }
    void resetCallbackStats() noexcept { resetStatsRequested.store (true); }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const* outputChannelData, int numOutputChannels,
                                           int numSamples, const juce::AudioIODeviceCallbackContext& context) override;

private:
    struct Timeline
    {
        std::vector<bass2midi::ExpectedNote> notes;
        std::vector<bass2midi::TimelineBar> bars;
    };

    void applyPendingSettings() noexcept;
    void applyPendingPalette() noexcept;
    void applyPendingSong() noexcept;
    void send (const bass2midi::BassToMidiProcessor::Output& output) noexcept;
    void publishDiagnostics() noexcept;

    MidiOutputSender& midiSender;

    bass2midi::BassToMidiProcessor processor;
    bool analysisReady = false;
    bool outputEnabled = true;
    int appliedSettingsVersion = -1;
    int appliedMidiChannel = 1;

    std::atomic<int> requestedChannel { 0 };
    std::atomic<float> inputPeakLinear { 0.0f };

    std::atomic<bool> pendingOutputEnabled { true };
    std::atomic<int> pendingMidiChannel { 1 };
    std::atomic<double> pendingGateOpenDbfs { -45.0 };
    std::atomic<int> pendingTransposeSemitones { 0 };
    std::atomic<int> settingsVersion { 0 };

    // Seqlock for the palette: odd = write in progress.
    std::atomic<std::uint32_t> paletteSequence { 0 };
    std::atomic<std::uint64_t> pendingPaletteLow { 0 }, pendingPaletteHigh { 0 };
    std::uint32_t appliedPaletteSequence = 0; // audio thread
    bool forcePaletteApply = true;            // audio thread (set in audioDeviceAboutToStart)

    // Song Mode. Timelines are owned by the message thread (`timelines`); the audio thread only
    // reads the one published in pendingTimeline and reports the one it uses in appliedTimeline.
    std::vector<std::unique_ptr<Timeline>> timelines;  // message thread
    std::atomic<const Timeline*> pendingTimeline { nullptr };
    std::atomic<const Timeline*> appliedTimeline { nullptr };
    const Timeline* currentTimeline = nullptr;          // audio thread
    bool forceTimelineApply = true;                     // audio thread
    std::atomic<int> pendingMode { 0 };
    std::atomic<int> pendingBarStart { -1 };
    std::atomic<int> pendingScoreBar { 0 }, pendingScoreCommand { 0 }, scoreCommandCounter { 0 };
    std::atomic<double> pendingScoreBpm { 120.0 };
    int appliedScoreCommandCounter = 0; // audio thread
    std::atomic<bool> pendingRepluck { false };
    std::atomic<int> pendingSongPosition { -1 };

    // Snapshot.
    std::atomic<bool> lastValid { false };
    std::atomic<double> lastFrequencyHz { 0.0 }, lastClarity { 0.0 }, lastRmsLinear { 0.0 };
    std::atomic<double> sampleRateHz { 0.0 };
    std::atomic<int> blockSize { 0 }, windowSamples { 0 }, hopSamples { 0 }, lastRungWindowSamples { 0 };
    std::atomic<int> soundingNote { -1 }, playedNote { -1 }, lastVelocity { 0 }, noteOnCount { 0 };
    std::atomic<int> paletteSize { 0 }, paletteRelievedFrames { 0 }, paletteDelayed { 0 };
    std::atomic<bool> songActive { false };
    std::atomic<int> songPosition { 0 }, songNoteCount { 0 }, attackCount { 0 }, lastAttackVelocity { 0 };
    std::atomic<int> lastPredictedNote { -1 }, lastValidatedNote { -1 }, lastValidationMatch { -1 };
    std::atomic<double> songConfidence { 0.0 }, songTempoRatio { 1.0 }, lastTriggerLatencyMs { -1.0 };
    std::atomic<int> predictions { 0 }, corrections { 0 }, resyncs { 0 }, extraAttacks { 0 }, ghostAttacks { 0 }, unpitchedAttacks { 0 };
    std::atomic<bool> barActive { false }, barOutputting { false }, scoreRunning { false }, scoreArmed { false };
    std::atomic<int> barWritten { -1 }, barsStarted { 0 }, barStops { 0 }, barFallbacks { 0 }, rootMatches { 0 }, rootMismatches { 0 };
    std::atomic<double> barTempoRatio { 1.0 }, scoreSeconds { 0.0 }, scoreBeat { 1.0 }, scoreBpm { 0.0 }, scoreConfidence { 0.0 };
    std::atomic<int> scorePlayedBar { -1 }, scoreWrittenBar { -1 }, scoreNextNote { -1 };

    // Callback cost, measured with the high-resolution tick counter (lock- and allocation-free).
    std::atomic<std::int64_t> worstCallbackTicks { 0 }, totalCallbackTicks { 0 }, callbackCount { 0 };
    std::atomic<bool> resetStatsRequested { false };
};
