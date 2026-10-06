#pragma once

#include <cstdint>
#include <vector>

namespace bass2midi
{
    // Collects arbitrarily sized input blocks into overlapping analysis windows.
    //
    // Frames are emitted on a fixed absolute grid: the first once windowSamples samples have been
    // received, then every hopSamples samples. Frame timing therefore depends only on the sample
    // count since reset(), never on the host block size - a property the tests rely on.
    //
    // Latency contributed here: a frame describes samples [end - windowSamples, end), and is
    // available at most hopSamples - 1 samples after any given input sample entered the window.
    class AnalysisFramer
    {
    public:
        // Non-real-time: allocates. Returns false for non-positive sizes or hop > window.
        bool prepare (int windowSamples, int hopSamples);

        // Real-time safe. Clears history and restarts the frame grid at sample 0.
        void reset() noexcept;

        int getWindowSamples() const noexcept { return windowSamples; }
        int getHopSamples() const noexcept { return hopSamples; }

        // Real-time safe. Calls onFrame (window, endSampleIndex, indexInBlock) for each completed
        // frame, where `window` is windowSamples samples (oldest first), `endSampleIndex` the
        // absolute index one past the frame's newest sample, and `indexInBlock` the position of that
        // newest sample inside `samples`.
        template <typename OnFrame>
        void push (const float* samples, int numSamples, OnFrame&& onFrame) noexcept
        {
            for (int i = 0; i < numSamples; ++i)
            {
                ring[static_cast<size_t> (writePos)] = samples[i];
                writePos = writePos + 1 == windowSamples ? 0 : writePos + 1;
                ++totalSamples;

                if (totalSamples >= windowSamples && (totalSamples - windowSamples) % hopSamples == 0)
                {
                    linearise();
                    onFrame (static_cast<const float*> (frame.data()), totalSamples, i);
                }
            }
        }

    private:
        void linearise() noexcept;

        std::vector<float> ring, frame;
        int windowSamples = 0;
        int hopSamples = 0;
        int writePos = 0;
        std::int64_t totalSamples = 0;
    };
}
