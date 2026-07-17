#include "RsEditor.h"
#include "RsTheme.h"
#include "SyntheticFeed.h"
#include "Mocks.h"
#include "RsBridge.h"

#include "factory_params/ParamStore.h"
#include "Params.h" // resonance_suppressor_params::buildRsParams — the REAL 64-param table (JUCE-free)

#include <visage/app.h>

//
// RS-editor harness entry point. Like the gallery, runEventLoop() unwinds the C++
// stack under Emscripten, so the app + editor + store + feed + mocks must outlive
// this call — hence static storage. The synthetic feed is advanced once per frame
// through the curve's onTick (SpectrumView pattern); freezing stops the loop.
//
// The canvas is fixed at the MAX resize size so rs_set_size (which resizes only
// the editor frame) can render min/default/max layouts into the top-left sub-rect
// without needing an unavailable native-window resize; the editor starts at the
// design size (1069x747) and the driver clips screenshots to the editor rect.
namespace { constexpr int kCanvasW = 1320, kCanvasH = 922, kDesignW = 1069, kDesignH = 747; }

int runRsEditor()
{
    static visage::ApplicationWindow app;
    static factory_params::ParamStore store (resonance_suppressor_params::buildRsParams());
    static SyntheticRsFeed feed (store);
    static rs_harness::MockPresetModel presets (store);
    static rs_harness::MockAbModel ab (store);
    static rs_ui::RsTheme theme = rs_ui::RsTheme::defaults();
    static rs_ui::RsEditor editor (theme, store, feed, presets, ab);

    app.addChild (editor);
    editor.setBounds (0.0f, 0.0f, (float) kDesignW, (float) kDesignH);
    // The analyser's per-frame tick advances the synthetic feed AND pumps the
    // editor's gesture queue (drains gesture-ends into undo). With the editor no
    // longer self-redrawing every frame (dirty-region, A3), this curve tick is the
    // live animation + undo-capture loop; only the analyser region repaints.
    editor.curve().onTick = [&] { editor.pumpGestures(); feed.advance(); };
    rs_harness::setBridgeTarget (&editor, &feed, &app);

    app.setTitle ("Resonance TatSuppressor · Visage");
    app.show (kCanvasW, kCanvasH);
    app.runEventLoop();
    return 0;
}

#if defined(_WIN32)
 #include <windows.h>
int WINAPI WinMain (HINSTANCE, HINSTANCE, LPSTR, int) { return runRsEditor(); }
#else
int main (int, char**) { return runRsEditor(); }
#endif
