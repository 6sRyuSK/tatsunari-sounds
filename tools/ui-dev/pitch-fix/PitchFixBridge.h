#pragma once

namespace factory_ui_visage { struct Theme; }
namespace pf_ui { class PfEditor; }
class SyntheticPfFeed;

namespace pf_harness
{
    void setBridgeTarget (pf_ui::PfEditor* editor, SyntheticPfFeed* feed,
                          factory_ui_visage::Theme* theme);
}
