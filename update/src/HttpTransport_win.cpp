//
// Windows WinHTTP GET transport for factory_update (plan §1.2).
//
#include "factory_update/HttpTransport.h"
#include "factory_update/Urls.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace factory_update
{
    namespace
    {
        class WinHttpTransport final : public HttpTransport
        {
        public:
            ~WinHttpTransport() override { cancel(); }

            void get (const std::string& url,
                      const std::optional<std::string>& ifNoneMatch,
                      std::function<void (HttpResult)> cb) override
            {
                cancel();
                std::lock_guard<std::mutex> lock (mu_);
                cancelled_ = false;
                worker_ = std::thread ([this, url, ifNoneMatch, cb = std::move (cb)]() mutable
                {
                    HttpResult r = doGet (url, ifNoneMatch);
                    if (cancelled_.load())
                        r.cancelled = true;
                    if (cb)
                        cb (std::move (r));
                });
            }

            void cancel() override
            {
                cancelled_ = true;
                if (worker_.joinable())
                    worker_.join();
            }

        private:
            HttpResult doGet (const std::string& url,
                              const std::optional<std::string>& ifNoneMatch)
            {
                HttpResult r;
                URL_COMPONENTS uc {};
                uc.dwStructSize = sizeof (uc);
                uc.dwSchemeLength = (DWORD) -1;
                uc.dwHostNameLength = (DWORD) -1;
                uc.dwUrlPathLength = (DWORD) -1;
                uc.dwExtraInfoLength = (DWORD) -1;

                std::wstring wurl (url.begin(), url.end());
                if (! WinHttpCrackUrl (wurl.c_str(), 0, 0, &uc))
                {
                    r.error = "bad url";
                    return r;
                }
                std::wstring host (uc.lpszHostName, uc.dwHostNameLength);
                std::wstring path (uc.lpszUrlPath, uc.dwUrlPathLength);
                if (uc.dwExtraInfoLength > 0)
                    path.append (uc.lpszExtraInfo, uc.dwExtraInfoLength);

                HINTERNET session = WinHttpOpen (L"tatsunari-sounds-update/1.0",
                                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                 WINHTTP_NO_PROXY_NAME,
                                                 WINHTTP_NO_PROXY_BYPASS, 0);
                if (! session) { r.error = "WinHttpOpen failed"; return r; }

                HINTERNET conn = WinHttpConnect (session, host.c_str(),
                                                 uc.nPort ? uc.nPort : INTERNET_DEFAULT_HTTPS_PORT, 0);
                if (! conn)
                {
                    WinHttpCloseHandle (session);
                    r.error = "WinHttpConnect failed";
                    return r;
                }

                DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
                HINTERNET req = WinHttpOpenRequest (conn, L"GET", path.c_str(), nullptr,
                                                    WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
                if (! req)
                {
                    WinHttpCloseHandle (conn);
                    WinHttpCloseHandle (session);
                    r.error = "WinHttpOpenRequest failed";
                    return r;
                }

                if (ifNoneMatch && ! ifNoneMatch->empty())
                {
                    std::wstring hdr = L"If-None-Match: "
                                     + std::wstring (ifNoneMatch->begin(), ifNoneMatch->end());
                    WinHttpAddRequestHeaders (req, hdr.c_str(), (DWORD) -1L,
                                              WINHTTP_ADDREQ_FLAG_ADD);
                }

                if (! WinHttpSendRequest (req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                    || ! WinHttpReceiveResponse (req, nullptr))
                {
                    WinHttpCloseHandle (req);
                    WinHttpCloseHandle (conn);
                    WinHttpCloseHandle (session);
                    r.error = "request failed";
                    return r;
                }

                DWORD status = 0, statusSize = sizeof (status);
                WinHttpQueryHeaders (req,
                                     WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                     WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                     WINHTTP_NO_HEADER_INDEX);
                r.status = (int) status;

                DWORD etagSize = 0;
                WinHttpQueryHeaders (req, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX,
                                     WINHTTP_NO_OUTPUT_BUFFER, &etagSize, WINHTTP_NO_HEADER_INDEX);
                if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && etagSize > 0)
                {
                    std::wstring etag ((etagSize / sizeof (wchar_t)) + 1, L'\0');
                    if (WinHttpQueryHeaders (req, WINHTTP_QUERY_ETAG, WINHTTP_HEADER_NAME_BY_INDEX,
                                             etag.data(), &etagSize, WINHTTP_NO_HEADER_INDEX))
                    {
                        etag.resize (etagSize / sizeof (wchar_t));
                        while (! etag.empty() && (etag.back() == L'\0' || etag.back() == L'\r'
                                                  || etag.back() == L'\n'))
                            etag.pop_back();
                        r.etag.assign (etag.begin(), etag.end());
                    }
                }

                for (;;)
                {
                    if (cancelled_.load()) { r.cancelled = true; break; }
                    DWORD avail = 0;
                    if (! WinHttpQueryDataAvailable (req, &avail))
                        break;
                    if (avail == 0)
                        break;
                    if (r.body.size() + avail > kMaxLatestBytes)
                    {
                        r.status = 0;
                        r.error = "response exceeds size limit";
                        r.body.clear();
                        break;
                    }
                    std::string chunk (avail, '\0');
                    DWORD read = 0;
                    if (! WinHttpReadData (req, chunk.data(), avail, &read))
                        break;
                    chunk.resize (read);
                    r.body += chunk;
                }

                WinHttpCloseHandle (req);
                WinHttpCloseHandle (conn);
                WinHttpCloseHandle (session);
                return r;
            }

            std::thread worker_;
            std::atomic<bool> cancelled_ { false };
            std::mutex mu_;
        };
    } // namespace

    std::unique_ptr<HttpTransport> makePlatformHttpTransport()
    {
        return std::make_unique<WinHttpTransport>();
    }
}
