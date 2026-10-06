#pragma once

#include "bass2midi/ExpectedNote.h"
#include "bass2midi/NoteStateMachine.h"

#include <array>
#include <cstdint>

namespace bass2midi
{
    // Bar playback ("Takt-Modus"): the player plays the bar's anchor note - usually the root on the
    // downbeat - and Bass2MIDI plays the song's complete bass part for that bar, fills included,
    // from the Guitar Pro timeline.
    //
    // This is player-synchronised playback, not transcription. Its limits are the safety rules:
    //  - Every bar starts only with an attack of the player near the bar's expected anchor time
    //    (the first attacked note of the bar; on beat 1 for most bars). Attacks elsewhere in the bar
    //    are ignored here.
    //  - Playback never runs more than one bar ahead: when no anchor attack arrives within
    //    its expected time (earlyToleranceBeats before .. lateToleranceBeats after), playback stops
    //    (Note Off) and waits. The early side is narrow: a player's own fill an eighth before the
    //    downbeat must not be taken as the downbeat.
    //  - The live tempo is learned from the spacing of anchor attacks (ratio to the score tempo);
    //    the first bar uses the score tempo times the last known ratio.
    //  - Bar tracking always runs. Output is trusted playback only while the played anchor pitches
    //    agree with the score: rootMismatchesToFallback consecutive contradicting verdicts (pitch
    //    class, octave ignored) switch to fallback - the caller then outputs Free-mode notes - and
    //    rootMatchesToResume consecutive matching verdicts resume playback.
    //  - Empty bars (rests) are passed through on the clock, at most maxEmptyBars in a row;
    //    longer rests stop playback (a break: restart at the next anchor attack).
    //  - After a stop the bar clock keeps running at the learned tempo: an attack that falls on the
    //    anchor time of a later bar (within maxResumeBars) resumes there - the band played on while
    //    the player paused. Any other attack starts the awaited bar: the player was late (slower
    //    than the clock) or paused, and playback follows the player. A late anchor within the tempo
    //    range also teaches the tempo (from the anchor before the stop).
    //
    // Real-time: no allocation; the timeline arrays are owned by the caller (see SongFollower).
    class BarPlayer
    {
    public:
        struct Settings
        {
            double earlyToleranceBeats = 0.2;   // anchor attack accepted this far before its expected time
            double lateToleranceBeats = 0.4;    // ... and this far after it
            double firstSpacingToleranceBars = 0.12; // until the live tempo is known (first bar change): +- this
                                                    // fraction of a bar, the band may be far from the file's tempo
            double minToleranceSeconds = 0.08;
            double tempoAdapt = 0.5;            // EMA step of the tempo ratio per bar
            double minTempoRatio = 0.6, maxTempoRatio = 1.6;
            double maxTempoStep = 0.15;         // largest ratio change per bar (a wrong anchor must not wreck the clock)
            int rootMismatchesToFallback = 2;
            int rootMatchesToResume = 2;
            int maxEmptyBars = 2;
            double minNoteSeconds = 0.03;       // shortest scheduled note
            int maxResumeBars = 16;             // bars the clock runs on after a stop
            double resumeDriftUnknownTempo = 0.12; // re-entry tolerance grows with the time bridged: this fraction
            double resumeDriftKnownTempo = 0.03;   // of it while the live tempo is unknown / known
            int maxAnchorVelocityDrop = 30;     // an anchor attack may be at most this many velocity units (~10 dB)
                                                // weaker than the previous anchor: precursor clicks and soft fills
                                                // inside the window are not downbeats

            bool isValid() const noexcept;
        };

        enum class State : std::uint8_t { idle, waiting, playing };

        static constexpr int maxEvents = 16;
        struct Output
        {
            std::array<NoteEvent, maxEvents> events {};
            int count = 0;
        };

        bool setSettings (const Settings& s) noexcept;
        void setTimeline (const ExpectedNote* notes, int noteCount, const TimelineBar* bars, int barCount) noexcept;
        void setOutput (int midiChannel, int transposeSemitones) noexcept { channel = midiChannel; transpose = transposeSemitones; }

        // Waits for the anchor attack of played bar `bar` (index in playing order). Ends a sounding note.
        void start (int bar, Output& out) noexcept;
        // Stops playback (Note Off) and goes idle.
        void stop (Output& out) noexcept;

        // An attack at `timeSeconds`. Returns true if it was taken as an anchor (a bar started).
        bool onAttack (double timeSeconds, int velocity, Output& out) noexcept;
        // The pitch path's verdict for the latest anchor attack.
        void onAnchorPitch (int playedMidiNote) noexcept;
        // Emits scheduled notes due by `nowSeconds`, and stops if an expected anchor did not come.
        void advance (double nowSeconds, Output& out) noexcept;

        // Diagnostics.
        State getState() const noexcept { return state; }
        bool isTrusted() const noexcept { return trusted; }      // false: caller outputs Free-mode notes
        bool isOutputting() const noexcept { return state == State::playing && trusted; }
        int getBar() const noexcept { return bar; }              // bar playing / waited for (playing order)
        int getWrittenBar() const noexcept { return bar >= 0 && bar < barCount ? bars[bar].writtenBar : -1; }
        double getTempoRatio() const noexcept { return tempoRatio; }
        int getSoundingNote() const noexcept { return soundingNote; } // output note, -1 none
        int getBarsStarted() const noexcept { return barsStarted; }
        int getStops() const noexcept { return stops; }
        int getRootMatches() const noexcept { return rootMatches; }
        int getRootMismatches() const noexcept { return rootMismatches; }
        int getFallbacks() const noexcept { return fallbacks; }
        double getNextAnchorSeconds() const noexcept { return nextAnchorTime; } // live time expected, < 0 none
        // Score position for display: inside the playing bar at the live tempo (never past its end);
        // while waiting, the awaited bar's anchor note. -1 when idle.
        double getScoreSecondsAt (double nowSeconds) const noexcept;

    private:
        int nextNonEmptyBar (int from) const noexcept;
        double anchorOffset (int b) const noexcept; // score seconds from bar start to its first note
        void startBar (int b, double anchorTime, int velocity, Output& out, int lateAfterBar = -1) noexcept;
        void noteOff (Output& out) noexcept;
        void noteOn (int midiNote, int velocity, Output& out) noexcept;
        void push (Output& out, NoteEvent e) noexcept;
        void computeNextAnchor() noexcept;
        int resumeBar (double timeSeconds) const noexcept; // -1: not on the clock's grid

        Settings settings;
        const ExpectedNote* notes = nullptr;
        const TimelineBar* bars = nullptr;
        int noteCount = 0, barCount = 0;
        int channel = 1, transpose = 0;

        State state = State::idle;
        int bar = 0;                    // current (playing) or awaited bar
        double barStartTime = 0.0;      // live time of the current bar's start (score start mapped)
        double anchorTime = 0.0;        // live time of the current bar's anchor attack
        double tempoRatio = 1.0;        // live tempo / score tempo
        int nextNote = 0;               // next note index to schedule in the current bar
        int velocity = 100;
        int nextBar = -1;               // bar whose anchor is expected next
        double nextAnchorTime = -1.0;
        double nextAnchorEarly = 0.1, nextAnchorLate = 0.1; // tolerances in live seconds
        int tempoUpdates = 0;           // the first anchor spacing sets the tempo directly
        bool clockRunning = false;      // a stop left a reference: anchorTime/bar of the last played bar
        int clockBar = -1;              // last played bar (its anchor is anchorTime)

        int soundingNote = -1, soundingChannel = 1;
        double soundingOffTime = 0.0;

        bool trusted = true;
        int consecutiveMismatches = 0, consecutiveMatches = 0;
        int anchorBarForVerdict = -1;

        int barsStarted = 0, stops = 0, rootMatches = 0, rootMismatches = 0, fallbacks = 0;
    };
}
