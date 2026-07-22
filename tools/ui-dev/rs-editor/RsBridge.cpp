#include "RsBridge.h"
#include "BridgeCommon.h" // jsonEscape / jsonRect / paramsListJson / … (shared with the gallery)

#include "RsEditor.h"
#include "SyntheticFeed.h"
#include "RsProfileModel.h"

#include "factory_params/ParamDesc.h"
#include "factory_core/ReductionProfile.h"
#include "factory_ui_visage/Fonts.h"

#include <visage/app.h>

#include <sstream>
#include <string>

#ifdef __EMSCRIPTEN__
 #include <emscripten/emscripten.h>
 #define KEEPALIVE EMSCRIPTEN_KEEPALIVE
#else
 #define KEEPALIVE
#endif

//
// RsBridge — the thin C surface the page JS / Playwright driver calls via
// Module.ccall for the RS editor. The core ui_* functions mirror the gallery's
// bridge (so the shared harness.js window.ui wrappers work unchanged); the rs_*
// functions add the RS-specific driver hooks (node select/positions, A-B, presets,
// undo, a deterministic UI-edit + clock for undo tests). Returned strings live in
// file-static buffers (copied out by ccall).
//
namespace
{
    rs_ui::RsEditor* g_editor = nullptr;
    SyntheticRsFeed* g_feed = nullptr;
    visage::ApplicationWindow* g_app = nullptr;
    std::string g_list, g_error, g_rect, g_plot, g_mini, g_dial, g_ventry, g_mvrect;
}

namespace rs_harness
{
    void setBridgeTarget (rs_ui::RsEditor* editor, SyntheticRsFeed* feed, visage::ApplicationWindow* app)
    { g_editor = editor; g_feed = feed; g_app = app; }
}

extern "C"
{
    // ---- core (window.ui) ------------------------------------------------------
    KEEPALIVE const char* ui_list_params()
    {
        g_list = g_editor != nullptr ? ui_dev_bridge::paramsListJson (g_editor->store()) : "[]";
        return g_list.c_str();
    }

    KEEPALIVE double ui_get_param (const char* id)
    {
        if (g_editor == nullptr || id == nullptr) return 0.0;
        const int i = g_editor->store().indexOf (id);
        return i < 0 ? 0.0 : (double) g_editor->store().value (i);
    }

    KEEPALIVE void ui_set_param (const char* id, double real)
    {
        if (g_editor == nullptr || id == nullptr) return;
        const int i = g_editor->store().indexOf (id);
        if (i < 0) return;
        g_editor->store().setFromHost (i, (float) real);
        g_editor->redrawAll();
    }

    KEEPALIVE void ui_freeze (int frozen)
    {
        if (g_feed) g_feed->setFrozen (frozen != 0);
        if (g_editor) g_editor->setFrozen (frozen != 0);
    }

    KEEPALIVE int ui_reload_theme (const char* jsonText)
    {
        return ui_dev_bridge::reloadThemeInto (g_editor, jsonText, g_error);
    }

    KEEPALIVE const char* ui_last_error() { return g_error.c_str(); }
    KEEPALIVE unsigned int ui_get_accent() { return g_editor ? g_editor->theme().palette.accent : 0u; }

    KEEPALIVE const char* ui_widget_rect (const char* key)
    {
        g_rect = "null";
        if (g_editor != nullptr && key != nullptr)
        {
            float x = 0, y = 0, w = 0, h = 0;
            if (g_editor->widgetRectInWindow (key, x, y, w, h))
                g_rect = ui_dev_bridge::jsonRect (x, y, w, h);
        }
        return g_rect.c_str();
    }

    KEEPALIVE int ui_set_font (const char* name)
    {
        if (! factory_ui_visage::setFontFamilyByName (name)) return 0;
        if (g_editor) g_editor->redrawAll();
        return 1;
    }
    KEEPALIVE const char* ui_font() { return factory_ui_visage::fontFamilyName(); }

    // ---- RS-specific (window.rs) ----------------------------------------------
    KEEPALIVE void   rs_select_node (int id) { if (g_editor) g_editor->openNode (id); }
    KEEPALIVE int    rs_selected_node()      { return g_editor ? g_editor->selectedNode() : -1; }

    KEEPALIVE double rs_node_x (int id)
    {
        float x = 0, y = 0;
        return (g_editor && g_editor->nodeCentreInWindow (id, x, y)) ? (double) x : -1.0;
    }
    KEEPALIVE double rs_node_y (int id)
    {
        float x = 0, y = 0;
        return (g_editor && g_editor->nodeCentreInWindow (id, x, y)) ? (double) y : -1.0;
    }

    KEEPALIVE int  rs_listen_node()  { return g_feed ? g_feed->getListenNode() : -1; }
    KEEPALIVE double rs_display_smooth() { return g_feed ? (double) g_feed->displaySmoothMs() : 0.0; }

    KEEPALIVE int  rs_ab_slot()      { return g_editor ? g_editor->abSlot() : 0; }
    KEEPALIVE void rs_set_ab (int s) { if (g_editor) g_editor->setAbSlot (s); }
    KEEPALIVE void rs_copy_ab()      { if (g_editor) g_editor->copyAb(); }

    KEEPALIVE int  rs_preset_index() { return g_editor ? g_editor->presetIndex() : -1; }
    KEEPALIVE void rs_preset_load (int i) { if (g_editor) g_editor->loadPreset (i); }

    // Simulate a UI gesture (begin/setFromUi/end) on a param, then pump the gesture
    // queue so the undo timeline captures it — deterministic, no fussy drag needed.
    KEEPALIVE void rs_ui_edit (const char* id, double real)
    {
        if (g_editor == nullptr || id == nullptr) return;
        const int i = g_editor->store().indexOf (id);
        if (i < 0) return;
        g_editor->store().setFromUiGestured (i, (float) real);
        g_editor->pumpGestures();
        g_editor->redrawAll();
    }

    KEEPALIVE void rs_undo() { if (g_editor) g_editor->doUndo(); }
    KEEPALIVE void rs_redo() { if (g_editor) g_editor->doRedo(); }
    KEEPALIVE int  rs_can_undo() { return (g_editor && g_editor->canUndo()) ? 1 : 0; }
    KEEPALIVE int  rs_can_redo() { return (g_editor && g_editor->canRedo()) ? 1 : 0; }
    KEEPALIVE void rs_set_clock (double s) { if (g_editor) g_editor->setClockOverride (s); }
    KEEPALIVE void rs_pump() { if (g_editor) g_editor->pumpGestures(); }

    KEEPALIVE int  rs_open_dropdown (int which) { return (g_editor && g_editor->openNamedDropdown (which)) ? 1 : 0; }
    KEEPALIVE int  rs_dropdown_open()  { return (g_editor && g_editor->dropdown() && g_editor->dropdown()->isOpen()) ? 1 : 0; }
    KEEPALIVE int  rs_dropdown_count() { return (g_editor && g_editor->dropdown()) ? g_editor->dropdown()->itemCount() : 0; }
    KEEPALIVE double rs_dropdown_x (int i)
    {
        float x = 0, y = 0;
        return (g_editor && g_editor->dropdown() && g_editor->dropdown()->rowCentreInWindow (i, x, y)) ? (double) x : -1.0;
    }
    KEEPALIVE double rs_dropdown_row_y (int i)
    {
        float x = 0, y = 0;
        return (g_editor && g_editor->dropdown() && g_editor->dropdown()->rowCentreInWindow (i, x, y)) ? (double) y : -1.0;
    }

    // Re-render the editor at window size (w,h) — the min/max layout screenshots.
    // The editor ALWAYS lays out at the fixed 1069x747 design; the whole thing is
    // uniform-zoomed, exactly like the CLAP shell's window setDpiScale, except here
    // the canvas/window is fixed at the max size (see main.cpp) and shared with the
    // driver's coordinate mapping — so we zoom the EDITOR FRAME (setDpiScale = h/747)
    // instead of the window, keeping window/mouse coords stable while the editor
    // renders design-scaled into its top-left (w x h) native sub-rect. The driver
    // clips the screenshot to (w,h). At the design size (1069x747) dpi == 1, so the
    // clicking tests (which run there) see window px == logical px unchanged.
    KEEPALIVE void rs_set_size (int w, int h)
    {
        (void) g_app; (void) w;
        if (! g_editor) return;
        const float dpi = (h > 0) ? (float) h / rs_ui::RsEditor::kDesignH : 1.0f;
        g_editor->setDpiScale (dpi);
        g_editor->setBounds (0.0f, 0.0f, rs_ui::RsEditor::kDesignW, rs_ui::RsEditor::kDesignH);
    }

    // Combined reduction-profile deviation (dB) at a frequency, evaluated over the
    // live node set with the SAME factory_core::reductionProfileDbAt the editor's
    // curve + the audio path use. The driver maps this through the plot's sens axis
    // to assert a band's node handle sits ON the drawn combined curve.
    KEEPALIVE double rs_profile_db_at (double freqHz)
    {
        if (g_editor == nullptr) return 0.0;
        rs_ui::RsProfileModel model (g_editor->store());
        return (double) factory_core::reductionProfileDbAt (freqHz, model.buildNodes());
    }

    // Needle centre + tip (window px) of the open node panel's mini-knob
    // (0=FREQ, 1=SENS, 2=WIDTH). The driver derives the needle-tip ANGLE from
    // this and checks it against an independent oracle (A2 needle-angle assert).
    KEEPALIVE const char* rs_mini_knob_tip (int which)
    {
        g_mini = "null";
        if (g_editor != nullptr)
        {
            float cx = 0, cy = 0, tx = 0, ty = 0;
            if (g_editor->miniKnobTipInWindow (which, cx, cy, tx, ty))
                g_mini = ui_dev_bridge::jsonNumObj ({ { "cx", cx }, { "cy", cy }, { "tx", tx }, { "ty", ty } });
        }
        return g_mini.c_str();
    }

    // Mini-knob value-ring geometry (window px): centre + centreline radius. The
    // driver samples ring pixels at the needle angle to assert the accent arc END
    // lines up with the needle — catching an arc-vs-needle divergence (fix 6).
    KEEPALIVE const char* rs_mini_knob_dial (int which)
    {
        g_dial = "null";
        if (g_editor != nullptr)
        {
            float cx = 0, cy = 0, arcR = 0;
            if (g_editor->miniKnobDialInWindow (which, cx, cy, arcR))
                g_dial = ui_dev_bridge::jsonNumObj ({ { "cx", cx }, { "cy", cy }, { "arcR", arcR } });
        }
        return g_dial.c_str();
    }

    // Active Pre/Post/Both mode as a visual segment index (0=Pre,1=Post,2=Both) —
    // the driver clicks a segment and asserts this equals its index (fix 8).
    KEEPALIVE int rs_analyzer_mode_segment() { return g_editor ? g_editor->analyzerModeSegment() : 2; }

    // Direct text-entry overlay (P3c port): is it open, its live text, and a node-
    // panel mini's value read-out rect (window px) to double-click open it.
    KEEPALIVE int rs_value_entry_open() { return (g_editor && g_editor->valueEntryOpen()) ? 1 : 0; }
    KEEPALIVE const char* rs_value_entry_text()
    {
        g_ventry = g_editor ? g_editor->valueEntryText() : std::string();
        return g_ventry.c_str();
    }
    KEEPALIVE const char* rs_mini_value_rect (int which)
    {
        g_mvrect = "null";
        if (g_editor != nullptr)
        {
            float x = 0, y = 0, w = 0, h = 0;
            if (g_editor->miniValueRectInWindow (which, x, y, w, h))
                g_mvrect = ui_dev_bridge::jsonRect (x, y, w, h);
        }
        return g_mvrect.c_str();
    }

    KEEPALIVE const char* rs_plot_rect()
    {
        g_plot = "null";
        if (g_editor != nullptr)
        {
            float x = 0, y = 0, w = 0, h = 0;
            if (g_editor->plotRectInWindow (x, y, w, h))
                g_plot = ui_dev_bridge::jsonRect (x, y, w, h);
        }
        return g_plot.c_str();
    }
}
