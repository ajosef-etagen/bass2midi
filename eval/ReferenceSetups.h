#pragma once

#include "bass2midi/YinPitchEstimator.h"

#include <string>

namespace bass2midi::eval
{
    // Fixed estimator + framing configurations used as measurement references. These are NOT the
    // Bass2MIDI tracker; they exist so later changes have a reproducible "before" to compare with.
    struct ReferenceSetup
    {
        std::string name;
        YinPitchEstimator::Config yin;
        int hopSamples = 0;
    };

    // Plain YIN framed the way Warf (github.com/dferreiramarques/warf, MIT) frames it by default:
    // window = next power of two >= 2 * ceil(sampleRate / 60 Hz), hop = window / 4, lags searched
    // from 2 up to window / 2, threshold 0.15. Reimplemented from its documented parameters; no
    // Warf code is used.
    ReferenceSetup warfLikeYin (double sampleRateHz);

    // Plain YIN covering the whole V1 range: lags for 38-420 Hz (E1 and G4 each with > 1 semitone
    // margin), window = 2 * longest lag, hop = 2.5 ms. Shows what full-range coverage costs with a
    // single fixed window and no bass-specific logic.
    ReferenceSetup fullRangeYin (double sampleRateHz);

    // Named threshold values for the reference setups, with units.
    inline constexpr double warfMinFrequencyHz = 60.0;
    inline constexpr double fullRangeMinFrequencyHz = 38.0;
    inline constexpr double fullRangeMaxFrequencyHz = 420.0;
    inline constexpr double fullRangeHopSeconds = 0.0025;
    inline constexpr double referenceYinThreshold = 0.15;
}
