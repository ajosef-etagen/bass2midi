#pragma once

#include "bass2midi/ExpectedNote.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace bass2midi
{
    // Live score follower: where in the song is the band now?
    //
    // A tempo clock (started at a bar with the BPM the player expects) moves the position; notes the
    // player actually plays correct it. Several position hypotheses run in parallel, each with its
    // own score position, tempo and log-likelihood:
    //
    //  - Every detected note (pitch and attack time) is compared, per hypothesis, with the written
    //    notes near that hypothesis's position: likelihood = best match (same pitch 1, octave 0.6)
    //    x timing closeness (Gaussian, timingSigmaSeconds) x the note's anchor weight (downbeats,
    //    strong beats and long notes count, fills hardly). Notes the score does not have (fills the
    //    player adds) match no hypothesis and change nothing.
    //  - A matching hypothesis is pulled towards the written note's time (correctionGain x anchor
    //    weight) and refines its tempo from the spacing of its matches.
    //  - Hypotheses far below the best are replaced: by small variations of the best one, or by
    //    jump candidates where the played note matches a strong anchor within +-jumpSearchBars (a
    //    skipped or repeated section). A jump candidate starts below the best and must earn its weight.
    //    When the best hypothesis itself explains missStreakForJumps notes in a row poorly, the
    //    weakest hypotheses are replaced by jump candidates regardless of their relative weight (all
    //    hypotheses near a wrong place fail alike, so relative weights alone would never trigger it).
    //
    //  - The clock waits for the player: a hypothesis's position never runs more than holdBeats past
    //    the next written note it expects the player to play (the next note after its last match
    //    with anchor weight >= waitMinAnchorWeight: downbeats and half bars; fills are passed). There it stops
    //    until the player plays again: a player who slows down or pauses holds the position, and
    //    the next played note continues from the awaited note (matched within a bar ahead). A short wait
    //    (<= pauseBeats) still measures the tempo, so a slower player is learned; a long pause does not.
    //
    // The best hypothesis gives the position; its weight share among the hypotheses agreeing with
    // it (within a quarter beat) is the confidence.
    //
    // Real-time: fixed arrays, no allocation; per note O(hypotheses x notes in the matching window).
    // The timeline arrays are owned by the caller (same rule as SongFollower).
    class ScorePositionTracker
    {
    public:
        static constexpr int maxHypotheses = 12;

        struct Settings
        {
            double timingSigmaSeconds = 0.08;   // timing spread of a played note around its written time
            double matchWindowBeats = 1.0;      // written notes considered around a hypothesis's position
            double octaveMatch = 0.6;           // likelihood of the right pitch class in another octave
            double background = 0.05;           // likelihood floor (a note the score does not explain)
            double correctionGain = 0.6;        // fraction of the timing error corrected (x anchor weight)
            double tempoAdapt = 0.5;            // EMA step of a hypothesis's tempo per matched spacing (x anchor weight)
            double minTempoSpanSeconds = 1.0;   // score span between matches needed to update the tempo
            double minTempoRatio = 0.5, maxTempoRatio = 2.0;
            double replaceBelowLog = 6.0;       // hypotheses this far (natural log) below the best are replaced
            double jumpPenaltyLog = 3.0;        // a jump candidate starts this far below the best
            double jumpPenaltyPerBar = 0.15;    // ... plus this per bar of jump distance (prefer the nearer of
                                                // musically identical places)
            int jumpSearchBars = 16;
            double jumpMinAnchorWeight = 0.7;   // only strong anchors are jump targets
            int missStreakForJumps = 6;         // poorly explained notes in a row that force jump candidates (2-4 jumped
                                                // wrongly on a live take with many variations)
            double poorLikelihood = 0.15;       // "poorly explained" for the best hypothesis
            int forcedJumps = 3;                // hypotheses replaced then
            double holdBeats = 1.0;             // the clock runs at most this far past the awaited note, then waits
            double pauseBeats = 2.0;            // a wait longer than this (live beats) is a pause: no tempo measured across it
            double waitMinAnchorWeight = 0.7;   // notes the clock waits for (0.7: downbeats and half-bar beats; waiting
                                                // on every beat lost the position on a live take full of variations)

            bool isValid() const noexcept;
        };

        struct Position
        {
            bool running = false;
            double scoreSeconds = 0.0;   // position in the timeline's (unrolled) score time
            int playedBar = -1;          // bar in playing order
            int writtenBar = -1;         // 0-based score bar
            double beat = 1.0;           // 1-based beat in the bar
            double bpm = 0.0;            // live tempo (quarter = beat of the bar's denominator) at this bar
            double tempoRatio = 1.0;     // live / score tempo
            double confidence = 0.0;     // 0..1
            int nextNote = -1;           // first timeline note at or after the position
            bool waiting = false;        // the clock stopped at the awaited note: the player has not played it yet
        };

        bool setSettings (const Settings& s) noexcept;
        void setTimeline (const ExpectedNote* notes, int noteCount, const TimelineBar* bars, int barCount) noexcept;

        // Arms the clock at played bar `bar` with the expected live tempo in BPM (beats of that bar's
        // time signature). The clock starts with the first played note (aligned to the bar's first
        // written note) or with begin().
        void arm (int bar, double bpm) noexcept;
        // Starts the clock now at the armed bar's start (e.g. on a count-in or a Start button).
        void begin (double timeSeconds) noexcept;
        void stop() noexcept { running = false; armed = false; }

        // A note the player played (pitch path verdict) with its attack time.
        void onNote (double attackTimeSeconds, int midiNote) noexcept;

        Position positionAt (double timeSeconds) const noexcept;

        bool isRunning() const noexcept { return running; }
        bool isArmed() const noexcept { return armed; }
        int getJumps() const noexcept { return jumps; }        // times the best hypothesis was a jump candidate
        int getNotesUsed() const noexcept { return notesUsed; }

    private:
        struct Hypothesis
        {
            double anchorScore = 0.0, anchorTime = 0.0, ratio = 1.0, logWeight = 0.0;
            double lastMatchScore = -1.0, lastMatchTime = 0.0; // for tempo refinement
            double awaitedScore = 1.0e300, holdScore = 1.0e300; // the note the clock waits for, and where it stops
            bool jump = false;
            double freeScoreAt (double t) const noexcept { return anchorScore + (t - anchorTime) * ratio; }
            double scoreAt (double t) const noexcept { return std::min (freeScoreAt (t), holdScore); }
            bool waitingAt (double t) const noexcept { return freeScoreAt (t) >= holdScore; }
        };

        int barAtScore (double scoreSeconds) const noexcept;
        double beatSecondsAt (int bar) const noexcept;
        void seed (double scoreSeconds, double timeSeconds, double ratio) noexcept;
        int best() const noexcept;
        // `resumed`: the hypothesis waited for the player; the note may be any note up to a bar ahead.
        double likelihood (const Hypothesis& h, double t, int midiNote, int& matchedNote, bool resumed = false) const noexcept;
        void setHold (Hypothesis& h) const noexcept;

        Settings settings;
        const ExpectedNote* notes = nullptr;
        const TimelineBar* bars = nullptr;
        int noteCount = 0, barCount = 0;

        std::array<Hypothesis, maxHypotheses> hypotheses {};
        int hypothesisCount = 0;
        bool armed = false, running = false;
        int armedBar = 0;
        double armedRatio = 1.0;
        int jumpCursor = 0, jumps = 0, notesUsed = 0;
        bool bestWasJump = false;
        int missStreak = 0;
        bool makeJumpCandidate (Hypothesis& h, double t, int midiNote, const Hypothesis& bestH, double bestScore, int bestBar, double beat) noexcept;
    };
}
