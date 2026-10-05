#pragma once

#include "bass2midi/AnalysisFramer.h"
#include "bass2midi/AttackDetector.h"
#include "bass2midi/ExpectedNote.h"
#include "bass2midi/MultiResolutionPitchTracker.h"
#include "bass2midi/NotePalette.h"
#include "bass2midi/NoteStateMachine.h"
#include "bass2midi/SongFollower.h"

#include <array>
#include <cstdint>

namespace bass2midi
{
    // The complete bass-to-MIDI chain, shared by the app and the offline evaluation so both run
    // exactly the same code.
    //
    // Free mode (default): AnalysisFramer -> MultiResolutionPitchTracker -> NoteStateMachine, as
    // before; the attack detector only feeds diagnostics.
    //
    // Song mode (a timeline is set and the mode is song): additionally, every attack of the
    // AttackDetector asks the SongFollower which expected note it is; if the follower is confident,
    // the note is started at once (NoteStateMachine::triggerPredicted) with the attack's velocity -
    // the pitch path is not waited for. The pitch path then validates: the first clear, consistent
    // note after the attack (validationFrames frames at >= validationMinClarity within
    // validationWindowMs) is reported to the follower (confidence, tempo, re-alignment), and if it
    // contradicts the prediction the state machine corrects the note (see NoteStateMachine, 6.).
    // Notes started by the pitch path (attack missed, or confidence too low) are reported too, so
    // the follower keeps its place in either case.
    //
    // Timing: events are stamped with the sample position at the end of the processed block (that
    // is when the host can send them). Latency of a predicted note = attack detection (<= 1 ms sub-hop)
    // + velocity window (2 ms) + block quantisation.
    //
    // Real-time: prepare() allocates; everything else is allocation- and lock-free. Timeline
    // ownership: see setTimeline.
    class BassToMidiProcessor
    {
    public:
        enum class Mode : std::uint8_t { free, song };

        struct Settings
        {
            MultiResolutionPitchTracker::Settings tracker;
            NoteStateMachine::Settings notes;   // frame interval / settle time are derived in prepare()
            AttackDetector::Settings attacks;
            SongFollower::Settings follower;
            double hopSeconds = 0.0025;
            double onsetSettleWindowFraction = 0.75;
            double validationWindowMs = 200.0;
            int validationFrames = 3;
            double validationMinClarity = 0.9;
            double toleranceCents = 40.0;       // a validation frame must be this close to a semitone
            double ghostRelativeDb = 20.0;      // Song Mode ignores attacks this far below the previous accepted
                                                // attack (dead-note clicks); they are left to the pitch path
            double ghostReferenceSeconds = 3.0; // ... if that attack is at most this old
            double unpitchedDecisionMs = 20.0;  // a predicted attack with no voiced frame (clarity >= unpitchedClarity)
            double unpitchedClarity = 0.8;      // within the longest analysis window (~53 ms at 48 kHz: the tracker
                                                // needs a window after the attack) plus this time is taken back:
                                                // Note Off, follower position restored. A fixed 60 ms retracted real
                                                // low notes on the first DI takes.
        };

        struct TimedEvent
        {
            NoteEvent event;
            std::int64_t sample = 0;   // end of the block in which it was decided
            bool predicted = false;    // Song Mode note started by an attack (not by the pitch path)
            bool correction = false;   // Note On replacing a contradicted prediction
        };

        static constexpr int maxEventsPerBlock = 64;
        struct Output
        {
            std::array<TimedEvent, maxEventsPerBlock> events {};
            int count = 0;
        };

        struct Diagnostics
        {
            PitchCandidate pitch;           // latest frame
            int rungWindowSamples = 0;
            bool paletteRelieved = false;
            int soundingPlayedNote = -1, soundingOutputNote = -1, lastVelocity = 0;
            // Attacks
            std::int64_t lastAttackSample = -1;
            int lastAttackVelocity = 0;
            int attackCount = 0;
            // Song Mode
            bool songActive = false;
            int position = 0, timelineCount = 0;
            double confidence = 0.0, tempoRatio = 1.0;
            int lastPredictedIndex = -1, lastPredictedNote = -1;
            int lastValidatedNote = -1;     // pitch path's verdict for the latest attack
            int lastValidationMatch = -1;   // 1 match, 0 mismatch, -1 none yet
            std::int64_t lastTriggerLatencySamples = -1; // predicted Note On block end - attack detection
            int predictions = 0, corrections = 0, matches = 0, mismatches = 0, resyncs = 0, extraAttacks = 0, ghostAttacks = 0, unpitchedAttacks = 0;
        };

        // Non-real-time.
        bool prepare (double sampleRateHz, const Settings& settings);
        int getFrameSamples() const noexcept { return framer.getWindowSamples(); }
        int getHopSamples() const noexcept { return framer.getHopSamples(); }
        double getSampleRate() const noexcept { return sampleRateHz; }

        // Real-time safe setters.
        bool setNoteSettings (int midiChannel, double gateOpenDbfs, int transposeSemitones) noexcept;
        void setPalette (const NotePalette& palette) noexcept;
        void setMode (Mode newMode) noexcept { mode = newMode; }
        Mode getMode() const noexcept { return mode; }
        // The array must stay valid and unchanged until the next setTimeline call has returned
        // (the app retires old timelines on the message thread only after the audio thread has
        // acknowledged the swap). Resets the position to 0.
        void setTimeline (const ExpectedNote* notes, int count) noexcept;
        void setSongPosition (int index) noexcept;

        // Real-time safe. Appends events to `out` (stops when full).
        void process (const float* samples, int numSamples, Output& out) noexcept;

        // Ends a sounding note (stop, mode/device change).
        void allNotesOff (Output& out) noexcept;

        const Diagnostics& getDiagnostics() const noexcept { return diagnostics; }
        std::int64_t getSamplePosition() const noexcept { return samplePosition; }

    private:
        void handleFrame (const float* frame, std::int64_t frameEnd, std::int64_t blockEnd, Output& out) noexcept;
        void emit (const NoteStateMachine::Output& o, std::int64_t sample, bool predicted, bool correction, Output& out) noexcept;
        double seconds (std::int64_t sample) const noexcept { return static_cast<double> (sample) / sampleRateHz; }
        bool songActive() const noexcept { return mode == Mode::song && follower.getCount() > 0; }

        Settings settings;
        double sampleRateHz = 0.0;
        AnalysisFramer framer;
        MultiResolutionPitchTracker tracker;
        NoteStateMachine noteMachine;
        AttackDetector attackDetector;
        SongFollower follower;
        Mode mode = Mode::free;
        int validationFramesNeeded = 3, validationWindowSamples = 9600;

        std::int64_t samplePosition = 0;

        // Latest accepted attack (Song Mode): ghost reference, and frames before it are hidden from the
        // state machine's correction rule while a prediction is pending.
        double acceptedAttackPeak = 0.0;
        std::int64_t acceptedAttackSample = -(std::int64_t { 1 } << 40);

        // Validation of the latest attack (Song Mode).
        bool validating = false;
        std::int64_t validationAttackSample = 0;
        int validationIndex = -1;       // follower index assigned to the attack (-1: none)
        int validationCandidate = -1, validationRun = 0;
        bool validationPredicted = false, validationVoiced = false;
        int unpitchedDecisionSamples = 2880;

        Diagnostics diagnostics;
    };
}
