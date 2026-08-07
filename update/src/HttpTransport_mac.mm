//
// macOS NSURLSession GET transport for factory_update (plan §1.2).
//
#import <Foundation/Foundation.h>

#include "factory_update/HttpTransport.h"
#include "factory_update/Urls.h"

#include <mutex>

namespace factory_update
{
    namespace
    {
        class MacHttpTransport final : public HttpTransport
        {
        public:
            MacHttpTransport()
            {
                session_ = [NSURLSession sessionWithConfiguration:
                    [NSURLSessionConfiguration ephemeralSessionConfiguration]];
            }

            ~MacHttpTransport() override
            {
                cancel();
            }

            void get (const std::string& url,
                      const std::optional<std::string>& ifNoneMatch,
                      std::function<void (HttpResult)> cb) override
            {
                cancel();
                NSString* nsUrl = [NSString stringWithUTF8String:url.c_str()];
                NSURL* u = [NSURL URLWithString:nsUrl];
                if (u == nil)
                {
                    if (cb)
                        cb (HttpResult { 0, {}, {}, "bad url", false });
                    return;
                }
                NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
                req.HTTPMethod = @"GET";
                req.timeoutInterval = 15.0;
                if (ifNoneMatch && ! ifNoneMatch->empty())
                {
                    NSString* etag = [NSString stringWithUTF8String:ifNoneMatch->c_str()];
                    [req setValue:etag forHTTPHeaderField:@"If-None-Match"];
                }

                std::lock_guard<std::mutex> lock (mu_);
                generation_++;
                const int gen = generation_;
                callback_ = std::move (cb);

                __block MacHttpTransport* self = this;
                task_ = [session_ dataTaskWithRequest:req
                                    completionHandler:^(NSData* data, NSURLResponse* resp, NSError* err)
                {
                    HttpResult r;
                    if (err != nil)
                    {
                        if (err.code == NSURLErrorCancelled)
                            r.cancelled = true;
                        else
                            r.error = err.localizedDescription.UTF8String
                                          ? err.localizedDescription.UTF8String
                                          : "request failed";
                    }
                    else if (auto* http = (NSHTTPURLResponse*) resp)
                    {
                        r.status = (int) http.statusCode;
                        NSString* etag = http.allHeaderFields[@"ETag"];
                        if (etag != nil)
                            r.etag = etag.UTF8String ? etag.UTF8String : "";
                        if (data != nil && data.length > 0
                            && data.length <= factory_update::kMaxLatestBytes)
                        {
                            r.body.assign ((const char*) data.bytes, (const char*) data.bytes + data.length);
                        }
                        else if (data != nil && data.length > factory_update::kMaxLatestBytes)
                        {
                            r.status = 0;
                            r.error = "response exceeds size limit";
                        }
                    }
                    std::function<void (HttpResult)> cbCopy;
                    {
                        std::lock_guard<std::mutex> g (self->mu_);
                        if (gen != self->generation_)
                            return; // superseded / cancelled
                        cbCopy = std::move (self->callback_);
                        self->callback_ = nullptr;
                        self->task_ = nil;
                    }
                    if (cbCopy)
                        cbCopy (std::move (r));
                }];
                [task_ resume];
            }

            void cancel() override
            {
                std::lock_guard<std::mutex> lock (mu_);
                generation_++;
                if (task_ != nil)
                {
                    [task_ cancel];
                    task_ = nil;
                }
                callback_ = nullptr;
            }

        private:
            NSURLSession* session_ = nil;
            NSURLSessionDataTask* task_ = nil;
            std::function<void (HttpResult)> callback_;
            std::mutex mu_;
            int generation_ = 0;
        };
    } // namespace

    std::unique_ptr<HttpTransport> makePlatformHttpTransport()
    {
        return std::make_unique<MacHttpTransport>();
    }
}
