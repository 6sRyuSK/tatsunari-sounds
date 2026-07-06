#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "factory_ui/FactoryChrome.h"
#include "factory_ui/FactoryLookAndFeel.h"

#include <array>
#include <atomic>
#include <cmath>

//
// TumbleVisualizer — the physics view. A message-thread mirror of the engine:
// each timer tick reads snapshotBalls / drainHits / boxAngle (all lock-free) and
// repaints. It NEVER writes physics state; the only writes back to the processor
// are pivotX / pivotY parameter edits from dragging the pivot handle. Paints
// exclusively with the factory_ui palette (slot accents = bandColour 0..3).
// Header-only, so the plugin's CMakeLists needs no change.
//
class TumbleVisualizer final : public juce::Component,
                               private juce::Timer
{
public:
    explicit TumbleVisualizer (TumbleDelayAudioProcessor& p)
        : processor (p)
    {
        auto& s = processor.apvts;
        shapeParam   = s.getRawParameterValue ("boxShape");
        pivotXParam  = s.getRawParameterValue ("pivotX");
        pivotYParam  = s.getRawParameterValue ("pivotY");
        boxSizeParam = s.getRawParameterValue ("boxSize");

        static constexpr const char* prefix[4] = { "a", "b", "c", "d" };
        for (int i = 0; i < 4; ++i)
        {
            const juce::String pre (prefix[i]);
            onParam[(size_t) i]       = s.getRawParameterValue (pre + "On");
            countParam[(size_t) i]    = s.getRawParameterValue (pre + "Count");
            timeParam[(size_t) i]     = s.getRawParameterValue (pre + "Time");
            ballSizeParam[(size_t) i] = s.getRawParameterValue (pre + "BallSize");
            speedParam[(size_t) i]    = s.getRawParameterValue (pre + "Speed");
        }

        pivotXHandle = s.getParameter ("pivotX");
        pivotYHandle = s.getParameter ("pivotY");

        lastTickMs = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (45);
    }

    ~TumbleVisualizer() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat();
        factory_ui::paintCard (g, bounds); // opaque fill clears the previous frame + frames the card

        // World -> screen: uniform scale, y flipped (world +y is up; gravity pulls
        // world -y, which must render downward). World span [-1.25,+1.25] fits the
        // shorter dimension.
        const Xform x { juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.5f,
                        bounds.getCentreX(), bounds.getCentreY() };

        const float px = pivotXParam->load();
        const float py = pivotYParam->load();
        const int   shapeIdx = (int) std::lround (shapeParam->load());
        const double theta = processor.boxAngle();
        const float ct = (float) std::cos (theta);
        const float st = (float) std::sin (theta);

        drawBox (g, shapeIdx, px, py, ct, st, x);

        for (int i = 0; i < ballCount; ++i)
            drawBall (g, ballBuf[(size_t) i], x);

        for (const auto& f : flashes)
            if (f.active) drawFlash (g, f, x);

        drawPivotHandle (g, x (px, py));
        drawHud (g, shapeIdx, bounds);
    }

    // ---- pivot drag: the ONLY UI -> physics path, and it goes through APVTS ----
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (pivotScreen().getDistanceFrom (e.position) <= 12.0f)
        {
            draggingPivot = true;
            if (pivotXHandle != nullptr) pivotXHandle->beginChangeGesture();
            if (pivotYHandle != nullptr) pivotYHandle->beginChangeGesture();
            writePivotFromMouse (e);
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (draggingPivot) writePivotFromMouse (e);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! draggingPivot) return;
        draggingPivot = false;
        if (pivotXHandle != nullptr) pivotXHandle->endChangeGesture();
        if (pivotYHandle != nullptr) pivotYHandle->endChangeGesture();
    }

private:
    // Small world->screen transform (uniform scale, y flipped).
    struct Xform
    {
        float scale = 1.0f, cx = 0.0f, cy = 0.0f;
        juce::Point<float> operator() (float wx, float wy) const noexcept
        {
            return { cx + wx * scale, cy - wy * scale };
        }
    };

    struct Flash { float x = 0, y = 0; int slot = 0; float intensity = 0; float age = 0; bool active = false; };
    static constexpr float kFlashLife = 0.45f;

    static int shapeN (int idx) noexcept // vertex count; 0 == circle
    {
        switch (idx)
        {
            case 0:  return 3;  // Triangle
            case 1:  return 4;  // Square
            case 2:  return 5;  // Pentagon
            case 3:  return 6;  // Hexagon
            case 4:  return 8;  // Octagon
            default: return 0;  // Circle
        }
    }

    void drawBox (juce::Graphics& g, int shapeIdx, float px, float py,
                  float ct, float st, const Xform& x) const
    {
        const juce::Colour wall = FactoryLookAndFeel::text();
        const juce::Colour fill = FactoryLookAndFeel::accentDim().withAlpha (0.14f);
        const int N = shapeN (shapeIdx);

        if (N == 0) // circle: center = pivot - Rot(pivot), radius 1
        {
            const float rpx = px * ct - py * st;
            const float rpy = px * st + py * ct;
            const auto c = x (px - rpx, py - rpy);
            const float rr = x.scale; // world radius 1
            g.setColour (fill);
            g.fillEllipse (c.x - rr, c.y - rr, rr * 2.0f, rr * 2.0f);
            g.setColour (wall);
            g.drawEllipse (c.x - rr, c.y - rr, rr * 2.0f, rr * 2.0f, 2.0f);
            return;
        }

        juce::Path path;
        for (int i = 0; i < N; ++i)
        {
            // Flat-bottom convention (matches the engine): base vertex i sits at
            // alpha = -pi/2 + pi/N + 2*pi*i/N; drawn vertex = pivot + Rot(v - pivot).
            const float a = juce::MathConstants<float>::twoPi * (float) i / (float) N
                            - juce::MathConstants<float>::halfPi
                            + juce::MathConstants<float>::pi / (float) N;
            const float bx = std::cos (a), by = std::sin (a);
            const float dx = bx - px, dy = by - py;
            const float rx = dx * ct - dy * st;
            const float ry = dx * st + dy * ct;
            const auto pt = x (px + rx, py + ry);
            if (i == 0) path.startNewSubPath (pt);
            else        path.lineTo (pt);
        }
        path.closeSubPath();
        g.setColour (fill);
        g.fillPath (path);
        g.setColour (wall);
        g.strokePath (path, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved,
                                                        juce::PathStrokeType::rounded));
    }

    void drawBall (juce::Graphics& g, const factory_core::TumbleDelay::BallView& b, const Xform& x) const
    {
        const auto p = x (b.x, b.y);
        const float rS = juce::jmax (2.0f, b.radius * x.scale);
        const auto col = FactoryLookAndFeel::bandColour (b.slot);
        const float e  = juce::jlimit (0.0f, 1.0f, b.energy);
        const float a  = 0.5f + 0.5f * e; // brightness follows kinetic energy

        g.setColour (col.withAlpha (0.16f * a)); // soft glow
        g.fillEllipse (p.x - rS * 1.9f, p.y - rS * 1.9f, rS * 3.8f, rS * 3.8f);

        g.setColour (col.withAlpha (a)); // body
        g.fillEllipse (p.x - rS, p.y - rS, rS * 2.0f, rS * 2.0f);

        if (b.generation > 0) // Refeed child generation -> bright ring
        {
            g.setColour (col.brighter (0.4f).withAlpha (0.9f));
            g.drawEllipse (p.x - rS, p.y - rS, rS * 2.0f, rS * 2.0f, juce::jmax (1.0f, rS * 0.28f));
        }

        const float hr = juce::jmax (1.0f, rS * 0.32f); // specular highlight (palette only)
        g.setColour (FactoryLookAndFeel::panel().withAlpha (0.55f * a));
        g.fillEllipse (p.x - rS * 0.3f - hr, p.y - rS * 0.3f - hr, hr * 2.0f, hr * 2.0f);
    }

    void drawFlash (juce::Graphics& g, const Flash& f, const Xform& x) const
    {
        const float t = juce::jlimit (0.0f, 1.0f, f.age / kFlashLife);
        const auto p = x (f.x, f.y);
        const auto col = FactoryLookAndFeel::bandColour (f.slot);
        const float inten = juce::jlimit (0.0f, 1.0f, f.intensity);

        const float rr = (0.04f + 0.32f * t) * x.scale * (0.6f + 0.9f * inten); // expanding ripple
        g.setColour (col.withAlpha ((1.0f - t) * 0.55f));
        g.drawEllipse (p.x - rr, p.y - rr, rr * 2.0f, rr * 2.0f, juce::jmax (1.0f, (1.0f - t) * 3.0f));

        const float cr = juce::jmax (1.5f, (0.05f * (1.0f - t) + 0.015f) * x.scale); // bright core
        g.setColour (col.brighter (0.5f).withAlpha ((1.0f - t) * 0.9f));
        g.fillEllipse (p.x - cr, p.y - cr, cr * 2.0f, cr * 2.0f);
    }

    void drawPivotHandle (juce::Graphics& g, juce::Point<float> p) const
    {
        const float hr = 6.0f;
        const auto col = FactoryLookAndFeel::accent();
        g.setColour (col.withAlpha (draggingPivot ? 0.35f : 0.18f));
        g.fillEllipse (p.x - hr, p.y - hr, hr * 2.0f, hr * 2.0f);
        g.setColour (col.withAlpha (draggingPivot ? 0.95f : 0.55f));
        g.drawLine (p.x - hr - 4.0f, p.y, p.x + hr + 4.0f, p.y, 1.2f);
        g.drawLine (p.x, p.y - hr - 4.0f, p.x, p.y + hr + 4.0f, 1.2f);
        g.drawEllipse (p.x - hr, p.y - hr, hr * 2.0f, hr * 2.0f, 2.0f);
    }

    void drawHud (juce::Graphics& g, int shapeIdx, juce::Rectangle<float> bounds) const
    {
        static constexpr const char* slotName[4] = { "A", "B", "C", "D" };
        const juce::String times (juce::CharPointer_UTF8 ("\xc3\x97")); // multiplication sign

        const int   N       = shapeN (shapeIdx);
        const float apothem = (N == 0) ? 1.0f : std::cos (juce::MathConstants<float>::pi / (float) N);
        const float boxSec  = juce::jmax (0.001f, boxSizeParam->load());

        g.setFont (juce::Font (juce::FontOptions (12.0f)));

        const float tx = bounds.getX() + 12.0f;
        const float lineH = 16.0f;
        float ty = bounds.getY() + 10.0f;

        for (int s = 0; s < 4; ++s)
        {
            if (onParam[(size_t) s]->load() < 0.5f) continue;

            const int   count  = (int) std::lround (countParam[(size_t) s]->load());
            const float timeMs = timeParam[(size_t) s]->load();
            const float ballSz = ballSizeParam[(size_t) s]->load() * 0.01f; // % -> fraction of R
            const float speed  = juce::jmax (0.001f, speedParam[(size_t) s]->load());
            const float vRef   = 2.0f / boxSec;                             // 2R / boxSize, R = 1
            const float flight = juce::jmax (0.0f, apothem - ballSz) / (speed * vRef); // seconds

            int alive = 0;
            for (int i = 0; i < ballCount; ++i)
                if (ballBuf[(size_t) i].slot == s) ++alive;

            g.setColour (FactoryLookAndFeel::bandColour (s));
            g.fillEllipse (tx, ty + 3.5f, 7.0f, 7.0f);

            const juce::String line =
                juce::String (slotName[s]) + "  " + juce::String (juce::roundToInt (timeMs)) + "ms " + times
                + juce::String (count) + "   ~" + juce::String (juce::roundToInt (flight * 1000.0f)) + "ms"
                + "   live " + juce::String (alive);

            g.setColour (FactoryLookAndFeel::textDim());
            g.drawText (line, juce::Rectangle<float> (tx + 13.0f, ty, bounds.getWidth() - 26.0f, lineH),
                        juce::Justification::centredLeft);
            ty += lineH;
        }
    }

    void timerCallback() override
    {
        ballCount = processor.snapshotBalls (ballBuf.data(), (int) ballBuf.size());

        // Drain every pending hit into the flash pool (loop: more may have queued
        // than one drain returns).
        factory_core::TumbleDelay::HitEvent hb[64];
        for (;;)
        {
            const int n = processor.drainHits (hb, (int) (sizeof (hb) / sizeof (hb[0])));
            for (int i = 0; i < n; ++i) spawnFlash (hb[i]);
            if (n < (int) (sizeof (hb) / sizeof (hb[0]))) break;
        }

        const double now = juce::Time::getMillisecondCounterHiRes();
        const float dt = (float) juce::jlimit (0.0, 0.25, (now - lastTickMs) * 0.001);
        lastTickMs = now;
        for (auto& f : flashes)
            if (f.active && (f.age += dt) >= kFlashLife)
                f.active = false;

        repaint();
    }

    void spawnFlash (const factory_core::TumbleDelay::HitEvent& h) noexcept
    {
        int idx = -1; float oldest = -1.0f;
        for (int i = 0; i < (int) flashes.size(); ++i)
        {
            if (! flashes[(size_t) i].active) { idx = i; break; }
            if (flashes[(size_t) i].age > oldest) { oldest = flashes[(size_t) i].age; idx = i; }
        }
        if (idx < 0) return;
        auto& f = flashes[(size_t) idx];
        f = { h.x, h.y, h.slot, h.intensity, 0.0f, true };
    }

    juce::Point<float> pivotScreen() const
    {
        const auto b = getLocalBounds().toFloat();
        const float scale = juce::jmin (b.getWidth(), b.getHeight()) / 2.5f;
        return { b.getCentreX() + pivotXParam->load() * scale,
                 b.getCentreY() - pivotYParam->load() * scale };
    }

    void writePivotFromMouse (const juce::MouseEvent& e)
    {
        const auto b = getLocalBounds().toFloat();
        const float scale = juce::jmax (1.0f, juce::jmin (b.getWidth(), b.getHeight()) / 2.5f);
        const float wx = juce::jlimit (-1.0f, 1.0f, (e.position.x - b.getCentreX()) / scale);
        const float wy = juce::jlimit (-1.0f, 1.0f, -(e.position.y - b.getCentreY()) / scale);
        if (pivotXHandle != nullptr) pivotXHandle->setValueNotifyingHost (pivotXHandle->convertTo0to1 (wx));
        if (pivotYHandle != nullptr) pivotYHandle->setValueNotifyingHost (pivotYHandle->convertTo0to1 (wy));
    }

    TumbleDelayAudioProcessor& processor;

    // Cached read-only parameter atomics (message-thread reads only).
    std::atomic<float>* shapeParam   = nullptr;
    std::atomic<float>* pivotXParam  = nullptr;
    std::atomic<float>* pivotYParam  = nullptr;
    std::atomic<float>* boxSizeParam = nullptr;
    std::array<std::atomic<float>*, 4> onParam {}, countParam {}, timeParam {}, ballSizeParam {}, speedParam {};

    // Pivot edit handles (the only writable path, via APVTS gestures).
    juce::RangedAudioParameter* pivotXHandle = nullptr;
    juce::RangedAudioParameter* pivotYHandle = nullptr;

    std::array<factory_core::TumbleDelay::BallView, factory_core::TumbleDelay::kMaxBalls> ballBuf {};
    int ballCount = 0;
    std::array<Flash, 64> flashes {};

    double lastTickMs = 0.0;
    bool draggingPivot = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TumbleVisualizer)
};
