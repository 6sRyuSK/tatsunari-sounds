//
// dsp_test.cpp — headless verification of the mochi-stretch DSP core.
//
// SCAFFOLD STUB: this test FAILS on purpose until real spec-based checks are
// written. See .claude/skills/write-dsp-test and docs/regression-policy.md —
// every check needs an independent oracle (never derived from the code under
// test) and must run across the full sample-rate matrix.
//
#include "factory_core/testing/DspInvariants.h"

#include <cstdio>
#include <string>

namespace
{
    namespace fct = factory_core::testing;

    int g_failures = 0;
    void fail (const std::string& m) { std::printf ("  FAIL: %s\n", m.c_str()); ++g_failures; }

    void coreTests (double Fs)
    {
        std::printf ("mochi-stretch core @ Fs=%.0f\n", Fs);

        // TODO(scaffold): replace with real checks. Typical gates:
        //   - independent static oracle for the quantitative behaviour
        //     (z-domain for filters; analytic counts/levels for non-linear)
        //   - fct::impulseResponseNonIncreasing (...) at the WORST-CASE setting
        //     for any feedback path
        //   - fct::allFinite / fct::peakAbs over a long hold with a realistic
        //     peak bound (never a 1e6 "not-NaN" tolerance)
        //   - fct::resolutionFollowsSampleRate (Fs) if the core uses FFT/STFT
        fail ("dsp_test is a scaffold stub — write spec-based checks for mochi-stretch");
    }
}

int main (int argc, char** argv)
{
    // Full standard rate matrix by default; CTest passes one rate as argv[1].
    for (double Fs : fct::sampleRatesFromArgs (argc, argv))
        coreTests (Fs);

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
