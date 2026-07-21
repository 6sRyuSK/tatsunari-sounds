#include "RsNodePanel.h"

#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Knob.h" // shared knobAngleForNorm / knobNeedleTip (A2)
#include "factory_ui_visage/ValueEntry.h" // stripLeadingNumber + ValueEntryRequest
#include "factory_params/Text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace rs_ui
{
    using factory_ui_visage::regularFont;
    using factory_ui_visage::boldFont;
    namespace fuv = factory_ui_visage;

    namespace
    {
        // Parse a FREQ text entry to raw Hz, mirroring the shipped JUCE NodePanel
        // freq valueFromTextFunction: the display is Hz/kHz, so a trailing k/kHz
        // (case/space tolerant) means kHz, and a bare number below the parameter
        // minimum whose *1000 lands in range is treated as kHz ("2.6" against a
        // "2.6kHz" display). Returns false (REVERT) when there is no leading number
        // after trimming + stripping the k/kHz suffix ("abc", "", "kHz") — the round
        // #4 follow-up (invalid reverts, not clamp-to-min). setFromUi clamps a valid
        // result to the range.
        bool parseFreqEntry (const std::string& text, double lo, double hi, float& out)
        {
            std::string t = text;
            auto isSpace = [] (char c) { return std::isspace ((unsigned char) c) != 0; };
            while (! t.empty() && isSpace (t.front())) t.erase (t.begin());
            while (! t.empty() && isSpace (t.back()))  t.pop_back();
            std::string low = t;
            for (char& c : low) c = (char) std::tolower ((unsigned char) c);
            bool asK = false;
            if (low.size() >= 3 && low.compare (low.size() - 3, 3, "khz") == 0) { t.resize (t.size() - 3); asK = true; }
            else if (! low.empty() && low.back() == 'k')                        { t.resize (t.size() - 1); asK = true; }
            double v = 0.0;
            if (! factory_params::tryParseNumber (t, v)) return false; // no leading number -> revert
            if (asK) { out = (float) (v * 1000.0); return true; }
            if (v < lo && v * 1000.0 >= lo && v * 1000.0 <= hi) { out = (float) (v * 1000.0); return true; }
            out = (float) v;
            return true;
        }

        // (The mini-knob sweep endpoints now come from the shared KnobMetrics —
        // theme_.base.knob.arcStart/arcEnd — via fuv::knobAngleForNorm, and the value
        // ring itself is drawn with the SHARED fuv::fillArcBand (Knob.h), the SAME
        // tiled-flatArc path the footer Knob uses. The old local strokeArc drew a
        // single canvas.arc that skipped fillArcBand's −90° screen offset, so the
        // ring landed 90° off from the needle at every value — the round-3 fix 6
        // needle/arc mismatch. Using the shared helper makes needle ≡ arc end.)

        // The 6 filter-type glyphs (viewBox 0 0 24 14), verbatim from NodePanel::eqGlyph.
        visage::Path eqGlyph (int type)
        {
            visage::Path p;
            switch (type)
            {
                case 0: p.moveTo (2, 12); p.bezierTo (8, 12, 8, 3, 12, 3); p.bezierTo (16, 3, 16, 12, 22, 12); break;
                case 1: p.moveTo (2, 4); p.lineTo (9, 4); p.bezierTo (12, 4, 12, 11, 15, 11); p.lineTo (22, 11); break;
                case 2: p.moveTo (2, 11); p.lineTo (9, 11); p.bezierTo (12, 11, 12, 4, 15, 4); p.lineTo (22, 4); break;
                case 3: p.moveTo (2, 11); p.lineTo (6, 11); p.bezierTo (8, 11, 8, 4, 10, 4); p.lineTo (14, 4);
                        p.bezierTo (16, 4, 16, 11, 18, 11); p.lineTo (22, 11); break;
                case 4: p.moveTo (2, 4); p.lineTo (8, 4); p.bezierTo (10, 4, 10, 12, 12, 12); p.bezierTo (14, 12, 14, 4, 16, 4); p.lineTo (22, 4); break;
                default: p.moveTo (3, 12); p.lineTo (21, 4); break;
            }
            return p;
        }
    } // namespace

    RsNodePanel::RsNodePanel (const RsTheme& theme, RsProfileModel& model, RsFeed& feed)
        : theme_ (theme), model_ (model), feed_ (feed)
    {
        setNode (0);
    }

    std::string RsNodePanel::nodeName() const
    {
        if (isCut_) return nodeId_ == 0 ? "Low Cut" : "High Cut";
        return "Band " + std::to_string (nodeId_ - 1);
    }

    std::uint32_t RsNodePanel::nodeColour() const
    {
        if (isCut_) return nodeId_ == 0 ? theme_.rs.orange : theme_.rs.highCutRing;
        return theme_.base.palette.bandColours[(std::size_t) ((nodeId_ - 2) % 6)];
    }

    void RsNodePanel::setNode (int id)
    {
        nodeId_ = id;
        isCut_ = RsProfileModel::isCut (id);
        choiceCount_ = isCut_ ? 4 : 6;

        auto& store = model_.store();
        freqK_.paramIndex = model_.idxFreq (id);
        freqK_.label = "FREQ"; freqK_.freq = true; freqK_.decimals = 0; freqK_.visible = true;
        freqK_.range = factory_params::makeRange (store.desc (freqK_.paramIndex));

        if (! isCut_)
        {
            sensK_.paramIndex = model_.idxSens (id);
            sensK_.label = "SENS"; sensK_.freq = false; sensK_.decimals = 2; sensK_.visible = true;
            sensK_.range = factory_params::makeRange (store.desc (sensK_.paramIndex));
            widthK_.paramIndex = model_.idxWidth (id);
            widthK_.label = "WIDTH"; widthK_.freq = false; widthK_.decimals = 2; widthK_.visible = true;
            widthK_.range = factory_params::makeRange (store.desc (widthK_.paramIndex));
        }
        else
        {
            sensK_.visible = false; widthK_.visible = false;
        }

        refreshListen();
        computeLayout();
        redraw();
    }

    void RsNodePanel::refreshListen() { listenOn_ = (feed_.getListenNode() == nodeId_); }

    void RsNodePanel::resized()
    {
        // Intrinsic-width guard (user-reported edge-clamp overlap). The editor
        // computes the panel's bounds in RsEditor::selectNode, but it reads
        // preferredWidth() BEFORE calling setNode(), so the width it hands us
        // reflects the PREVIOUSLY bound node — a band opened right after a cut is
        // sized to the cut's 350, a cut after a band to 500. At that stale width
        // computeLayout() reflows into a collision: the right-anchored FREQ/SENS/
        // WIDTH knob column (knobsX = rx + rw − knobsW) slides left over the
        // left-anchored TYPE buttons, and the "Listen" chip clips to "Lis…". The
        // JUCE oracle (SuppressionCurveComponent::selectNode → positionPanel) sets
        // the node FIRST and only then reads preferredWidth(), so the panel's width
        // is ALWAYS its intrinsic preferredWidth() and only its x/y is placed and
        // clamped. The PRIMARY fix reorders RsEditor::selectNode to that order, so we
        // are normally handed the right width; this stays as defense-in-depth for any
        // other caller. setNode() has run by the time we are sized (isCut_ is correct
        // here), so enforce the intrinsic width AND re-clamp x so widening can never
        // push the panel past its parent's right edge.
        const float pw = (float) preferredWidth();
        if (std::abs (width() - pw) > 0.5f)
        {
            float nx = x();
            if (const visage::Frame* p = parent())
                nx = std::clamp (nx, 0.0f, std::max (0.0f, p->width() - pw));
            setBounds (nx, y(), pw, height()); // re-enters resized() at the intrinsic width
            return;
        }
        computeLayout();
    }

    void RsNodePanel::computeLayout()
    {
        const float w = width(), h = height();
        closeBtn_ = { w - 28.0f, 8.0f, 18.0f, 18.0f };

        // inner reduced(14,12)
        float rx = 14.0f, ry = 12.0f, rw = w - 28.0f, rh = h - 24.0f;

        // right knob column
        const float knobW = 52.0f, kgap = 10.0f;
        const float knobsW = isCut_ ? knobW : knobW * 3 + kgap * 2;
        float knobsX = rx + rw - knobsW;
        rw -= (knobsW + 16.0f);
        const float knobsY = ry + 18.0f, knobsH = rh - 18.0f;
        freqK_.area = { knobsX, knobsY, knobW, knobsH };
        if (! isCut_)
        {
            sensK_.area  = { knobsX + knobW + kgap, knobsY, knobW, knobsH };
            widthK_.area = { knobsX + 2.0f * (knobW + kgap), knobsY, knobW, knobsH };
        }

        // header row (26)
        float hx = rx, hy = ry;
        dotRect_ = { hx, hy + (26.0f - 14.0f) * 0.5f, 14.0f, 14.0f };
        hx += 18.0f + 4.0f;
        nameRect_ = { hx, hy, 76.0f, 26.0f };
        hx += 76.0f + 8.0f;
        onBadge_ = { hx, hy + 2.0f, 40.0f, 22.0f };
        hx += 40.0f + 6.0f;
        const float listenW = std::min (90.0f, rx + rw - hx);
        listenBadge_ = { hx, hy + 2.0f, std::max (40.0f, listenW), 22.0f };

        // caption + choice row (at ry + 26 + 18)
        float cy = ry + 26.0f + 18.0f;
        captionRect_ = { rx, cy, (isCut_ ? 52.0f : 38.0f), 30.0f };
        float bx = rx + captionRect_.w + 8.0f;
        const float bw = isCut_ ? 40.0f : 32.0f, bgap = 4.0f, bh = 27.0f;
        for (int i = 0; i < choiceCount_; ++i)
        {
            choiceBtns_[(std::size_t) i] = { bx, cy + (30.0f - bh) * 0.5f, bw, bh };
            bx += bw + bgap;
        }
    }

    std::string RsNodePanel::valueText (const MiniKnob& k) const
    {
        const float v = model_.store().value (k.paramIndex);
        if (k.freq)
        {
            char buf[16];
            if (v >= 1000.0f)
            {
                const float kk = v / 1000.0f;
                const bool whole = std::abs (kk - std::round (kk)) < 0.05f;
                if (whole) std::snprintf (buf, sizeof buf, "%dkHz", (int) std::round (kk));
                else       std::snprintf (buf, sizeof buf, "%.1fkHz", kk);
            }
            else std::snprintf (buf, sizeof buf, "%dHz", (int) std::round (v));
            return buf;
        }
        return factory_params::formatValue (model_.store().desc (k.paramIndex), v, k.decimals);
    }

    // Shared mini-knob dial geometry. value -> normalised -> ring/needle angle
    // goes through the SHARED knob helper + the SHARED KnobMetrics sweep
    // (fuv::knobAngleForNorm, theme_.base.knob), so a mini-knob points at exactly
    // the same angle the footer Knob would for the same normalised value — ONE
    // implementation, no duplicated 225°/495° constants (A2).
    RsNodePanel::MiniDial RsNodePanel::miniKnobDial (const MiniKnob& k) const
    {
        const Rect& a = k.area;
        // Row heights + full-diameter dial match the JUCE NodePanel mini RsKnob
        // (name 14 / value 14, dial = min(w, h-name-value)); the old 13/13 + 3px
        // radius trim made the minis read smaller than the oracle (round-3 fix 7).
        const float labelH = 14.0f, valueH = 14.0f;
        const float dialTop = a.y + labelH, dialH = a.h - labelH - valueH;
        const auto& km = theme_.base.knob;
        MiniDial d;
        d.R     = std::min (a.w, dialH) * 0.5f;
        d.cx    = a.x + a.w * 0.5f;
        d.cy    = dialTop + dialH * 0.5f;
        d.band  = std::max (2.5f, d.R * 0.30f);
        d.arcR  = d.R - d.band * 0.5f;
        d.bodyR = std::max (2.0f, d.R - d.band);
        d.len   = d.bodyR * km.needleLengthRatio;
        const float norm = factory_params::convertTo0to1 (k.range, model_.store().value (k.paramIndex));
        d.toAng = fuv::knobAngleForNorm (km, norm);
        return d;
    }

    bool RsNodePanel::miniKnobTipInWindow (int which, float& cx, float& cy, float& tx, float& ty) const
    {
        const MiniKnob* ks[] = { &freqK_, &sensK_, &widthK_ };
        if (which < 0 || which > 2 || ! ks[which]->visible) return false;
        const MiniDial d = miniKnobDial (*ks[which]);
        const visage::Point tip = fuv::knobNeedleTip (d.cx, d.cy, d.len, d.toAng);
        const visage::Point o = positionInWindow();
        cx = o.x + d.cx; cy = o.y + d.cy;
        tx = o.x + tip.x; ty = o.y + tip.y;
        return true;
    }

    bool RsNodePanel::miniKnobDialInWindow (int which, float& cx, float& cy, float& arcR) const
    {
        const MiniKnob* ks[] = { &freqK_, &sensK_, &widthK_ };
        if (which < 0 || which > 2 || ! ks[which]->visible) return false;
        const MiniDial d = miniKnobDial (*ks[which]);
        const visage::Point o = positionInWindow();
        cx = o.x + d.cx; cy = o.y + d.cy; arcR = d.arcR;
        return true;
    }

    void RsNodePanel::drawMiniKnob (visage::Canvas& canvas, const MiniKnob& k)
    {
        if (! k.visible) return;
        const Rect& a = k.area;
        const float labelH = 14.0f, valueH = 14.0f; // match miniKnobDial + JUCE (fix 7)
        // label
        canvas.setColor (visage::Color (theme_.base.palette.text));
        canvas.text (k.label, boldFont (10.0f), visage::Font::kCenter, a.x, a.y, a.w, labelH);

        // Small donut + needle (design reference: NodePanel minis in salmon).
        const auto& p  = theme_.base.palette;
        const auto& km = theme_.base.knob;
        const std::uint32_t accent = theme_.rs.orange; // salmon #ff9472
        const MiniDial d = miniKnobDial (k);
        constexpr float kTwoPi = 6.28318530718f;

        // Three solid zones through the SHARED fillArcBand so the value ring's screen
        // angle matches the needle (round-3 fix 6): accent 0->value, accentDim
        // remainder, panelLo dead zone.
        canvas.setColor (visage::Color (accent));
        fuv::fillArcBand (canvas, d.cx, d.cy, d.arcR, km.arcStart, d.toAng, d.band);
        canvas.setColor (visage::Color (p.accentDim));
        fuv::fillArcBand (canvas, d.cx, d.cy, d.arcR, d.toAng, km.arcEnd, d.band);
        canvas.setColor (visage::Color (p.panelLo));
        fuv::fillArcBand (canvas, d.cx, d.cy, d.arcR, km.arcEnd, km.arcStart + kTwoPi, d.band);

        canvas.setColor (visage::Brush::radial (visage::Color (0xffffffff), visage::Color (p.panelLo),
                                                visage::Point (d.cx, d.cy - d.bodyR * 0.28f), d.bodyR * 1.15f, d.bodyR * 1.15f));
        canvas.circle (d.cx - d.bodyR, d.cy - d.bodyR, d.bodyR * 2.0f);
        canvas.setColor (visage::Color (p.track));
        canvas.ring (d.cx - d.R, d.cy - d.R, 2.0f * d.R, 1.0f);
        const visage::Point tip = fuv::knobNeedleTip (d.cx, d.cy, d.len, d.toAng);
        canvas.setColor (visage::Color (accent));
        canvas.segment (d.cx, d.cy, tip.x, tip.y, 2.5f, true);

        // value (accent)
        canvas.setColor (visage::Color (accent));
        canvas.text (valueText (k), boldFont (11.0f), visage::Font::kCenter, a.x, a.y + a.h - valueH, a.w, valueH);
    }

    void RsNodePanel::draw (visage::Canvas& canvas)
    {
        computeLayout();
        const float w = width(), h = height();

        // card
        canvas.setColor (visage::Color (0xffffffff).withAlpha (0.97f));
        canvas.roundedRectangle (0.0f, 0.0f, w, h, theme_.rs.radiusPopover);
        canvas.setColor (visage::Color (theme_.base.palette.track));
        canvas.roundedRectangleBorder (0.5f, 0.5f, w - 1.0f, h - 1.0f, theme_.rs.radiusPopover, 1.0f);

        // header dot
        canvas.setColor (visage::Color (theme_.rs.dotRing));
        canvas.circle (dotRect_.x - 3.0f, dotRect_.y - 3.0f, dotRect_.w + 6.0f);
        canvas.setColor (visage::Color (nodeColour()));
        canvas.circle (dotRect_.x, dotRect_.y, dotRect_.w);
        // name
        canvas.setColor (visage::Color (theme_.base.palette.text));
        canvas.text (nodeName(), boldFont (15.0f), visage::Font::kLeft, nameRect_.x, nameRect_.y, nameRect_.w, nameRect_.h);

        // ON badge
        {
            const bool on = model_.nodeOn (nodeId_);
            canvas.setColor (visage::Color (on ? theme_.base.palette.positive : theme_.rs.footerBg));
            canvas.roundedRectangle (onBadge_.x, onBadge_.y, onBadge_.w, onBadge_.h, theme_.rs.radiusBadge);
            if (! on)
            {
                canvas.setColor (visage::Color (theme_.base.palette.track));
                canvas.roundedRectangleBorder (onBadge_.x + 0.5f, onBadge_.y + 0.5f, onBadge_.w - 1.0f, onBadge_.h - 1.0f, theme_.rs.radiusBadge, 1.0f);
            }
            canvas.setColor (visage::Color (on ? 0xffffffff : theme_.base.palette.textSecondary));
            canvas.text (on ? "ON" : "OFF", boldFont (11.0f), visage::Font::kCenter, onBadge_.x, onBadge_.y, onBadge_.w, onBadge_.h);
        }

        // Listen badge
        {
            canvas.setColor (visage::Color (listenOn_ ? theme_.base.palette.positive : theme_.rs.footerBg));
            canvas.roundedRectangle (listenBadge_.x, listenBadge_.y, listenBadge_.w, listenBadge_.h, theme_.rs.radiusBadge);
            canvas.setColor (visage::Color (listenOn_ ? theme_.base.palette.positive : theme_.base.palette.track));
            canvas.roundedRectangleBorder (listenBadge_.x + 0.5f, listenBadge_.y + 0.5f, listenBadge_.w - 1.0f, listenBadge_.h - 1.0f, theme_.rs.radiusBadge, 1.0f);
            const float dotD = 7.0f, dotX = listenBadge_.x + 8.0f, dotY = listenBadge_.y + listenBadge_.h * 0.5f - dotD * 0.5f;
            canvas.setColor (visage::Color (listenOn_ ? 0xffffffff : theme_.base.palette.textSecondary));
            canvas.circle (dotX, dotY, dotD);
            canvas.text ("Listen", boldFont (11.0f), visage::Font::kLeft, dotX + dotD + 4.0f, listenBadge_.y, listenBadge_.w - 24.0f, listenBadge_.h);
        }

        // close X
        {
            const Rect& c = closeBtn_;
            const float in = 4.0f;
            canvas.setColor (visage::Color (theme_.rs.textFaint));
            canvas.segment (c.x + in, c.y + in, c.x + c.w - in, c.y + c.h - in, 1.8f, true);
            canvas.segment (c.x + in, c.y + c.h - in, c.x + c.w - in, c.y + in, 1.8f, true);
        }

        // caption + choice buttons
        canvas.setColor (visage::Color (theme_.rs.typeCaption));
        canvas.text (isCut_ ? "SLOPE" : "TYPE", boldFont (10.0f), visage::Font::kLeft, captionRect_.x, captionRect_.y, captionRect_.w, captionRect_.h);

        const int sel = isCut_ ? model_.cutSlope (nodeId_) : model_.nodeType (nodeId_);
        const char* slopeLabels[] = { "6", "12", "24", "48" };
        for (int i = 0; i < choiceCount_; ++i)
        {
            const Rect& b = choiceBtns_[(std::size_t) i];
            const bool active = (i == sel);
            canvas.setColor (visage::Color (active ? theme_.base.palette.accent : theme_.rs.footerBg));
            canvas.roundedRectangle (b.x, b.y, b.w, b.h, 8.0f);
            if (! active)
            {
                canvas.setColor (visage::Color (theme_.base.palette.track));
                canvas.roundedRectangleBorder (b.x + 0.5f, b.y + 0.5f, b.w - 1.0f, b.h - 1.0f, 8.0f, 1.0f);
            }
            const std::uint32_t fg = active ? 0xffffffff : theme_.rs.iconInactive;
            if (isCut_)
            {
                canvas.setColor (visage::Color (fg));
                canvas.text (slopeLabels[i], boldFont (11.0f), visage::Font::kCenter, b.x, b.y, b.w, b.h);
            }
            else
            {
                const float boxW = 24.0f, boxH = 14.0f;
                const float ax = b.x + 6.0f, ay = b.y + 7.0f, aw = b.w - 12.0f, ah = b.h - 14.0f;
                const float sc = std::min (aw / boxW, ah / boxH);
                visage::Path g = eqGlyph (i).scaled (sc).translated (ax + aw * 0.5f - boxW * sc * 0.5f,
                                                                     ay + ah * 0.5f - boxH * sc * 0.5f);
                canvas.setColor (visage::Color (fg));
                canvas.fill (g.stroke (2.2f * sc, visage::Path::Join::Round, visage::Path::EndCap::Round));
            }
        }

        // mini-knobs
        drawMiniKnob (canvas, freqK_);
        drawMiniKnob (canvas, sensK_);
        drawMiniKnob (canvas, widthK_);
    }

    void RsNodePanel::writeKnob (MiniKnob& k, float norm)
    {
        norm = std::clamp (norm, 0.0f, 1.0f);
        model_.store().setFromUi (k.paramIndex, factory_params::convertFrom0to1 (k.range, norm));
        if (onNodeEdited) onNodeEdited (nodeId_);
        redraw();
    }

    // Value read-out sub-rect (frame-local): the bottom valueH band of the mini area
    // (matches drawMiniKnob's value text row).
    RsNodePanel::Rect RsNodePanel::miniValueRect (const MiniKnob& k) const
    {
        constexpr float valueH = 14.0f;
        return { k.area.x, k.area.y + k.area.h - valueH, k.area.w, valueH };
    }

    bool RsNodePanel::miniValueRectInWindow (int which, float& x, float& y, float& w, float& h) const
    {
        const MiniKnob* ks[] = { &freqK_, &sensK_, &widthK_ };
        if (which < 0 || which > 2 || ! ks[which]->visible) return false;
        const Rect r = miniValueRect (*ks[which]);
        const visage::Point o = positionInWindow();
        x = o.x + r.x; y = o.y + r.y; w = r.w; h = r.h;
        return true;
    }

    void RsNodePanel::openMiniEntry (MiniKnob& k)
    {
        if (! requestValueEntry) return;
        const Rect r = miniValueRect (k);
        const visage::Point o = positionInWindow();
        fuv::ValueEntryRequest req;
        req.x = o.x + r.x; req.y = o.y + r.y; req.w = r.w; req.h = r.h;
        req.prefill = fuv::stripLeadingNumber (valueText (k)); // "2.6kHz"->"2.6", "7.40 dB"->"7.40"
        req.fontPx  = 11.0f; // the mini value read-out font
        const int  pi     = k.paramIndex;
        const bool isFreq = k.freq;
        req.commit = [this, pi, isFreq] (const std::string& t) { commitMiniEntry (pi, isFreq, t); };
        requestValueEntry (req);
    }

    void RsNodePanel::commitMiniEntry (int paramIndex, bool isFreq, const std::string& text)
    {
        auto& store = model_.store();
        // FREQ routes through the Hz/kHz parser; both branches share the ValueText
        // revert-on-invalid contract ("abc" reverts — no gesture, no write).
        if (isFreq)
        {
            const factory_params::ParamDesc& desc = store.desc (paramIndex);
            float real = 0.0f;
            if (! parseFreqEntry (text, desc.minValue, desc.maxValue, real)) return;
            store.setFromUiGestured (paramIndex, real); // snapToLegalValue clamps + snaps
        }
        else if (! fuv::commitEntryText (store, paramIndex, text))
            return;
        if (onNodeEdited) onNodeEdited (nodeId_);
        if (onGestureEnd) onGestureEnd();
        redraw();
    }

    void RsNodePanel::mouseDown (const visage::MouseEvent& e)
    {
        const visage::Point pos = e.position;

        if (closeBtn_.contains (pos)) { if (onCloseRequested) onCloseRequested(); return; }

        if (onBadge_.contains (pos))
        {
            const bool turningOff = model_.nodeOn (nodeId_);
            model_.store().setFromUiGestured (model_.idxOn (nodeId_), turningOff ? 0.0f : 1.0f);
            if (turningOff && feed_.getListenNode() == nodeId_) { feed_.setListenNode (-1); refreshListen(); }
            if (onNodeEdited) onNodeEdited (nodeId_);
            if (onGestureEnd) onGestureEnd();
            redraw();
            return;
        }
        if (listenBadge_.contains (pos))
        {
            feed_.setListenNode (listenOn_ ? -1 : nodeId_);
            refreshListen();
            redraw();
            return;
        }
        for (int i = 0; i < choiceCount_; ++i)
            if (choiceBtns_[(std::size_t) i].contains (pos))
            {
                const int idx = isCut_ ? model_.idxSlope (nodeId_) : model_.idxType (nodeId_);
                model_.store().setFromUiGestured (idx, (float) i);
                if (onNodeEdited) onNodeEdited (nodeId_);
                if (onGestureEnd) onGestureEnd();
                redraw();
                return;
            }

        // mini-knobs
        MiniKnob* ks[] = { &freqK_, &sensK_, &widthK_ };
        for (int i = 0; i < 3; ++i)
            if (ks[i]->visible && ks[i]->area.contains (pos))
            {
                // Value read-out double-click opens the direct text entry (FREQ uses
                // the Hz/kHz parser); elsewhere in the mini it falls through to the
                // reset below — matching the footer knobs' value-row-edits behaviour.
                if (e.repeatClickCount() >= 2 && requestValueEntry && miniValueRect (*ks[i]).contains (pos))
                {
                    openMiniEntry (*ks[i]);
                    return;
                }
                // Alt-click (or double-click) resets this mini-knob to its parameter
                // default, matching the footer knobs (round-3 fix 5); no drag begins.
                if (e.isAltDown() || e.repeatClickCount() >= 2)
                {
                    const float def = model_.store().desc (ks[i]->paramIndex).defaultValue;
                    model_.store().setFromUiGestured (ks[i]->paramIndex, def);
                    if (onNodeEdited) onNodeEdited (nodeId_);
                    if (onGestureEnd) onGestureEnd();
                    redraw();
                    return;
                }
                dragKnob_ = i;
                dragNorm_ = factory_params::convertTo0to1 (ks[i]->range, model_.store().value (ks[i]->paramIndex));
                dragLast_ = pos;
                model_.store().beginGesture (ks[i]->paramIndex);
                return;
            }
    }

    void RsNodePanel::mouseDrag (const visage::MouseEvent& e)
    {
        if (dragKnob_ < 0) return;
        MiniKnob* ks[] = { &freqK_, &sensK_, &widthK_ };
        MiniKnob& k = *ks[dragKnob_];
        const float fine = e.isShiftDown() ? 0.25f : 1.0f;
        const float dy = dragLast_.y - e.position.y; // up = increase
        const float dx = e.position.x - dragLast_.x;
        dragLast_ = e.position;
        dragNorm_ = std::clamp (dragNorm_ + (dx + dy) / 200.0f * fine, 0.0f, 1.0f);
        writeKnob (k, dragNorm_);
    }

    void RsNodePanel::mouseUp (const visage::MouseEvent&)
    {
        if (dragKnob_ >= 0)
        {
            MiniKnob* ks[] = { &freqK_, &sensK_, &widthK_ };
            model_.store().endGesture (ks[dragKnob_]->paramIndex);
            dragKnob_ = -1;
            if (onGestureEnd) onGestureEnd();
        }
    }
}
