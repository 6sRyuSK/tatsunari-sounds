#include "PitchFixBridge.h"
#include "PluginHarness.h"
#include "PfEditor.h"
#include "SyntheticFeed.h"

#include <utility>

#ifdef __EMSCRIPTEN__
 #include <emscripten/emscripten.h>
 #define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
 #define KEEPALIVE
#endif

namespace
{
    pf_ui::PfEditor* g_editor = nullptr;
    SyntheticPfFeed* g_feed = nullptr;
}

namespace pf_harness
{
    void setBridgeTarget (pf_ui::PfEditor* editor, SyntheticPfFeed* feed,
                          factory_ui_visage::Theme* theme)
    {
        g_editor = editor;
        g_feed = feed;
        auto target = ui_dev_harness::makeEditorTarget (editor, theme);
        target.freeze = [editor, feed] (bool b) {
            if (feed) feed->setFrozen (b);
            if (editor) editor->redrawAll();
        };
        target.feedSpectrum = [editor, feed] (double phase) {
            if (feed) feed->setPhase (phase);
            if (editor) editor->redrawAll();
        };
        ui_dev_harness::attach (std::move (target));
    }
}

extern "C"
{
    KEEPALIVE void pf_set_feed (double detected, double target, double shift, int latency, double sampleRate)
    {
        if (g_feed) g_feed->setValues ((float) detected, (float) target, (float) shift,
                                       latency, (float) sampleRate);
        if (g_editor) g_editor->redrawAll();
    }
    KEEPALIVE double pf_detected() { return g_feed ? g_feed->detected() : 0.0; }
    KEEPALIVE double pf_target() { return g_feed ? g_feed->target() : 0.0; }
    KEEPALIVE double pf_shift() { return g_feed ? g_feed->shift() : 0.0; }
    KEEPALIVE int pf_latency() { return g_feed ? g_feed->latency() : 0; }
}
