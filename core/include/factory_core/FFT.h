#pragma once
//
// factory_core/FFT.h — minimal radix-2 Cooley–Tukey complex FFT, JUCE-independent
// and header-only so it can be exercised headless. `prepare(order)` precomputes
// the bit-reversal permutation and twiddle factors; `forward` / `inverse` then run
// in place on a caller-owned, preallocated buffer of `size()` complex values and
// never allocate (safe on the audio thread). `inverse` applies the 1/N scaling, so
// inverse(forward(x)) == x.
//
#include <complex>
#include <vector>
#include <cmath>

namespace factory_core
{
    class FFT
    {
    public:
        using cd = std::complex<double>;

        void prepare (int order)
        {
            ord = order;
            n   = 1 << order;

            rev.resize ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                int r = 0;
                for (int j = 0; j < ord; ++j)
                    r |= ((i >> j) & 1) << (ord - 1 - j);
                rev[(size_t) i] = r;
            }

            tw.resize ((size_t) (n / 2));
            for (int i = 0; i < n / 2; ++i)
                tw[(size_t) i] = std::polar (1.0, -2.0 * kPi * i / n);
        }

        int size() const noexcept { return n; }

        void forward (cd* a) const noexcept { transform (a, false); }
        void inverse (cd* a) const noexcept { transform (a, true); }

    private:
        void transform (cd* a, bool inv) const noexcept
        {
            for (int i = 0; i < n; ++i)
            {
                const int r = rev[(size_t) i];
                if (r > i) std::swap (a[i], a[r]);
            }

            for (int len = 2; len <= n; len <<= 1)
            {
                const int half = len >> 1;
                const int step = n / len;
                for (int i = 0; i < n; i += len)
                {
                    for (int j = 0; j < half; ++j)
                    {
                        cd w = tw[(size_t) (j * step)];
                        if (inv) w = std::conj (w);
                        const cd u = a[i + j];
                        const cd v = a[i + j + half] * w;
                        a[i + j]        = u + v;
                        a[i + j + half] = u - v;
                    }
                }
            }

            if (inv)
            {
                const double s = 1.0 / n;
                for (int i = 0; i < n; ++i) a[i] *= s;
            }
        }

        static constexpr double kPi = 3.14159265358979323846;
        int ord = 0, n = 0;
        std::vector<int> rev;
        std::vector<cd>  tw;
    };
} // namespace factory_core
