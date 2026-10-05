#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace bass2midi
{
    // Fast, pitch-independent attack (pluck) detector for Song Mode.
    //
    // Works on sub-hops of subHopMs (default 1 ms) straight from the input samples, independent of
    // the pitch-analysis framing. Two detection functions, either may fire:
    //
    //  - transient: energy above highPassHz (4th-order Butterworth; a 2nd-order slope leaks
    //    too much of the ringing note's own partials, so a re-pluck of A1 did not stand out). A pluck excites broadband
    //    energy while the upper partials of a ringing note have mostly decayed, so this also sees a
    //    re-pluck of a still-ringing string - which the full-band envelope (and the pitch path's
    //    onset rule) cannot.
    //  - level: full-band peak level, for very dark tones without high-frequency content.
    //  - periodicity break (optional, usePeriodicity; only while setPeriodSamples() gives the
    //    sounding note's period P - tie it to the sounding/expected note, a raw per-frame estimate
    //    that jumps to a wrong rung makes the residual jump too): the
    //    residual x[n] - x[n-P] relative to the signal energy. A ringing note is periodic (residual
    //    -20..-30 dB on real DI takes); a new pluck or fretted change breaks the periodicity at once
    //    (residual ~0 dB) even when neither the level nor the high band rises - the typical
    //    finger-style re-pluck of a ringing string. Fires when the residual stays >= breakDb for
    //    breakHops sub-hops after having been <= steadyDb within lookbackMs, and the held level is
    //    within breakMaxDropDb of the note's highest held level (release glides are quieter).
    //
    // Each detection function is held at its maximum over holdMs (> the longest period, E1: 24.3 ms):
    // per-millisecond values of a low note swing by > 20 dB within one period, the held value does
    // not, while a new pluck raises it at once. A function fires when its held value exceeds its
    // lowest held value in the last lookbackMs by riseDb, the full-band peak is above gateDbfs, and refractoryMs have passed
    // since the last attack. The attack is reported velocityWindowMs later, with the full-band peak
    // over that window (velocity measures the early attack only, not the decay).
    //
    // Latency (algorithmic, from the first sample of the pluck): up to one sub-hop to detect, plus
    // velocityWindowMs, plus up to one sub-hop of reporting granularity. Defaults: ~2-4 ms.
    //
    // Real-time: no allocation, no locks; O(1) per sample plus O(lookback) per sub-hop.
    class AttackDetector
    {
    public:
        struct Settings
        {
            double subHopMs = 1.0;          // detection granularity
            double highPassHz = 800.0;      // transient band edge
            double transientRiseDb = 12.0;  // rise of the high-passed energy over its recent minimum
            double levelRiseDb = 9.0;       // rise of the full-band peak over its recent minimum
            double holdMs = 26.0;           // max-hold of each detection function (> longest period)
            double lookbackMs = 12.0;       // span for the recent minimum of the held values
            double gateDbfs = -45.0;        // full-band peak needed for an attack
            double refractoryMs = 45.0;     // minimum spacing of attacks (16ths at 166 BPM = 90 ms)
            double velocityWindowMs = 2.0;  // peak measurement after detection, delays the report
            double velocityFloorDbfs = -40.0; // peak mapped to minVelocity
            double velocityCeilDbfs = -6.0;   // peak mapped to 127
            int minVelocity = 20;
            bool usePeriodicity = false;    // off by default: on the first real takes it found nearly every
                                            // ringing re-pluck but also fired ~3x too often (see docs/song-mode.md)
            double breakDb = -4.0;          // periodicity residual that counts as a break
            double steadyDb = -14.0;        // residual that counts as steady (required before a break)
            int breakHops = 2;              // consecutive sub-hops >= breakDb
            double breakMaxDropDb = 15.0;   // break ignored this far below the note's highest held level
            double maxPeriodMs = 30.0;      // longest period accepted by setPeriodSamples (E1 = 24.3 ms)

            bool isValid() const noexcept;
        };

        struct Attack
        {
            std::int64_t detectedAtSample = 0; // end of the sub-hop in which the rise was seen
            std::int64_t reportedAtSample = 0; // when it was reported (detected + velocity window)
            double peakLinear = 0.0;           // full-band peak over the velocity window
            int velocity = 0;                  // 1..127
            enum class Kind : std::uint8_t { transient, level, periodicity };
            Kind kind = Kind::transient;       // which detection function fired
        };

        // Real-time safe. Period of the currently sounding note in samples (from the pitch path, or
        // the expected note in Song Mode); 0 disables the periodicity function.
        void setPeriodSamples (double periodSamples) noexcept;

        // Non-real-time (computes coefficients, allocates the history). Returns false for invalid
        // settings / sample rate.
        bool prepare (double sampleRateHz, const Settings& settings);
        void reset() noexcept;

        // Real-time safe. Processes n samples; returns true and fills `attack` if an attack was
        // reported inside this block (at most one per call: refractoryMs exceeds any sane block).
        bool process (const float* samples, int n, Attack& attack) noexcept;

        std::int64_t getSamplePosition() const noexcept { return position; }
        double getLastLevelDbfs() const noexcept { return lastLevelDb; }

        static int velocityFromPeak (double peakLinear, const Settings& s) noexcept;

    private:
        static constexpr int maxLookback = 128;

        Settings settings;
        double sampleRateHz = 0.0;
        int subHopSamples = 48, lookbackHops = 12, holdHops = 26, refractorySamples = 0, velocityWindowSamples = 0;
        struct Biquad
        {
            double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
            double process (double x) noexcept
            {
                const double y = b0 * x + z1; // transposed direct form II
                z1 = b1 * x - a1 * y + z2;
                z2 = b2 * x - a2 * y;
                return y;
            }
        };
        std::array<Biquad, 2> highPass; // 4th-order Butterworth as two sections

        std::int64_t position = 0;
        int inHop = 0;
        double hopEnergy = 0.0, hopPeak = 0.0;
        std::array<double, maxLookback> transientRaw {}, levelRaw {};   // per sub-hop values (dB)
        std::array<double, maxLookback> transientHeld {}, levelHeld {}; // max-held values (dB)
        int ringPos = 0, ringCount = 0;
        std::int64_t lastAttackAt = -(std::int64_t { 1 } << 40);

        // Periodicity function.
        std::vector<float> history;  // input ring, power-of-two size > max period + sub-hop
        std::size_t historyMask = 0;
        int periodSamples = 0;
        double residualEnergy = 0.0, residualReference = 0.0;
        std::array<double, maxLookback> residualRing {};
        int breakRun = 0;
        double noteHeldMaxDb = -240.0; // highest held level since the last attack

        bool measuring = false;          // inside the velocity window of a detected attack
        Attack pending;
        double lastLevelDb = -180.0;
    };
}
