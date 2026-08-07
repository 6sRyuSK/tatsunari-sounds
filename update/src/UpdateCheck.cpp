#include "factory_update/UpdateCheck.h"
#include "factory_update/SemVer.h"
#include "factory_update/Urls.h"

#include <utility>

namespace factory_update
{
    namespace
    {
        std::int64_t defaultClock()
        {
            using namespace std::chrono;
            return duration_cast<seconds> (system_clock::now().time_since_epoch()).count();
        }
    }

    UpdateCheck::UpdateCheck (std::string slug,
                              std::string currentVersion,
                              HttpTransport& transport,
                              std::filesystem::path prefsPath,
                              Clock clock)
        : slug_ (std::move (slug)),
          currentVersion_ (std::move (currentVersion)),
          transport_ (transport),
          prefsPath_ (std::move (prefsPath)),
          clock_ (clock ? std::move (clock) : Clock (defaultClock))
    {
        loadPrefs();
        if (! prefs_.optedIn)
        {
            state_ = UpdateState::Disabled;
            needsOptInPrompt_ = true;
        }
        else
        {
            state_ = UpdateState::Idle;
            applyCached (clock_());
        }
    }

    void UpdateCheck::loadPrefs()
    {
        UpdatePrefs p;
        if (loadUpdatePrefs (prefsPath_, p))
            prefs_ = std::move (p);
    }

    void UpdateCheck::persistPrefs()
    {
        saveUpdatePrefs (prefsPath_, prefs_);
    }

    void UpdateCheck::applyCached (std::int64_t /*now*/)
    {
        if (prefs_.cachedSlug != slug_ || prefs_.cachedLatest.empty())
            return;
        info_.currentVersion = currentVersion_;
        info_.latestVersion = prefs_.cachedLatest;
        info_.highlights = prefs_.cachedHighlights;
        info_.changelogUrl = prefs_.cachedChangelogUrl;
        hasInfo_ = true;
        if (isUpdateAvailable (currentVersion_, prefs_.cachedLatest))
            state_ = UpdateState::UpdateAvailable;
        else
            state_ = UpdateState::Current;
    }

    void UpdateCheck::onEditorShown()
    {
        editorVisible_ = true;
        if (! prefs_.optedIn)
        {
            state_ = UpdateState::Disabled;
            needsOptInPrompt_ = true;
            return;
        }
        maybeStartCheck (clock_());
    }

    void UpdateCheck::onEditorHidden()
    {
        editorVisible_ = false;
        if (checkInFlight_)
        {
            transport_.cancel();
            checkInFlight_ = false;
            if (state_ == UpdateState::Checking)
                state_ = hasInfo_
                    ? (isUpdateAvailable (currentVersion_, info_.latestVersion)
                           ? UpdateState::UpdateAvailable
                           : UpdateState::Current)
                    : UpdateState::Idle;
        }
    }

    void UpdateCheck::acceptOptIn()
    {
        prefs_.optedIn = true;
        needsOptInPrompt_ = false;
        persistPrefs();
        state_ = UpdateState::Idle;
        if (editorVisible_)
            maybeStartCheck (clock_());
    }

    void UpdateCheck::declineOptIn()
    {
        needsOptInPrompt_ = false;
        prefs_.optedIn = false;
        persistPrefs();
        state_ = UpdateState::Disabled;
    }

    void UpdateCheck::maybeStartCheck (std::int64_t now)
    {
        if (! prefs_.optedIn || checkInFlight_ || ! editorVisible_)
            return;
        if (prefs_.lastSuccessUnix > 0
            && (now - prefs_.lastSuccessUnix) < checkIntervalSec_
            && hasInfo_)
        {
            // Within the 24h gate — keep cached state.
            if (state_ == UpdateState::Idle)
                applyCached (now);
            return;
        }

        state_ = UpdateState::Checking;
        checkInFlight_ = true;
        std::optional<std::string> etag;
        if (! prefs_.etag.empty())
            etag = prefs_.etag;

        transport_.get (std::string (kLatestJSONURL), etag,
                        [this] (HttpResult r)
                        {
                            std::lock_guard<std::mutex> lock (pendingMu_);
                            pending_ = std::move (r);
                        });
    }

    void UpdateCheck::poll()
    {
        std::optional<HttpResult> r;
        {
            std::lock_guard<std::mutex> lock (pendingMu_);
            if (pending_)
            {
                r = std::move (*pending_);
                pending_.reset();
            }
        }
        if (r)
            applyHttpResult (std::move (*r));
    }

    void UpdateCheck::applyHttpResult (HttpResult r)
    {
        checkInFlight_ = false;
        if (r.cancelled || ! editorVisible_)
        {
            if (state_ == UpdateState::Checking)
                state_ = hasInfo_ ? state_ : UpdateState::Idle;
            return;
        }
        if (r.status == 304)
        {
            prefs_.lastSuccessUnix = clock_();
            if (! r.etag.empty())
                prefs_.etag = r.etag;
            persistPrefs();
            applyCached (prefs_.lastSuccessUnix);
            return;
        }
        if (r.status < 200 || r.status >= 300 || r.body.empty())
        {
            state_ = UpdateState::TransientError;
            // Soft: do not clear a previously known updateAvailable cache visually
            // on the next shown — badge stays off for TransientError (plan: no warn).
            return;
        }

        LatestDocument doc;
        std::string err;
        if (! parseLatestDocument (r.body, doc, err))
        {
            state_ = UpdateState::TransientError;
            return;
        }
        const LatestPlugin* plug = findPlugin (doc, slug_);
        if (plug == nullptr)
        {
            // Unknown plugin in feed → treat as current (no badge).
            state_ = UpdateState::Current;
            prefs_.lastSuccessUnix = clock_();
            if (! r.etag.empty())
                prefs_.etag = r.etag;
            prefs_.cachedSlug = slug_;
            prefs_.cachedLatest = currentVersion_;
            prefs_.cachedHighlights.clear();
            prefs_.cachedChangelogUrl.clear();
            persistPrefs();
            hasInfo_ = true;
            info_ = { currentVersion_, currentVersion_, {}, {} };
            return;
        }

        prefs_.lastSuccessUnix = clock_();
        if (! r.etag.empty())
            prefs_.etag = r.etag;
        prefs_.cachedSlug = plug->slug;
        prefs_.cachedLatest = plug->latest;
        prefs_.cachedHighlights = plug->highlights;
        prefs_.cachedChangelogUrl = plug->changelogUrl;
        persistPrefs();

        info_.currentVersion = currentVersion_;
        info_.latestVersion = plug->latest;
        info_.highlights = plug->highlights;
        info_.changelogUrl = plug->changelogUrl;
        hasInfo_ = true;

        if (isUpdateAvailable (currentVersion_, plug->latest))
            state_ = UpdateState::UpdateAvailable;
        else
            state_ = UpdateState::Current;
    }

    std::optional<UpdateInfo> UpdateCheck::available() const
    {
        if (state_ != UpdateState::UpdateAvailable || ! hasInfo_)
            return std::nullopt;
        return info_;
    }
}
