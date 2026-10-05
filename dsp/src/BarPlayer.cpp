#include "bass2midi/BarPlayer.h"

#include <algorithm>
#include <cmath>

namespace bass2midi
{
    bool BarPlayer::Settings::isValid() const noexcept
    {
        const auto positive = [] (double v) { return std::isfinite (v) && v > 0.0; };
        return positive (earlyToleranceBeats) && earlyToleranceBeats < 2.0 && positive (lateToleranceBeats) && lateToleranceBeats < 2.0
            && std::isfinite (minToleranceSeconds) && minToleranceSeconds >= 0.0
            && std::isfinite (tempoAdapt) && tempoAdapt >= 0.0 && tempoAdapt <= 1.0
            && positive (minTempoRatio) && std::isfinite (maxTempoRatio) && maxTempoRatio >= minTempoRatio
            && std::isfinite (maxTempoStep) && maxTempoStep >= 0.0
            && std::isfinite (firstSpacingToleranceBars) && firstSpacingToleranceBars >= 0.0 && firstSpacingToleranceBars < 0.5
            && rootMismatchesToFallback >= 1 && rootMatchesToResume >= 1
            && maxEmptyBars >= 0 && maxEmptyBars <= 64
            && maxResumeBars >= 0 && maxResumeBars <= 256
            && maxAnchorVelocityDrop >= 0 && maxAnchorVelocityDrop <= 127
            && std::isfinite (resumeDriftUnknownTempo) && resumeDriftUnknownTempo >= 0.0 && resumeDriftUnknownTempo < 0.5
            && std::isfinite (resumeDriftKnownTempo) && resumeDriftKnownTempo >= 0.0 && resumeDriftKnownTempo < 0.5
            && std::isfinite (minNoteSeconds) && minNoteSeconds >= 0.0;
    }

    bool BarPlayer::setSettings (const Settings& s) noexcept
    {
        if (! s.isValid())
            return false;
        settings = s;
        tempoRatio = std::clamp (tempoRatio, settings.minTempoRatio, settings.maxTempoRatio);
        return true;
    }

    void BarPlayer::setTimeline (const ExpectedNote* newNotes, int newNoteCount, const TimelineBar* newBars, int newBarCount) noexcept
    {
        const bool valid = newNotes != nullptr && newBars != nullptr && newNoteCount > 0 && newBarCount > 0;
        notes = valid ? newNotes : nullptr;
        bars = valid ? newBars : nullptr;
        noteCount = valid ? newNoteCount : 0;
        barCount = valid ? newBarCount : 0;
        state = State::idle;
        soundingNote = -1; // the caller ends any sounding note before replacing the timeline
        tempoRatio = std::clamp (1.0, settings.minTempoRatio, settings.maxTempoRatio);
    }

    void BarPlayer::push (Output& out, NoteEvent e) noexcept
    {
        if (out.count < maxEvents)
            out.events[static_cast<std::size_t> (out.count++)] = e;
    }

    void BarPlayer::noteOff (Output& out) noexcept
    {
        if (soundingNote >= 0)
            push (out, { NoteEvent::Type::noteOff, soundingChannel, soundingNote, 0 });
        soundingNote = -1;
    }

    void BarPlayer::noteOn (int midiNote, int noteVelocity, Output& out) noexcept
    {
        const int outputNote = midiNote + transpose;
        noteOff (out);
        if (outputNote < 0 || outputNote > 127)
            return;
        push (out, { NoteEvent::Type::noteOn, channel, outputNote, std::clamp (noteVelocity, 1, 127) });
        soundingNote = outputNote;
        soundingChannel = channel;
    }

    int BarPlayer::nextNonEmptyBar (int from) const noexcept
    {
        for (int b = from; b < barCount && b <= from + settings.maxEmptyBars; ++b)
            if (bars[b].noteCount > 0)
                return b;
        return -1;
    }

    double BarPlayer::anchorOffset (int b) const noexcept
    {
        return bars[b].noteCount > 0 ? notes[bars[b].firstNote].startSeconds - bars[b].startSeconds : 0.0;
    }

    void BarPlayer::start (int startBar, Output& out) noexcept
    {
        noteOff (out);
        if (barCount == 0)
        {
            state = State::idle;
            return;
        }
        startBar = std::clamp (startBar, 0, barCount - 1);
        int b = startBar;
        while (b < barCount && bars[b].noteCount == 0)
            ++b;
        if (b >= barCount)
        {
            state = State::idle;
            return;
        }
        bar = b;
        nextBar = b;
        nextAnchorTime = -1.0; // any attack starts it
        state = State::waiting;
        clockRunning = false;
        tempoUpdates = 0;
        trusted = true;
        consecutiveMatches = consecutiveMismatches = 0;
        anchorBarForVerdict = -1;
    }

    int BarPlayer::resumeBar (double timeSeconds) const noexcept
    {
        if (! clockRunning || clockBar < 0)
            return bar;
        const double reference = bars[clockBar].startSeconds + anchorOffset (clockBar);
        int best = -1;
        double bestDistance = 1.0e9;
        for (int b = clockBar + 1; b < barCount && b <= clockBar + settings.maxResumeBars; ++b)
        {
            if (bars[b].noteCount == 0)
                continue;
            const double expected = anchorTime + (bars[b].startSeconds + anchorOffset (b) - reference) / tempoRatio;
            const double beatLive = bars[b].lengthSeconds / std::max (1, bars[b].numerator) / tempoRatio;
            // The clock extrapolates: its error grows with the time bridged, more so before the live
            // tempo has been measured even once.
            const double drift = (tempoUpdates == 0 ? settings.resumeDriftUnknownTempo : settings.resumeDriftKnownTempo)
                               * std::max (0.0, expected - anchorTime);
            const double early = std::max ({ settings.minToleranceSeconds, settings.earlyToleranceBeats * beatLive, drift });
            const double late = std::max ({ settings.minToleranceSeconds, settings.lateToleranceBeats * beatLive, drift });
            const double distance = std::abs (timeSeconds - expected);
            if (timeSeconds >= expected - early && timeSeconds <= expected + late && distance < bestDistance)
            {
                best = b;
                bestDistance = distance;
            }
        }
        if (best >= 0)
            return best;
        // Beyond the clock's reach: the next attack starts the awaited bar.
        const int last = std::min (barCount - 1, clockBar + settings.maxResumeBars);
        const double horizon = anchorTime + (bars[last].startSeconds + bars[last].lengthSeconds - reference) / tempoRatio;
        return timeSeconds > horizon ? bar : -1;
    }

    void BarPlayer::stop (Output& out) noexcept
    {
        noteOff (out);
        state = State::idle;
    }

    void BarPlayer::computeNextAnchor() noexcept
    {
        const int nb = nextNonEmptyBar (bar + 1);
        nextBar = nb;
        if (nb < 0)
        {
            nextAnchorTime = -1.0;
            return;
        }
        nextAnchorTime = barStartTime + (bars[nb].startSeconds + anchorOffset (nb) - bars[bar].startSeconds) / tempoRatio;
        const double beatLive = bars[nb].lengthSeconds / std::max (1, bars[nb].numerator) / tempoRatio;
        nextAnchorEarly = std::max (settings.minToleranceSeconds, settings.earlyToleranceBeats * beatLive);
        nextAnchorLate = std::max (settings.minToleranceSeconds, settings.lateToleranceBeats * beatLive);
        if (tempoUpdates == 0)
        {
            const double wide = settings.firstSpacingToleranceBars * bars[bar].lengthSeconds / tempoRatio;
            nextAnchorEarly = std::max (nextAnchorEarly, wide);
            nextAnchorLate = std::max (nextAnchorLate, wide);
        }
    }

    void BarPlayer::startBar (int b, double attackTime, int attackVelocity, Output& out) noexcept
    {
        if (state == State::playing)
        {
            // Learn the live tempo from the anchor-to-anchor spacing.
            const double scoreSpan = (bars[b].startSeconds + anchorOffset (b)) - (bars[bar].startSeconds + anchorOffset (bar));
            const double liveSpan = attackTime - anchorTime;
            if (scoreSpan > 0.0 && liveSpan > 0.0)
            {
                const double observed = std::clamp (scoreSpan / liveSpan, settings.minTempoRatio, settings.maxTempoRatio);
                // The first spacing replaces the score tempo outright (the band rarely plays at the
                // file's tempo); later ones are smoothed against downbeat jitter.
                const double adapt = tempoUpdates == 0 ? 1.0 : settings.tempoAdapt;
                const double limit = tempoUpdates == 0 ? settings.maxTempoRatio : settings.maxTempoStep;
                const double step = std::clamp (adapt * (observed - tempoRatio), -limit, limit);
                tempoRatio = std::clamp (tempoRatio + step, settings.minTempoRatio, settings.maxTempoRatio);
                ++tempoUpdates;
            }
        }

        bar = b;
        anchorTime = attackTime;
        barStartTime = attackTime - anchorOffset (b) / tempoRatio;
        velocity = attackVelocity;
        nextNote = bars[b].firstNote;
        state = State::playing;
        ++barsStarted;
        anchorBarForVerdict = b;

        // The anchor note sounds now: the attack is its timing.
        const auto& anchor = notes[nextNote];
        if (trusted)
        {
            noteOn (anchor.midiNote, velocity, out);
            soundingOffTime = attackTime + std::max (settings.minNoteSeconds, anchor.durationSeconds / tempoRatio);
        }
        else
            noteOff (out);
        ++nextNote;
        computeNextAnchor();
    }

    bool BarPlayer::onAttack (double timeSeconds, int attackVelocity, Output& out) noexcept
    {
        if (state == State::waiting)
        {
            const int b = resumeBar (timeSeconds);
            if (b < 0)
                return false; // off the running clock's grid: not a downbeat
            clockRunning = false;
            startBar (b, timeSeconds, attackVelocity, out);
            return true;
        }
        if (state == State::playing && nextBar >= 0 && timeSeconds >= nextAnchorTime - nextAnchorEarly
            && timeSeconds <= nextAnchorTime + nextAnchorLate && attackVelocity >= velocity - settings.maxAnchorVelocityDrop)
        {
            startBar (nextBar, timeSeconds, attackVelocity, out);
            return true;
        }
        return false; // inside the bar: the score plays these notes
    }

    void BarPlayer::onAnchorPitch (int playedMidiNote) noexcept
    {
        if (anchorBarForVerdict < 0 || anchorBarForVerdict >= barCount || bars[anchorBarForVerdict].noteCount == 0)
            return;
        const int expected = notes[bars[anchorBarForVerdict].firstNote].midiNote;
        anchorBarForVerdict = -1;
        const bool match = ((playedMidiNote - expected) % 12 + 12) % 12 == 0; // octave-tolerant: the score sets the octave
        if (match)
        {
            ++rootMatches;
            ++consecutiveMatches;
            consecutiveMismatches = 0;
            if (! trusted && consecutiveMatches >= settings.rootMatchesToResume)
                trusted = true;
        }
        else
        {
            ++rootMismatches;
            ++consecutiveMismatches;
            consecutiveMatches = 0;
            if (trusted && consecutiveMismatches >= settings.rootMismatchesToFallback)
            {
                trusted = false; // advance() silences the sounding note
                ++fallbacks;
            }
        }
    }

    void BarPlayer::advance (double nowSeconds, Output& out) noexcept
    {
        if (state == State::idle)
            return;

        if (! trusted)
            noteOff (out);
        else if (soundingNote >= 0 && nowSeconds >= soundingOffTime)
            noteOff (out);

        if (state != State::playing)
            return;

        // Scheduled notes of the current bar.
        const int end = bars[bar].firstNote + bars[bar].noteCount;
        while (nextNote < end)
        {
            const auto& n = notes[nextNote];
            const double due = barStartTime + (n.startSeconds - bars[bar].startSeconds) / tempoRatio;
            if (due > nowSeconds)
                break;
            if (trusted)
            {
                noteOn (n.midiNote, velocity, out);
                soundingOffTime = due + std::max (settings.minNoteSeconds, n.durationSeconds / tempoRatio);
            }
            ++nextNote;
        }

        // No anchor in time: stop, never play ahead into a bar the player did not start.
        if (nextBar >= 0 && nowSeconds > nextAnchorTime + nextAnchorLate)
        {
            noteOff (out);
            ++stops;
            clockRunning = true;
            clockBar = bar;
            state = State::waiting;
            bar = nextBar;
            nextAnchorTime = -1.0;
            return;
        }
        if (nextBar < 0 && nowSeconds > barStartTime + bars[bar].lengthSeconds / tempoRatio)
        {
            // End of the song or a long rest: wait at the next bar with notes (if any).
            noteOff (out);
            int b = bar + 1;
            while (b < barCount && bars[b].noteCount == 0)
                ++b;
            ++stops;
            if (b >= barCount)
            {
                state = State::idle;
                return;
            }
            clockRunning = true; // a written break: the clock runs on, the next bar's downbeat resumes
            clockBar = bar;
            state = State::waiting;
            bar = b;
            nextAnchorTime = -1.0;
        }
    }
}
