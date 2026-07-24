//
// DeqCurveView.cpp — the Visage port of the JUCE EqCurveComponent (see DeqCurveView.h).
// Draws the analyser + per-band response fills + combined curve + draggable nodes, and
// edits freq/gain/Q through the ParamStore gesture path.
//
#include "DeqCurveView.h"

#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Chrome.h"

#include "factory_core/Filters.h"
#include "factory_core/Biquad.h"
#include "factory_core/StftResolution.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <string>

namespace deq_ui
{
    using factory_ui_visage::LogFreqAxis;
    using factory_ui_visage::VerticalAxis;

    namespace
    {
        // Analyser FFT order. Matches the old JUCE editor's 8192-point (order 13)
        // low-frequency resolution (~5.9 Hz bins) at the 48 kHz reference, and — unlike
        // the old fixed order — scales one order per octave of sample rate so that bin
        // width holds through 192 kHz (fixed orders are forbidden by CLAUDE.md). Still
        // sourced from factory_core::fftOrderForSampleRate, just with a finer base.
        int analyzerOrder (double sampleRate)
        {
            return factory_core::fftOrderForSampleRate (sampleRate, /*baseOrder=*/13,
                                                        /*refSampleRate=*/48000.0, /*maxOrder=*/15);
        }

        std::string idFor (int band, const char* suffix)
        {
            return "b" + std::to_string (band) + "_" + suffix;
        }

        // H(e^jw) of one biquad section (mirrors EqCurveComponent::evalH).
        std::complex<double> evalH (const factory_core::BiquadCoeffs& c,
                                    std::complex<double> z1, std::complex<double> z2)
        {
            return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
        }

        float gainToDb (double linear)
        {
            return (float) (20.0 * std::log10 (std::max (linear, 1.0e-12)));
        }

        std::uint32_t withAlpha (std::uint32_t argb, float a)
        {
            const std::uint32_t alpha = (std::uint32_t) std::clamp (a * 255.0f, 0.0f, 255.0f);
            return (alpha << 24) | (argb & 0x00ffffffu);
        }
    }

    DeqCurveView::DeqCurveView (const factory_ui_visage::Theme& theme,
                                factory_params::ParamStore& store, DeqFeed& feed)
        : theme_ (theme), store_ (store), feed_ (feed)
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            BandIx& ix = bx_[(size_t) b];
            ix.on    = store_.indexOf (idFor (b, "on"));
            ix.byp   = store_.indexOf (idFor (b, "byp"));
            ix.lsn   = store_.indexOf (idFor (b, "lsn"));
            ix.chan  = store_.indexOf (idFor (b, "chan"));
            ix.type  = store_.indexOf (idFor (b, "type"));
            ix.freq  = store_.indexOf (idFor (b, "freq"));
            ix.gain  = store_.indexOf (idFor (b, "gain"));
            ix.q     = store_.indexOf (idFor (b, "q"));
            ix.slope = store_.indexOf (idFor (b, "slope"));
            ix.dyn   = store_.indexOf (idFor (b, "dyn"));
        }
    }

    // ── axes ──────────────────────────────────────────────────────────────────
    float DeqCurveView::freqToX (float f) const { return LogFreqAxis { plot_.x, plot_.w }.freqToX (f); }
    float DeqCurveView::xToFreq (float x) const { return LogFreqAxis { plot_.x, plot_.w }.xToFreq (x); }
    float DeqCurveView::gainToY (float gdb) const
    {
        return plot_.y + ((kMaxGain - gdb) / (2.0f * kMaxGain)) * plot_.h;
    }
    float DeqCurveView::yToGain (float y) const
    {
        return kMaxGain - ((y - plot_.y) / plot_.h) * (2.0f * kMaxGain);
    }

    bool DeqCurveView::isCutType (int b) const
    {
        const int t = bandTypeInt (b);
        return t == (int) factory_core::BandType::HighPass || t == (int) factory_core::BandType::LowPass;
    }
    int DeqCurveView::bandStages (int b) const
    {
        if (! isCutType (b)) return 1;
        const int slope = (int) store_.value (bx_[(size_t) b].slope);
        return std::clamp (slope + 1, 1, factory_core::DynamicEqBand::kMaxStages);
    }
    float DeqCurveView::bandGainDb (int b) const
    {
        if (bandDynamic (b) && ! bandBypassed (b)) return feed_.liveGainDb (b);
        return store_.value (bx_[(size_t) b].gain);
    }
    std::uint32_t DeqCurveView::bandColour (int b) const
    {
        const auto& bc = theme_.palette.bandColours;
        return bc[(size_t) (b % (int) bc.size())];
    }
    visage::Point DeqCurveView::nodePos (int b) const
    {
        const float f = store_.value (bx_[(size_t) b].freq);
        const float g = isCutType (b) ? 0.0f : bandGainDb (b);
        return { freqToX (f), gainToY (std::clamp (g, -kMaxGain, kMaxGain)) };
    }

    void DeqCurveView::setSelectedBand (int b) { if (selected_ != b) { selected_ = b; redraw(); } }
    void DeqCurveView::setFrozen (bool frozen) { frozen_ = frozen; redraw(); }

    void DeqCurveView::resized() { computeLayout(); }

    void DeqCurveView::computeLayout()
    {
        plot_ = { 12.0f, 12.0f, std::max (1.0f, width() - 24.0f), std::max (1.0f, height() - 24.0f - 14.0f) };
        // Pre / Post toggle chips, top-right of the plot.
        const float chipW = 52.0f, chipH = 18.0f, gap = 6.0f;
        const float top = plot_.y + 8.0f;
        postChip_ = { plot_.x + plot_.w - 8.0f - chipW, top, chipW, chipH };
        preChip_  = { postChip_.x - gap - chipW, top, chipW, chipH };
    }

    // ── draw ───────────────────────────────────────────────────────────────────
    void DeqCurveView::draw (visage::Canvas& canvas)
    {
        // Advance the analyser at ~30 Hz (matching the JUCE editor), decoupled from the
        // paint rate: pull fresh samples + smooth only when a 1/30 s tick is due, so the
        // trace does not move at the (double-speed) vsync rate.
        const double t = canvas.time();
        const bool advance = ! frozen_ && (lastAdvance_ < 0.0 || (t - lastAdvance_) >= (1.0 / kAnalyzerFps));
        if (advance)
        {
            lastAdvance_ = t;
            if (onTick) onTick();
            updateSpectra();
        }

        computeLayout();

        const auto& p = theme_.palette;

        // Card background with a gentle top-light gradient.
        canvas.setColor (visage::Brush::vertical (visage::Color (p.panel), visage::Color (p.panelLo)));
        canvas.roundedRectangle (0.0f, 0.0f, width(), height(), 10.0f);

        drawGrid (canvas);
        if (showPre_)  drawSpectrum (canvas, 0, false, p.accent);
        if (showPost_) drawSpectrum (canvas, 1, true,  p.positive);
        drawBandsAndResponse (canvas);
        drawNodes (canvas);
        drawModeChip (canvas);

        canvas.setColor (visage::Color (p.track));
        canvas.roundedRectangleBorder (0.5f, 0.5f, width() - 1.0f, height() - 1.0f, 10.0f, 1.2f);

        if (! frozen_) redraw(); // self-drive the analyser
    }

    void DeqCurveView::drawGrid (visage::Canvas& canvas)
    {
        const auto& p = theme_.palette;
        const visage::Font font = factory_ui_visage::regularFont (10.0f);

        struct FL { float f; const char* s; };
        for (auto fl : { FL{50,"50"}, FL{100,"100"}, FL{200,"200"}, FL{500,"500"},
                         FL{1000,"1k"}, FL{2000,"2k"}, FL{5000,"5k"}, FL{10000,"10k"} })
        {
            const float x = freqToX (fl.f);
            canvas.setColor (withAlpha (p.track, 0.7f));
            canvas.segment (x, plot_.y, x, plot_.y + plot_.h, 1.0f, false);
            canvas.setColor (visage::Color (p.textDim));
            canvas.text (fl.s, font, visage::Font::kCenter, x - 18.0f, plot_.y + plot_.h + 1.0f, 36.0f, 12.0f);
        }
        for (float gdb = -kMaxGain + 6.0f; gdb < kMaxGain; gdb += 6.0f)
        {
            const float y = gainToY (gdb);
            const bool zero = std::abs (gdb) < 0.01f;
            canvas.setColor (withAlpha (p.track, zero ? 0.95f : 0.45f));
            canvas.segment (plot_.x, y, plot_.x + plot_.w, y, 1.0f, false);
            if (zero || std::abs (std::fmod (gdb, 12.0f)) < 0.01f)
            {
                canvas.setColor (visage::Color (p.textDim));
                const std::string s = (gdb > 0 ? "+" : "") + std::to_string ((int) gdb);
                canvas.text (s, font, visage::Font::kLeft, plot_.x + 2.0f, y - 11.0f, 30.0f, 11.0f);
            }
        }
    }

    // Pull the latest ring samples into whichever spectra are shown and advance their
    // smoothing/peak state. Called at the ~30 Hz analyser rate, NOT per paint.
    void DeqCurveView::updateSpectra()
    {
        const double sr = feed_.sampleRate();
        if (sr <= 0.0) return;
        if (sr != lastSr_) { const int ord = analyzerOrder (sr); modelPre_.setup (ord); modelPost_.setup (ord); lastSr_ = sr; }

        factory_ui_visage::SpectrumModel::Options opt;
        opt.tiltDbPerOct = 3.0f; // flatten pink-ish spectra for display

        auto pump = [&] (factory_ui_visage::SpectrumModel& model, bool post)
        {
            if (model.size() <= 0) model.setup (analyzerOrder (sr));
            const int fftSize = model.size();
            if (fftSize <= 0) return;
            scratch_.resize ((size_t) fftSize);
            feed_.copyAnalyzerSamples (scratch_.data(), fftSize, post);
            model.writeSamples (scratch_.data(), fftSize);
            model.update (sr, opt);
        };
        if (showPre_)  pump (modelPre_,  false);
        if (showPost_) pump (modelPost_, true);
    }

    void DeqCurveView::drawSpectrum (visage::Canvas& canvas, int channel, bool post, std::uint32_t colour)
    {
        (void) channel;
        auto& model = post ? modelPost_ : modelPre_;
        const double sr = feed_.sampleRate();
        const int fftSize = model.size();
        if (sr <= 0.0 || fftSize <= 0) return;

        const LogFreqAxis xAxis { plot_.x, plot_.w };
        const VerticalAxis yAxis { plot_.y, plot_.h, kMaxDb, kMinDb };
        const float baselineY = plot_.y + plot_.h;

        // Smoothed trace (line) + its filled area to the baseline, plus an independent
        // peak-hold outline — the JUCE EqCurveComponent drew the peak line separately
        // from the smoothed fill, and the house SpectrumView keeps all three.
        visage::Path line, area, peak;
        bool started = false;
        float lastX = plot_.x;
        const int numBins = model.numBins();
        for (int bin = 1; bin < numBins; ++bin)
        {
            const float freq = factory_ui_visage::SpectrumModel::binFrequency (bin, sr, fftSize);
            if (freq < 20.0f || freq > 20000.0f) continue;
            const float x  = xAxis.freqToX (freq);
            const float y  = yAxis.toY (model.smoothedDb (bin));
            const float yp = yAxis.toY (model.peakDb (bin));
            if (! started)
            {
                line.moveTo (x, y);
                area.moveTo (x, baselineY); area.lineTo (x, y);
                peak.moveTo (x, yp);
                started = true;
            }
            else
            {
                line.lineTo (x, y);
                area.lineTo (x, y);
                peak.lineTo (x, yp);
            }
            lastX = x;
        }
        if (! started) return;
        area.lineTo (lastX, baselineY);
        area.close();

        canvas.setColor (visage::Brush::vertical (visage::Color (withAlpha (colour, 0.28f)),
                                                  visage::Color (withAlpha (colour, 0.02f))));
        canvas.fill (area);
        canvas.setColor (withAlpha (colour, 0.55f));
        canvas.fill (peak.stroke (1.0f, visage::Path::Join::Round, visage::Path::EndCap::Round));
        canvas.setColor (withAlpha (colour, 0.80f));
        canvas.fill (line.stroke (1.4f, visage::Path::Join::Round, visage::Path::EndCap::Round));
    }

    void DeqCurveView::drawBandsAndResponse (visage::Canvas& canvas)
    {
        const auto& p = theme_.palette;
        const double sr = feed_.sampleRate();
        if (sr <= 0.0) return;

        constexpr int kMaxStages = factory_core::DynamicEqBand::kMaxStages;
        std::array<std::array<factory_core::BiquadCoeffs, (size_t) kMaxStages>, (size_t) kNumBands> coeffs {};
        std::array<int, (size_t) kNumBands> stages {};
        std::array<bool, (size_t) kNumBands> on {}, byp {};

        for (int b = 0; b < kNumBands; ++b)
        {
            on[(size_t) b]  = bandOn (b);
            byp[(size_t) b] = bandBypassed (b);
            if (! on[(size_t) b]) continue;
            const auto type = (factory_core::BandType) bandTypeInt (b);
            const double f = store_.value (bx_[(size_t) b].freq);
            const double q = store_.value (bx_[(size_t) b].q);
            if (isCutType (b))
            {
                const int n = bandStages (b);
                stages[(size_t) b] = n;
                for (int s = 0; s < n; ++s)
                    coeffs[(size_t) b][(size_t) s] = factory_core::designHpLpStage (type, f, q, s, n, sr);
            }
            else
            {
                stages[(size_t) b] = 1;
                coeffs[(size_t) b][0] = factory_core::designFilter (type, f, bandGainDb (b), q, sr);
            }
        }

        const float y0 = gainToY (0.0f);
        const int steps = std::max (2, (int) (plot_.w / 2.0f)); // ~2px columns
        std::array<visage::Path, (size_t) kNumBands> bandFill;
        std::array<bool, (size_t) kNumBands> startedBand {};
        visage::Path total;

        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot_.x + (float) i * plot_.w / (float) steps;
            const float freq = xToFreq (x);
            const double wd = 2.0 * 3.14159265358979323846 * freq / sr;
            const std::complex<double> z1 = std::exp (std::complex<double> (0.0, -wd));
            const std::complex<double> z2 = std::exp (std::complex<double> (0.0, -2.0 * wd));

            std::complex<double> h (1.0, 0.0);
            for (int b = 0; b < kNumBands; ++b)
            {
                if (! on[(size_t) b]) continue;
                std::complex<double> hb (1.0, 0.0);
                for (int s = 0; s < stages[(size_t) b]; ++s)
                    hb *= evalH (coeffs[(size_t) b][(size_t) s], z1, z2);
                if (! byp[(size_t) b]) h *= hb;

                const float yB = gainToY (std::clamp (gainToDb (std::abs (hb)), -kMaxGain, kMaxGain));
                auto& fp = bandFill[(size_t) b];
                if (! startedBand[(size_t) b]) { fp.moveTo (x, y0); fp.lineTo (x, yB); startedBand[(size_t) b] = true; }
                else fp.lineTo (x, yB);
            }

            const float yT = gainToY (std::clamp (gainToDb (std::abs (h)), -kMaxGain, kMaxGain));
            if (i == 0) total.moveTo (x, yT);
            else        total.lineTo (x, yT);
        }

        // Per-band translucent fills (active-band emphasised).
        for (int b = 0; b < kNumBands; ++b)
        {
            if (! startedBand[(size_t) b]) continue;
            const bool active = (b == selected_ || b == hover_);
            if (byp[(size_t) b])
            {
                canvas.setColor (withAlpha (p.textDim, active ? 0.85f : 0.55f));
                canvas.fill (bandFill[(size_t) b].stroke (active ? 1.4f : 1.0f, visage::Path::Join::Round, visage::Path::EndCap::Round));
                continue;
            }
            const std::uint32_t col = bandColour (b);
            visage::Path fp = bandFill[(size_t) b];
            fp.lineTo (plot_.x + plot_.w, y0);
            fp.close();
            canvas.setColor (withAlpha (col, active ? 0.26f : 0.14f));
            canvas.fill (fp);
            canvas.setColor (withAlpha (col, active ? 0.9f : 0.55f));
            canvas.fill (bandFill[(size_t) b].stroke (active ? 1.6f : 1.0f, visage::Path::Join::Round, visage::Path::EndCap::Round));
        }

        // Combined response: soft glow under a crisp coral stroke.
        canvas.setColor (withAlpha (p.accent, 0.18f));
        canvas.fill (total.stroke (6.0f, visage::Path::Join::Round, visage::Path::EndCap::Round));
        canvas.setColor (visage::Color (p.accent));
        canvas.fill (total.stroke (2.4f, visage::Path::Join::Round, visage::Path::EndCap::Round));
    }

    void DeqCurveView::drawNodes (visage::Canvas& canvas)
    {
        const auto& p = theme_.palette;
        const visage::Font font = factory_ui_visage::boldFont (10.0f);

        for (int b = 0; b < kNumBands; ++b)
        {
            if (! bandOn (b)) continue;
            const visage::Point c = nodePos (b);
            const bool sel = (b == selected_);
            const bool hov = (b == hover_);
            const float rad = sel ? 9.0f : (hov ? 8.0f : 7.0f);
            const bool byp = bandBypassed (b);
            const std::uint32_t col = byp ? p.textDim : bandColour (b);

            if (! byp && (sel || hov))
            {
                canvas.setColor (withAlpha (col, 0.30f));
                const float gr = rad * 2.0f;
                canvas.circle (c.x - gr, c.y - gr, gr * 2.0f);
            }
            // White rim + coloured core.
            canvas.setColor (0xffffffff);
            canvas.circle (c.x - rad - 2.0f, c.y - rad - 2.0f, (rad + 2.0f) * 2.0f);
            canvas.setColor (withAlpha (col, byp ? 0.55f : 1.0f));
            canvas.circle (c.x - rad, c.y - rad, rad * 2.0f);
            if (sel)
            {
                canvas.setColor (withAlpha (0xffffffff, 0.9f));
                canvas.ring (c.x - rad, c.y - rad, rad * 2.0f, 2.0f);
            }
            if (bandListening (b))
            {
                canvas.setColor (visage::Color (p.positive));
                const float lr = rad + 5.0f;
                canvas.ring (c.x - lr, c.y - lr, lr * 2.0f, 2.0f);
            }
            else if (! byp && bandDynamic (b))
            {
                canvas.setColor (withAlpha (0xffffffff, 0.85f));
                const float dr = rad + 3.5f;
                canvas.ring (c.x - dr, c.y - dr, dr * 2.0f, 1.2f);
            }
            canvas.setColor (0xffffffff);
            canvas.text (std::to_string (b + 1), font, visage::Font::kCenter, c.x - rad, c.y - rad, rad * 2.0f, rad * 2.0f);
        }
    }

    void DeqCurveView::drawModeChip (visage::Canvas& canvas)
    {
        const auto& p = theme_.palette;
        const visage::Font font = factory_ui_visage::boldFont (10.0f);
        auto chip = [&] (const Rect& r, const char* label, bool on, std::uint32_t accent)
        {
            canvas.setColor (on ? withAlpha (accent, 0.9f) : withAlpha (p.track, 0.6f));
            canvas.roundedRectangle (r.x, r.y, r.w, r.h, 6.0f);
            canvas.setColor (on ? 0xffffffff : (std::uint32_t) p.textDim);
            canvas.text (label, font, visage::Font::kCenter, r.x, r.y, r.w, r.h);
        };
        chip (preChip_,  "PRE",  showPre_,  p.accent);
        chip (postChip_, "POST", showPost_, p.positive);
    }

    // ── interaction ─────────────────────────────────────────────────────────────
    int DeqCurveView::nodeAt (visage::Point pos) const
    {
        int best = -1;
        float bestD = 14.0f;
        for (int b = 0; b < kNumBands; ++b)
        {
            if (! bandOn (b)) continue;
            const visage::Point c = nodePos (b);
            const float d = std::sqrt ((c.x - pos.x) * (c.x - pos.x) + (c.y - pos.y) * (c.y - pos.y));
            if (d <= bestD) { bestD = d; best = b; }
        }
        return best;
    }
    int DeqCurveView::firstFreeBand() const
    {
        for (int b = 0; b < kNumBands; ++b) if (! bandOn (b)) return b;
        return -1;
    }
    int DeqCurveView::typeForFraction (float frac) const
    {
        if (frac < 0.12f) return (int) factory_core::BandType::HighPass;
        if (frac < 0.25f) return (int) factory_core::BandType::LowShelf;
        if (frac > 0.88f) return (int) factory_core::BandType::LowPass;
        if (frac > 0.75f) return (int) factory_core::BandType::HighShelf;
        return (int) factory_core::BandType::Bell;
    }
    int DeqCurveView::modeSegAt (visage::Point pos) const
    {
        auto in = [&] (const Rect& r) { return pos.x >= r.x && pos.x < r.x + r.w && pos.y >= r.y && pos.y < r.y + r.h; };
        if (in (preChip_))  return 0;
        if (in (postChip_)) return 1;
        return -1;
    }

    void DeqCurveView::beginNodeGesture (int b)
    {
        store_.beginGesture (bx_[(size_t) b].freq);
        store_.beginGesture (bx_[(size_t) b].gain);
    }
    void DeqCurveView::endNodeGesture (int b)
    {
        store_.endGesture (bx_[(size_t) b].freq);
        store_.endGesture (bx_[(size_t) b].gain);
    }
    void DeqCurveView::setParamUi (int paramIndex, float value) { store_.setFromUi (paramIndex, value); redraw(); }
    void DeqCurveView::setParamGestured (int paramIndex, float value) { store_.setFromUiGestured (paramIndex, value); redraw(); }

    void DeqCurveView::mouseDown (const visage::MouseEvent& e)
    {
        const visage::Point pos = e.position;

        // Pre/Post toggle chips.
        const int seg = modeSegAt (pos);
        if (seg == 0) { showPre_  = ! showPre_;  redraw(); return; }
        if (seg == 1) { showPost_ = ! showPost_; redraw(); return; }

        const int hit = nodeAt (pos);

        // A double-click is EXACTLY the 2nd consecutive click (repeatClickCount() == 2), not
        // ">= 2": otherwise the very next click after a double-click-add — which visage counts
        // as click #3 — would re-trigger the add/remove branch and immediately delete the node
        // the user just created (the reported "add then a click deletes it" bug). Click #3+ and
        // singles both fall through to the select/drag path below.
        if (e.repeatClickCount() == 2)
        {
            if (hit >= 0) { setParamGestured (bx_[(size_t) hit].on, 0.0f); hover_ = -1; if (onBandEdited) onBandEdited (hit); redraw(); return; }
            // Double-click empty space -> add a band whose type follows the x position.
            const int slot = firstFreeBand();
            if (slot < 0) return;
            const float x = std::clamp (pos.x, plot_.x, plot_.x + plot_.w);
            const float frac = (x - plot_.x) / std::max (1.0f, plot_.w);
            const int type = typeForFraction (frac);
            setParamGestured (bx_[(size_t) slot].byp, 0.0f);
            setParamGestured (bx_[(size_t) slot].type, (float) type);
            setParamGestured (bx_[(size_t) slot].freq, xToFreq (x));
            setParamGestured (bx_[(size_t) slot].q, 0.707f);
            if (type != (int) factory_core::BandType::HighPass && type != (int) factory_core::BandType::LowPass)
            {
                const float g = yToGain (std::clamp (pos.y, plot_.y, plot_.y + plot_.h));
                setParamGestured (bx_[(size_t) slot].gain, std::clamp (g, -kMaxGain, kMaxGain));
            }
            setParamGestured (bx_[(size_t) slot].on, 1.0f); // activate last
            selected_ = slot;
            if (onSelectBand) onSelectBand (slot);
            redraw();
            return;
        }

        if (hit >= 0)
        {
            selected_ = hit;
            dragging_ = hit;
            if (onSelectBand) onSelectBand (hit);
            beginNodeGesture (hit);
            redraw();
        }
    }

    void DeqCurveView::mouseDrag (const visage::MouseEvent& e)
    {
        if (dragging_ < 0) return;
        const float f = xToFreq (std::clamp (e.position.x, plot_.x, plot_.x + plot_.w));
        const float gdb = yToGain (std::clamp (e.position.y, plot_.y, plot_.y + plot_.h));
        setParamUi (bx_[(size_t) dragging_].freq, f);
        if (! isCutType (dragging_))
            setParamUi (bx_[(size_t) dragging_].gain, gdb);
        if (onBandEdited) onBandEdited (dragging_);
    }

    void DeqCurveView::mouseUp (const visage::MouseEvent&)
    {
        if (dragging_ >= 0) endNodeGesture (dragging_);
        dragging_ = -1;
    }

    void DeqCurveView::mouseMove (const visage::MouseEvent& e)
    {
        const int h = nodeAt (e.position);
        if (h != hover_) { hover_ = h; redraw(); }
    }
    void DeqCurveView::mouseExit (const visage::MouseEvent&)
    {
        if (hover_ != -1) { hover_ = -1; redraw(); }
    }

    bool DeqCurveView::mouseWheel (const visage::MouseEvent& e)
    {
        const int band = (hover_ >= 0) ? hover_ : nodeAt (e.position);
        if (band < 0) return false;
        const float q = store_.value (bx_[(size_t) band].q);
        const float nq = std::clamp (q * std::exp (e.wheel_delta_y * 1.2f), 0.1f, 18.0f);
        setParamGestured (bx_[(size_t) band].q, nq);
        if (onBandEdited) onBandEdited (band);
        return true;
    }
} // namespace deq_ui
