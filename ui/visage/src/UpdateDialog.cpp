#include "factory_ui_visage/UpdateDialog.h"
#include "factory_ui_visage/Fonts.h"
#include "factory_ui_visage/Chrome.h"

#include <algorithm>

namespace factory_ui_visage
{
    namespace
    {
        constexpr float kPanelW = 420.0f;
        constexpr float kBtnH = 32.0f;
        constexpr float kBtnGap = 10.0f;
    }

    UpdateDialog::UpdateDialog (const Theme& theme) : theme_ (theme)
    {
        setVisible (false);
        setAcceptsKeystrokes (true);
    }

    void UpdateDialog::openOptIn()
    {
        mode_ = Mode::OptIn;
        primaryLabel_ = "Enable";
        secondaryLabel_ = "Not now";
        highlights_.clear();
        open_ = true;
        setVisible (true);
        setOnTop (true);
        layoutButtons();
        requestKeyboardFocus();
        redraw();
    }

    void UpdateDialog::openUpdate (const std::string& currentVersion,
                                   const std::string& latestVersion,
                                   const std::vector<std::string>& highlights,
                                   bool installerPresent)
    {
        mode_ = Mode::UpdateAvailable;
        currentVersion_ = currentVersion;
        latestVersion_ = latestVersion;
        highlights_ = highlights;
        installerPresent_ = installerPresent;
        primaryLabel_ = installerPresent ? "Update" : "Get installer";
        secondaryLabel_ = "Close";
        open_ = true;
        setVisible (true);
        setOnTop (true);
        layoutButtons();
        requestKeyboardFocus();
        redraw();
    }

    void UpdateDialog::close()
    {
        if (! open_)
            return;
        open_ = false;
        setVisible (false);
        redraw();
    }

    void UpdateDialog::layoutButtons()
    {
        panelW_ = kPanelW;
        panelH_ = (mode_ == Mode::OptIn) ? 220.0f : 280.0f;
        if (mode_ == Mode::UpdateAvailable)
            panelH_ += (float) std::min<std::size_t> (highlights_.size(), 4) * 18.0f;
        panelX_ = std::max (0.0f, (width() - panelW_) * 0.5f);
        panelY_ = std::max (0.0f, (height() - panelH_) * 0.5f);

        const float btnY = panelY_ + panelH_ - 24.0f - kBtnH;
        const float btnW = 140.0f;
        secondary_ = { panelX_ + panelW_ - 24.0f - btnW * 2 - kBtnGap, btnY, btnW, kBtnH,
                       secondaryLabel_, false };
        primary_   = { panelX_ + panelW_ - 24.0f - btnW, btnY, btnW, kBtnH,
                       primaryLabel_, true };
    }

    int UpdateDialog::hitButton (float x, float y) const
    {
        auto hit = [&] (const Button& b)
        {
            return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h;
        };
        if (hit (primary_)) return 1;
        if (hit (secondary_)) return 0;
        return -1;
    }

    void UpdateDialog::draw (visage::Canvas& canvas)
    {
        if (! open_)
            return;
        // Dim scrim
        canvas.setColor (visage::Color (0x66000000u));
        canvas.roundedRectangle (0.0f, 0.0f, width(), height(), 0.0f);

        layoutButtons();
        paintCardShell (canvas, panelX_, panelY_, panelW_, panelH_,
                        theme_.card.cornerRadius,
                        visage::Color (theme_.palette.panel),
                        visage::Color (theme_.palette.track));

        const float pad = 24.0f;
        float y = panelY_ + pad;
        canvas.setColor (theme_.palette.text);
        if (mode_ == Mode::OptIn)
        {
            canvas.text ("Update checks", boldFont (theme_.font.title),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 28.0f);
            y += 36.0f;
            canvas.setColor (theme_.palette.textSecondary);
            canvas.text ("May we check for plugin updates once a day?",
                         regularFont (theme_.font.label),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 22.0f);
            y += 28.0f;
            canvas.text ("No data is sent — this is a simple GET request.",
                         regularFont (theme_.font.caption),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 20.0f);
            y += 22.0f;
            canvas.text ("送信するデータはありません（単なる GET です）。",
                         regularFont (theme_.font.caption),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 20.0f);
        }
        else
        {
            canvas.text ("Update available", boldFont (theme_.font.title),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 28.0f);
            y += 36.0f;
            canvas.setColor (theme_.palette.textSecondary);
            const std::string ver = "Latest " + latestVersion_ + "  ·  Installed " + currentVersion_;
            canvas.text (ver.c_str(), regularFont (theme_.font.label),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 22.0f);
            y += 28.0f;
            for (std::size_t i = 0; i < highlights_.size() && i < 4; ++i)
            {
                const std::string line = std::string ("• ") + highlights_[i];
                canvas.text (line.c_str(), regularFont (theme_.font.caption),
                             visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 18.0f);
                y += 18.0f;
            }
            y += 8.0f;
            canvas.setColor (theme_.palette.textDim);
            canvas.text ("A terminal will open. Quit your DAW before installing.",
                         regularFont (theme_.font.caption),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 18.0f);
            y += 18.0f;
            canvas.text ("ターミナルが開きます。DAW を終了してから実行してください。",
                         regularFont (theme_.font.caption),
                         visage::Font::kLeft, panelX_ + pad, y, panelW_ - pad * 2, 18.0f);
        }

        auto drawBtn = [&] (const Button& b, bool hover)
        {
            const auto fill = b.primary
                ? visage::Color (hover ? theme_.palette.accent : theme_.palette.accent)
                : visage::Color (hover ? theme_.palette.track : theme_.palette.panelLo);
            canvas.setColor (fill);
            canvas.roundedRectangle (b.x, b.y, b.w, b.h, b.h * 0.5f);
            canvas.setColor (b.primary ? visage::Color (0xffffffffu)
                                       : visage::Color (theme_.palette.text));
            canvas.text (b.label.c_str(), boldFont (theme_.font.caption),
                         visage::Font::kCenter, b.x, b.y, b.w, b.h);
        };
        drawBtn (secondary_, hoverBtn_ == 0);
        drawBtn (primary_, hoverBtn_ == 1);
    }

    void UpdateDialog::mouseMove (const visage::MouseEvent& e)
    {
        const int h = hitButton (e.position.x, e.position.y);
        if (h != hoverBtn_)
        {
            hoverBtn_ = h;
            redraw();
        }
    }

    void UpdateDialog::mouseDown (const visage::MouseEvent& e)
    {
        const int btn = hitButton (e.position.x, e.position.y);
        if (btn == 1)
        {
            if (mode_ == Mode::OptIn)
            {
                close();
                if (onAcceptOptIn) onAcceptOptIn();
            }
            else
            {
                close();
                if (onPrimaryAction) onPrimaryAction();
            }
            return;
        }
        if (btn == 0)
        {
            close();
            if (mode_ == Mode::OptIn)
            {
                if (onDeclineOptIn) onDeclineOptIn();
            }
            else if (onDismiss)
                onDismiss();
            return;
        }
        // Outside the panel → decline / dismiss
        if (e.position.x < panelX_ || e.position.x > panelX_ + panelW_
            || e.position.y < panelY_ || e.position.y > panelY_ + panelH_)
        {
            close();
            if (mode_ == Mode::OptIn)
            {
                if (onDeclineOptIn) onDeclineOptIn();
            }
            else if (onDismiss)
                onDismiss();
        }
    }

    bool UpdateDialog::keyPress (const visage::KeyEvent& e)
    {
        if (! open_)
            return false;
        if (e.keyCode() == visage::KeyCode::Escape)
        {
            close();
            if (mode_ == Mode::OptIn)
            {
                if (onDeclineOptIn) onDeclineOptIn();
            }
            else if (onDismiss)
                onDismiss();
            return true;
        }
        return false;
    }
}
