#pragma once
//
// factory_ui_visage::UpdateUiHost — wires UpdateCheck ↔ UpdateBadge/UpdateDialog
// so each plugin editor only has to layout the badge and call tick()/onShown()/
// onHidden(). Requires factory_update (linked by clap GUI shells only).
//
#include "factory_ui_visage/UpdateBadge.h"
#include "factory_ui_visage/UpdateDialog.h"

#include "factory_update/UpdateCheck.h"
#include "factory_update/InstallerLocator.h"
#include "factory_update/HttpTransport.h"

#include <memory>
#include <string>

namespace factory_ui_visage
{
    class UpdateUiHost
    {
    public:
        UpdateUiHost (const Theme& theme,
                      const std::string& slug,
                      const std::string& currentVersion,
                      factory_update::HttpTransport* transport = nullptr)
            : ownedTransport_ (transport ? nullptr
                                         : factory_update::makePlatformHttpTransport()),
              transport_ (transport ? transport : ownedTransport_.get()),
              check_ (slug, currentVersion, *transport_),
              badge_ (theme),
              dialog_ (theme)
        {
            badge_.onClick = [this] { openUpdateDialog(); };
            dialog_.onAcceptOptIn = [this]
            {
                check_.acceptOptIn();
                syncBadge();
            };
            dialog_.onDeclineOptIn = [this]
            {
                check_.declineOptIn();
                syncBadge();
            };
            dialog_.onPrimaryAction = [this] { runPrimaryAction(); };
            dialog_.onDismiss = [] {};
        }

        factory_update::UpdateCheck& check() noexcept { return check_; }
        UpdateBadge& badge() noexcept { return badge_; }
        UpdateDialog& dialog() noexcept { return dialog_; }

        void onShown()
        {
            check_.onEditorShown();
            if (check_.needsOptInPrompt() && ! dialog_.isOpen())
                dialog_.openOptIn();
            syncBadge();
        }

        void onHidden()
        {
            check_.onEditorHidden();
            if (dialog_.isOpen())
                dialog_.close();
            syncBadge();
        }

        // Call once per UI frame (from the editor's frame tick).
        void tick()
        {
            check_.poll();
            if (check_.needsOptInPrompt() && ! dialog_.isOpen())
                dialog_.openOptIn();
            syncBadge();
        }

        void layoutBadge (float x, float y, float w, float h)
        {
            badge_.setBounds (x, y, w, h);
        }

        void layoutDialog (float editorW, float editorH)
        {
            dialog_.setBounds (0.0f, 0.0f, editorW, editorH);
        }

    private:
        void syncBadge()
        {
            const bool show = check_.state() == factory_update::UpdateState::UpdateAvailable;
            if (badge_.isVisible() != show)
                badge_.setVisible (show);
            if (show)
                badge_.redraw();
        }

        void openUpdateDialog()
        {
            auto info = check_.available();
            if (! info)
                return;
            const bool present = factory_update::findInstallerBinary().has_value();
            dialog_.openUpdate (info->currentVersion, info->latestVersion,
                                info->highlights, present);
        }

        void runPrimaryAction()
        {
            if (auto loc = factory_update::findInstallerBinary())
            {
                if (! factory_update::launchInstaller (loc->path, check_.slug()))
                    factory_update::openDistributePageAndCopyOneLiner();
            }
            else
                factory_update::openDistributePageAndCopyOneLiner();
        }

        std::unique_ptr<factory_update::HttpTransport> ownedTransport_;
        factory_update::HttpTransport* transport_ = nullptr;
        factory_update::UpdateCheck check_;
        UpdateBadge badge_;
        UpdateDialog dialog_;
    };
}
