#pragma once

#include "bass2midi/ExpectedNote.h"

#include <array>
#include <cstdint>

namespace bass2midi
{
    // Song Mode position follower: follows the player through the expected-note timeline of a song
    // by its attacks, not by a clock, and predicts which note an attack is.
    //
    // Position = index of the next expected note. Per attack (time t, seconds of live time):
    //  1. Timing: the score time elapsed since the previous attack, scaled by the live/score tempo
    //     ratio, is compared with the score distances to the next maxSkip notes. The next note is
    //     taken unless the attack comes clearly after the following note's time (skipMargin); then
    //     notes are skipped (their attacks were missed). Phrasing that differs from the written
    //     rhythm is common, so skipping needs clear evidence. An attack much earlier than the next
    //     note (< earlyAttackFraction of its distance) is treated as an extra attack (ghost note,
    //     fill): no prediction, the position stays. A gap far beyond the window is a pause: the
    //     next note is taken.
    //  2. The prediction is only issued while the confidence is >= minPredictConfidence; below it
    //     the caller falls back to Free-mode pitch detection, and the follower keeps tracking.
    //  3. Validation: the caller reports the pitch actually played (onPlayedNote) once the pitch
    //     path knows it. A match raises the confidence and refines the tempo ratio; a mismatch lowers
    //     it and searches +-resyncSearch notes for a place where the last resyncMatches played notes
    //     fit the score and - while the follower is still confident - whose score time fits the time
    //     since the last matched note; if one is found the position jumps there (re-alignment).
    //
    // Real-time: no allocation; the timeline is an immutable array owned by the caller, which must
    // keep it alive until a new one is set (see setTimeline). All methods are O(window) at most.
    class SongFollower
    {
    public:
        struct Settings
        {
            double initialConfidence = 0.8;    // after setPosition
            double minPredictConfidence = 0.5; // below: no predictions (Free fallback)
            double confidenceRate = 0.35;      // step towards 1 (match) or 0 (mismatch) per validated note
            int maxSkip = 3;                   // notes one attack may move ahead (missed attacks)
            double skipMargin = -0.5;          // skip note k only if the attack is later than note k+1's time by this
                                               // fraction of their spacing (-0.5 = nearest note in time)
            double earlyAttackFraction = 0.4;  // attack earlier than this fraction of the next distance: extra
            double pauseSeconds = 1.5;         // elapsed this far beyond the skip window: pause, take the next note
            double earlyMatchToleranceSeconds = 0.15; // an early pitch-path note equal to the next note counts as it
            double earlyMatchToleranceFraction = 0.25; // only this close to its expected time (larger of both)
            double minTempoRatio = 0.6;        // live/score tempo ratio bounds (1 = score tempo)
            double maxTempoRatio = 1.6;
            double tempoAdapt = 0.3;           // EMA step of the tempo ratio per matched note pair
            double minTempoSpanSeconds = 0.08; // score distance needed to update the tempo ratio
            int resyncSearch = 8;              // notes searched before/after the position on a mismatch
            int resyncMatches = 2;             // played notes that must fit the score to re-align
            double resyncTimeToleranceSeconds = 0.5; // ... and the place must fit the time since the last match
            double resyncTimeToleranceFraction = 0.25; // (whichever tolerance is larger; short riffs repeat often)

            bool isValid() const noexcept;
        };

        struct Prediction
        {
            bool valid = false;   // a note to send now
            int index = -1;       // timeline index the attack was assigned to (-1: extra attack)
            int midiNote = -1;
        };

        bool setSettings (const Settings& s) noexcept;
        const Settings& getSettings() const noexcept { return settings; }

        // Real-time safe (pointer swap only). Position 0, confidence initial.
        void setTimeline (const ExpectedNote* notes, int count) noexcept;
        // Real-time safe. The next attack is expected to be notes[index].
        void setPosition (int index) noexcept;

        Prediction onAttack (double timeSeconds) noexcept;

        // The latest attack turned out to be unpitched (a dead-note click): restore the position it
        // consumed. Only the latest onAttack can be undone.
        void undoLastAttack() noexcept;

        // The pitch path's verdict for a played note. `attackTimeSeconds`: when it was attacked.
        // `predictedIndex`: the index onAttack assigned to that attack, or -1 for a note the attack
        // detector missed (Free-mode note) - it is then placed by timing like an attack.
        void onPlayedNote (int midiNote, double attackTimeSeconds, int predictedIndex) noexcept;

        // Diagnostics.
        int getPosition() const noexcept { return position; }
        int getCount() const noexcept { return count; }
        double getConfidence() const noexcept { return confidence; }
        double getTempoRatio() const noexcept { return tempoRatio; }
        const ExpectedNote* noteAt (int index) const noexcept { return index >= 0 && index < count ? notes + index : nullptr; }
        int getMatches() const noexcept { return matches; }
        int getMismatches() const noexcept { return mismatches; }
        int getResyncs() const noexcept { return resyncs; }
        int getExtraAttacks() const noexcept { return extraAttacks; }

    private:
        int chooseIndex (double timeSeconds, bool& extra) const noexcept;
        void markAttack (int index, double timeSeconds) noexcept;
        bool tryResync (int around) noexcept;

        Settings settings;
        const ExpectedNote* notes = nullptr;
        int count = 0;

        int position = 0;
        double confidence = 0.8, tempoRatio = 1.0;
        int refIndex = -1;            // timeline index of the latest attack
        double refTime = 0.0;
        int lastMatchedIndex = -1;    // for the tempo estimate
        double lastMatchedTime = 0.0;

        static constexpr int historySize = 8;
        std::array<int, historySize> playedHistory {}; // last played notes (validated pitch), newest last
        std::array<double, historySize> playedTimes {};
        int historyCount = 0;

        int matches = 0, mismatches = 0, resyncs = 0, extraAttacks = 0;
        bool undoAvailable = false;
        int undoPosition = 0, undoRefIndex = -1;
        double undoRefTime = 0.0;
    };
}
