#pragma once

namespace bass2midi
{
    // One attacked note of a song's bass part in playing order (repeats unrolled, ties merged,
    // simultaneous notes reduced to the lowest), as consumed by the Song Mode follower. Times are
    // at the score's own tempo; the follower scales them to the live tempo.
    struct ExpectedNote
    {
        int midiNote = 0;
        double startSeconds = 0.0;     // from the start of the unrolled song
        double durationSeconds = 0.0;  // written length incl. tied continuations
        int bar = 0;                   // written (score) bar index, 0-based, for display
        double beat = 1.0;             // 1-based beat within the bar, in units of the time-signature denominator
        int playedBar = 0;             // index of its bar in playing order (see TimelineBar)
    };

    // One bar in playing order (repeats unrolled), for bar-wise following and bar playback. Times at
    // the score's own tempo. Its notes are notes[firstNote .. firstNote + noteCount) of the timeline.
    struct TimelineBar
    {
        int writtenBar = 0;            // 0-based score bar index
        double startSeconds = 0.0;
        double lengthSeconds = 2.0;
        int numerator = 4;             // beats per bar (for display / beat length)
        int firstNote = 0;
        int noteCount = 0;
    };
}
