#pragma once

namespace rs_ui { class RsEditor; }
class SyntheticRsFeed;
namespace visage { class ApplicationWindow; }

//
// The JS <-> WASM bridge for the RS-editor harness app needs a live editor + the
// synthetic feed to act on (and the app window, to resize the canvas for the
// min/max layout screenshots). rs-editor/main.cpp calls this once after the editor
// is added to the window.
//
namespace rs_harness
{
    void setBridgeTarget (rs_ui::RsEditor* editor, SyntheticRsFeed* feed, visage::ApplicationWindow* app);
}
