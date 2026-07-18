#include "RsSuppressionCurveView.h"

#include "factory_ui_visage/Fonts.h"

#include <visage_graphics/canvas.h>
#include <visage_graphics/path.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace rs_ui
{
    using factory_ui_visage::regularFont;
    using factory_ui_visage::boldFont;
    namespace fuv = factory_ui_visage;

    namespace
    {
        constexpr float kPlotInset = 12.0f;
        constexpr float kControlsH = 22.0f;
        constexpr float kLabelsH   = 14.0f;

        std::string hzLabel (float f)
        {
            if (f >= 1000.0f)
            {
                const float k = f / 1000.0f;
                const bool whole = std::abs (k - std::round (k)) < 0.05f;
                char buf[16];
                if (whole) std::snprintf (buf, sizeof buf, "%d kHz", (int) std::round (k));
                else       std::snprintf (buf, sizeof buf, "%.2f kHz", k);
                return buf;
            }
            char buf[16];
            std::snprintf (buf, sizeof buf, "%d Hz", (int) std::round (f));
            return buf;
        }

        // -------- A3: downsampled trace primitives (fast in software raster) ----
        // The analyser used to emit THOUSANDS of per-PIXEL fill(rect)/segment
        // primitives per frame (the path-atlas workaround), which capped the dev
        // (-O0) build at ~3 fps. Two things fix that here:
        //   (1) sample every trace at a COARSE column step (kStepPx below) so the
        //       primitive COUNT — the -O0 submission bottleneck — drops several-
        //       fold with no visible loss (the curves read the same at ~4-6 px);
        //   (2) draw from primitives (rounded segments for lines, edge-to-edge
        //       vertical bars for fills) so nothing touches the path-fill atlas
        //       (immune to the "large plot-spanning path poisons every path fill"
        //       bug) AND the software rasteriser only shades the thin covered band.
        // NB: visage's GraphLine (canvas.graphLine/graphFill) was evaluated per the
        // brief but REJECTED — see the report. It is one draw per trace, but each
        // is a FULL-PLOT quad whose per-pixel fragment shader (a 20-sample distance
        // loop) is coverage-bound; under headless SwiftShader (a software
        // rasteriser — the CI gate + the dev harness) that measured ~2.3x SLOWER
        // than these primitives, regardless of data resolution. On real GPU
        // hardware GraphLine would win; the software raster path these primitives
        // take is the one that has to be fast for the dev loop + the headless gate.
        //
        // Column count for a plot span at ~kStepPx spacing (>= 2 points). Tuned so
        // the analyser reads the same but the primitive count (the -O0 submission
        // bottleneck) is a fraction of the old per-pixel counts.
        constexpr float kStepSpectrumPx = 4.0f; // spectra keep resonance detail
        constexpr float kStepProfilePx  = 7.0f; // profile/per-node curves are smooth

        int columnCount (float widthPx, float stepPx)
        {
            return std::max (2, (int) std::round (widthPx / std::max (1.0f, stepPx)) + 1);
        }

        std::vector<float>& scratchColumns()
        {
            static thread_local std::vector<float> buf; // GUI-thread only
            return buf;
        }

        // Stroke a per-column screen-y trace (spanning [x0, x0+w]) as connected
        // rounded segments — one primitive per column step.
        void strokeColumns (visage::Canvas& canvas, float x0, float w,
                            const std::vector<float>& screenY, float width)
        {
            const int n = (int) screenY.size();
            if (n < 2 || w <= 0.0f) return;
            const float dx = w / (float) (n - 1);
            for (int i = 1; i < n; ++i)
                canvas.segment (x0 + (float) (i - 1) * dx, screenY[(std::size_t) (i - 1)],
                                x0 + (float) i * dx, screenY[(std::size_t) i], width, /*rounded*/ true);
        }

        // Same, but SKIP floored runs (columns at/below floorY) — a low/high cut
        // rolled to the plot floor shouldn't stroke a flat line out to the edge
        // (mirrors SuppressionCurveComponent's combined-curve floor trim).
        void strokeColumnsTrimFloor (visage::Canvas& canvas, float x0, float w,
                                     const std::vector<float>& screenY, float width, float floorY)
        {
            const int n = (int) screenY.size();
            if (n < 2 || w <= 0.0f) return;
            const float dx = w / (float) (n - 1);
            for (int i = 1; i < n; ++i)
            {
                const float ya = screenY[(std::size_t) (i - 1)], yb = screenY[(std::size_t) i];
                if (ya >= floorY - 0.5f && yb >= floorY - 0.5f) continue; // whole segment on the floor
                canvas.segment (x0 + (float) (i - 1) * dx, ya, x0 + (float) i * dx, yb, width, true);
            }
        }

        // Fill the region between a per-column trace and a horizontal baseline
        // (screen-y) as a SINGLE closed path — the exact idiom the shipped JUCE
        // editor (g.fillPath(area)) and the gallery SpectrumView both use, so the
        // fill is one continuous shape with ONE coverage per pixel: no internal
        // edges, therefore no seams. The previous version drew one translucent
        // vertical BAR per column with a ~1px overlap; because the fill is
        // translucent, each overlap composited twice and read as a darker vertical
        // stripe — the comb/hatch the user reported (round-3 fix 1). (A triangle-
        // strip was tried too, but visage anti-aliases every triangle edge
        // independently, so the shared per-column edges still left ~1px seams.) The
        // caller sets the brush (solid or a plot-anchored vertical gradient) first;
        // it applies across the whole shape, matching factory_ui::fillSpectrumArea.
        void fillAreaToBaseline (visage::Canvas& canvas, float x0, float w,
                                 const std::vector<float>& screenY, float baselineY)
        {
            const int n = (int) screenY.size();
            if (n < 2 || w <= 0.0f) return;
            const float dx = w / (float) (n - 1);
            visage::Path area;
            area.moveTo (x0, baselineY);
            for (int i = 0; i < n; ++i)
                area.lineTo (x0 + (float) i * dx, screenY[(std::size_t) i]);
            area.lineTo (x0 + (float) (n - 1) * dx, baselineY);
            area.close();
            canvas.fill (area);
        }
    } // namespace

    RsSuppressionCurveView::RsSuppressionCurveView (const RsTheme& theme, RsProfileModel& model, RsFeed& feed)
        : theme_ (theme), model_ (model), feed_ (feed)
    {
        setAcceptsKeystrokes (false);
    }

    std::uint32_t RsSuppressionCurveView::bandColour (int id) const
    {
        // Band id -> the shared 6-hue band palette (id-2, mod 6) — matches
        // FactoryLookAndFeel::bandColour and the node dots.
        const int b = (id - 2) % 6;
        return theme_.base.palette.bandColours[(std::size_t) ((b + 6) % 6)];
    }

    // ---- axes (verbatim from SuppressionCurveComponent) ---------------------

    float RsSuppressionCurveView::freqToT (float f)
    {
        f = std::clamp (f, 20.0f, 20000.0f);
        const float lf = std::log (f);
        auto seg = [lf] (float f0, float f1, float t0, float t1)
        { return t0 + (t1 - t0) * (lf - std::log (f0)) / (std::log (f1) - std::log (f0)); };
        if (f <= 500.0f)  return seg (20.0f,   500.0f,   0.0f,  0.30f);
        if (f <= 9000.0f) return seg (500.0f,  9000.0f,  0.30f, 0.85f);
        return                   seg (9000.0f, 20000.0f, 0.85f, 1.0f);
    }

    float RsSuppressionCurveView::tToFreq (float t)
    {
        t = std::clamp (t, 0.0f, 1.0f);
        auto seg = [t] (float t0, float t1, float f0, float f1)
        { return std::exp (std::log (f0) + (std::log (f1) - std::log (f0)) * (t - t0) / (t1 - t0)); };
        if (t <= 0.30f) return seg (0.0f,  0.30f, 20.0f,   500.0f);
        if (t <= 0.85f) return seg (0.30f, 0.85f, 500.0f,  9000.0f);
        return                seg (0.85f, 1.0f,  9000.0f, 20000.0f);
    }

    float RsSuppressionCurveView::freqToX (float f) const { return plot_.x + freqToT (f) * plot_.w; }
    float RsSuppressionCurveView::xToFreq (float x) const { return tToFreq ((x - plot_.x) / std::max (1.0f, plot_.w)); }

    float RsSuppressionCurveView::dbToY (float db) const
    {
        return plot_.y + (kMaxDb - std::clamp (db, kMinDb, kMaxDb)) / (kMaxDb - kMinDb) * plot_.h;
    }
    float RsSuppressionCurveView::sensToY (float s) const
    {
        return plot_.y + plot_.h - (std::clamp (s, RsProfileModel::kSensMin, RsProfileModel::kSensMax)
                                    - RsProfileModel::kSensMin) / (RsProfileModel::kSensMax - RsProfileModel::kSensMin) * plot_.h;
    }
    float RsSuppressionCurveView::yToSens (float y) const
    {
        return RsProfileModel::kSensMin + (plot_.y + plot_.h - y) / std::max (1.0f, plot_.h)
               * (RsProfileModel::kSensMax - RsProfileModel::kSensMin);
    }

    visage::Point RsSuppressionCurveView::nodePos (int id) const
    {
        return visage::Point (freqToX (model_.nodeFreq (id)), sensToY (model_.nodeSens (id)));
    }

    // ---- layout -------------------------------------------------------------

    void RsSuppressionCurveView::computeLayout()
    {
        const float w = width(), h = height();
        const float ix = kPlotInset, iy = kPlotInset;
        const float iw = std::max (1.0f, w - 2.0f * kPlotInset);
        controlsRow_ = { ix, iy, iw, kControlsH };
        const float plotY = iy + kControlsH + kLabelsH;
        plot_ = { ix, plotY, iw, std::max (1.0f, h - kPlotInset - plotY) };

        // controls: mode chip (left 132), GR badge (right 100), freeze (right of it 66).
        modeChip_  = { controlsRow_.x, controlsRow_.y, 132.0f, controlsRow_.h };
        grBadge_   = { controlsRow_.x + controlsRow_.w - 100.0f, controlsRow_.y, 100.0f, controlsRow_.h };
        freezeChip_= { grBadge_.x - 6.0f - 66.0f, controlsRow_.y, 66.0f, controlsRow_.h };
    }

    void RsSuppressionCurveView::resized() { computeLayout(); }

    // ---- feed snapshot ------------------------------------------------------

    void RsSuppressionCurveView::snapshotFeed()
    {
        snapBins_ = feed_.bins();
        snapSr_   = feed_.sampleRate();
        snapPre_.resize ((std::size_t) snapBins_);
        snapPost_.resize ((std::size_t) snapBins_);
        snapRed_.resize ((std::size_t) snapBins_);
        const std::atomic<float>* pre = feed_.magPreDb();
        const std::atomic<float>* post = feed_.magDb();
        const std::atomic<float>* red = feed_.redDb();
        for (int k = 0; k < snapBins_; ++k)
        {
            snapPre_[(std::size_t) k]  = pre[k].load (std::memory_order_relaxed);
            snapPost_[(std::size_t) k] = post[k].load (std::memory_order_relaxed);
            snapRed_[(std::size_t) k]  = red[k].load (std::memory_order_relaxed);
        }
    }

    void RsSuppressionCurveView::setFrozen (bool frozen)
    {
        frozen_ = frozen;
        if (frozen_)
            snapshotFeed();
        redraw();
    }

    // ---- draw ---------------------------------------------------------------

    void RsSuppressionCurveView::draw (visage::Canvas& canvas)
    {
        computeLayout();
        if (! frozen_)
        {
            if (onTick) onTick();
            snapshotFeed(); // read the live feed each frame
        }

        drawPlotCard (canvas);
        drawGrid (canvas);
        drawAnalyzer (canvas);
        drawReduction (canvas);
        drawProfile (canvas);
        drawNodes (canvas);
        drawHeaderControls (canvas);
        drawTooltip (canvas);

        // GR peak-hold + keep the animation alive (unless frozen).
        if (! frozen_)
        {
            const int n = 2 * (snapBins_ - 1);
            float worst = 0.0f;
            for (int k = 0; k < snapBins_; ++k)
            {
                const float f = n > 0 ? (float) ((double) k * snapSr_ / n) : 0.0f;
                if (f >= 20.0f && f <= 20000.0f)
                    worst = std::min (worst, snapRed_[(std::size_t) k]);
            }
            updateGrHold (worst);
            redraw();
        }
    }

    void RsSuppressionCurveView::drawPlotCard (visage::Canvas& canvas)
    {
        const float w = width(), h = height();
        canvas.setColor (visage::Brush::vertical (visage::Color (theme_.rs.plotTop),
                                                  visage::Color (theme_.rs.plotBottom)));
        canvas.roundedRectangle (0.0f, 0.0f, w, h, theme_.rs.radiusCard);
        canvas.setColor (visage::Color (theme_.base.palette.track));
        canvas.roundedRectangleBorder (0.5f, 0.5f, w - 1.0f, h - 1.0f, theme_.rs.radiusCard, 1.0f);
    }

    void RsSuppressionCurveView::drawGrid (visage::Canvas& canvas)
    {
        struct FL { float f; const char* s; bool strong; };
        const FL fls[] = { {50,"50",false}, {100,"100",true}, {200,"200",false}, {500,"500",false},
                           {1000,"1k",true}, {2000,"2k",false}, {5000,"5k",false}, {10000,"10k",true} };
        for (const FL& fl : fls)
        {
            const float x = freqToX (fl.f);
            canvas.setColor (visage::Color (fl.strong ? theme_.base.palette.track : theme_.rs.borderLight));
            canvas.segment (x, plot_.y, x, plot_.y + plot_.h, 1.0f, false);
            canvas.setColor (visage::Color (fl.strong ? theme_.base.palette.textDim : theme_.rs.textFaint));
            // v2.1.0 grid labels are weight 600 (-> bold in the JUCE mapping).
            canvas.text (fl.s, boldFont (10.0f), visage::Font::kCenter, x - 18.0f, plot_.y - 13.0f, 36.0f, 12.0f);
        }
        for (float db = 0.0f; db >= -60.0f; db -= 12.0f)
        {
            const float y = dbToY (db);
            canvas.setColor (visage::Color (db == 0.0f ? theme_.base.palette.track : theme_.rs.borderLight));
            canvas.segment (plot_.x, y, plot_.x + plot_.w, y, 1.0f, false);
        }
    }

    // Resample a per-bin dB array onto `cols` evenly-spaced plot COLUMNS (the
    // non-uniform log-frequency axis means bins aren't evenly spaced in x), with
    // linear interpolation between bins, returning screen-y per column.
    void RsSuppressionCurveView::sampleSpectrumColumns (const std::vector<float>& bins,
                                                        int cols, std::vector<float>& out) const
    {
        const int n = 2 * (snapBins_ - 1);
        out.resize ((std::size_t) cols);
        for (int i = 0; i < cols; ++i)
        {
            const float x  = plot_.x + (cols > 1 ? (float) i / (float) (cols - 1) : 0.0f) * plot_.w;
            const float f  = xToFreq (x);
            const float kf = f * (float) n / (float) snapSr_;
            const int   k0 = std::clamp ((int) std::floor (kf), 1, snapBins_ - 2);
            const float t  = std::clamp (kf - (float) k0, 0.0f, 1.0f);
            const float db = bins[(std::size_t) k0] * (1.0f - t) + bins[(std::size_t) (k0 + 1)] * t;
            out[(std::size_t) i] = dbToY (db);
        }
    }

    // PRE = FilledArea (kV201Style): a vertical-gradient fill (muted taupe #b9a39b,
    // 0.22 at the plot top -> 0.02 at the foot) hanging to the plot bottom, NO
    // line. POST = a thin SOLID coral line (accent, 1.4 px, 0.85 alpha). Both from
    // downsampled primitives (A3): the PRE gradient is anchored to the PLOT band
    // (matching factory_ui::fillSpectrumArea) so the alpha at any y is the same
    // regardless of the trace height.
    void RsSuppressionCurveView::drawAnalyzer (visage::Canvas& canvas)
    {
        if (snapBins_ < 2 || plot_.w < 2.0f || plot_.h <= 0.0f) return;
        const int cols = columnCount (plot_.w, kStepSpectrumPx);
        std::vector<float>& colY = scratchColumns();

        if (analyzerMode_ != AnalyzerMode::Post)
        {
            sampleSpectrumColumns (snapPre_, cols, colY);
            canvas.setColor (visage::Brush::linear (
                visage::Color (theme_.rs.preColour).withAlpha (theme_.rs.preFillTopAlpha),
                visage::Color (theme_.rs.preColour).withAlpha (theme_.rs.preFillBotAlpha),
                visage::Point (plot_.x, plot_.y), visage::Point (plot_.x, plot_.y + plot_.h)));
            fillAreaToBaseline (canvas, plot_.x, plot_.w, colY, plot_.y + plot_.h); // to the plot bottom
        }
        if (analyzerMode_ != AnalyzerMode::Pre)
        {
            sampleSpectrumColumns (snapPost_, cols, colY);
            canvas.setColor (visage::Color (theme_.rs.postColour).withAlpha (theme_.rs.postLineAlpha));
            strokeColumns (canvas, plot_.x, plot_.w, colY, theme_.rs.postLineWidth);
        }
    }

    // Reduction "curtain" = AreaFromZero (kV201Style): it hangs from the 0 dB
    // gridline down to dbToY(reduction) — NOT from the plot top — in teal
    // (palette.positive), fill 0.28 under a 0.8 / 1px stroke, clamped only by the
    // −60 dB floor. Downsampled bars + a stroked lower edge (A3).
    void RsSuppressionCurveView::drawReduction (visage::Canvas& canvas)
    {
        const int n = 2 * (snapBins_ - 1);
        if (n <= 0 || plot_.w < 2.0f || plot_.h <= 0.0f) return;

        const float zeroY = dbToY (0.0f);
        const int   cols  = columnCount (plot_.w, kStepSpectrumPx);
        std::vector<float>& colY = scratchColumns();
        colY.resize ((std::size_t) cols);
        for (int i = 0; i < cols; ++i)
        {
            const float x   = plot_.x + (cols > 1 ? (float) i / (float) (cols - 1) : 0.0f) * plot_.w;
            const float f   = xToFreq (x);
            const float kf  = f * (float) n / (float) snapSr_;
            const int   k0  = std::clamp ((int) std::floor (kf), 1, snapBins_ - 2);
            const float t   = std::clamp (kf - (float) k0, 0.0f, 1.0f);
            const float red = std::clamp (snapRed_[(std::size_t) k0] * (1.0f - t) + snapRed_[(std::size_t) (k0 + 1)] * t,
                                          -60.0f, 0.0f);
            colY[(std::size_t) i] = dbToY (red); // red <= 0 -> at/below the 0 dB line
        }

        canvas.setColor (visage::Color (theme_.base.palette.positive).withAlpha (theme_.rs.curtainFillAlpha));
        fillAreaToBaseline (canvas, plot_.x, plot_.w, colY, zeroY); // from the 0 dB line down
        if (theme_.rs.curtainStrokeAlpha > 0.0f && theme_.rs.curtainStrokeWidth > 0.0f)
        {
            canvas.setColor (visage::Color (theme_.base.palette.positive).withAlpha (theme_.rs.curtainStrokeAlpha));
            strokeColumns (canvas, plot_.x, plot_.w, colY, theme_.rs.curtainStrokeWidth);
        }
    }

    // The reduction-profile face (kV201Style PerNodePlusCombined). Geometry is the
    // shipped SuppressionCurveComponent's: the sens baseline sits at sensToY(0) —
    // the plot's vertical MIDDLE — and the ±30 dB sens range spans the full plot
    // height, evaluated with factory_core::reductionProfileDbAt (the SAME shape the
    // audio path runs) so a band's dot rides its own peak on the curve. Two layers,
    // back to front:
    //   (1) each ACTIVE node's own contribution — a translucent FILL (0.12) from
    //       the baseline plus a coloured STROKE (0.7) — bands in their band hue,
    //       cuts dimmed; then
    //   (2) the COMBINED profile over ALL nodes as a SOLID coral line (2.2 px,
    //       alpha 1) with a soft glow under it (0.22 / 5 px), floor-trimmed so a
    //       cut rolled to the plot floor doesn't stroke a flat edge.
    // All from downsampled primitives (A3) — no plot-wide path fills.
    void RsSuppressionCurveView::drawProfile (visage::Canvas& canvas)
    {
        if (plot_.w < 2.0f || plot_.h <= 0.0f) return;
        const auto  nodes  = model_.buildNodes();
        const float y0     = sensToY (0.0f);          // nominal (0 dB) baseline = plot middle
        const float floorY = plot_.y + plot_.h;

        // reductionProfileDbAt is heavier than a bin lookup and the curves are
        // smooth, so sample coarser than the spectra.
        const int cols = columnCount (plot_.w, kStepProfilePx);
        auto sampleProfile = [&] (const factory_core::ReductionNodes& cfg, std::vector<float>& out)
        {
            out.resize ((std::size_t) cols);
            for (int i = 0; i < cols; ++i)
            {
                const float x = plot_.x + (cols > 1 ? (float) i / (float) (cols - 1) : 0.0f) * plot_.w;
                out[(std::size_t) i] = sensToY ((float) factory_core::reductionProfileDbAt (xToFreq (x), cfg));
            }
        };

        std::vector<float> curveY; // reused across nodes (keeps capacity)

        // (1) Per-node fill + stroke (bands + cuts that are ON), under the combined.
        if (theme_.rs.perNodeFillAlpha > 0.0f || theme_.rs.perNodeStrokeAlpha > 0.0f)
        {
            for (int id = 0; id < RsProfileModel::kNumNodes; ++id)
            {
                if (! model_.nodeOn (id)) continue;
                const std::uint32_t col = RsProfileModel::isCut (id) ? theme_.base.palette.textDim
                                                                     : bandColour (id);
                sampleProfile (RsProfileModel::singleNode (nodes, id), curveY);
                if (theme_.rs.perNodeFillAlpha > 0.0f)
                {
                    canvas.setColor (visage::Color (col).withAlpha (theme_.rs.perNodeFillAlpha));
                    fillAreaToBaseline (canvas, plot_.x, plot_.w, curveY, y0); // from the baseline
                }
                if (theme_.rs.perNodeStrokeAlpha > 0.0f)
                {
                    canvas.setColor (visage::Color (col).withAlpha (theme_.rs.perNodeStrokeAlpha));
                    strokeColumns (canvas, plot_.x, plot_.w, curveY, 1.0f);
                }
            }
        }

        // (2) Combined reduction curve: soft glow under a crisp SOLID stroke,
        //     floor-trimmed.
        sampleProfile (nodes, curveY);
        if (theme_.rs.profileGlowAlpha > 0.0f && theme_.rs.profileGlowWidth > 0.0f)
        {
            canvas.setColor (visage::Color (theme_.rs.profileColour).withAlpha (theme_.rs.profileGlowAlpha));
            strokeColumnsTrimFloor (canvas, plot_.x, plot_.w, curveY, theme_.rs.profileGlowWidth, floorY);
        }
        canvas.setColor (visage::Color (theme_.rs.profileColour).withAlpha (theme_.rs.profileStrokeAlpha));
        strokeColumnsTrimFloor (canvas, plot_.x, plot_.w, curveY, theme_.rs.profileStrokeWidth, floorY);
    }

    void RsSuppressionCurveView::drawNodes (visage::Canvas& canvas)
    {
        const int listen = feed_.getListenNode();
        for (int id = 0; id < RsProfileModel::kNumNodes; ++id)
        {
            const bool on = model_.nodeOn (id);
            if (! RsProfileModel::isCut (id) && ! on) continue; // off bands hidden
            const float a = on ? 1.0f : 0.3f;
            const visage::Point p = nodePos (id);
            // v2.1.0 grows a node handle only when SELECTED (hover drives the
            // tooltip, not the handle size — see SuppressionCurveComponent::drawNodes).
            const bool selected = (id == selectedNode_);

            if (RsProfileModel::isCut (id))
            {
                const std::uint32_t ring = (id == 0) ? theme_.rs.orange : theme_.rs.highCutRing;
                // dashed vertical guide
                canvas.setColor (visage::Color (theme_.rs.textFaint).withAlpha (a));
                for (float yy = plot_.y; yy < plot_.y + plot_.h; yy += 8.0f)
                    canvas.segment (p.x, yy, p.x, std::min (yy + 4.0f, plot_.y + plot_.h), 1.0f, false);

                const float sz = selected ? theme_.rs.cutHandleSel : theme_.rs.cutHandle;
                const float x0 = p.x - sz * 0.5f, y0 = p.y - sz * 0.5f;
                if (id == listen)
                {
                    canvas.setColor (visage::Color (theme_.base.palette.positive).withAlpha (0.9f));
                    canvas.roundedRectangleBorder (x0 - 5.0f, y0 - 5.0f, sz + 10.0f, sz + 10.0f,
                                                   theme_.rs.radiusCutHandle + 3.0f, 2.0f);
                }
                canvas.setColor (visage::Color (theme_.rs.nodeShadow));
                canvas.roundedRectangleShadow (x0, y0 + 2.0f, sz, sz, theme_.rs.radiusCutHandle, 5.0f);
                canvas.setColor (visage::Color (ring).withAlpha (a));
                canvas.roundedRectangle (x0 - 2.0f, y0 - 2.0f, sz + 4.0f, sz + 4.0f, theme_.rs.radiusCutHandle + 1.5f);
                canvas.setColor (visage::Color (theme_.rs.footerBg).withAlpha (a));
                canvas.roundedRectangle (x0, y0, sz, sz, theme_.rs.radiusCutHandle);
            }
            else
            {
                const std::uint32_t col = bandColour (id);
                const float dotD = selected ? theme_.rs.bandDotSel : theme_.rs.bandDot;
                const float haloD = dotD + 6.0f;
                if (id == listen)
                {
                    canvas.setColor (visage::Color (theme_.base.palette.positive).withAlpha (0.9f));
                    const float rd = haloD + 7.0f;
                    canvas.ring (p.x - rd * 0.5f, p.y - rd * 0.5f, rd, 2.0f);
                }
                canvas.setColor (visage::Color (theme_.rs.nodeShadow));
                canvas.roundedRectangleShadow (p.x - dotD * 0.5f, p.y - dotD * 0.5f + 2.0f, dotD, dotD, dotD * 0.5f, 6.0f);
                canvas.setColor (visage::Color (0xffffffff));
                canvas.circle (p.x - haloD * 0.5f, p.y - haloD * 0.5f, haloD);
                canvas.setColor (visage::Color (col));
                canvas.circle (p.x - dotD * 0.5f, p.y - dotD * 0.5f, dotD);
            }
        }
    }

    void RsSuppressionCurveView::drawHeaderControls (visage::Canvas& canvas)
    {
        auto chipShell = [&] (const Rect& b)
        {
            canvas.setColor (visage::Color (theme_.rs.chipBg));
            canvas.roundedRectangle (b.x, b.y, b.w, b.h, theme_.rs.radiusBadge);
            canvas.setColor (visage::Color (theme_.base.palette.track));
            canvas.roundedRectangleBorder (b.x + 0.5f, b.y + 0.5f, b.w - 1.0f, b.h - 1.0f, theme_.rs.radiusBadge, 1.0f);
        };

        // A1: Pre / Post / Both.
        chipShell (modeChip_);
        {
            const char* names[] = { "Pre", "Post", "Both" };
            const AnalyzerMode modes[] = { AnalyzerMode::Pre, AnalyzerMode::Post, AnalyzerMode::Both };
            const float segW = (modeChip_.w - 6.0f) / 3.0f;
            for (int i = 0; i < 3; ++i)
            {
                const float sx = modeChip_.x + 3.0f + (float) i * segW;
                const bool active = (analyzerMode_ == modes[i]);
                if (active)
                {
                    canvas.setColor (visage::Color (theme_.base.palette.accent));
                    canvas.roundedRectangle (sx + 1.0f, modeChip_.y + 4.0f, segW - 2.0f, modeChip_.h - 8.0f, theme_.rs.radiusBadge - 2.0f);
                }
                else if (i == hoverModeSeg_)
                {
                    // Per-segment hover feedback (fix 8): a faint accent wash on the
                    // hovered, non-active segment.
                    canvas.setColor (visage::Color (theme_.base.palette.accent).withAlpha (0.14f));
                    canvas.roundedRectangle (sx + 1.0f, modeChip_.y + 4.0f, segW - 2.0f, modeChip_.h - 8.0f, theme_.rs.radiusBadge - 2.0f);
                }
                canvas.setColor (visage::Color (active ? 0xffffffff : theme_.base.palette.textDim));
                canvas.text (names[i], boldFont (9.5f), visage::Font::kCenter, sx, modeChip_.y, segW, modeChip_.h);
            }
        }

        // A2: Freeze.
        chipShell (freezeChip_);
        canvas.setColor (visage::Color (theme_.base.palette.textSecondary));
        canvas.text (frozen_ ? "Frozen" : "Freeze", boldFont (9.5f), visage::Font::kCenter,
                     freezeChip_.x, freezeChip_.y, freezeChip_.w, freezeChip_.h);

        // A3: GR peak-hold badge.
        canvas.setColor (visage::Color (theme_.rs.grBg));
        canvas.roundedRectangle (grBadge_.x, grBadge_.y, grBadge_.w, grBadge_.h, theme_.rs.radiusBadge);
        canvas.setColor (visage::Color (theme_.rs.grBorder));
        canvas.roundedRectangleBorder (grBadge_.x + 0.5f, grBadge_.y + 0.5f, grBadge_.w - 1.0f, grBadge_.h - 1.0f, theme_.rs.radiusBadge, 1.0f);
        char gr[24]; std::snprintf (gr, sizeof gr, "GR %.1f dB", grPeakDb_);
        canvas.setColor (visage::Color (theme_.rs.grText));
        canvas.text (gr, boldFont (9.5f), visage::Font::kCenter, grBadge_.x, grBadge_.y, grBadge_.w, grBadge_.h);
    }

    void RsSuppressionCurveView::drawTooltip (visage::Canvas& canvas)
    {
        const int id = dragging_ >= 0 ? dragging_ : hoverNode_;
        if (id < 0) return;
        const visage::Point anchor = dragging_ >= 0 ? dragVirtual_ : nodePos (id);

        const float f = model_.nodeFreq (id);
        std::string text = hzLabel (f);
        if (! RsProfileModel::isCut (id))
        {
            char buf[48];
            std::snprintf (buf, sizeof buf, "   %.1f dB   %.2f oct", model_.nodeSens (id), model_.nodeWidth (id));
            text += buf;
        }
        const float w = 8.0f * (float) text.size() + 14.0f;
        float bx = std::clamp (anchor.x - w * 0.5f, plot_.x, plot_.x + plot_.w - w);
        float by = std::max (plot_.y, anchor.y - 26.0f);
        canvas.setColor (visage::Color (theme_.base.palette.text).withAlpha (0.9f));
        canvas.roundedRectangle (bx, by, w, 18.0f, 5.0f);
        canvas.setColor (visage::Color (0xffffffff));
        canvas.text (text, boldFont (10.5f), visage::Font::kCenter, bx, by, w, 18.0f);
    }

    void RsSuppressionCurveView::updateGrHold (float instantDb)
    {
        constexpr int   kHoldFrames = 30;
        constexpr float kDecay = 0.5f;
        if (instantDb <= grPeakDb_) { grPeakDb_ = instantDb; grHold_ = kHoldFrames; }
        else if (grHold_ > 0) { --grHold_; }
        else { grPeakDb_ = std::min (0.0f, grPeakDb_ + kDecay); if (grPeakDb_ > instantDb) grPeakDb_ = instantDb; }
    }

    // ---- interactions -------------------------------------------------------

    int RsSuppressionCurveView::nodeAt (visage::Point pos) const
    {
        int best = -1; float bestD = theme_.rs.nodeHitRadius;
        for (int id = 0; id < RsProfileModel::kNumNodes; ++id)
        {
            if (! RsProfileModel::isCut (id) && ! model_.nodeOn (id)) continue;
            const visage::Point np = nodePos (id);
            const float d = std::sqrt ((np.x - pos.x) * (np.x - pos.x) + (np.y - pos.y) * (np.y - pos.y));
            if (d <= bestD) { bestD = d; best = id; }
        }
        return best;
    }

    void RsSuppressionCurveView::setSelectedNode (int id) { selectedNode_ = id; redraw(); }

    void RsSuppressionCurveView::beginGesture (int id)
    {
        model_.store().beginGesture (model_.idxFreq (id));
        if (! RsProfileModel::isCut (id))
            model_.store().beginGesture (model_.idxSens (id));
    }
    void RsSuppressionCurveView::endGesture (int id)
    {
        model_.store().endGesture (model_.idxFreq (id));
        if (! RsProfileModel::isCut (id))
            model_.store().endGesture (model_.idxSens (id));
    }
    void RsSuppressionCurveView::setParam (int paramIndex, float value)
    {
        if (paramIndex >= 0) model_.store().setFromUi (paramIndex, value);
    }
    void RsSuppressionCurveView::setParamGestured (int paramIndex, float value)
    {
        if (paramIndex < 0) return;
        model_.store().beginGesture (paramIndex);
        model_.store().setFromUi (paramIndex, value);
        model_.store().endGesture (paramIndex);
    }

    // Which Pre/Post/Both segment a frame-local point is over (-1 if outside). The
    // segment geometry mirrors drawHeaderControls: 3 equal segments inside a 3px
    // inset. True per-segment hit-testing so a click lands on the segment under the
    // cursor instead of cycling the mode (round-3 fix 8).
    int RsSuppressionCurveView::modeSegAt (visage::Point pos) const
    {
        if (pos.x < modeChip_.x || pos.x >= modeChip_.x + modeChip_.w
            || pos.y < modeChip_.y || pos.y >= modeChip_.y + modeChip_.h)
            return -1;
        const float segW = (modeChip_.w - 6.0f) / 3.0f;
        const int seg = (int) std::floor ((pos.x - (modeChip_.x + 3.0f)) / std::max (1.0f, segW));
        return std::clamp (seg, 0, 2);
    }

    void RsSuppressionCurveView::mouseDown (const visage::MouseEvent& e)
    {
        const visage::Point pos = e.position;

        // header chips
        auto inRect = [] (const Rect& r, const visage::Point& p)
        { return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h; };

        // Pre/Post/Both: a TRUE 3-way segmented selector — the clicked segment
        // becomes the mode (was: click anywhere cycles). (round-3 fix 8)
        const int seg = modeSegAt (pos);
        if (seg >= 0)
        {
            const AnalyzerMode modes[] = { AnalyzerMode::Pre, AnalyzerMode::Post, AnalyzerMode::Both };
            if (analyzerMode_ != modes[seg]) { analyzerMode_ = modes[seg]; redraw(); }
            return;
        }
        if (inRect (freezeChip_, pos)) { setFrozen (! frozen_); return; }

        const int hit = nodeAt (pos);
        if (hit < 0)
        {
            // Double-click empty space enables the lowest OFF band here.
            if (e.repeatClickCount() >= 2) { addBandAt (pos); return; }
            if (selectedNode_ >= 0 && onSelectNode) onSelectNode (-1);
            selectedNode_ = -1;
            redraw();
            return;
        }
        selectedNode_ = hit;
        if (onSelectNode) onSelectNode (hit);

        // Alt-click a node resets the params its DRAG controls — freq (+ sens for
        // bands) — to their ParamStore defaults; width / type / enabled are left
        // untouched (round-3 fix 5). No drag begins.
        if (e.isAltDown())
        {
            auto& store = model_.store();
            setParamGestured (model_.idxFreq (hit), store.desc (model_.idxFreq (hit)).defaultValue);
            if (! RsProfileModel::isCut (hit))
                setParamGestured (model_.idxSens (hit), store.desc (model_.idxSens (hit)).defaultValue);
            if (onNodeEdited) onNodeEdited (hit);
            if (onGestureEnd) onGestureEnd();
            redraw();
            return;
        }

        dragging_ = hit;
        dragAnchor_ = pos;
        dragVirtual_ = nodePos (hit);
        beginGesture (hit);
        redraw();
    }

    void RsSuppressionCurveView::mouseDrag (const visage::MouseEvent& e)
    {
        if (dragging_ < 0) return;
        const visage::Point pos = e.position;
        const float scale = e.isShiftDown() ? 0.15f : 1.0f;
        dragVirtual_.x += (pos.x - dragAnchor_.x) * scale;
        dragVirtual_.y += (pos.y - dragAnchor_.y) * scale;
        dragAnchor_ = pos;
        dragVirtual_.x = std::clamp (dragVirtual_.x, plot_.x, plot_.x + plot_.w);
        dragVirtual_.y = std::clamp (dragVirtual_.y, plot_.y, plot_.y + plot_.h);

        setParam (model_.idxFreq (dragging_), xToFreq (dragVirtual_.x));
        if (! RsProfileModel::isCut (dragging_))
            setParam (model_.idxSens (dragging_), yToSens (dragVirtual_.y));
        if (onNodeEdited) onNodeEdited (dragging_);
        redraw();
    }

    void RsSuppressionCurveView::mouseUp (const visage::MouseEvent& e)
    {
        if (dragging_ >= 0)
        {
            // A double-click (repeatClickCount >= 2) on a node resets sens (band) /
            // on empty space adds the lowest OFF band there.
            if (e.repeatClickCount() >= 2)
            {
                if (! RsProfileModel::isCut (dragging_))
                    setParamGestured (model_.idxSens (dragging_), 0.0f);
            }
            endGesture (dragging_);
            dragging_ = -1;
            if (onGestureEnd) onGestureEnd();
            redraw();
        }
    }

    void RsSuppressionCurveView::mouseMove (const visage::MouseEvent& e)
    {
        const int h = nodeAt (e.position);
        if (h != hoverNode_) { hoverNode_ = h; redraw(); }
        const int ms = modeSegAt (e.position); // per-segment hover feedback (fix 8)
        if (ms != hoverModeSeg_) { hoverModeSeg_ = ms; redraw(); }
    }

    void RsSuppressionCurveView::mouseExit (const visage::MouseEvent&)
    {
        if (hoverNode_ >= 0) { hoverNode_ = -1; redraw(); }
        if (hoverModeSeg_ >= 0) { hoverModeSeg_ = -1; redraw(); }
    }

    bool RsSuppressionCurveView::mouseWheel (const visage::MouseEvent& e)
    {
        int id = nodeAt (e.position);
        if (id < 0) id = selectedNode_;
        if (id < 0) return false;
        const float raw = e.wheel_delta_y != 0.0f ? e.wheel_delta_y : e.precise_wheel_delta_y;
        if (raw == 0.0f) return false;
        const bool up = raw > 0.0f;
        if (RsProfileModel::isCut (id))
        {
            const int cur = model_.cutSlope (id);
            const int next = std::clamp (cur + (up ? 1 : -1), 0, 3);
            if (next != cur) { setParamGestured (model_.idxSlope (id), (float) next); if (onNodeEdited) onNodeEdited (id); if (onGestureEnd) onGestureEnd(); }
        }
        else
        {
            const float cur = model_.nodeWidth (id);
            const float next = std::clamp (cur * (up ? 1.15f : 1.0f / 1.15f), 0.10f, 2.00f);
            if (next != cur) { setParamGestured (model_.idxWidth (id), next); if (onNodeEdited) onNodeEdited (id); if (onGestureEnd) onGestureEnd(); }
        }
        redraw();
        return true;
    }

    void RsSuppressionCurveView::addBandAt (visage::Point pos)
    {
        int target = -1;
        for (int b = 0; b < RsProfileModel::kNumBands; ++b)
            if (! model_.nodeOn (2 + b)) { target = 2 + b; break; }
        if (target < 0) return;
        const float f = xToFreq (std::clamp (pos.x, plot_.x, plot_.x + plot_.w));
        const float s = yToSens (std::clamp (pos.y, plot_.y, plot_.y + plot_.h));
        setParamGestured (model_.idxFreq (target), f);
        setParamGestured (model_.idxSens (target), s);
        setParamGestured (model_.idxWidth (target), 0.5f);
        setParamGestured (model_.idxType (target), 0.0f);
        setParamGestured (model_.idxOn (target), 1.0f);
        selectedNode_ = target;
        if (onSelectNode) onSelectNode (target);
        if (onGestureEnd) onGestureEnd();
        redraw();
    }

    bool RsSuppressionCurveView::nodeCentreInWindow (int id, float& x, float& y) const
    {
        if (! RsProfileModel::isCut (id) && ! model_.nodeOn (id)) return false;
        const visage::Point local = nodePos (id);
        const visage::Point o = positionInWindow();
        x = o.x + local.x; y = o.y + local.y;
        return true;
    }

    bool RsSuppressionCurveView::plotRectInWindow (float& x, float& y, float& w, float& h) const
    {
        const visage::Point o = positionInWindow();
        x = o.x + plot_.x; y = o.y + plot_.y; w = plot_.w; h = plot_.h;
        return true;
    }
}
