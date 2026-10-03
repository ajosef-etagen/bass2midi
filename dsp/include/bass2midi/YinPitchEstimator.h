#pragma once

#include "bass2midi/Fft.h"
#include "bass2midi/PitchCandidate.h"

#include <complex>
#include <vector>

namespace bass2midi
{
    // YIN pitch estimator (de Cheveigne & Kawahara, "YIN, a fundamental frequency estimator for
    // speech and music", JASA 2002), steps 1-5: difference function, cumulative mean normalised
    // difference d'(tau), absolute threshold, local-minimum descent and parabolic interpolation.
    //
    // Independent implementation for Bass2MIDI. It is used as the *reference* estimator for the
    // Phase 0 baseline; bass-specific changes (adaptive windows, octave validation) are meant to be
    // measured against it, not patched into it.
    //
    // The difference function is computed via FFT cross-correlation:
    //   d(tau) = E[0, W) + E[tau, tau + W) - 2 r(tau),   r(tau) = sum_{j<W} x_j x_{j+tau}
    // with integration length W = windowSamples - maxLagSamples. Cost per call is
    // O(N log N) with N = next power of two >= windowSamples.
    class YinPitchEstimator
    {
    public:
        struct Config
        {
            int windowSamples = 2048;  // analysed samples per call
            int minLagSamples = 2;     // shortest period searched (sets the highest frequency); >= 2
            int maxLagSamples = 1024;  // longest period searched (sets the lowest frequency)
            double threshold = 0.15;   // absolute d'(tau) threshold, dimensionless (paper: 0.10-0.15)

            // Requires 2 <= minLag < maxLag and an integration length W = window - maxLag >= maxLag,
            // i.e. at least two periods of the lowest searched frequency inside the window.
            bool isValid() const noexcept;

            // Lags covering [lowestHz, highestHz] at the given rate, window = two longest periods.
            static Config forFrequencyRange (double sampleRateHz, double lowestHz, double highestHz, double threshold);
        };

        // Non-real-time: allocates all working buffers. Returns false (and leaves the estimator
        // unprepared) for an invalid configuration or sample rate.
        bool prepare (double sampleRateHz, const Config& config);

        bool isPrepared() const noexcept { return prepared; }
        const Config& getConfig() const noexcept { return config; }
        double getSampleRateHz() const noexcept { return sampleRateHz; }
        double getLowestFrequencyHz() const noexcept { return sampleRateHz / config.maxLagSamples; }
        double getHighestFrequencyHz() const noexcept { return sampleRateHz / config.minLagSamples; }

        // Real-time safe: no allocation, bounded work. `window` holds exactly
        // getConfig().windowSamples samples, oldest first.
        PitchCandidate estimate (const float* window) noexcept;

        // d'(tau) of the last estimate() call, index = lag in samples, size maxLagSamples + 1.
        // For diagnostics/tests only.
        const std::vector<double>& getLastCmndf() const noexcept { return cmndf; }

    private:
        Config config;
        double sampleRateHz = 0.0;
        bool prepared = false;

        Fft fft;
        std::vector<std::complex<double>> spectrumX, spectrumY;
        std::vector<double> energyPrefix; // energyPrefix[i] = sum_{k<i} x_k^2
        std::vector<double> cmndf;
    };
}
