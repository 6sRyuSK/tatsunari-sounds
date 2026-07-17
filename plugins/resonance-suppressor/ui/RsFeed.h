#pragma once

#include <atomic>
#include <cstddef>

//
// rs_ui::RsFeed — the UI data-feed contract for the resonance-suppressor Visage
// editor. It is the ONE seam between the JUCE-free editor and whatever produces
// the analyser data: the harness supplies a synthetic implementation (deterministic
// pink-ish input + moving resonant peaks + a reduction curtain that responds to
// Depth); P4's plugin shell supplies a real one that just forwards the processor's
// already-published lock-free snapshots.
//
// The three spectra mirror ResonanceSuppressorAudioProcessor's published arrays
// exactly (pubMag / pubMagPre / pubRed — each std::array<std::atomic<float>,
// kMaxBins>), so the real impl returns pointers straight into them:
//   * magDb()    — POST-suppression magnitude in dB   (processor.displayMagDb)
//   * magPreDb() — PRE-gain (input) magnitude in dB    (processor.displayMagPreDb)
//   * redDb()    — per-bin gain reduction in dB, <= 0  (processor.displayRedDb)
// bins() is the live bin count (processor.binsForDisplay) and sampleRate() the
// rate the bins were captured at (processor.getSampleRateForDisplay); bin k maps
// to f = k * sampleRate() / (2 * (bins() - 1)).
//
// The two write hooks are the editor's only outbound channel: Listen solos one
// reduction node's removed signal (setListenNode / getListenNode, id convention
// 0 = low cut, 1 = high cut, 2.. = bands, -1 = off — matching the processor's
// non-APVTS listenNode) and setDisplaySmoothMs pushes the analyser's display time
// smoothing (the ex-DevPanel "tempo smoothing"). The info getters are optional
// read-outs the editor may surface.
//
// Header-only, JUCE-free, visage-free. GUI-thread only on the editor side; the
// atomic spectra are the lock-free hand-off from the audio thread.
//
namespace rs_ui
{
    class RsFeed
    {
    public:
        virtual ~RsFeed() = default;

        // Live analyser grid.
        virtual int    bins() const noexcept = 0;
        virtual double sampleRate() const noexcept = 0;

        // Per-bin spectra (length >= bins()); read element k with [k].load(relaxed).
        virtual const std::atomic<float>* magDb()    const noexcept = 0; // post
        virtual const std::atomic<float>* magPreDb() const noexcept = 0; // pre
        virtual const std::atomic<float>* redDb()    const noexcept = 0; // reduction (<= 0)

        // Listen solo (editor -> processor). -1 disables.
        virtual void setListenNode (int nodeId) noexcept = 0;
        virtual int  getListenNode() const noexcept = 0;

        // Analyser display time smoothing, ms (>= 0). Ex-AnalyzerDevPanel control.
        virtual void setDisplaySmoothMs (float ms) noexcept = 0;

        // Optional info read-outs (a UI may show them; a feed may return 0 / "").
        virtual int         latencySamples() const noexcept { return 0; }
        virtual const char* qualityLabel()   const noexcept { return ""; }

        // Convenience: centre frequency of bin k for this feed's grid.
        float binFrequency (int bin) const noexcept
        {
            const int n = 2 * (bins() - 1);
            return n > 0 ? static_cast<float> (static_cast<double> (bin) * sampleRate() / n) : 0.0f;
        }
    };
}
