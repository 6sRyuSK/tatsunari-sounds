#pragma once
//
// DeqAnalyzerWindow.h — the SINGLE source of truth for the tn-equalizer analyser's window
// order and for the capacity of the display rings that have to hold it.
//
// The two numbers used to live apart: the pre/post rings were a hardcoded 1<<14 in the
// processor while the editor picked its FFT order. That was consistent only as long as the
// editor's order was ALSO fixed (the JUCE EqCurveComponent's 8192 points at every rate).
// Once the order became rate-adaptive the two crossed:
//
//   88.2 / 96 kHz : order 14 -> fftSize 16384 == ringSize. copyAnalyzerSamples' seqlock
//                   margin (ringSize - num) collapses to 0, so with audio running the
//                   consistency check can never pass and every window is torn.
//   176.4 / 192 kHz: order 15 -> fftSize 32768 >  ringSize. The masked read laps the ring,
//                   so the "window" is the SAME 16384 samples twice — the FFT sees a
//                   period-16384 signal and paints a comb (energy in even bins only).
//
// Deriving both numbers here makes `ringSize >= 2 * fftSize` true at every rate by
// construction instead of by assumption.
//
#include "factory_core/StftResolution.h"

namespace deq_analyzer
{
    // Analyser RESOLUTION, rate-adaptive (fixed orders are forbidden — CLAUDE.md):
    // order 13 = 8192 points (~5.9 Hz bins) at the 48 kHz reference — the low-frequency
    // resolution the JUCE editor had — plus one order per octave of sample rate, so the bin
    // width holds all the way up (192 kHz -> order 15 -> 192000/32768 = 5.86 Hz).
    inline constexpr int    kBaseOrder = 13;
    inline constexpr double kRefRate   = 48000.0;
    inline constexpr int    kMaxOrder  = 15;

    inline int fftOrder (double sampleRate) noexcept
    {
        return factory_core::fftOrderForSampleRate (sampleRate, kBaseOrder, kRefRate, kMaxOrder);
    }
    inline int fftSize (double sampleRate) noexcept { return 1 << fftOrder (sampleRate); }

    // CAPACITY of the pre/post display rings — sized for the LARGEST window the analyser can
    // ever request (kMaxOrder) plus a full window of head-room, so the reader always keeps a
    // non-zero seqlock margin and can never wrap inside one window.
    //
    // This is a worst-case capacity, NOT a resolution: the order above stays rate-adaptive,
    // which is what the "resolution follows the sample rate" rule is about. Keeping the
    // capacity a compile-time constant rather than resizing it per rate is deliberate — the
    // rings are read by the GUI while prepareToPlay / clap activate may run on another
    // thread, so reallocating them under the reader would trade a display artefact for a
    // use-after-free.
    inline constexpr int kRingSize = 1 << (kMaxOrder + 1); // 65536 = 2x the largest window
    inline constexpr int kRingMask = kRingSize - 1;

    static_assert (kRingSize - (1 << kMaxOrder) >= (1 << kMaxOrder),
                   "the display ring must leave a full analyser window of seqlock margin");
}
