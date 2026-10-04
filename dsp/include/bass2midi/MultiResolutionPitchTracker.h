#pragma once

#include "bass2midi/NotePalette.h"
#include "bass2midi/PitchCandidate.h"
#include "bass2midi/YinPitchEstimator.h"

#include <array>

namespace bass2midi
{
    // Pitch-adaptive analysis windows ("window controller" + pitch candidates in CLAUDE.md).
    //
    // A ladder of YIN estimators runs on the NEWEST samples of the frame, each with a different
    // longest lag (lowest frequency) and a window of maxLag * (1 + integrationFraction). Per frame the
    // shortest rung whose estimate is valid, clear enough, and octave-safe wins:
    //
    //   inside range <=>  period <= (1 - edgeGuard) x the rung's longest lag. A period just beyond a
    //                     rung's lag range shows up as a d' minimum clipped at the range edge (e.g. A#1
    //                     read as B1 by the 60 Hz rung); such candidates are left to the next rung.
    //   octave-safe  <=>  the lag at twice the period was examined by the sub-octave check
    //                     (2P inside that rung's lag range), OR 2P exceeds the longest period of the
    //                     whole operating range (no lower octave of this note can be a bass note).
    //
    // This keeps short windows from reporting the 2nd harmonic of a low note (e.g. E2 for E1): such a
    // candidate is only accepted once a rung long enough to examine 2P confirms it. The longest rung
    // covers the whole range and is always octave-safe, so the result is never worse-covered than the
    // single full-range window - only earlier where a shorter window suffices.
    //
    // Resulting window (and thus roughly the Note On latency after an onset), with the defaults:
    // about 1.5 x the rung's longest period, i.e. ~3 periods for notes whose sub-octave is in range
    // (>= ~76 Hz) and ~1.5 periods of the next longer rung below that.
    //
    // Song palette (optional, see setPalette): a shorter rung's candidate that is NOT octave-safe may
    // still be used when the palette contains its note but neither of the two lower octaves. It only
    // fills frames the free tracker would leave without a valid result (typically the first frames
    // after an attack, while the longer windows still straddle the onset): as soon as the rung that
    // is octave-safe for it yields a valid result, that result wins, exactly as without a palette. A
    // wrong early guess (an off-palette note played below a palette note) is therefore corrected
    // rather than locked in.
    //
    // Real-time: prepare() allocates; estimate() is allocation-free. Cost: up to rungCount YIN runs per
    // frame (stops at the first accepted rung).
    class MultiResolutionPitchTracker
    {
    public:
        static constexpr int maxRungs = 8;

        struct Settings
        {
            double lowestHz = 38.0;               // operating range (E1 with > 1 semitone margin)
            double highestHz = 420.0;             // (G4 with > 1 semitone margin)
            std::array<double, maxRungs> rungLowestHz { 150.0, 120.0, 96.0, 76.0, 60.0, 48.0, 0.0, 0.0 };
                                                  // shortest first; 0 = unused; lowestHz is always added last
            double integrationFraction = 0.5;     // YIN integration length W = fraction * rung maxLag
            double threshold = 0.15;              // YIN absolute threshold (all rungs)
            double earlyMinClarity = 0.93;        // clarity a rung shorter than the longest needs to win
                                                  // (0.90 let a few weak-fundamental frames through, 0.93 none)
            double edgeGuard = 0.06;              // fraction of a shorter rung's lag range kept clear at the top
            double subOctaveRatio = 0.5;          // see YinPitchEstimator::Config
            double subOctaveFloor = 0.02;
            double silenceDropDb = 20.0;          // see YinPitchEstimator::Config (all rungs)
            double levelChangeDb = 12.0;          // see YinPitchEstimator::Config (all rungs)
            bool paletteOctaveRelief = true;      // use the song palette as described above (no effect when empty)

            bool isValid() const noexcept;
        };

        struct Result
        {
            PitchCandidate pitch;
            int rung = -1;          // index of the winning rung (0 = shortest), -1 if none valid
            int windowSamples = 0;  // window of the winning rung (or the longest rung if none)
            bool paletteRelieved = false; // the result exists only because of the song palette
        };

        // Non-real-time. Returns false for invalid settings or sample rate.
        bool prepare (double sampleRateHz, const Settings& settings);

        // Frame length the caller must provide to estimate(): the longest rung's window.
        int getFrameSamples() const noexcept { return frameSamples; }
        int getRungCount() const noexcept { return rungCount; }
        int getRungWindowSamples (int rung) const noexcept { return estimators[static_cast<size_t> (rung)].getConfig().windowSamples; }

        // Real-time safe. Notes in played-note space (before any MIDI transposition); empty = Free mode.
        void setPalette (const NotePalette& newPalette) noexcept { palette = newPalette; }
        const NotePalette& getPalette() const noexcept { return palette; }

        // Real-time safe. `frame` holds getFrameSamples() samples, oldest first.
        Result estimate (const float* frame) noexcept;

    private:
        Settings settings;
        double sampleRateHz = 0.0;
        int rungCount = 0;
        int frameSamples = 0;
        int rangeMaxLagSamples = 0;
        NotePalette palette;

        bool paletteRelieves (const PitchCandidate& candidate) const noexcept;
        std::array<YinPitchEstimator, maxRungs> estimators;
    };
}
