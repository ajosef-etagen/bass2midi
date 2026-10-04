#include "bass2midi/MultiResolutionPitchTracker.h"
#include "bass2midi/MidiNoteUtils.h"

#include <algorithm>
#include <cmath>

namespace bass2midi
{
    bool MultiResolutionPitchTracker::Settings::isValid() const noexcept
    {
        if (! (lowestHz > 0.0 && highestHz > lowestHz * 2.0))
            return false;
        if (! (integrationFraction >= 0.25 && integrationFraction <= 2.0))
            return false;
        if (! (edgeGuard >= 0.0 && edgeGuard < 0.5))
            return false;
        if (! (threshold > 0.0 && threshold < 1.0 && earlyMinClarity > 0.0 && earlyMinClarity <= 1.0))
            return false;
        if (! (silenceDropDb >= 0.0 && std::isfinite (silenceDropDb) && levelChangeDb >= 0.0 && std::isfinite (levelChangeDb)))
            return false;
        if (! (subOctaveRatio >= 0.0 && subOctaveRatio < 1.0 && subOctaveFloor >= 0.0 && subOctaveFloor < 1.0))
            return false;

        // Rungs: strictly decreasing lowest frequency, all above the range floor, zeros only at the end.
        double previous = highestHz;
        bool ended = false;
        for (const double hz : rungLowestHz)
        {
            if (hz <= 0.0)
            {
                ended = true;
                continue;
            }
            if (ended || hz >= previous || hz <= lowestHz)
                return false;
            previous = hz;
        }
        return true;
    }

    bool MultiResolutionPitchTracker::prepare (double newSampleRateHz, const Settings& newSettings)
    {
        rungCount = 0;
        if (! newSettings.isValid() || ! (newSampleRateHz > 0.0))
            return false;

        settings = newSettings;
        sampleRateHz = newSampleRateHz;
        rangeMaxLagSamples = static_cast<int> (std::ceil (sampleRateHz / settings.lowestHz));
        const int minLag = std::max (2, static_cast<int> (std::floor (sampleRateHz / settings.highestHz)));

        std::array<double, maxRungs + 1> lowest {};
        int count = 0;
        for (const double hz : settings.rungLowestHz)
            if (hz > 0.0 && count < maxRungs - 1)
                lowest[static_cast<size_t> (count++)] = hz;
        lowest[static_cast<size_t> (count++)] = settings.lowestHz; // the full-range rung, always last

        for (int i = 0; i < count; ++i)
        {
            YinPitchEstimator::Config c;
            c.maxLagSamples = static_cast<int> (std::ceil (sampleRateHz / lowest[static_cast<size_t> (i)]));
            c.minLagSamples = std::min (minLag, c.maxLagSamples - 1);
            c.windowSamples = c.maxLagSamples + std::max (c.minLagSamples, static_cast<int> (std::lround (settings.integrationFraction * c.maxLagSamples)));
            c.threshold = settings.threshold;
            c.subOctaveRatio = settings.subOctaveRatio;
            c.subOctaveFloor = settings.subOctaveFloor;
            c.silenceDropDb = settings.silenceDropDb;
            c.levelChangeDb = settings.levelChangeDb;

            if (! estimators[static_cast<size_t> (i)].prepare (sampleRateHz, c))
                return false;
        }

        rungCount = count;
        frameSamples = estimators[static_cast<size_t> (count - 1)].getConfig().windowSamples;
        return true;
    }

    bool MultiResolutionPitchTracker::paletteRelieves (const PitchCandidate& candidate) const noexcept
    {
        if (! settings.paletteOctaveRelief || palette.empty() || ! (candidate.frequencyHz > 0.0))
            return false;
        const int note = static_cast<int> (std::lround (midi::noteFromFrequencyHz (candidate.frequencyHz)));
        return palette.contains (note) && ! palette.contains (note - 12) && ! palette.contains (note - 24);
    }

    MultiResolutionPitchTracker::Result MultiResolutionPitchTracker::estimate (const float* frame) noexcept
    {
        Result result;
        if (rungCount == 0)
            return result;

        Result relieved; // first palette-relieved candidate, used only if nothing octave-safe is valid

        for (int i = 0; i < rungCount; ++i)
        {
            auto& estimator = estimators[static_cast<size_t> (i)];
            const int window = estimator.getConfig().windowSamples;
            const auto candidate = estimator.estimate (frame + (frameSamples - window));
            const bool longest = i == rungCount - 1;

            if (longest)
            {
                if (! candidate.valid && relieved.rung >= 0)
                    return relieved;
                result.pitch = candidate;
                result.rung = candidate.valid ? i : -1;
                result.windowSamples = window;
                return result;
            }

            if (! candidate.valid || candidate.clarity < settings.earlyMinClarity)
                continue;

            if (candidate.periodSamples > (1.0 - settings.edgeGuard) * estimator.getConfig().maxLagSamples)
                continue;

            const bool octaveSafe = candidate.subOctaveChecked || 2.0 * candidate.periodSamples > rangeMaxLagSamples;
            if (octaveSafe)
            {
                result.pitch = candidate;
                result.rung = i;
                result.windowSamples = window;
                return result;
            }

            if (relieved.rung < 0 && paletteRelieves (candidate))
            {
                relieved.pitch = candidate;
                relieved.rung = i;
                relieved.windowSamples = window;
                relieved.paletteRelieved = true;
            }
        }

        return result; // unreachable: the longest rung always returns
    }
}
