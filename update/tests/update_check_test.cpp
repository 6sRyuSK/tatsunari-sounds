//
// Headless unit tests for factory_update (opt-in, 24h gate, ETag/304, cancel,
// SemVer, latest.json parse). No network — FakeHttpTransport only.
//
#include "factory_update/HttpTransport.h"
#include "factory_update/LatestDocument.h"
#include "factory_update/SemVer.h"
#include "factory_update/UpdateCheck.h"
#include "factory_update/UpdatePrefs.h"
#include "factory_update/Urls.h"
#include "factory_update/InstallerLocator.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace factory_update;

namespace
{
int g_failures = 0;

void fail (const std::string& msg)
{
    ++g_failures;
    std::printf ("  FAIL: %s\n", msg.c_str());
}
void expect (bool cond, const std::string& msg)
{
    if (! cond)
        fail (msg);
}

std::string readFile (const fs::path& p)
{
    std::ifstream in (p);
    return std::string (std::istreambuf_iterator<char> (in), {});
}

fs::path g_fixtures;
fs::path g_tmp;

fs::path tmpPrefs (const char* name = "update-prefs.json")
{
    return g_tmp / name;
}
} // namespace

void testUrls()
{
    std::printf ("testUrls\n");
    expect (kLatestJSONURL.find ("6sryusk.com") != std::string_view::npos, "host");
    expect (kLatestJSONURL.find ("/tatsunarisounds/updates/v1/latest.json") != std::string_view::npos,
            "path");
    expect (kMaxLatestBytes == 256 * 1024, "max bytes");
}

void testSemVer()
{
    std::printf ("testSemVer\n");
    expect (parseSemVer ("1.2.3").has_value(), "parse ok");
    expect (! parseSemVer ("1.2.3-beta").has_value(), "reject prerelease");
    expect (! parseSemVer ("1.2").has_value(), "reject short");
    expect (isUpdateAvailable ("0.2.1", "0.3.0"), "newer");
    expect (! isUpdateAvailable ("0.3.0", "0.3.0"), "equal");
    expect (! isUpdateAvailable ("1.0.0", "0.9.9"), "older");
    expect (! isUpdateAvailable ("bad", "1.0.0"), "bad current");
}

void testParseLatest()
{
    std::printf ("testParseLatest\n");
    LatestDocument doc;
    std::string err;
    const auto body = readFile (g_fixtures / "latest_minimal.json");
    expect (parseLatestDocument (body, doc, err), "parse minimal: " + err);
    expect (doc.schema == 1, "schema");
    expect (doc.plugins.size() == 1, "one plugin");
    expect (doc.plugins[0].slug == "resonance-suppressor", "slug");
    expect (doc.plugins[0].latest == "1.0.0", "latest");

    expect (! parseLatestDocument ("{", doc, err), "bad json");
    expect (! parseLatestDocument ("{\"schema\":2,\"generated\":\"x\",\"plugins\":[]}", doc, err),
            "bad schema");

    const auto full = readFile (g_fixtures / "latest_full.json");
    expect (parseLatestDocument (full, doc, err), "parse full: " + err);
    expect (findPlugin (doc, "resonance-suppressor") != nullptr, "find rs");
}

void testPrefsRoundtrip()
{
    std::printf ("testPrefsRoundtrip\n");
    const auto path = tmpPrefs ("prefs-roundtrip.json");
    fs::remove (path);
    UpdatePrefs p;
    p.optedIn = true;
    p.etag = "\"abc\"";
    p.lastSuccessUnix = 12345;
    p.cachedSlug = "pitch-force";
    p.cachedLatest = "0.2.0";
    p.cachedHighlights = { "a", "b" };
    p.cachedChangelogUrl = "https://6sryusk.com/tatsunarisounds/notes/x";
    expect (saveUpdatePrefs (path, p), "save");
    UpdatePrefs q;
    expect (loadUpdatePrefs (path, q), "load");
    expect (q.optedIn, "optedIn");
    expect (q.etag == p.etag, "etag");
    expect (q.lastSuccessUnix == 12345, "ts");
    expect (q.cachedSlug == "pitch-force", "slug");
    expect (q.cachedLatest == "0.2.0", "latest");
    expect (q.cachedHighlights.size() == 2, "highlights");
}

void testOptInAndCheck()
{
    std::printf ("testOptInAndCheck\n");
    const auto path = tmpPrefs ("prefs-optin.json");
    fs::remove (path);
    FakeHttpTransport http;
    http.next.status = 200;
    http.next.etag = "\"e1\"";
    http.next.body =
        R"({"schema":1,"generated":"2026-07-30T00:00:00Z","plugins":[)"
        R"({"slug":"pitch-force","latest":"9.9.9","highlights":["hi"]}]})";

    std::int64_t now = 1'000'000;
    UpdateCheck uc ("pitch-force", "0.1.0", http, path, [&] { return now; });
    expect (uc.state() == UpdateState::Disabled, "starts disabled");
    expect (uc.needsOptInPrompt(), "needs opt-in");

    uc.onEditorShown();
    expect (uc.state() == UpdateState::Disabled, "still disabled before accept");
    uc.declineOptIn();
    expect (uc.state() == UpdateState::Disabled, "decline stays disabled");
    expect (! uc.needsOptInPrompt(), "prompt cleared");

    fs::remove (path);
    UpdateCheck uc2 ("pitch-force", "0.1.0", http, path, [&] { return now; });
    uc2.onEditorShown();
    uc2.acceptOptIn();
    uc2.poll();
    expect (http.getCount >= 1, "GET fired");
    expect (http.lastUrl.find ("latest.json") != std::string::npos, "url");
    expect (uc2.state() == UpdateState::UpdateAvailable, "update available");
    auto info = uc2.available();
    expect (info.has_value(), "info");
    if (info)
        expect (info->latestVersion == "9.9.9", "latest set");
}

void testEtag304AndInterval()
{
    std::printf ("testEtag304AndInterval\n");
    const auto path = tmpPrefs ("prefs-etag.json");
    fs::remove (path);
    FakeHttpTransport http;
    http.next.status = 200;
    http.next.etag = "\"v1\"";
    http.next.body =
        R"({"schema":1,"generated":"2026-07-30T00:00:00Z","plugins":[)"
        R"({"slug":"pitch-force","latest":"2.0.0","highlights":["x"]}]})";

    std::int64_t now = 5'000;
    UpdateCheck uc ("pitch-force", "1.0.0", http, path, [&] { return now; });
    uc.acceptOptIn();
    uc.onEditorShown();
    uc.poll();
    expect (uc.state() == UpdateState::UpdateAvailable, "first check");
    expect (http.getCount == 1, "one get");

    now += 60;
    uc.onEditorHidden();
    uc.onEditorShown();
    uc.poll();
    expect (http.getCount == 1, "gated");

    uc.setCheckIntervalSeconds (1);
    now += 10;
    http.next = HttpResult { 304, {}, "\"v1\"", {}, false };
    uc.onEditorHidden();
    uc.onEditorShown();
    uc.poll();
    expect (http.getCount == 2, "second get");
    expect (http.lastIfNoneMatch.has_value() && *http.lastIfNoneMatch == "\"v1\"", "etag sent");
    expect (uc.state() == UpdateState::UpdateAvailable, "304 keeps available");
}

void testCancel()
{
    std::printf ("testCancel\n");
    const auto path = tmpPrefs ("prefs-cancel.json");
    fs::remove (path);
    struct DeferredTransport : HttpTransport
    {
        std::function<void (HttpResult)> pending;
        void get (const std::string&, const std::optional<std::string>&,
                  std::function<void (HttpResult)> cb) override
        {
            pending = std::move (cb);
        }
        void cancel() override
        {
            if (pending)
            {
                HttpResult r;
                r.cancelled = true;
                pending (r);
                pending = nullptr;
            }
        }
    } http;

    std::int64_t now = 1;
    UpdateCheck uc ("pitch-force", "1.0.0", http, path, [&] { return now; });
    uc.acceptOptIn();
    uc.onEditorShown();
    expect (uc.state() == UpdateState::Checking, "checking");
    uc.onEditorHidden();
    uc.poll();
    expect (uc.state() != UpdateState::Checking, "cancelled out of checking");
}

void testInstallerPaths()
{
    std::printf ("testInstallerPaths\n");
    const auto user = installerBinaryPath (InstallerScope::User);
    const auto system = installerBinaryPath (InstallerScope::System);
    expect (! user.empty(), "user path");
    expect (! system.empty(), "system path");
    expect (user.filename() == "tatsunari" || user.filename() == "tatsunari.exe", "name");
}

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf (stderr, "usage: %s <fixtures-dir>\n", argv[0]);
        return 2;
    }
    g_fixtures = argv[1];
    g_tmp = fs::temp_directory_path() / "factory_update_test";
    fs::create_directories (g_tmp);

    testUrls();
    testSemVer();
    testParseLatest();
    testPrefsRoundtrip();
    testOptInAndCheck();
    testEtag304AndInterval();
    testCancel();
    testInstallerPaths();

    if (g_failures == 0)
        std::printf ("OK\n");
    else
        std::printf ("%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
