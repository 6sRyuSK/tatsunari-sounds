#pragma once
//
// plugins/dynamic-eq/ui/DeqCurveView.h — deq_ui::DeqCurveView, the Pro-Q-style EQ
// centrepiece, ported from the JUCE EqCurveComponent onto a visage::Frame. It draws a
// spectrum analyser (pre/post) behind the combined EQ response curve, with one
// draggable, per-band-coloured node per active band (x = frequency, y = gain). Each
// active band draws its own pastel filled response; dynamic bands "breathe" by following
// the core's published live gain.
//
// Interactions (mirroring the JUCE component): click a node to select it (opens the band
// panel via onSelectBand) and drag it — x = freq, y = gain (bells/shelves). Double-click
// empty space adds a band (type follows the horizontal position); double-click a node
// removes it. The wheel over a node edits its Q. Every edit goes through the ParamStore
// gesture path. GUI-thread only; the DeqFeed atomics are the audio-thread hand-off.
//
#include "DeqModels.h"

#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/SpectrumModel.h"
#include "factory_params/ParamStore.h"

#include "factory_core/DynamicEqBand.h"

#include <visage_ui/frame.h>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace deq_ui
{
    class DeqCurveView : public visage::Frame
    {
    public:
        static constexpr int kNumBands = 24;

        DeqCurveView (const factory_ui_visage::Theme& theme, factory_params::ParamStore& store, DeqFeed& feed);

        // Band selected on the curve (-1 = none). The editor positions the band panel.
        std::function<void (int)> onSelectBand;
        // A band's parameters changed via a curve gesture (drag/wheel/double-click) — lets
        // the editor refresh the open band panel.
        std::function<void (int)> onBandEdited;
        // Once per animated frame (the host flush hook rides here).
        std::function<void()> onTick;

        void setSelectedBand (int b);
        int  selectedBand() const noexcept { return selected_; }
        void setFrozen (bool frozen);

        void draw (visage::Canvas& canvas) override;
        void resized() override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseDrag (const visage::MouseEvent& e) override;
        void mouseUp (const visage::MouseEvent& e) override;
        void mouseMove (const visage::MouseEvent& e) override;
        void mouseExit (const visage::MouseEvent& e) override;
        bool mouseWheel (const visage::MouseEvent& e) override;

    private:
        struct Rect { float x = 0, y = 0, w = 0, h = 0; };
        struct BandIx { int on, byp, lsn, chan, type, freq, gain, q, slope, dyn; };

        static constexpr float kMaxGain = 24.0f;
        static constexpr float kMinDb = -100.0f, kMaxDb = 0.0f; // analyser dB axis

        // --- axes ---
        float freqToX (float f) const;
        float xToFreq (float x) const;
        float gainToY (float gdb) const;
        float yToGain (float y) const;

        // --- param reads (by cached index) ---
        bool  bandOn (int b) const        { return store_.value (bx_[(size_t) b].on)  > 0.5f; }
        bool  bandBypassed (int b) const  { return store_.value (bx_[(size_t) b].byp) > 0.5f; }
        bool  bandListening (int b) const { return store_.value (bx_[(size_t) b].lsn) > 0.5f; }
        bool  bandDynamic (int b) const   { return store_.value (bx_[(size_t) b].dyn) > 0.5f; }
        int   bandTypeInt (int b) const   { return (int) store_.value (bx_[(size_t) b].type); }
        int   bandChannel (int b) const   { return (int) store_.value (bx_[(size_t) b].chan); }
        bool  isCutType (int b) const;
        int   bandStages (int b) const;
        float bandGainDb (int b) const;   // dynamic bands follow the live gain
        visage::Point nodePos (int b) const;

        // --- drawing layers ---
        void computeLayout();
        void drawGrid (visage::Canvas&);
        void drawSpectrum (visage::Canvas&, int channel, bool post, std::uint32_t colour);
        void drawBandsAndResponse (visage::Canvas&);
        void drawNodes (visage::Canvas&);
        void drawModeChip (visage::Canvas&);

        // --- interaction helpers ---
        int  nodeAt (visage::Point pos) const;
        int  firstFreeBand() const;
        int  typeForFraction (float frac) const;
        int  modeSegAt (visage::Point pos) const; // 0=Pre,1=Post,-1=outside
        void beginNodeGesture (int b);
        void endNodeGesture (int b);
        void setParamUi (int paramIndex, float value);         // held-drag write
        void setParamGestured (int paramIndex, float value);   // one-shot
        std::uint32_t bandColour (int b) const;

        const factory_ui_visage::Theme& theme_;
        factory_params::ParamStore&     store_;
        DeqFeed&                        feed_;

        std::array<BandIx, (size_t) kNumBands> bx_ {};

        // layout (frame-local)
        Rect plot_, preChip_, postChip_;

        int  selected_ = 0;
        int  dragging_ = -1;
        int  hover_    = -1;
        bool showPre_  = true;
        bool showPost_ = false;
        bool frozen_   = false;

        // Pre/Post spectra: SpectrumModel is single-channel, so two instances (channel 0
        // = pre, 1 = post). Rate-adaptive order (fixed order forbidden by CLAUDE.md).
        factory_ui_visage::SpectrumModel modelPre_, modelPost_;
        std::vector<float> scratch_; // FFT input scratch, refilled from the feed each frame
        double lastSr_ = 0.0;
    };
} // namespace deq_ui
