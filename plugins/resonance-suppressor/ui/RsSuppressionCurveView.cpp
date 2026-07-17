#include "RsSuppressionCurveView.h"

#include "factory_ui_visage/Fonts.h"

#include <visage_graphics/path.h>

#include <algorithm>
#include <cmath>
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

        visage::Path strokedPolyline (const std::vector<visage::Point>& pts)
        {
            visage::Path p;
            for (std::size_t i = 0; i < pts.size(); ++i)
                (i == 0) ? p.moveTo (pts[i]) : p.lineTo (pts[i]);
            return p;
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
        const float baseline = plot_.y + plot_.h;

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

        if (analyzerMode_ != AnalyzerMode::Post)
        {
            build (snapPre_, pts);
            if (pts.size() >= 2)
            {
                visage::Path area;
                area.moveTo (pts.front().x, baseline);
                for (const auto& p : pts) area.lineTo (p.x, p.y);
                area.lineTo (pts.back().x, baseline);
                area.close();
                canvas.setColor (visage::Brush::vertical (
                    visage::Color (theme_.rs.inputColour).withAlpha (theme_.rs.inputFillTopAlpha),
                    visage::Color (theme_.rs.inputColour).withAlpha (theme_.rs.inputFillBotAlpha)));
                canvas.fill (area);
            }
        }
        if (analyzerMode_ != AnalyzerMode::Pre)
        {
            build (snapPost_, pts);
            if (pts.size() >= 2)
            {
                canvas.setColor (visage::Color (theme_.rs.postColour).withAlpha (theme_.rs.postLineAlpha));
                canvas.fill (strokedPolyline (pts).stroke (theme_.rs.postLineWidth,
                             visage::Path::Join::Round, visage::Path::EndCap::Round));
            }
        }
    }

    void RsSuppressionCurveView::drawReduction (visage::Canvas& canvas)
    {
        const int n = 2 * (snapBins_ - 1);
        if (n <= 0) return;
        const float top = dbToY (0.0f);
        const float baseY = plot_.y + plot_.h;

        std::vector<visage::Point> pts;
        for (int k = 1; k < snapBins_; ++k)
        {
            const float f = (float) ((double) k * snapSr_ / n);
            if (f < 20.0f) continue;
            if (f > 20000.0f) break;
            const float red = std::clamp (snapRed_[(std::size_t) k], -60.0f, 0.0f);
            const float y = std::min (dbToY (red), baseY);
            pts.push_back (visage::Point (freqToX (f), y));
        }
        if (pts.size() < 2) return;

        visage::Path area;
        area.moveTo (pts.front().x, top);
        for (const auto& p : pts) area.lineTo (p.x, p.y);
        area.lineTo (pts.back().x, top);
        area.close();
        canvas.setColor (visage::Color (theme_.rs.teal).withAlpha (theme_.rs.deltaFillAlpha));
        canvas.fill (area);
        if (theme_.rs.deltaStrokeAlpha > 0.0f && theme_.rs.deltaStrokeWidth > 0.0f)
        {
            canvas.setColor (visage::Color (theme_.rs.teal).withAlpha (theme_.rs.deltaStrokeAlpha));
            canvas.fill (strokedPolyline (pts).stroke (theme_.rs.deltaStrokeWidth,
                         visage::Path::Join::Round, visage::Path::EndCap::Round));
        }
    }

    void RsSuppressionCurveView::drawProfile (visage::Canvas& canvas)
    {
        const auto nodes = model_.buildNodes();
        const int steps = std::max (2, (int) plot_.w);
        const float y0 = sensToY (0.0f);
        const float floorY = plot_.y + plot_.h;

        // Per-node translucent fills + coloured strokes (v2.0.1 PerNodePlusCombined).
        for (int id = 0; id < RsProfileModel::kNumNodes; ++id)
        {
            if (! model_.nodeOn (id)) continue;
            const auto single = RsProfileModel::singleNode (nodes, id);
            std::vector<visage::Point> pts;
            pts.reserve ((std::size_t) steps + 1);
            for (int i = 0; i <= steps; ++i)
            {
                const float x = plot_.x + (float) i * plot_.w / steps;
                const float yB = sensToY ((float) factory_core::reductionProfileDbAt (xToFreq (x), single));
                pts.push_back (visage::Point (x, yB));
            }
            const std::uint32_t col = RsProfileModel::isCut (id) ? theme_.base.palette.textDim : bandColour (id);
            if (theme_.rs.perNodeFillAlpha > 0.0f)
            {
                visage::Path fp = strokedPolyline (pts);
                fp.lineTo (plot_.x + plot_.w, y0);
                fp.lineTo (plot_.x, y0);
                fp.close();
                canvas.setColor (visage::Color (col).withAlpha (theme_.rs.perNodeFillAlpha));
                canvas.fill (fp);
            }
            if (theme_.rs.perNodeStrokeAlpha > 0.0f)
            {
                canvas.setColor (visage::Color (col).withAlpha (theme_.rs.perNodeStrokeAlpha));
                canvas.fill (strokedPolyline (pts).stroke (1.0f, visage::Path::Join::Round, visage::Path::EndCap::Round));
            }
        }

        // Combined response: soft glow under a crisp stroke. Break the path where it
        // sits on the floor (a cut rolled all the way off) so we don't stroke a flat
        // run out to the edges (mirrors the JUCE floor-trimming, simplified).
        std::vector<visage::Point> run;
        auto flushRun = [&] ()
        {
            if (run.size() < 2) { run.clear(); return; }
            if (theme_.rs.combinedGlowAlpha > 0.0f && theme_.rs.combinedGlowWidth > 0.0f)
            {
                canvas.setColor (visage::Color (theme_.rs.combinedColour).withAlpha (theme_.rs.combinedGlowAlpha));
                canvas.fill (strokedPolyline (run).stroke (theme_.rs.combinedGlowWidth,
                             visage::Path::Join::Round, visage::Path::EndCap::Round));
            }
            canvas.setColor (visage::Color (theme_.rs.combinedColour).withAlpha (theme_.rs.combinedStrokeAlpha));
            canvas.fill (strokedPolyline (run).stroke (theme_.rs.combinedStrokeWidth,
                         visage::Path::Join::Round, visage::Path::EndCap::Round));
            run.clear();
        };
        for (int i = 0; i <= steps; ++i)
        {
            const float x = plot_.x + (float) i * plot_.w / steps;
            const float yT = sensToY ((float) factory_core::reductionProfileDbAt (xToFreq (x), nodes));
            if (yT >= floorY - 0.5f) // at the floor: land, then lift the pen
            {
                if (! run.empty()) { run.push_back (visage::Point (x, yT)); flushRun(); }
            }
            else
            {
                run.push_back (visage::Point (x, yT));
            }
        }
        flushRun();
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
                    canvas.setColor (visage::Color (theme_.rs.teal).withAlpha (0.9f));
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
                    canvas.setColor (visage::Color (theme_.rs.teal).withAlpha (0.9f));
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
