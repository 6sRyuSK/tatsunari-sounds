#pragma once
//
// factory_ui_visage::UpdateBadge — small passive "UPDATE" pill. Visible only
// when the editor's UpdateCheck reports UpdateAvailable. Click opens the
// UpdateDialog (wired by the editor).
//
#include "factory_ui_visage/Theme.h"

#include <visage_ui/frame.h>

#include <functional>
#include <string>

namespace factory_ui_visage
{
    class UpdateBadge : public visage::Frame
    {
    public:
        explicit UpdateBadge (const Theme& theme);

        std::function<void()> onClick;

        void setLabel (const std::string& text);
        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseEnter (const visage::MouseEvent& e) override;
        void mouseExit (const visage::MouseEvent& e) override;

    private:
        const Theme& theme_;
        std::string label_ = "UPDATE";
        bool hover_ = false;
    };
}
