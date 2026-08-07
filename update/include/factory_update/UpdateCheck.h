#pragma once
//
// UpdateCheck — badge-side state machine (plan §1). Visage-free / JUCE-free.
//
//   disabled → idle → checking → current | updateAvailable | transientError
//
// Network work is asynchronous; call poll() once per UI frame to drain results.
//
#include "factory_update/HttpTransport.h"
#include "factory_update/LatestDocument.h"
#include "factory_update/UpdatePrefs.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace factory_update
{
    enum class UpdateState
    {
        Disabled,         // not opted in (or declined this session)
        Idle,             // opted in, waiting for the 24h gate
        Checking,
        Current,
        UpdateAvailable,
        TransientError,
    };

    struct UpdateInfo
    {
        std::string currentVersion;
        std::string latestVersion;
        std::vector<std::string> highlights;
        std::string changelogUrl;
    };

    class UpdateCheck
    {
    public:
        using Clock = std::function<std::int64_t()>; // UTC unix seconds

        // owns nothing of transport; caller keeps it alive for this object's life.
        UpdateCheck (std::string slug,
                     std::string currentVersion,
                     HttpTransport& transport,
                     std::filesystem::path prefsPath = defaultUpdatePrefsPath(),
                     Clock clock = nullptr);

        void onEditorShown();
        void onEditorHidden(); // cancel in-flight
        void poll();           // drain async results; call every UI frame

        UpdateState state() const noexcept { return state_; }
        std::optional<UpdateInfo> available() const;
        bool needsOptInPrompt() const noexcept { return needsOptInPrompt_; }

        void acceptOptIn();
        void declineOptIn(); // "今はしない" / dismiss

        const UpdatePrefs& prefs() const noexcept { return prefs_; }
        const std::string& slug() const noexcept { return slug_; }
        const std::string& currentVersion() const noexcept { return currentVersion_; }

        // Test hook: force the 24h gate open.
        void setCheckIntervalSeconds (std::int64_t s) { checkIntervalSec_ = s; }

    private:
        void loadPrefs();
        void persistPrefs();
        void maybeStartCheck (std::int64_t now);
        void applyHttpResult (HttpResult r);
        void applyCached (std::int64_t now);

        std::string slug_;
        std::string currentVersion_;
        HttpTransport& transport_;
        std::filesystem::path prefsPath_;
        Clock clock_;
        std::int64_t checkIntervalSec_ = 24 * 60 * 60;

        UpdatePrefs prefs_;
        UpdateState state_ = UpdateState::Disabled;
        bool needsOptInPrompt_ = false;
        bool checkInFlight_ = false;
        bool editorVisible_ = false;

        std::mutex pendingMu_;
        std::optional<HttpResult> pending_;
        UpdateInfo info_;
        bool hasInfo_ = false;
    };
}
