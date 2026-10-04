#pragma once

namespace bass2midi
{
    // Output of a single pitch-estimator run over one analysis window. Every pitch estimator
    // (reference YIN now, MPM / bass-adapted YIN later) reports this same structure so they can be
    // compared on the same corpus.
    struct PitchCandidate
    {
        bool valid = false;          // false: no lag passed the estimator's acceptance test
        double frequencyHz = 0.0;    // 0 when !valid
        double periodSamples = 0.0;  // interpolated period, 0 when !valid
        double clarity = 0.0;        // 0..1, estimator-specific confidence (YIN: 1 - d'(tau))
        double bestLagSamples = 0.0; // lag of the global d' minimum, reported even when !valid
        double windowRmsLinear = 0.0; // RMS of the analysed window, linear full scale

        // Octave diagnostics (YIN with sub-octave check enabled):
        bool subOctaveChecked = false; // the lag at twice the reported period was inside the search range and examined
        int subOctaveSwitches = 0;     // how often the estimate was moved down an octave by the check
    };
}
