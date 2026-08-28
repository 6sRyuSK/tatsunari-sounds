#include "factory_update/HttpTransport.h"

#include <memory>

namespace factory_update
{
    namespace
    {
        // Soft-fail transport for Linux / EMSCRIPTEN / bring-up. Shipping hosts
        // (macOS / Windows) replace this with a platform TU.
        class StubHttpTransport final : public HttpTransport
        {
        public:
            void get (const std::string&,
                      const std::optional<std::string>&,
                      std::function<void (HttpResult)> cb) override
            {
                if (cb)
                    cb (HttpResult { 0, {}, {}, "http transport unavailable on this platform", false });
            }
            void cancel() override {}
        };
    }

#if ! defined(__APPLE__) && ! defined(_WIN32)
    std::unique_ptr<HttpTransport> makePlatformHttpTransport()
    {
        return std::make_unique<StubHttpTransport>();
    }
#endif
}
