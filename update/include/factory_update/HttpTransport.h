#pragma once
//
// Thin HTTP GET transport for the update feed. Platform adapters:
//   macOS  → NSURLSession (.mm)
//   Windows → WinHTTP
//   else   → stub (always fails soft)
// Tests inject FakeHttpTransport.
//
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace factory_update
{
    struct HttpResult
    {
        int status = 0;                 // 0 = transport failure / cancelled
        std::string body;
        std::string etag;
        std::string error;              // human-readable when status == 0
        bool cancelled = false;
    };

    class HttpTransport
    {
    public:
        virtual ~HttpTransport() = default;

        // Fire-and-forget async GET. Implementations may invoke cb synchronously
        // (FakeHttpTransport) or on a background thread — UpdateCheck marshals
        // onto the UI thread via poll().
        virtual void get (const std::string& url,
                          const std::optional<std::string>& ifNoneMatch,
                          std::function<void (HttpResult)> cb) = 0;

        virtual void cancel() = 0;
    };

    // Deterministic in-process transport for unit tests.
    class FakeHttpTransport final : public HttpTransport
    {
    public:
        HttpResult next;
        int getCount = 0;
        std::string lastUrl;
        std::optional<std::string> lastIfNoneMatch;
        bool cancelled_ = false;

        void get (const std::string& url,
                  const std::optional<std::string>& ifNoneMatch,
                  std::function<void (HttpResult)> cb) override
        {
            ++getCount;
            lastUrl = url;
            lastIfNoneMatch = ifNoneMatch;
            if (cb)
                cb (next);
        }

        void cancel() override { cancelled_ = true; }
    };

    // Platform factory: macOS/Windows return a real client; others a soft-fail stub.
    std::unique_ptr<HttpTransport> makePlatformHttpTransport();
}
