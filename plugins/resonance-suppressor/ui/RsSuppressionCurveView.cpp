#include "RsSuppressionCurveView.h"

#include "factory_ui_visage/Fonts.h"

#include <visage_graphics/path.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

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

        // Stroke a polyline as connected flat GPU segments. visage's path-fill
        // atlas silently drops every path fill in a frame that also submits a
        // large plot-spanning path (which the analyser traces are), so the PRE
        // line / per-node outlines are drawn from primitives instead of
        // canvas.fill(path.stroke(...)).
        void strokePolyline (visage::Canvas& canvas, const std::vector<visage::Point>& pts, float width)
        {
            for (std::size_t i = 1; i < pts.size(); ++i)
                canvas.segment (pts[i - 1].x, pts[i - 1].y, pts[i].x, pts[i].y, width, /*rounded*/ true);
        }

        // Stroke a polyline as a dash pattern (on/off px) — visage has no dashed
        // stroke, so we walk the path and emit rounded segments for the "on" runs.
        void strokeDashedPolyline (visage::Canvas& canvas, const std::vector<visage::Point>& pts,
                                   float width, float on, float off)
        {
            if (pts.size() < 2 || on <= 0.0f)
                return;
            bool penDown = true;
            float used = 0.0f; // distance into the current on/off run
            for (std::size_t i = 1; i < pts.size(); ++i)
            {
                const visage::Point a = pts[i - 1], b = pts[i];
                const float segLen = std::sqrt ((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                float t = 0.0f;
                while (t < segLen)
                {
                    const float runLen = penDown ? on : off;
                    const float step = std::min (runLen - used, segLen - t);
                    if (penDown && step > 0.0f)
                    {
                        const float u0 = t / segLen, u1 = (t + step) / segLen;
                        canvas.segment (a.x + (b.x - a.x) * u0, a.y + (b.y - a.y) * u0,
                                        a.x + (b.x - a.x) * u1, a.y + (b.y - a.y) * u1, width, true);
                    }
                    t += step;
                    used += step;
                    if (used >= runLen - 0.001f) { penDown = ! penDown; used = 0.0f; }
                }
            }
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
            canvas.text (fl.s, regularFont (10.0f), visage::Font::kCenter, x - 18.0f, plot_.y - 13.0f, 36.0f, 12.0f);
        }
        for (float db = 0.0f; db >= -60.0f; db -= 12.0f)
        {
            const float y = dbToY (db);
            canvas.setColor (visage::Color (db == 0.0f ? theme_.base.palette.track : theme_.rs.borderLight));
            canvas.segment (plot_.x, y, plot_.x + plot_.w, y, 1.0f, false);
        }
    }

    void RsSuppressionCurveView::drawAnalyzer (visage::Canvas& canvas)
    {
        const int n = 2 * (snapBins_ - 1);
        if (n <= 0) return;

        auto build = [&] (const std::vector<float>& data, std::vector<visage::Point>& out)
        {
            out.clear();
            for (int k = 1; k < snapBins_; ++k)
            {
                const float f = (float) ((double) k * snapSr_ / n);
                if (f < 20.0f) continue;
                if (f > 20000.0f) break;
                out.push_back (visage::Point (freqToX (f), dbToY (data[(std::size_t) k])));
            }
        };

        std::vector<visage::Point> pts;

        // PRE (input): a solid coral line (design reference).
        if (analyzerMode_ != AnalyzerMode::Post)
        {
            build (snapPre_, pts);
            if (pts.size() >= 2)
            {
                canvas.setColor (visage::Color (theme_.rs.preColour));
                strokePolyline (canvas, pts, theme_.rs.preLineWidth);
            }
        }
        // POST (output): a thin SOLID coral line (kV201Style role — the dashed
        // line is the combined reduction profile, drawn in drawProfile).
        if (analyzerMode_ != AnalyzerMode::Pre)
        {
            build (snapPost_, pts);
            if (pts.size() >= 2)
            {
                canvas.setColor (visage::Color (theme_.rs.postColour).withAlpha (theme_.rs.postLineAlpha));
                strokePolyline (canvas, pts, theme_.rs.postLineWidth);
            }
        }
    }

    void RsSuppressionCurveView::drawReduction (visage::Canvas& canvas)
    {
        const int n = 2 * (snapBins_ - 1);
        if (n <= 0) return;
        // The curtain hangs from the plot's TOP edge (design reference), its depth
        // proportional to the reduction, clamped to curtainClampFrac of the plot.
        const float top   = plot_.y;
        const float maxY  = plot_.y + theme_.rs.curtainClampFrac * plot_.h;
        const float scale = plot_.h / 96.0f; // -96 dB spans the plot height

        // Visage's polygon triangulator chokes on the near-degenerate curtain
        // contour (most bins sit exactly on the top edge, only a few dip deep),
        // so instead of one filled path we emit one filled rect per plot pixel
        // column, each hanging from the top down to that column's deepest
        // reduction. Columns are 1px wide and pixel-aligned edge-to-edge, so the
        // 0.34 alpha never double-covers (no seams) — and rects always rasterise.
        canvas.setColor (visage::Color (theme_.base.palette.positive)
                             .withAlpha (theme_.rs.curtainFillAlpha));
        int   curCol   = std::numeric_limits<int>::min();
        float curDepth = 0.0f;
        auto flush = [&]
        {
            if (curCol != std::numeric_limits<int>::min() && curDepth > 0.25f)
                canvas.fill ((float) curCol, top, 1.0f, curDepth);
        };
        for (int k = 1; k < snapBins_; ++k)
        {
            const float f = (float) ((double) k * snapSr_ / n);
            if (f < 20.0f) continue;
            if (f > 20000.0f) break;
            const float red   = std::clamp (snapRed_[(std::size_t) k], -60.0f, 0.0f);
            const float depth = std::min (top + (-red) * scale, maxY) - top;
            const int   col   = (int) std::floor (freqToX (f));
            if (col != curCol) { flush(); curCol = col; curDepth = depth; }
            else               { curDepth = std::max (curDepth, depth); }
        }
        flush();
    }

    // The reduction-profile face (design reference 2026-07-17). Geometry is the
    // shipped SuppressionCurveComponent's: the sens baseline sits at sensToY(0) —
    // the plot's vertical MIDDLE — and the ±30 dB sens range spans the full plot
    // height (plot.h/60 px per dB), so bumps stay subdued and never balloon to the
    // top. Two layers, back to front:
    //   (1) each active band's own contribution as a SUBTLE pale fill hugging the
    //       baseline (rs.analyzer.perNodeFillAlpha), and
    //   (2) the COMBINED profile — factory_core::reductionProfileDbAt over ALL
    //       nodes (the same shape the audio path runs) — as a dashed muted-coral
    //       line that passes through the node dots (whose y is sensToY(sens), so a
    //       band's dot rides its own peak on the curve).
    // Both are drawn from primitives (tiled fill rects / SEGMENT dash pieces): a
    // plot-wide filled or stroked path would poison the frame's path atlas and
    // silently drop every path fill (see strokePolyline / strokeDashedPolyline).
    void RsSuppressionCurveView::drawProfile (visage::Canvas& canvas)
    {
        const auto  nodes = model_.buildNodes();
        const int   steps = std::max (2, (int) plot_.w);
        const float y0    = sensToY (0.0f); // nominal (0 dB) baseline = plot middle

        // (1) Per-node contribution fills — subtle, from the baseline to each
        // active band's own bump, as tiled edge-to-edge bars.
        if (theme_.rs.perNodeFillAlpha > 0.0f)
        {
            for (int id = 0; id < RsProfileModel::kNumNodes; ++id)
            {
                if (RsProfileModel::isCut (id) || ! model_.nodeOn (id)) continue;
                const auto single = RsProfileModel::singleNode (nodes, id);
                canvas.setColor (visage::Color (bandColour (id)).withAlpha (theme_.rs.perNodeFillAlpha));
                float prevX = plot_.x;
                float prevY = sensToY ((float) factory_core::reductionProfileDbAt (xToFreq (plot_.x), single));
                for (int i = 1; i <= steps; ++i)
                {
                    const float x = plot_.x + (float) i * plot_.w / steps;
                    const float y = sensToY ((float) factory_core::reductionProfileDbAt (xToFreq (x), single));
                    const float top = std::min (prevY, y0);
                    const float hgt = std::abs (y0 - prevY);
                    if (hgt > 0.5f)
                        canvas.fill (prevX, top, std::max (1.0f, x - prevX), hgt);
                    prevX = x; prevY = y;
                }
            }
        }

        // (2) The combined profile as a dashed line through the dots. Trim runs
        // that sit on the plot floor (a low/high cut rolled fully off) into their
        // own sub-polylines so we never dash a flat line along the bottom edge —
        // mirrors SuppressionCurveComponent's floor trim.
        const float floorY = plot_.y + plot_.h;
        std::vector<std::vector<visage::Point>> subpaths;
        std::vector<visage::Point> cur;
        bool penDown = false, haveAnchor = false;
        visage::Point anchor;
        auto flushCur = [&] { if (cur.size() >= 2) subpaths.push_back (cur); cur.clear(); };
        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot_.x + (float) i * plot_.w / steps;
            const float y = sensToY ((float) factory_core::reductionProfileDbAt (xToFreq (x), nodes));
            if (y >= floorY - 0.5f)                 // on the floor: hold the pen
            {
                if (penDown) { cur.push_back (visage::Point (x, y)); flushCur(); penDown = false; }
                anchor = visage::Point (x, y); haveAnchor = true;
            }
            else if (! penDown)                     // lifting off the floor
            {
                if (haveAnchor) cur.push_back (anchor);
                cur.push_back (visage::Point (x, y));
                penDown = true; haveAnchor = false;
            }
            else cur.push_back (visage::Point (x, y));
        }
        flushCur();

        canvas.setColor (visage::Color (theme_.rs.profileColour));
        for (const auto& sp : subpaths)
            strokeDashedPolyline (canvas, sp, theme_.rs.profileLineWidth,
                                  theme_.rs.profileDashOn, theme_.rs.profileDashOff);
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
            const bool selected = (id == selectedNode_) || (id == hoverNode_);

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

    void RsSuppressionCurveView::mouseDown (const visage::MouseEvent& e)
    {
        const visage::Point pos = e.position;

        // header chips
        auto inRect = [] (const Rect& r, const visage::Point& p)
        { return p.x >= r.x && p.x < r.x + r.w && p.y >= r.y && p.y < r.y + r.h; };
        if (inRect (modeChip_, pos))
        {
            analyzerMode_ = (analyzerMode_ == AnalyzerMode::Both) ? AnalyzerMode::Pre
                          : (analyzerMode_ == AnalyzerMode::Pre)  ? AnalyzerMode::Post
                                                                  : AnalyzerMode::Both;
            redraw(); return;
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
    }

    void RsSuppressionCurveView::mouseExit (const visage::MouseEvent&)
    {
        if (hoverNode_ >= 0) { hoverNode_ = -1; redraw(); }
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
