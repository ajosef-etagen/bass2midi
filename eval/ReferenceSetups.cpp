#include "ReferenceSetups.h"

#include "bass2midi/Fft.h"

#include <algorithm>
#include <cmath>

namespace bass2midi::eval
{
    ReferenceSetup warfLikeYin (double sampleRateHz)
    {
        const int longestPeriod = static_cast<int> (std::ceil (sampleRateHz / warfMinFrequencyHz));
        const int window = Fft::nextPowerOfTwo (2 * longestPeriod);

        ReferenceSetup setup;
        setup.name = "warf-like-yin";
        setup.yin.windowSamples = window;
        setup.yin.minLagSamples = 2;
        setup.yin.maxLagSamples = window / 2;
        setup.yin.threshold = referenceYinThreshold;
        setup.hopSamples = window / 4;
        return setup;
    }

    ReferenceSetup fullRangeYin (double sampleRateHz)
    {
        ReferenceSetup setup;
        setup.name = "full-range-yin";
        setup.yin = YinPitchEstimator::Config::forFrequencyRange (sampleRateHz, fullRangeMinFrequencyHz,
                                                                  fullRangeMaxFrequencyHz, referenceYinThreshold);
        setup.hopSamples = std::max (1, static_cast<int> (std::lround (sampleRateHz * fullRangeHopSeconds)));
        return setup;
    }
}
