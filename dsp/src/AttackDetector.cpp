#include "bass2midi/AttackDetector.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace bass2midi
{
    namespace
    {
        double toDb (double linear) noexcept { return linear > 1.0e-12 ? 20.0 * std::log10 (linear) : -240.0; }
        double energyToDb (double meanSquare) noexcept { return meanSquare > 1.0e-24 ? 10.0 * std::log10 (meanSquare) : -240.0; }
    }

    bool AttackDetector::Settings::isValid() const noexcept
    {
        const auto finitePositive = [] (double v) { return std::isfinite (v) && v > 0.0; };
        return finitePositive (subHopMs) && subHopMs <= 10.0
            && finitePositive (highPassHz)
            && finitePositive (transientRiseDb) && finitePositive (levelRiseDb)
            && finitePositive (lookbackMs) && finitePositive (holdMs)
            && std::isfinite (gateDbfs) && gateDbfs <= 0.0
            && std::isfinite (refractoryMs) && refractoryMs >= 0.0
            && std::isfinite (velocityWindowMs) && velocityWindowMs >= 0.0 && velocityWindowMs <= 20.0
            && std::isfinite (velocityFloorDbfs) && std::isfinite (velocityCeilDbfs) && velocityCeilDbfs > velocityFloorDbfs
            && minVelocity >= 1 && minVelocity <= 127
            && std::isfinite (breakDb) && std::isfinite (steadyDb) && steadyDb < breakDb
            && breakHops >= 1 && breakHops < 16
            && std::isfinite (breakMaxDropDb) && breakMaxDropDb >= 0.0
            && finitePositive (maxPeriodMs) && maxPeriodMs <= 100.0;
    }

    bool AttackDetector::prepare (double newSampleRateHz, const Settings& newSettings)
    {
        if (! newSettings.isValid() || ! (newSampleRateHz > 0.0) || newSettings.highPassHz >= 0.45 * newSampleRateHz)
            return false;

        const int hop = std::max (1, static_cast<int> (std::lround (newSettings.subHopMs * 1.0e-3 * newSampleRateHz)));
        const int lookback = static_cast<int> (std::ceil (newSettings.lookbackMs / newSettings.subHopMs));
        const int hold = static_cast<int> (std::ceil (newSettings.holdMs / newSettings.subHopMs));
        if (lookback < 1 || lookback >= maxLookback || hold < 1 || hold >= maxLookback)
            return false;

        settings = newSettings;
        sampleRateHz = newSampleRateHz;
        subHopSamples = hop;
        lookbackHops = lookback;
        holdHops = hold;
        refractorySamples = static_cast<int> (std::lround (settings.refractoryMs * 1.0e-3 * sampleRateHz));
        velocityWindowSamples = static_cast<int> (std::lround (settings.velocityWindowMs * 1.0e-3 * sampleRateHz));

        // 4th-order Butterworth high-pass: two RBJ sections with Q = 0.5412 and 1.3066.
        const double w0 = 2.0 * std::numbers::pi * settings.highPassHz / sampleRateHz;
        const double cosw = std::cos (w0);
        const std::array<double, 2> qs { 0.54119610014619701, 1.3065629648763764 };
        for (std::size_t k = 0; k < highPass.size(); ++k)
        {
            const double alpha = std::sin (w0) / (2.0 * qs[k]);
            const double a0 = 1.0 + alpha;
            auto& f = highPass[k];
            f.b0 = (1.0 + cosw) / 2.0 / a0;
            f.b1 = -(1.0 + cosw) / a0;
            f.b2 = f.b0;
            f.a1 = -2.0 * cosw / a0;
            f.a2 = (1.0 - alpha) / a0;
        }

        std::size_t size = 1;
        while (size < static_cast<std::size_t> (std::ceil (settings.maxPeriodMs * 1.0e-3 * sampleRateHz)) + static_cast<std::size_t> (hop) + 2)
            size <<= 1;
        history.assign (size, 0.0f);
        historyMask = size - 1;

        reset();
        return true;
    }

    void AttackDetector::setPeriodSamples (double newPeriodSamples) noexcept
    {
        const double maxPeriod = settings.maxPeriodMs * 1.0e-3 * sampleRateHz;
        periodSamples = settings.usePeriodicity && newPeriodSamples >= 2.0 && newPeriodSamples <= maxPeriod ? static_cast<int> (std::lround (newPeriodSamples)) : 0;
    }

    void AttackDetector::reset() noexcept
    {
        for (auto& f : highPass)
            f.z1 = f.z2 = 0.0;
        position = 0;
        inHop = 0;
        hopEnergy = hopPeak = 0.0;
        ringPos = ringCount = 0;
        lastAttackAt = -(std::int64_t { 1 } << 40);
        measuring = false;
        lastLevelDb = -180.0;
        std::fill (history.begin(), history.end(), 0.0f);
        residualEnergy = residualReference = 0.0;
        residualRing.fill (0.0);
        breakRun = 0;
        noteHeldMaxDb = -240.0;
    }

    int AttackDetector::velocityFromPeak (double peakLinear, const Settings& s) noexcept
    {
        const double position = (toDb (peakLinear) - s.velocityFloorDbfs) / (s.velocityCeilDbfs - s.velocityFloorDbfs);
        return s.minVelocity + static_cast<int> (std::lround (std::clamp (position, 0.0, 1.0) * (127 - s.minVelocity)));
    }

    bool AttackDetector::process (const float* samples, int n, Attack& attack) noexcept
    {
        bool reported = false;
        if (sampleRateHz <= 0.0)
            return false;

        for (int i = 0; i < n; ++i)
        {
            const double x = samples[i];
            const double y = highPass[1].process (highPass[0].process (x));

            hopEnergy += y * y;

            history[static_cast<std::size_t> (position) & historyMask] = samples[i];
            if (periodSamples > 0 && position >= periodSamples)
            {
                const double delayed = history[static_cast<std::size_t> (position - periodSamples) & historyMask];
                const double residual = x - delayed;
                residualEnergy += residual * residual;
                residualReference += x * x + delayed * delayed;
            }
            const double magnitude = std::abs (x);
            hopPeak = std::max (hopPeak, magnitude);
            ++position;

            if (measuring)
            {
                pending.peakLinear = std::max (pending.peakLinear, magnitude);
                if (position - pending.detectedAtSample >= velocityWindowSamples && ! reported)
                {
                    measuring = false;
                    pending.reportedAtSample = position;
                    pending.velocity = velocityFromPeak (pending.peakLinear, settings);
                    attack = pending;
                    reported = true;
                }
            }

            if (++inHop < subHopSamples)
                continue;

            // End of a sub-hop: evaluate the detection functions.
            const double transientDb = energyToDb (hopEnergy / subHopSamples);
            const double levelDb = toDb (hopPeak);
            lastLevelDb = levelDb;

            // Periodicity residual of this sub-hop (0 dB = no relation to one period earlier).
            const bool periodic = periodSamples > 0 && residualReference > 1.0e-12;
            const double residualDb = periodic ? 10.0 * std::log10 (std::max (2.0 * residualEnergy / residualReference, 1.0e-12)) : 0.0;
            residualEnergy = residualReference = 0.0;

            transientRaw[static_cast<std::size_t> (ringPos)] = transientDb;
            levelRaw[static_cast<std::size_t> (ringPos)] = levelDb;
            const auto at = [this] (int hopsAgo) { return static_cast<std::size_t> ((ringPos - hopsAgo + maxLookback) % maxLookback); };

            double heldTransient = transientDb, heldLevel = levelDb;
            for (int k = 1; k < std::min (holdHops, ringCount + 1); ++k)
            {
                heldTransient = std::max (heldTransient, transientRaw[at (k)]);
                heldLevel = std::max (heldLevel, levelRaw[at (k)]);
            }
            double minTransient = heldTransient, minLevel = heldLevel;
            for (int k = 1; k <= std::min (lookbackHops, ringCount); ++k)
            {
                minTransient = std::min (minTransient, transientHeld[at (k)]);
                minLevel = std::min (minLevel, levelHeld[at (k)]);
            }
            transientHeld[at (0)] = heldTransient;
            levelHeld[at (0)] = heldLevel;
            residualRing[at (0)] = periodic ? residualDb : 0.0;
            noteHeldMaxDb = std::max (noteHeldMaxDb, heldLevel);
            breakRun = periodic && residualDb >= settings.breakDb ? breakRun + 1 : 0;
            ringPos = (ringPos + 1) % maxLookback;
            ringCount = std::min (ringCount + 1, maxLookback);

            const bool armed = ! measuring && position - lastAttackAt >= refractorySamples && levelDb >= settings.gateDbfs;
            const bool transientRise = heldTransient - minTransient >= settings.transientRiseDb;
            const bool levelRise = heldLevel - minLevel >= settings.levelRiseDb;
            // The steady reference must precede the break run: at (1) is this sub-hop, at (1..breakRun)
            // the run, so look at the lookbackHops before it.
            double steadyBefore = 0.0;
            for (int k = breakRun + 1; k <= std::min (breakRun + lookbackHops, ringCount); ++k)
                steadyBefore = std::min (steadyBefore, residualRing[at (k)]);
            const bool periodicityBreak = periodic && breakRun >= settings.breakHops && steadyBefore <= settings.steadyDb
                                       && heldLevel >= noteHeldMaxDb - settings.breakMaxDropDb;
            if (armed && (transientRise || levelRise || periodicityBreak))
            {
                lastAttackAt = position;
                noteHeldMaxDb = heldLevel;
                breakRun = 0;
                pending = {};
                pending.detectedAtSample = position;
                pending.peakLinear = hopPeak;
                pending.kind = transientRise ? Attack::Kind::transient : levelRise ? Attack::Kind::level : Attack::Kind::periodicity;
                measuring = true;
                if (velocityWindowSamples == 0 && ! reported)
                {
                    measuring = false;
                    pending.reportedAtSample = position;
                    pending.velocity = velocityFromPeak (pending.peakLinear, settings);
                    attack = pending;
                    reported = true;
                }
            }

            inHop = 0;
            hopEnergy = 0.0;
            hopPeak = 0.0;
        }
        return reported;
    }
}
