#pragma once
//
// factory_ui_visage::UpdateDialog — full-cover scrim modal for:
//   * first-run opt-in ("送信するデータはありません（単なる GET です）")
//   * update-available details + primary action
// Pattern mirrors Dropdown (scrim + Esc + outside click dismisses as decline).
//
#include "factory_ui_visage/Theme.h"

#include <visage_ui/frame.h>

#include <functional>
#include <string>
#include <vector>

namespace factory_ui_visage
{
    class UpdateDialog : public visage::Frame
    {
    public:
        enum class Mode { OptIn, UpdateAvailable };

        explicit UpdateDialog (const Theme& theme);

        void openOptIn();
        void openUpdate (const std::string& currentVersion,
                         const std::string& latestVersion,
                         const std::vector<std::string>& highlights,
                         bool installerPresent);
        void close();
        bool isOpen() const noexcept { return open_; }
        Mode mode() const noexcept { return mode_; }

        // Opt-in
        std::function<void()> onAcceptOptIn;
        std::function<void()> onDeclineOptIn; // also fired on dismiss

        // Update-available primary button
        std::function<void()> onPrimaryAction; // launch installer OR open download page
        std::function<void()> onDismiss;

        void draw (visage::Canvas& canvas) override;
        void mouseDown (const visage::MouseEvent& e) override;
        void mouseMove (const visage::MouseEvent& e) override;
        bool keyPress (const visage::KeyEvent& e) override;

    private:
        struct Button { float x = 0, y = 0, w = 0, h = 0; std::string label; bool primary = false; };

        void layoutButtons();
        int hitButton (float x, float y) const; // -1 none, 0 secondary, 1 primary

        const Theme& theme_;
        Mode mode_ = Mode::OptIn;
        bool open_ = false;
        bool installerPresent_ = false;
        std::string currentVersion_;
        std::string latestVersion_;
        std::vector<std::string> highlights_;
        std::string primaryLabel_;
        std::string secondaryLabel_;

        float panelX_ = 0, panelY_ = 0, panelW_ = 0, panelH_ = 0;
        Button primary_, secondary_;
        int hoverBtn_ = -1;
    };
}
