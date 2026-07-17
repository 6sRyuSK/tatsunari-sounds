#pragma once

#include "RsFeed.h"

#include "factory_params/ParamStore.h"

#include <array>
#include <atomic>
#include <cmath>

//
// SyntheticRsFeed — the harness implementation of rs_ui::RsFeed: a deterministic
// analyser generator (no host, no audio thread) so the RS editor renders a lively,
// reproducible spectrum in the WASM dev loop. It publishes the same three atomic
// spectra a real feed would (pre / post / reduction) on a fixed 48 kHz / order-11
// grid, driven by:
//   * a pink-ish spectral hump (base shape);
//   * two-to-three resonant peaks that slide in frequency with an animation phase;
//   * a reduction "curtain" that hangs under those peaks, its DEPTH scaled by the
//     live "depth" parameter (observed via a ChangeSweeper) — so turning Depth up
//     visibly deepens the teal curtain.
// advance() steps the phase and regenerates a frame; when frozen it holds the last
// frame (so a re-snapshot is byte-identical -> deterministic screenshots).
//
class SyntheticRsFeed : public rs_ui::RsFeed
{
public:
    static constexpr int    kOrder = 11;
    static constexpr int    kN     = 1 << kOrder;   // 2048
    static constexpr int    kBins  = kN / 2 + 1;    // 1025
    static constexpr double kSr    = 48000.0;

    explicit SyntheticRsFeed (factory_params::ParamStore& store)
        : store_ (store), sweeper_ (store)
    {
        depthIdx_ = store_.indexOf ("depth");
        redScale_ = store_.value (depthIdx_) / 100.0f;
        regenerate();
    }

    int    bins() const noexcept override { return kBins; }
    double sampleRate() const noexcept override { return kSr; }
    const std::atomic<float>* magDb()    const noexcept override { return post_.data(); }
    const std::atomic<float>* magPreDb() const noexcept override { return pre_.data(); }
    const std::atomic<float>* redDb()    const noexcept override { return red_.data(); }

    void setListenNode (int id) noexcept override { listen_.store (id, std::memory_order_relaxed); }
    int  getListenNode() const noexcept override { return listen_.load (std::memory_order_relaxed); }
    void setDisplaySmoothMs (float ms) noexcept override { smoothMs_.store (ms, std::memory_order_relaxed); }
    int  latencySamples() const noexcept override { return kN; }
    const char* qualityLabel() const noexcept override { return "Normal"; }

    float displaySmoothMs() const noexcept { return smoothMs_.load (std::memory_order_relaxed); }

    // Freeze holds a DETERMINISTIC frame that reflects the live Depth: on freeze we
    // re-read Depth and regenerate at a fixed phase, so a frozen screenshot is stable
    // AND the teal reduction curtain tracks the Depth param (which the animated path
    // only picks up via the ChangeSweeper on the next advance()).
    void setFrozen (bool frozen)
    {
        frozen_ = frozen;
        if (frozen_)
        {
            redScale_ = store_.value (depthIdx_) / 100.0f;
            phase_ = 0.35;
            regenerate();
        }
    }

    // Advance one animation frame (unless frozen). Regenerates the three spectra.
    void advance()
    {
        if (frozen_) return;
        sweeper_.sweep (store_, [this] (int i) { if (i == depthIdx_) redScale_ = store_.value (depthIdx_) / 100.0f; });
        phase_ += 0.03;
        regenerate();
    }

private:
    static float bump (float f, float fc, float amp, float widthOct)
    {
        const float x = std::log2 (std::max (1.0f, f) / std::max (1.0f, fc)) / std::max (0.01f, widthOct);
        return amp * std::exp (-0.5f * x * x);
    }

    void regenerate()
    {
        const double s = 0.5 + 0.5 * std::sin (phase_);
        const double s2 = 0.5 + 0.5 * std::sin (phase_ * 0.7 + 1.0);
        const double s3 = 0.5 + 0.5 * std::sin (phase_ * 0.5 + 2.3);
        const float f1 = (float) (300.0 * std::pow (10.0, 0.5 * s));    // 300..950
        const float f2 = (float) (2000.0 * std::pow (10.0, 0.35 * s2)); // 2k..4.5k
        const float f3 = (float) (6000.0 * std::pow (10.0, 0.25 * s3)); // 6k..10.7k

        for (int k = 0; k < kBins; ++k)
        {
            const float f = (float) ((double) k * kSr / kN);
            const float lf = std::log10 (std::max (20.0f, f));
            const float base = -30.0f - 9.0f * std::fabs (lf - 2.845f); // hump ~700 Hz

            // Sharp resonance peaks for the PRE spectrum.
            const float pk = bump (f, f1, 24.0f, 0.12f)
                           + bump (f, f2, 20.0f, 0.11f)
                           + bump (f, f3, 16.0f, 0.10f);
            // Broader, deeper reduction "curtains" around those resonances, scaled
            // by Depth — so the teal curtain hangs prominently and the POST spectrum
            // dips into notches (like the design reference).
            const float redBumps = bump (f, f1, 1.0f, 0.30f)
                                  + bump (f, f2, 1.0f, 0.26f)
                                  + bump (f, f3, 1.0f, 0.24f);

            const float pre = std::clamp (base + pk, -90.0f, 6.0f);
            const float redAmt = std::clamp (-redScale_ * redBumps * 42.0f, -48.0f, 0.0f);
            const float post = std::clamp (pre + redAmt, -90.0f, 6.0f);

            pre_[(std::size_t) k].store (pre, std::memory_order_relaxed);
            post_[(std::size_t) k].store (post, std::memory_order_relaxed);
            red_[(std::size_t) k].store (redAmt, std::memory_order_relaxed);
        }
    }

    factory_params::ParamStore& store_;
    factory_params::ChangeSweeper sweeper_;
    int   depthIdx_ = 0;
    float redScale_ = 0.3f;
    double phase_ = 0.35;   // fixed default => reproducible first/frozen frame
    bool  frozen_ = false;

    std::array<std::atomic<float>, kBins> pre_ {}, post_ {}, red_ {};
    std::atomic<int>   listen_ { -1 };
    std::atomic<float> smoothMs_ { 50.0f };
};
