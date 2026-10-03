#include "bass2midi/YinPitchEstimator.h"

#include <algorithm>
#include <cmath>

namespace bass2midi
{
    bool YinPitchEstimator::Config::isValid() const noexcept
    {
        const int integrationSamples = windowSamples - maxLagSamples;
        return minLagSamples >= 2
            && maxLagSamples > minLagSamples
            && integrationSamples >= maxLagSamples
            && threshold > 0.0 && threshold < 1.0;
    }

    YinPitchEstimator::Config YinPitchEstimator::Config::forFrequencyRange (double sampleRateHz, double lowestHz,
                                                                           double highestHz, double threshold)
    {
        Config c;
        c.maxLagSamples = static_cast<int> (std::ceil (sampleRateHz / lowestHz));
        c.minLagSamples = std::max (2, static_cast<int> (std::floor (sampleRateHz / highestHz)));
        c.windowSamples = 2 * c.maxLagSamples;
        c.threshold = threshold;
        return c;
    }

    bool YinPitchEstimator::prepare (double newSampleRateHz, const Config& newConfig)
    {
        prepared = false;

        if (! newConfig.isValid() || ! (newSampleRateHz > 0.0))
            return false;

        config = newConfig;
        sampleRateHz = newSampleRateHz;

        // No circular wrap-around: the largest index touched is (W - 1) + maxLag = windowSamples - 1.
        fft.prepare (Fft::nextPowerOfTwo (config.windowSamples));

        const auto fftSize = static_cast<size_t> (fft.getSize());
        spectrumX.assign (fftSize, {});
        spectrumY.assign (fftSize, {});
        energyPrefix.assign (static_cast<size_t> (config.windowSamples) + 1, 0.0);
        cmndf.assign (static_cast<size_t> (config.maxLagSamples) + 1, 1.0);

        prepared = true;
        return true;
    }

    PitchCandidate YinPitchEstimator::estimate (const float* window) noexcept
    {
        PitchCandidate result;
        if (! prepared)
            return result;

        const int n = config.windowSamples;
        const int maxLag = config.maxLagSamples;
        const int minLag = config.minLagSamples;
        const int integration = n - maxLag;
        const int fftSize = fft.getSize();

        energyPrefix[0] = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const auto s = static_cast<double> (window[i]);
            energyPrefix[static_cast<size_t> (i) + 1] = energyPrefix[static_cast<size_t> (i)] + s * s;
        }
        result.windowRmsLinear = std::sqrt (energyPrefix[static_cast<size_t> (n)] / static_cast<double> (n));

        // r(tau) = IFFT( X * conj(Y) ), x = whole window, y = first W samples.
        for (int i = 0; i < fftSize; ++i)
        {
            const double s = i < n ? static_cast<double> (window[i]) : 0.0;
            spectrumX[static_cast<size_t> (i)] = { s, 0.0 };
            spectrumY[static_cast<size_t> (i)] = { i < integration ? s : 0.0, 0.0 };
        }

        fft.perform (spectrumX.data(), false);
        fft.perform (spectrumY.data(), false);

        for (int i = 0; i < fftSize; ++i)
            spectrumX[static_cast<size_t> (i)] *= std::conj (spectrumY[static_cast<size_t> (i)]);

        fft.perform (spectrumX.data(), true);

        // Steps 2+3: difference function and cumulative mean normalisation.
        const double energyHead = energyPrefix[static_cast<size_t> (integration)];
        double runningSum = 0.0;
        cmndf[0] = 1.0;

        for (int tau = 1; tau <= maxLag; ++tau)
        {
            const double energyShifted = energyPrefix[static_cast<size_t> (tau + integration)]
                                       - energyPrefix[static_cast<size_t> (tau)];
            const double correlation = spectrumX[static_cast<size_t> (tau)].real();
            const double difference = std::max (0.0, energyHead + energyShifted - 2.0 * correlation);

            runningSum += difference;
            cmndf[static_cast<size_t> (tau)] = runningSum > 0.0
                                                   ? difference * static_cast<double> (tau) / runningSum
                                                   : 1.0;
        }

        // Global minimum in the search range - diagnostic, reported even when nothing qualifies.
        int bestLag = minLag;
        for (int tau = minLag; tau < maxLag; ++tau)
            if (cmndf[static_cast<size_t> (tau)] < cmndf[static_cast<size_t> (bestLag)])
                bestLag = tau;
        result.bestLagSamples = static_cast<double> (bestLag);

        // Step 4: absolute threshold - first lag below the threshold, then descend to its local minimum.
        int lagEstimate = -1;
        for (int tau = minLag; tau < maxLag; ++tau)
        {
            if (cmndf[static_cast<size_t> (tau)] < config.threshold)
            {
                while (tau + 1 < maxLag && cmndf[static_cast<size_t> (tau) + 1] < cmndf[static_cast<size_t> (tau)])
                    ++tau;
                lagEstimate = tau;
                break;
            }
        }

        if (lagEstimate < 0)
        {
            result.clarity = std::clamp (1.0 - cmndf[static_cast<size_t> (bestLag)], 0.0, 1.0);
            return result;
        }

        // Step 5: parabolic interpolation (minLag >= 2 and lagEstimate < maxLag keep tau +/- 1 in range).
        const double left = cmndf[static_cast<size_t> (lagEstimate) - 1];
        const double centre = cmndf[static_cast<size_t> (lagEstimate)];
        const double right = cmndf[static_cast<size_t> (lagEstimate) + 1];
        const double denominator = left - 2.0 * centre + right;

        double period = static_cast<double> (lagEstimate);
        if (std::abs (denominator) > 1.0e-12)
            period += std::clamp (0.5 * (left - right) / denominator, -0.5, 0.5);

        result.valid = true;
        result.periodSamples = period;
        result.frequencyHz = sampleRateHz / period;
        result.clarity = std::clamp (1.0 - centre, 0.0, 1.0);
        return result;
    }
}
