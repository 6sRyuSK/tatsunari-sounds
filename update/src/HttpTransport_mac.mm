//
// macOS NSURLSession GET transport for factory_update (plan §1.2).
//
#import <Foundation/Foundation.h>

#include "factory_update/HttpTransport.h"
#include "factory_update/Urls.h"

#include <memory>
#include <mutex>

namespace factory_update
{
    namespace
    {
        // Everything the completion handler touches lives here, behind a
        // shared_ptr the block OWNS a reference to.
        //
        // The block must never capture the transport itself. NSURLSession runs a
        // completion handler asynchronously on its own queue, and [task cancel]
        // does not wait for it: cancelling from ~MacHttpTransport() (which is what
        // the editor's destructor does) returns immediately, the object is freed,
        // and the handler still runs afterwards with NSURLErrorCancelled. A raw
        // `self` capture would then dereference freed memory just to read the
        // mutex and generation it uses to decide it has been superseded — a
        // use-after-free that takes the DAW down. Holding the state in a
        // shared_ptr keeps exactly that state alive for as long as any in-flight
        // handler can still reach it, and no longer.
        struct SharedState
        {
            std::mutex mu;
            int generation = 0;
            std::function<void (HttpResult)> callback;
            NSURLSessionDataTask* task = nil;
        };

        class MacHttpTransport final : public HttpTransport
        {
        public:
            MacHttpTransport()
                : state_ (std::make_shared<SharedState>())
            {
                session_ = [NSURLSession sessionWithConfiguration:
                    [NSURLSessionConfiguration ephemeralSessionConfiguration]];
            }

            ~MacHttpTransport() override
            {
                cancel();
                // Release the session's own resources and guarantee no further
                // task starts. Outstanding handlers may still fire; they only
                // touch state_, which outlives this object via the block.
                [session_ invalidateAndCancel];
                session_ = nil;
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

                // Captured BY VALUE into the block, so the block holds its own
                // reference to the state (see the comment on SharedState).
                std::shared_ptr<SharedState> state = state_;

                int gen = 0;
                {
                    std::lock_guard<std::mutex> lock (state->mu);
                    state->generation++;
                    gen = state->generation;
                    state->callback = std::move (cb);
                }

                NSURLSessionDataTask* task =
                    [session_ dataTaskWithRequest:req
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
                        std::lock_guard<std::mutex> g (state->mu);
                        if (gen != state->generation)
                            return; // superseded / cancelled
                        cbCopy = std::move (state->callback);
                        state->callback = nullptr;
                        state->task = nil;
                    }
                    if (cbCopy)
                        cbCopy (std::move (r));
                }];

                {
                    std::lock_guard<std::mutex> lock (state->mu);
                    // A cancel() between building the request and here already
                    // bumped the generation; don't start a task nobody will read.
                    if (gen != state->generation)
                        return;
                    state->task = task;
                }
                [task resume];
            }

            void cancel() override
            {
                NSURLSessionDataTask* task = nil;
                {
                    std::lock_guard<std::mutex> lock (state_->mu);
                    state_->generation++;
                    task = state_->task;
                    state_->task = nil;
                    state_->callback = nullptr;
                }
                // Cancelled outside the lock: -cancel is asynchronous, and the
                // handler it eventually schedules takes the same mutex.
                if (task != nil)
                    [task cancel];
            }

        private:
            NSURLSession* session_ = nil;
            std::shared_ptr<SharedState> state_;
        };
    } // namespace

    std::unique_ptr<HttpTransport> makePlatformHttpTransport()
    {
        return std::make_unique<MacHttpTransport>();
    }
}
