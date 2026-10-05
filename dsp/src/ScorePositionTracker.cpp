#include "bass2midi/ScorePositionTracker.h"

#include <algorithm>
#include <cmath>

namespace bass2midi
{
    bool ScorePositionTracker::Settings::isValid() const noexcept
    {
        const auto positive = [] (double v) { return std::isfinite (v) && v > 0.0; };
        const auto unit = [] (double v) { return std::isfinite (v) && v >= 0.0 && v <= 1.0; };
        return positive (timingSigmaSeconds) && positive (matchWindowBeats) && unit (octaveMatch)
            && unit (background) && background > 0.0 && unit (correctionGain) && unit (tempoAdapt)
            && std::isfinite (minTempoSpanSeconds) && minTempoSpanSeconds >= 0.0
            && positive (minTempoRatio) && std::isfinite (maxTempoRatio) && maxTempoRatio >= minTempoRatio
            && positive (replaceBelowLog) && std::isfinite (jumpPenaltyLog) && jumpPenaltyLog >= 0.0
            && jumpSearchBars >= 0 && jumpSearchBars <= 256 && unit (jumpMinAnchorWeight)
            && std::isfinite (jumpPenaltyPerBar) && jumpPenaltyPerBar >= 0.0
            && missStreakForJumps >= 1 && unit (poorLikelihood) && forcedJumps >= 0 && forcedJumps < maxHypotheses;
    }

    bool ScorePositionTracker::setSettings (const Settings& s) noexcept
    {
        if (! s.isValid())
            return false;
        settings = s;
        return true;
    }

    void ScorePositionTracker::setTimeline (const ExpectedNote* newNotes, int newNoteCount, const TimelineBar* newBars, int newBarCount) noexcept
    {
        const bool valid = newNotes != nullptr && newBars != nullptr && newNoteCount > 0 && newBarCount > 0;
        notes = valid ? newNotes : nullptr;
        bars = valid ? newBars : nullptr;
        noteCount = valid ? newNoteCount : 0;
        barCount = valid ? newBarCount : 0;
        armed = running = false;
        hypothesisCount = 0;
    }

    int ScorePositionTracker::barAtScore (double scoreSeconds) const noexcept
    {
        int lo = 0, hi = barCount - 1;
        while (lo < hi)
        {
            const int mid = (lo + hi + 1) / 2;
            if (bars[mid].startSeconds <= scoreSeconds)
                lo = mid;
            else
                hi = mid - 1;
        }
        return lo;
    }

    double ScorePositionTracker::beatSecondsAt (int bar) const noexcept
    {
        return bars[bar].lengthSeconds / std::max (1, bars[bar].numerator);
    }

    void ScorePositionTracker::arm (int bar, double bpm) noexcept
    {
        if (barCount == 0)
            return;
        armedBar = std::clamp (bar, 0, barCount - 1);
        const double scoreBpm = 60.0 / beatSecondsAt (armedBar);
        armedRatio = bpm > 0.0 && std::isfinite (bpm) ? std::clamp (bpm / scoreBpm, settings.minTempoRatio, settings.maxTempoRatio) : 1.0;
        armed = true;
        running = false;
        hypothesisCount = 0;
    }

    void ScorePositionTracker::seed (double scoreSeconds, double timeSeconds, double ratio) noexcept
    {
        // Around the start: the exact alignment, a few tempo variants, and offsets up to a bar.
        const double beat = beatSecondsAt (barAtScore (scoreSeconds));
        const double offsets[maxHypotheses] = { 0, 0, 0, -0.5, 0.5, -1, 1, -2, 2, -4, 4, 0 };
        const double ratios[maxHypotheses] = { 1, 0.95, 1.05, 1, 1, 1, 1, 1, 1, 1, 1, 0.9 };
        hypothesisCount = maxHypotheses;
        for (int i = 0; i < maxHypotheses; ++i)
        {
            auto& h = hypotheses[static_cast<std::size_t> (i)];
            h = {};
            h.anchorScore = scoreSeconds + offsets[i] * beat;
            h.anchorTime = timeSeconds;
            h.ratio = std::clamp (ratio * ratios[i], settings.minTempoRatio, settings.maxTempoRatio);
            h.logWeight = i == 0 ? 0.0 : -0.5;
        }
        bestWasJump = false;
    }

    void ScorePositionTracker::begin (double timeSeconds) noexcept
    {
        if (barCount == 0)
            return;
        seed (bars[armedBar].startSeconds, timeSeconds, armedRatio);
        running = true;
        armed = false;
    }

    int ScorePositionTracker::best() const noexcept
    {
        int b = 0;
        for (int i = 1; i < hypothesisCount; ++i)
            if (hypotheses[static_cast<std::size_t> (i)].logWeight > hypotheses[static_cast<std::size_t> (b)].logWeight)
                b = i;
        return b;
    }

    double ScorePositionTracker::likelihood (const Hypothesis& h, double t, int midiNote, int& matchedNote) const noexcept
    {
        matchedNote = -1;
        const double s = h.scoreAt (t);
        const double window = settings.matchWindowBeats * beatSecondsAt (barAtScore (std::max (0.0, s)));
        // First note at or after s - window (notes are sorted by start).
        int lo = 0, hi = noteCount;
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (notes[mid].startSeconds < s - window)
                lo = mid + 1;
            else
                hi = mid;
        }
        double bestValue = 0.0;
        for (int i = lo; i < noteCount && notes[i].startSeconds <= s + window; ++i)
        {
            const int diff = notes[i].midiNote - midiNote;
            const double pitch = diff == 0 ? 1.0 : (diff % 12 == 0 ? settings.octaveMatch : 0.0);
            if (pitch <= 0.0)
                continue;
            const double liveError = (notes[i].startSeconds - s) / h.ratio;
            const double timing = std::exp (-0.5 * liveError * liveError / (settings.timingSigmaSeconds * settings.timingSigmaSeconds));
            const double value = pitch * timing * (0.3 + 0.7 * notes[i].anchorWeight);
            if (value > bestValue)
            {
                bestValue = value;
                matchedNote = i;
            }
        }
        return bestValue;
    }

    void ScorePositionTracker::onNote (double t, int midiNote) noexcept
    {
        if (barCount == 0)
            return;
        if (armed && ! running)
        {
            // The first played note starts the clock, aligned to the armed bar's first written note.
            const auto& bar = bars[armedBar];
            const double start = bar.noteCount > 0 ? notes[bar.firstNote].startSeconds : bar.startSeconds;
            seed (start, t, armedRatio);
            running = true;
            armed = false;
        }
        if (! running)
            return;
        ++notesUsed;

        double maxLog = -1.0e300;
        for (int i = 0; i < hypothesisCount; ++i)
        {
            auto& h = hypotheses[static_cast<std::size_t> (i)];
            int matched = -1;
            const double l = likelihood (h, t, midiNote, matched);
            h.logWeight += std::log (settings.background + (1.0 - settings.background) * l);

            if (matched >= 0 && l > 0.2)
            {
                const auto& n = notes[matched];
                const double s = h.scoreAt (t);
                const double corrected = s + settings.correctionGain * n.anchorWeight * (n.startSeconds - s);
                if (h.lastMatchScore >= 0.0 && n.startSeconds - h.lastMatchScore >= settings.minTempoSpanSeconds && t > h.lastMatchTime)
                {
                    const double observed = std::clamp ((n.startSeconds - h.lastMatchScore) / (t - h.lastMatchTime),
                                                        settings.minTempoRatio, settings.maxTempoRatio);
                    h.ratio += settings.tempoAdapt * n.anchorWeight * (observed - h.ratio);
                }
                if (n.anchorWeight >= 0.5) // tempo spans from solid anchors only
                {
                    h.lastMatchScore = n.startSeconds;
                    h.lastMatchTime = t;
                }
                h.anchorScore = corrected;
                h.anchorTime = t;
            }
            maxLog = std::max (maxLog, h.logWeight);
        }
        for (int i = 0; i < hypothesisCount; ++i)
            hypotheses[static_cast<std::size_t> (i)].logWeight -= maxLog;

        // Replace hopeless hypotheses: alternately a variation of the best, and a jump candidate where
        // this note is a strong anchor within reach (a skipped or repeated section).
        const int b = best();
        const auto bestH = hypotheses[static_cast<std::size_t> (b)];
        const double bestScore = bestH.scoreAt (t);
        const int bestBar = barAtScore (bestScore);
        const double beat = beatSecondsAt (bestBar);

        int unused = -1;
        const double bestLikelihood = likelihood (bestH, t, midiNote, unused);
        missStreak = bestLikelihood < settings.poorLikelihood ? missStreak + 1 : 0;
        if (missStreak >= settings.missStreakForJumps)
        {
            // Everything near the best fails: force jump candidates into the weakest slots.
            for (int k = 0; k < settings.forcedJumps; ++k)
            {
                int worst = -1;
                for (int i = 0; i < hypothesisCount; ++i)
                    if (i != b && ! hypotheses[static_cast<std::size_t> (i)].jump
                        && (worst < 0 || hypotheses[static_cast<std::size_t> (i)].logWeight < hypotheses[static_cast<std::size_t> (worst)].logWeight))
                        worst = i;
                if (worst < 0 || ! makeJumpCandidate (hypotheses[static_cast<std::size_t> (worst)], t, midiNote, bestH, bestScore, bestBar, beat))
                    break;
            }
            missStreak = 0;
        }

        bool makeJump = true;
        int variation = 0;
        for (int i = 0; i < hypothesisCount; ++i)
        {
            auto& h = hypotheses[static_cast<std::size_t> (i)];
            if (i == b || h.logWeight > -settings.replaceBelowLog)
                continue;
            const bool replaced = makeJump && makeJumpCandidate (h, t, midiNote, bestH, bestScore, bestBar, beat);
            if (! replaced)
            {
                const double offsets[4] = { 0.25, -0.25, 0.5, -0.5 };
                const double ratios[4] = { 1.02, 0.98, 1.0, 1.0 };
                h = bestH;
                h.anchorScore = bestScore + offsets[variation % 4] * beat;
                h.anchorTime = t;
                h.ratio = std::clamp (bestH.ratio * ratios[variation % 4], settings.minTempoRatio, settings.maxTempoRatio);
                h.logWeight = -1.0;
                h.jump = false;
                ++variation;
            }
            makeJump = ! makeJump;
        }

        auto& top = hypotheses[static_cast<std::size_t> (b)];
        if (top.jump)
        {
            ++jumps;
            top.jump = false;
        }
    }

    bool ScorePositionTracker::makeJumpCandidate (Hypothesis& h, double t, int midiNote, const Hypothesis& bestH, double bestScore,
                                                  int bestBar, double beat) noexcept
    {
        if (settings.jumpSearchBars <= 0)
            return false;
        const double from = bars[std::max (0, bestBar - settings.jumpSearchBars)].startSeconds;
        const int lastBar = std::min (barCount - 1, bestBar + settings.jumpSearchBars);
        const double to = bars[lastBar].startSeconds + bars[lastBar].lengthSeconds;
        // Candidates: strong anchors with this pitch, not where the best already is. Taken nearest
        // first (cursor = rank), because repeated sections make far candidates equally plausible.
        constexpr int maxCandidates = 32;
        std::array<int, maxCandidates> found {};
        int candidates = 0;
        for (int k = 0; k < noteCount && candidates < maxCandidates; ++k)
        {
            const auto& n = notes[k];
            if (n.startSeconds < from || n.startSeconds > to || n.midiNote != midiNote
                || n.anchorWeight < settings.jumpMinAnchorWeight || std::abs (n.startSeconds - bestScore) < 2.0 * beat)
                continue;
            found[static_cast<std::size_t> (candidates++)] = k;
        }
        if (candidates == 0)
            return false;
        std::sort (found.begin(), found.begin() + candidates, [&] (int x, int y)
                   { return std::abs (notes[x].startSeconds - bestScore) < std::abs (notes[y].startSeconds - bestScore); });
        if (jumpCursor >= candidates)
            jumpCursor = 0;
        const int chosen = found[static_cast<std::size_t> (jumpCursor)];
        jumpCursor = (jumpCursor + 1) % candidates;
        const double barsAway = std::abs (notes[chosen].startSeconds - bestScore) / (beat * std::max (1, bars[bestBar].numerator));
        h = {};
        h.anchorScore = notes[chosen].startSeconds;
        h.anchorTime = t;
        h.ratio = bestH.ratio;
        h.logWeight = -settings.jumpPenaltyLog - settings.jumpPenaltyPerBar * barsAway;
        h.jump = true;
        return true;
    }

    ScorePositionTracker::Position ScorePositionTracker::positionAt (double t) const noexcept
    {
        Position p;
        if (barCount == 0)
            return p;
        p.running = running;
        if (! running)
        {
            p.scoreSeconds = bars[armedBar].startSeconds;
            p.tempoRatio = armedRatio;
        }
        else
        {
            const auto& h = hypotheses[static_cast<std::size_t> (best())];
            p.scoreSeconds = std::max (0.0, h.scoreAt (t));
            p.tempoRatio = h.ratio;

            double total = 0.0, agreeing = 0.0;
            const double quarterBeat = 0.25 * beatSecondsAt (barAtScore (p.scoreSeconds));
            for (int i = 0; i < hypothesisCount; ++i)
            {
                const auto& o = hypotheses[static_cast<std::size_t> (i)];
                const double w = std::exp (o.logWeight);
                total += w;
                if (std::abs (o.scoreAt (t) - p.scoreSeconds) <= quarterBeat)
                    agreeing += w;
            }
            p.confidence = total > 0.0 ? agreeing / total : 0.0;
        }
        p.playedBar = barAtScore (p.scoreSeconds);
        const auto& bar = bars[p.playedBar];
        p.writtenBar = bar.writtenBar;
        const double beatSeconds = beatSecondsAt (p.playedBar);
        p.beat = 1.0 + std::clamp ((p.scoreSeconds - bar.startSeconds) / beatSeconds, 0.0, static_cast<double> (std::max (1, bar.numerator)) - 1.0e-9);
        p.bpm = 60.0 / beatSeconds * p.tempoRatio;
        int lo = 0, hi = noteCount;
        while (lo < hi)
        {
            const int mid = (lo + hi) / 2;
            if (notes[mid].startSeconds < p.scoreSeconds - 1.0e-6)
                lo = mid + 1;
            else
                hi = mid;
        }
        p.nextNote = lo < noteCount ? lo : -1;
        return p;
    }
}
