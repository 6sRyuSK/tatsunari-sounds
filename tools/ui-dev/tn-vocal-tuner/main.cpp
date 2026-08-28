#include "PfEditor.h"
#include "PfParams.h"
#include "PfPresets.h"
#include "HarnessPresetModel.h"
#include "PitchFixBridge.h"
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
        return { pitch_fix_presets::kExclude,
                 pitch_fix_presets::kExclude + pitch_fix_presets::kNumExclude };
    }
}

int runPitchFix()
{
    static visage::ApplicationWindow app;
    static factory_ui_visage::Theme theme = factory_ui_visage::Theme::defaults();
    static factory_params::ParamStore store (pitch_fix_params::buildPfParams());
    static SyntheticPfFeed feed;
    static HarnessPresetModel<pf_ui::PfPresetModel> presets (
        store, pitch_fix_presets::bank, presetExclusions());
    static const pf_ui::PfUiFeed feedView = feed.view();
    static pf_ui::PfEditor editor (theme, store, feedView, presets);

    app.addChild (editor);
    editor.setBounds (0.0f, 0.0f, (float) pf_ui::PfEditor::kDesignW, (float) pf_ui::PfEditor::kDesignH);
    editor.setFrameTick ([&] { feed.advance(); });
    pf_harness::setBridgeTarget (&editor, &feed, &theme);

    app.setTitle ("TN Vocal Tuner - Visage");
    app.show (pf_ui::PfEditor::kDesignW, pf_ui::PfEditor::kDesignH);
    app.runEventLoop();
    return 0;
}

#if defined(_WIN32)
 #include <windows.h>
int WINAPI WinMain (HINSTANCE, HINSTANCE, LPSTR, int) { return runPitchFix(); }
#else
int main (int, char**) { return runPitchFix(); }
#endif
