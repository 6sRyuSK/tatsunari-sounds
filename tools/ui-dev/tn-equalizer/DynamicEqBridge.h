#pragma once

namespace factory_ui_visage { struct Theme; }
namespace deq_ui { class DeqEditor; }
class SyntheticDeqFeed;

namespace deq_harness
{
    void setBridgeTarget (deq_ui::DeqEditor* editor, SyntheticDeqFeed* feed,
                          factory_ui_visage::Theme* theme);
}
