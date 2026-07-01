#pragma once
//
// ModelHandoff — lock-free ownership handoff of a heap object from the message
// thread to the audio thread, with retirement of the old object back to the message
// thread for deletion (so nothing is ever freed on the audio thread). Used to swap
// NAM models and IR kernels while processBlock keeps running.
//
// Threading contract:
//   * Message thread only: publish(), publishEmpty(), drainRetired(), destructor.
//   * Audio thread only:   updateLive().
// A single atomic `pending` slot carries the newest object message->audio; a small
// SPSC ring carries retired objects audio->message. The audio thread OWNS `live`
// (only it reads/writes it), so an object in use is never touched by the message
// thread until the audio thread has retired it and moved on.
//
#include <atomic>
#include <memory>

template <typename T>
class ModelHandoff
{
public:
    ModelHandoff() = default;

    ~ModelHandoff()
    {
        // No audio thread is running at destruction time.
        drainRetired();
        delete pending.exchange (nullptr, std::memory_order_acquire);
        delete live;
        live = nullptr;
    }

    ModelHandoff (const ModelHandoff&) = delete;
    ModelHandoff& operator= (const ModelHandoff&) = delete;

    // Message thread: install a new object (ownership transferred). Any previous
    // pending-but-unconsumed object was never made live, so it is safe to delete on
    // this thread.
    void publish (std::unique_ptr<T> next)
    {
        T* old = pending.exchange (next.release(), std::memory_order_acq_rel);
        delete old;
    }

    // Message thread: request the slot be emptied (live object retired to us).
    void publishEmpty()
    {
        delete pending.exchange (nullptr, std::memory_order_acq_rel);
        clearRequested.store (true, std::memory_order_release);
    }

    // Audio thread: consume any pending object / clear request and return the current
    // live pointer (may be null). Retires the previous live object to the message
    // thread. Cheap: a couple of atomics.
    T* updateLive() noexcept
    {
        if (clearRequested.exchange (false, std::memory_order_acq_rel))
        {
            if (live != nullptr) { retire (live); live = nullptr; }
        }
        if (T* p = pending.exchange (nullptr, std::memory_order_acq_rel))
        {
            if (live != nullptr) retire (live);
            live = p;
        }
        return live;
    }

    // Message thread: delete objects retired by the audio thread.
    void drainRetired()
    {
        T* p = nullptr;
        while (retirePop (p)) delete p;
    }

private:
    static constexpr int kRetireN = 32;

    void retire (T* p) noexcept
    {
        const int t  = retireTail.load (std::memory_order_relaxed);
        const int nt = (t + 1) % kRetireN;
        if (nt == retireHead.load (std::memory_order_acquire))
            return;                                  // full (pathological): drop rather than free on audio thread
        retireBuf[(size_t) t] = p;
        retireTail.store (nt, std::memory_order_release);
    }

    bool retirePop (T*& p) noexcept
    {
        const int h = retireHead.load (std::memory_order_relaxed);
        if (h == retireTail.load (std::memory_order_acquire))
            return false;
        p = retireBuf[(size_t) h];
        retireHead.store ((h + 1) % kRetireN, std::memory_order_release);
        return true;
    }

    std::atomic<T*>   pending { nullptr };
    std::atomic<bool> clearRequested { false };
    T* live = nullptr;                               // audio-thread owned

    T* retireBuf[kRetireN] {};
    std::atomic<int> retireHead { 0 };
    std::atomic<int> retireTail { 0 };
};
