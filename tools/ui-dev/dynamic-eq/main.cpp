#include "DeqEditor.h"
#include "DeqParams.h"
#include "FactoryPresets.h"
#include "HarnessPresetModel.h"
#include "DynamicEqBridge.h"
#include "SyntheticFeed.h"

#include "factory_params/ParamStore.h"
#include "factory_ui_visage/Theme.h"

#include <visage/app.h>

#include <string>
#include <vector>

namespace
{
    std::vector<std::string> presetExclusions()
    {
        return { dynamic_eq_presets::kExclude,
                 dynamic_eq_presets::kExclude + dynamic_eq_presets::kNumExclude };
    }
}

int runDynamicEq()
{
    static visage::ApplicationWindow app;
    static factory_ui_visage::Theme theme = factory_ui_visage::Theme::defaults();
    static factory_params::ParamStore store (dynamic_eq_params::buildDeqParams());
    static SyntheticDeqFeed feed (store);
    static HarnessPresetModel<deq_ui::DeqPresetModel> presets (
        store, dynamic_eq_presets::bank, presetExclusions());
    static deq_ui::DeqEditor editor (theme, store, feed, presets);

    app.addChild (editor);
    editor.setBounds (0.0f, 0.0f, (float) deq_ui::DeqEditor::kDesignW, (float) deq_ui::DeqEditor::kDesignH);
    editor.setFrameTick ([&] { feed.advance(); });
    deq_harness::setBridgeTarget (&editor, &feed, &theme);

    app.setTitle ("Dynamic Tatsunari EQ - Visage");
    app.show (deq_ui::DeqEditor::kDesignW, deq_ui::DeqEditor::kDesignH);
    app.runEventLoop();
    return 0;
}

#if defined(_WIN32)
 #include <windows.h>
int WINAPI WinMain (HINSTANCE, HINSTANCE, LPSTR, int) { return runDynamicEq(); }
#else
int main (int, char**) { return runDynamicEq(); }
#endif
