#include "DynamicEqBridge.h"
#include "PluginHarness.h"
#include "BridgeCommon.h"
#include "DeqEditor.h"
#include "SyntheticFeed.h"

#include <string>
#include <utility>

#ifdef __EMSCRIPTEN__
 #include <emscripten/emscripten.h>
 #define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
 #define KEEPALIVE
#endif

namespace
{
    deq_ui::DeqEditor* g_editor = nullptr;
    SyntheticDeqFeed* g_feed = nullptr;
    std::string g_plot;
}

namespace deq_harness
{
    void setBridgeTarget (deq_ui::DeqEditor* editor, SyntheticDeqFeed* feed,
                          factory_ui_visage::Theme* theme)
    {
        g_editor = editor;
        g_feed = feed;
        auto target = ui_dev_harness::makeEditorTarget (editor, theme);
        target.freeze = [editor, feed] (bool b) {
            if (feed) feed->setFrozen (b);
            if (editor)
            {
                if (b) editor->curve().refreshAnalyzer();
                editor->curve().setFrozen (b);
            }
        };
        target.feedSpectrum = [editor, feed] (double phase) {
            if (feed) feed->setPhase (phase);
            if (editor) editor->curve().refreshAnalyzer();
        };
        ui_dev_harness::attach (std::move (target));
    }
}

extern "C"
{
    KEEPALIVE void deq_select_band (int band) { if (g_editor) g_editor->selectBand (band); }
    KEEPALIVE int deq_selected_band() { return g_editor ? g_editor->selectedBand() : -1; }
    KEEPALIVE double deq_node_x (int band)
    {
        float x = 0, y = 0;
        return g_editor && g_editor->curve().nodeCentreInWindow (band, x, y) ? (double) x : -1.0;
    }
    KEEPALIVE double deq_node_y (int band)
    {
        float x = 0, y = 0;
        return g_editor && g_editor->curve().nodeCentreInWindow (band, x, y) ? (double) y : -1.0;
    }
    KEEPALIVE void deq_set_phase (double phase)
    {
        if (g_feed) g_feed->setPhase (phase);
        if (g_editor) g_editor->curve().refreshAnalyzer();
    }
    KEEPALIVE double deq_live_gain (int band) { return g_feed ? g_feed->liveGainDb (band) : 0.0; }
    KEEPALIVE const char* deq_plot_rect()
    {
        g_plot = "null";
        if (g_editor)
        {
            float x = 0, y = 0, w = 0, h = 0;
            if (g_editor->curve().plotRectInWindow (x, y, w, h))
                g_plot = ui_dev_bridge::jsonRect (x, y, w, h);
        }
        return g_plot.c_str();
    }
}
