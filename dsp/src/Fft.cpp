#include "bass2midi/Fft.h"

#include <cassert>
#include <cmath>
#include <numbers>
#include <utility>

namespace bass2midi
{
    int Fft::nextPowerOfTwo (int n) noexcept
    {
        int p = 1;
        while (p < n)
            p *= 2;
        return p;
    }

    void Fft::prepare (int newSize)
    {
        assert (newSize >= 2 && (newSize & (newSize - 1)) == 0);
        size = newSize;

        int bits = 0;
        while ((1 << bits) < size)
            ++bits;

        bitReversed.assign (static_cast<size_t> (size), 0);
        for (int i = 0; i < size; ++i)
        {
            int reversed = 0;
            for (int b = 0; b < bits; ++b)
                if ((i >> b) & 1)
                    reversed |= 1 << (bits - 1 - b);
            bitReversed[static_cast<size_t> (i)] = reversed;
        }

        twiddles.resize (static_cast<size_t> (size / 2));
        for (int k = 0; k < size / 2; ++k)
        {
            const double angle = -2.0 * std::numbers::pi * static_cast<double> (k) / static_cast<double> (size);
            twiddles[static_cast<size_t> (k)] = { std::cos (angle), std::sin (angle) };
        }
    }

    void Fft::perform (std::complex<double>* data, bool inverse) const noexcept
    {
        for (int i = 0; i < size; ++i)
        {
            const int j = bitReversed[static_cast<size_t> (i)];
            if (i < j)
                std::swap (data[i], data[j]);
        }

        for (int length = 2; length <= size; length *= 2)
        {
            const int half = length / 2;
            const int twiddleStride = size / length;

            for (int start = 0; start < size; start += length)
            {
                for (int k = 0; k < half; ++k)
                {
                    auto w = twiddles[static_cast<size_t> (k * twiddleStride)];
                    if (inverse)
                        w = std::conj (w);

                    const auto even = data[start + k];
                    const auto odd = data[start + k + half] * w;
                    data[start + k] = even + odd;
                    data[start + k + half] = even - odd;
                }
            }
        }

        if (inverse)
        {
            const double scale = 1.0 / static_cast<double> (size);
            for (int i = 0; i < size; ++i)
                data[i] *= scale;
        }
    }
}
