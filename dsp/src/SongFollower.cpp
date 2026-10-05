#include "bass2midi/SongFollower.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace bass2midi
{
    bool SongFollower::Settings::isValid() const noexcept
    {
        const auto unit = [] (double v) { return std::isfinite (v) && v >= 0.0 && v <= 1.0; };
        return unit (initialConfidence) && unit (minPredictConfidence)
            && std::isfinite (confidenceRate) && confidenceRate > 0.0 && confidenceRate < 1.0
            && maxSkip >= 0 && maxSkip <= 16
            && unit (earlyAttackFraction)
            && std::isfinite (pauseSeconds) && pauseSeconds >= 0.0
            && std::isfinite (minTempoRatio) && minTempoRatio > 0.0 && maxTempoRatio >= minTempoRatio && std::isfinite (maxTempoRatio)
            && unit (tempoAdapt)
            && std::isfinite (minTempoSpanSeconds) && minTempoSpanSeconds >= 0.0
            && resyncSearch >= 0 && resyncSearch <= 64
            && resyncMatches >= 1 && resyncMatches <= historySize
            && std::isfinite (resyncTimeToleranceSeconds) && resyncTimeToleranceSeconds >= 0.0
            && unit (resyncTimeToleranceFraction);
    }

    bool SongFollower::setSettings (const Settings& s) noexcept
    {
        if (! s.isValid())
            return false;
        settings = s;
        tempoRatio = std::clamp (tempoRatio, settings.minTempoRatio, settings.maxTempoRatio);
        return true;
    }

    void SongFollower::setTimeline (const ExpectedNote* newNotes, int newCount) noexcept
    {
        notes = newCount > 0 ? newNotes : nullptr;
        count = notes != nullptr ? newCount : 0;
        tempoRatio = std::clamp (1.0, settings.minTempoRatio, settings.maxTempoRatio);
        setPosition (0);
    }

    void SongFollower::setPosition (int index) noexcept
    {
        position = std::clamp (index, 0, std::max (0, count));
        refIndex = -1;
        lastMatchedIndex = -1;
        historyCount = 0;
        confidence = settings.initialConfidence;
    }

    int SongFollower::chooseIndex (double timeSeconds, bool& extra) const noexcept
    {
        extra = false;
        if (position >= count)
            return -1;
        if (refIndex < 0)
            return position;

        const double elapsedScore = (timeSeconds - refTime) * tempoRatio;
        const double base = notes[refIndex].startSeconds;
        const auto distance = [&] (int k) { return notes[k].startSeconds - base; };

        const double next = distance (position);
        if (next > 0.0 && elapsedScore < settings.earlyAttackFraction * next)
        {
            extra = true;
            return -1;
        }

        const int last = std::min (count - 1, position + settings.maxSkip);
        if (elapsedScore > distance (last) + settings.pauseSeconds)
            return position; // a pause the score does not have: resume with the next note

        int best = position;
        double bestCost = std::abs (elapsedScore - next);
        for (int k = position + 1; k <= last; ++k)
        {
            const double cost = std::abs (elapsedScore - distance (k));
            if (cost < bestCost)
            {
                best = k;
                bestCost = cost;
            }
        }
        return best;
    }

    void SongFollower::markAttack (int index, double timeSeconds) noexcept
    {
        refIndex = index;
        refTime = timeSeconds;
        position = index + 1;
    }

    SongFollower::Prediction SongFollower::onAttack (double timeSeconds) noexcept
    {
        Prediction p;
        bool extra = false;
        const int k = chooseIndex (timeSeconds, extra);
        if (extra)
        {
            ++extraAttacks;
            return p;
        }
        if (k < 0)
            return p;

        undoAvailable = true;
        undoPosition = position;
        undoRefIndex = refIndex;
        undoRefTime = refTime;
        markAttack (k, timeSeconds);
        p.index = k;
        p.midiNote = notes[k].midiNote;
        p.valid = confidence >= settings.minPredictConfidence;
        return p;
    }

    void SongFollower::undoLastAttack() noexcept
    {
        if (! undoAvailable)
            return;
        position = undoPosition;
        refIndex = undoRefIndex;
        refTime = undoRefTime;
        undoAvailable = false;
        ++extraAttacks;
    }

    bool SongFollower::tryResync (int around) noexcept
    {
        const int m = settings.resyncMatches;
        if (historyCount < m)
            return false;

        const double now = playedTimes[static_cast<std::size_t> (historyCount - 1)];
        const auto fits = [&] (int k)
        {
            if (k - (m - 1) < 0 || k >= count)
                return false;
            // While still confident, the place must also fit the time since the last match; once lost
            // (confidence below the prediction threshold) that reference is suspect itself.
            if (lastMatchedIndex >= 0 && confidence >= settings.minPredictConfidence)
            {
                const double elapsedScore = (now - lastMatchedTime) * tempoRatio;
                const double expected = notes[lastMatchedIndex].startSeconds + elapsedScore;
                const double tolerance = std::max (settings.resyncTimeToleranceSeconds, settings.resyncTimeToleranceFraction * elapsedScore);
                if (std::abs (notes[k].startSeconds - expected) > tolerance)
                    return false;
            }
            for (int j = 0; j < m; ++j)
                if (notes[k - j].midiNote != playedHistory[static_cast<std::size_t> (historyCount - 1 - j)])
                    return false;
            return true;
        };

        for (int d = 1; d <= settings.resyncSearch; ++d)
            for (const int k : { around + d, around - d }) // nearest first, forward preferred
                if (fits (k))
                {
                    markAttack (k, now);
                    lastMatchedIndex = k;
                    lastMatchedTime = now;
                    confidence = std::max (confidence, std::min (1.0, settings.minPredictConfidence + 0.1));
                    ++resyncs;
                    return true;
                }
        return false;
    }

    void SongFollower::onPlayedNote (int midiNote, double attackTimeSeconds, int predictedIndex) noexcept
    {
        if (count == 0)
            return;
        undoAvailable = false;

        if (historyCount == historySize)
        {
            for (int i = 1; i < historySize; ++i)
            {
                playedHistory[static_cast<std::size_t> (i - 1)] = playedHistory[static_cast<std::size_t> (i)];
                playedTimes[static_cast<std::size_t> (i - 1)] = playedTimes[static_cast<std::size_t> (i)];
            }
            --historyCount;
        }
        playedHistory[static_cast<std::size_t> (historyCount)] = midiNote;
        playedTimes[static_cast<std::size_t> (historyCount)] = attackTimeSeconds;
        ++historyCount;

        int index = predictedIndex;
        if (index < 0 || index >= count)
        {
            // A note without an assigned attack (the attack detector missed it): place it by timing.
            bool extra = false;
            index = chooseIndex (attackTimeSeconds, extra);
            if (extra && position < count && notes[position].midiNote == midiNote)
                index = position, extra = false; // early, but exactly the next note
            if (extra || index < 0)
                return; // a note the score does not have here (fill): says nothing about the position
            markAttack (index, attackTimeSeconds);
        }

        if (notes[index].midiNote == midiNote)
        {
            ++matches;
            confidence += settings.confidenceRate * (1.0 - confidence);
            if (lastMatchedIndex >= 0 && index > lastMatchedIndex)
            {
                const double span = notes[index].startSeconds - notes[lastMatchedIndex].startSeconds;
                const double live = attackTimeSeconds - lastMatchedTime;
                if (span >= settings.minTempoSpanSeconds && live > 0.0)
                {
                    const double observed = std::clamp (span / live, settings.minTempoRatio, settings.maxTempoRatio);
                    tempoRatio += settings.tempoAdapt * (observed - tempoRatio);
                }
            }
            lastMatchedIndex = index;
            lastMatchedTime = attackTimeSeconds;
            return;
        }

        ++mismatches;
        confidence -= settings.confidenceRate * confidence;
        tryResync (index);
    }
}
