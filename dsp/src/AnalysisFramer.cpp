#include "bass2midi/AnalysisFramer.h"

#include <algorithm>

namespace bass2midi
{
    bool AnalysisFramer::prepare (int newWindowSamples, int newHopSamples)
    {
        if (newWindowSamples <= 0 || newHopSamples <= 0 || newHopSamples > newWindowSamples)
            return false;

        windowSamples = newWindowSamples;
        hopSamples = newHopSamples;
        ring.assign (static_cast<size_t> (windowSamples), 0.0f);
        frame.assign (static_cast<size_t> (windowSamples), 0.0f);
        reset();
        return true;
    }

    void AnalysisFramer::reset() noexcept
    {
        std::fill (ring.begin(), ring.end(), 0.0f);
        writePos = 0;
        totalSamples = 0;
    }

    void AnalysisFramer::linearise() noexcept
    {
        // writePos points at the oldest sample once the ring is full.
        const auto oldest = ring.begin() + writePos;
        const auto afterOldest = std::copy (oldest, ring.end(), frame.begin());
        std::copy (ring.begin(), oldest, afterOldest);
    }
}
