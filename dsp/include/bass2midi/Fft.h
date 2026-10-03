#pragma once

#include <complex>
#include <vector>

namespace bass2midi
{
    // Minimal iterative radix-2 complex FFT in double precision.
    // prepare() allocates (non-real-time); perform() is allocation-free and bounded.
    class Fft
    {
    public:
        // size must be a power of two >= 2.
        void prepare (int size);

        int getSize() const noexcept { return size; }

        // In-place transform of exactly getSize() values. The inverse is scaled by 1/size.
        void perform (std::complex<double>* data, bool inverse) const noexcept;

        static int nextPowerOfTwo (int n) noexcept;

    private:
        int size = 0;
        std::vector<int> bitReversed;
        std::vector<std::complex<double>> twiddles; // exp(-2*pi*i*k/size), k < size/2
    };
}
