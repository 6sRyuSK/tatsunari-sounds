#include "RsSuppressionCurveView.h"

#include "factory_ui_visage/Fonts.h"

#include <visage_graphics/canvas.h>
#include <visage_graphics/path.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

        // -------- round #5: EVERY stroked trace is ONE visage::Path -----------
        // Overlap-compositing was the jagged-curve bug class (same as the old comb/
        // hatch fill): a trace drawn as a CHAIN of translucent rounded capsules
        // (canvas.segment) double-composites at every joint, so the joints read as a
        // beaded "comb" — the soft glow beading under the crisp combined line the user
        // zoomed. Round #4 fixed only the MAIN combined line (one path); its glow, the
        // per-node curves and the animated pre/post/reduction traces were still capsule
        // chains that beaded. This round strokes EVERY trace as a single round-join /
        // round-cap visage::Path (one coverage mask, filled via the path atlas — the
        // gallery SpectrumView idiom and the z-domain equivalent of the shipped
        // PathStrokeType(curved, rounded)), so no trace has translucent self-overlap:
        // the combined MAIN + its GLOW, each per-node curve, the POST spectrum line and
        // the reduction-curtain stroke. The glow follows the JUCE oracle exactly — a
        // WIDER, low-alpha stroke of the SAME combined path (not a second capsule
        // chain); see rebuildProfileCache. (Measured: glow 7 px bead amplitude
        // 6.3 -> 0.5, POST-line 4 px bead 2.2 -> 0.07 — the comb is gone.)
        //
        // Perf: the predecessor measured stroking every trace as a path ~2x the busy-
        // state SOFTWARE-raster cost; confirmed (~2.5x, all 10 nodes on, MAX layout,
        // -O0 SwiftShader — the pathological worst case). Mitigated three ways.
        // (1) Animated traces sample at 4 px — the pre-round-5 density. (The brief
        // suggested 2 px, but that is FINER than the baseline and measurably doubled the
        // animated path's atlas-triangle cost for no visible gain on the noisy spectra —
        // and, since those traces are now smooth PATHS rather than capsule chains, 4 px
        // no longer facets. So 4 px keeps the animated layer at its baseline cost while
        // the path stroke removes the beads.) (2) All polyline storage is reused frame-
        // to-frame (member Paths cleared + refilled), so a stroke never allocates fresh.
        // (3) The reduction-PROFILE layer
        // is STATIC between parameter edits, so its sampling (reductionProfileDbAt,
        // heavier than a bin lookup) + Path::stroke() tessellation run once per edit,
        // cached behind a signature — a static animated frame just re-fills the cache.
        // KEY FINDING: caching does NOT cut the per-frame atlas RASTER — each
        // canvas.fill re-adds its path to the atlas (needs_update=true) so the atlas
        // re-rasterises it every frame regardless of whether the geometry changed — so
        // (3) only saves CPU (sampling + tessellation), not the software-raster
        // coverage that IS the SwiftShader worst case. On native GPU (the real target)
        // the atlas rasterises cheaply, so the fix is effectively free there; the clean
        // way to also cut the software-raster cost is a cached offscreen layer for the
        // static profile (one composite/frame) — noted as a follow-up, not done here
        // (it needs a frame-hierarchy split that would disturb the interaction hit-
        // testing the Playwright suite covers).
        //
        // Column count for a plot span at ~kStepPx spacing (>= 2 points).
        constexpr float kStepSpectrumPx = 4.0f; // animated spectra + reduction (baseline density; smooth as paths)
        constexpr float kStepProfilePx  = 7.0f; // per-node curves (thin, faint, cached path outlines)
        constexpr float kStepCombinedPx = 1.0f; // COMBINED coral curve + glow: per-pixel (cached path)

        int columnCount (float widthPx, float stepPx)
        {
            return std::max (2, (int) std::round (widthPx / std::max (1.0f, stepPx)) + 1);
        }

        std::vector<float>& scratchColumns()
        {
            static thread_local std::vector<float> buf; // GUI-thread only
            return buf;
        }

        // Build an OPEN polyline visage::Path from a per-column screen-y trace
        // (spanning [x0, x0+w]) into `dst` (cleared + reused). One subpath -> one
        // canvas.fill(dst.stroke(...)) rasterises the whole trace as ONE coverage
        // mask, so there are no translucent per-segment overlaps to double-composite
        // (the beaded "comb"). Returns true if the path has >= 2 points.
        bool buildColumnsPath (visage::Path& dst, float x0, float w,
                               const std::vector<float>& screenY)
        {
            dst.clear();
            const int n = (int) screenY.size();
            if (n < 2 || w <= 0.0f) return false;
            const float dx = w / (float) (n - 1);
            dst.moveTo (x0, screenY[0]);
            for (int i = 1; i < n; ++i)
                dst.lineTo (x0 + (float) i * dx, screenY[(std::size_t) i]);
            return dst.numPoints() >= 2;
        }

        // Same, but SKIP floored runs (columns at/below floorY) — a low/high cut
        // rolled to the plot floor shouldn't stroke a flat line out to the edge
        // (mirrors SuppressionCurveComponent's combined-curve floor trim). Each
        // off-floor run is its own SUBPATH of the one Path: land on the floor (one
        // anchor), lift, and rise out of that same anchor — so it stays a single
        // stroke primitive with no capsule joints.
        bool buildColumnsPathTrimFloor (visage::Path& dst, float x0, float w,
                                        const std::vector<float>& screenY, float floorY)
        {
            dst.clear();
            const int n = (int) screenY.size();
            if (n < 2 || w <= 0.0f) return false;
            const float dx = w / (float) (n - 1);
            bool penDown = false, haveAnchor = false;
            float anchorX = 0.0f, anchorY = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float x = x0 + (float) i * dx;
                const float y = screenY[(std::size_t) i];
                if (y >= floorY - 0.5f) // at the floor: hold the pen, don't stroke a horizontal run
                {
                    if (penDown) { dst.lineTo (x, y); penDown = false; } // land on the floor, then lift
                    anchorX = x; anchorY = y; haveAnchor = true;
                }
                else if (! penDown) // lifting off the floor: rise out of the last floor point
                {
                    if (haveAnchor) { dst.moveTo (anchorX, anchorY); dst.lineTo (x, y); }
                    else             dst.moveTo (x, y);
                    penDown = true; haveAnchor = false;
                }
                else dst.lineTo (x, y);
            }
            return dst.numPoints() >= 2;
        }

        // Stroke a prebuilt polyline as ONE round-join / round-cap path. Brush is set
        // by the caller. Used for the animated per-frame traces (POST spectrum,
        // reduction curtain stroke); the static profile layer caches its outlines.
        // src is non-const because visage::Path::stroke() is a non-const member.
        void fillStroke (visage::Canvas& canvas, visage::Path& src, float width)
        {
            if (src.numPoints() >= 2)
                canvas.fill (src.stroke (width, visage::Path::Join::Round, visage::Path::EndCap::Round));
        }

        // Build the region between a per-column trace and a horizontal baseline
        // (screen-y) as a SINGLE closed path into `dst` (cleared + reused) — the exact
        // idiom the shipped JUCE editor (g.fillPath(area)) and the gallery
        // SpectrumView both use, so the fill is one continuous shape with ONE coverage
        // per pixel: no internal edges, therefore no seams (the earlier per-column
        // translucent-bar version composited each overlap twice and read as the
        // comb/hatch the user reported). The caller sets the brush (solid or a plot-
        // anchored vertical gradient) before filling; it applies across the shape,
        // matching factory_ui::fillSpectrumArea.
        void buildAreaToBaseline (visage::Path& dst, float x0, float w,
                                  const std::vector<float>& screenY, float baselineY)
        {
            dst.clear();
            const int n = (int) screenY.size();
            if (n < 2 || w <= 0.0f) return;
            const float dx = w / (float) (n - 1);
            dst.moveTo (x0, baselineY);
            for (int i = 0; i < n; ++i)
                dst.lineTo (x0 + (float) i * dx, screenY[(std::size_t) i]);
            dst.lineTo (x0 + (float) (n - 1) * dx, baselineY);
            dst.close();
        }

        // Build-and-fill helper for the ANIMATED area fills (PRE spectrum, reduction
        // curtain) — reuses one thread_local Path so a per-frame fill never allocates.
        void fillAreaToBaseline (visage::Canvas& canvas, float x0, float w,
                                 const std::vector<float>& screenY, float baselineY)
        {
            static thread_local visage::Path area;
            buildAreaToBaseline (area, x0, w, screenY, baselineY);
            if (area.numPoints() >= 2) canvas.fill (area);
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
            if (buildColumnsPath (animPath_, plot_.x, plot_.w, colY))
            {
                canvas.setColor (visage::Color (theme_.rs.postColour).withAlpha (theme_.rs.postLineAlpha));
                fillStroke (canvas, animPath_, theme_.rs.postLineWidth); // one path, no capsule beads
            }
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
            if (buildColumnsPath (animPath_, plot_.x, plot_.w, colY))
            {
                canvas.setColor (visage::Color (theme_.base.palette.positive).withAlpha (theme_.rs.curtainStrokeAlpha));
                fillStroke (canvas, animPath_, theme_.rs.curtainStrokeWidth); // one path, no capsule beads
            }
        }
    }

    // The reduction-profile face (kV201Style PerNodePlusCombined). Geometry is the
    // shipped SuppressionCurveComponent's: the sens baseline sits at sensToY(0) — the
    // plot's vertical MIDDLE — and the ±30 dB sens range spans the full plot height,
    // evaluated with factory_core::reductionProfileDbAt (the SAME shape the audio path
    // runs) so a band's dot rides its own peak on the curve. Two layers, back to front:
    //   (1) each ACTIVE node's own contribution — a translucent FILL (0.12) from the
    //       baseline plus a coloured STROKE (0.7) — bands in their band hue, cuts dimmed;
    //   (2) the COMBINED profile over ALL nodes as a SOLID coral line (2.2 px) with a
    //       soft glow under it (0.22 / 5 px), floor-trimmed so a cut rolled to the plot
    //       floor doesn't stroke a flat edge.
    // Round #5: every stroke here is ONE round-join visage::Path (no capsule chains) —
    // the per-node 1 px curves, and the combined main + its glow (the glow a WIDER
    // stroke of the SAME combined path — the JUCE oracle idiom — so the halo the user
    // zoomed no longer beads). This whole layer is STATIC between parameter edits, so
    // its sampling + the Path::stroke() tessellation are cached behind a signature
    // (profileSignature), rebuilt only on change (rebuildProfileCache); an animated
    // frame just re-fills the cached outlines with the live theme colours.
    void RsSuppressionCurveView::drawProfile (visage::Canvas& canvas)
    {
        if (plot_.w < 2.0f || plot_.h <= 0.0f) return;
        const float y0     = sensToY (0.0f);       // nominal (0 dB) baseline = plot middle
        const float floorY = plot_.y + plot_.h;

        const std::uint64_t sig = profileSignature();
        if (! profileCacheValid_ || sig != profileSig_)
        {
            rebuildProfileCache (y0, floorY);
            profileSig_ = sig;
            profileCacheValid_ = true;
        }

        // (1) per-node fills + strokes (under the combined), in build order. Colour is
        //     re-read from the live theme each frame (so a theme hot-reload recolours
        //     without a geometry rebuild); the geometry is cached.
        for (const ProfileNodeGeom& g : nodeCache_)
        {
            const std::uint32_t col = RsProfileModel::isCut (g.id) ? theme_.base.palette.textDim
                                                                   : bandColour (g.id);
            if (theme_.rs.perNodeFillAlpha > 0.0f && g.fillArea.numPoints() >= 2)
            {
                canvas.setColor (visage::Color (col).withAlpha (theme_.rs.perNodeFillAlpha));
                canvas.fill (g.fillArea);
            }
            if (theme_.rs.perNodeStrokeAlpha > 0.0f && g.strokeOutline.numPoints() >= 2)
            {
                canvas.setColor (visage::Color (col).withAlpha (theme_.rs.perNodeStrokeAlpha));
                canvas.fill (g.strokeOutline); // single overlap-free path (cached outline)
            }
        }

        // (2) combined glow (wide, low alpha) then the crisp main line — BOTH are
        //     strokes of the SAME floor-trimmed path, so the glow is one coverage mask
        //     too: no capsule-joint beads under the line the user zoomed.
        if (theme_.rs.profileGlowAlpha > 0.0f && combinedGlowOutline_.numPoints() >= 2)
        {
            canvas.setColor (visage::Color (theme_.rs.profileColour).withAlpha (theme_.rs.profileGlowAlpha));
            canvas.fill (combinedGlowOutline_);
        }
        if (combinedMainOutline_.numPoints() >= 2)
        {
            canvas.setColor (visage::Color (theme_.rs.profileColour).withAlpha (theme_.rs.profileStrokeAlpha));
            canvas.fill (combinedMainOutline_);
        }
    }

    // Signature of every input the cached profile geometry depends on: the plot rect
    // (resize), the stroke widths/alphas (theme reload) and every node's params (edits).
    // When it changes, drawProfile re-tessellates; otherwise it re-fills the cache.
    // Colours are NOT hashed — they are applied live at fill time, not baked into the
    // geometry — so a palette-only theme change recolours without a rebuild.
    std::uint64_t RsSuppressionCurveView::profileSignature() const
    {
        auto mix = [] (std::uint64_t h, std::uint64_t v)
        { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h; };
        auto fb = [] (float f) { std::uint32_t u = 0; std::memcpy (&u, &f, sizeof u); return (std::uint64_t) u; };

        std::uint64_t h = 1469598103934665603ULL;
        h = mix (h, fb (plot_.x)); h = mix (h, fb (plot_.y));
        h = mix (h, fb (plot_.w)); h = mix (h, fb (plot_.h));
        h = mix (h, fb (theme_.rs.profileStrokeWidth));
        h = mix (h, fb (theme_.rs.profileGlowWidth));
        h = mix (h, fb (theme_.rs.profileGlowAlpha));
        h = mix (h, fb (theme_.rs.profileStrokeAlpha));
        h = mix (h, fb (theme_.rs.perNodeFillAlpha));
        h = mix (h, fb (theme_.rs.perNodeStrokeAlpha));
        for (int id = 0; id < RsProfileModel::kNumNodes; ++id)
        {
            h = mix (h, model_.nodeOn (id) ? 0x11ULL : 0x22ULL);
            h = mix (h, fb (model_.nodeFreq (id)));
            if (RsProfileModel::isCut (id))
                h = mix (h, (std::uint64_t) model_.cutSlope (id));
            else
            {
                h = mix (h, fb (model_.nodeSens (id)));
                h = mix (h, fb (model_.nodeWidth (id)));
                h = mix (h, (std::uint64_t) model_.nodeType (id));
            }
        }
        return h;
    }

    // Re-tessellate the static profile layer: sample each ON node's own contribution +
    // the combined profile (reductionProfileDbAt — heavier than a bin lookup, so this
    // runs once per edit, not once per animated frame), build each as a single Path,
    // and pre-stroke the outlines. The combined glow + main are two strokes of the SAME
    // floor-trimmed path (glow wider / lower alpha — the JUCE oracle idiom). This is the
    // cold path; local Paths here are not per-frame.
    void RsSuppressionCurveView::rebuildProfileCache (float y0, float floorY)
    {
        const auto nodes = model_.buildNodes();
        std::vector<float>& curveY = scratchColumns();

        auto sampleProfile = [&] (const factory_core::ReductionNodes& cfg, int cols)
        {
            curveY.resize ((std::size_t) cols);
            for (int i = 0; i < cols; ++i)
            {
                const float x = plot_.x + (cols > 1 ? (float) i / (float) (cols - 1) : 0.0f) * plot_.w;
                curveY[(std::size_t) i] = sensToY ((float) factory_core::reductionProfileDbAt (xToFreq (x), cfg));
            }
        };
        const int colsNode     = columnCount (plot_.w, kStepProfilePx);
        const int colsCombined = columnCount (plot_.w, kStepCombinedPx);

        // (1) per-node geometry (fill area + 1 px stroke outline), for ON nodes only.
        nodeCache_.clear();
        for (int id = 0; id < RsProfileModel::kNumNodes; ++id)
        {
            if (! model_.nodeOn (id)) continue;
            ProfileNodeGeom g;
            g.id = id;
            sampleProfile (RsProfileModel::singleNode (nodes, id), colsNode);
            buildAreaToBaseline (g.fillArea, plot_.x, plot_.w, curveY, y0); // fill: single closed path
            visage::Path poly;
            if (buildColumnsPath (poly, plot_.x, plot_.w, curveY))          // stroke: single overlap-free path
                g.strokeOutline = poly.stroke (1.0f, visage::Path::Join::Round, visage::Path::EndCap::Round);
            nodeCache_.push_back (std::move (g));
        }

        // (2) combined glow + main = two strokes of the SAME floor-trimmed path.
        sampleProfile (nodes, colsCombined);
        combinedGlowOutline_.clear();
        combinedMainOutline_.clear();
        visage::Path combined;
        if (buildColumnsPathTrimFloor (combined, plot_.x, plot_.w, curveY, floorY))
        {
            if (theme_.rs.profileGlowAlpha > 0.0f && theme_.rs.profileGlowWidth > 0.0f)
                combinedGlowOutline_ = combined.stroke (theme_.rs.profileGlowWidth,
                                                        visage::Path::Join::Round, visage::Path::EndCap::Round);
            combinedMainOutline_ = combined.stroke (theme_.rs.profileStrokeWidth,
                                                    visage::Path::Join::Round, visage::Path::EndCap::Round);
        }
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
