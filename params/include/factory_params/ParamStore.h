#pragma once

#include "factory_params/ParamDesc.h"
#include "factory_params/Range.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

//
// factory_params::ParamStore — the runtime parameter value store. In the final
// architecture this is the single source of truth for live parameter values; in
// this phase nothing wires it (it ships with unit tests only).
//
// THREAD CONTRACT
//   * The store owns the descriptors and, per parameter, one atomic<float> real
//     value (init = default) and one atomic<uint32_t> change epoch.
//   * value(idx) / epoch(idx) are relaxed atomic loads, callable from ANY thread
//     (incl. the audio thread) — wait-free, no allocation.
//   * Epochs may be bumped from ANY thread: both setFromHost (host/driver thread)
//     and setFromUi (UI thread) bump. A reader only ever compares epochs.
//   * Host-write queue: a bounded lock-free SPSC ring. The SINGLE producer is the
//     UI thread (setFromUi / beginGesture / endGesture enqueue); the SINGLE
//     consumer is the host driver (drainHostWrites). Overflow DROPS the event and
//     bumps droppedHostWrites() — dropping (never blocking) keeps the UI thread
//     from ever stalling, and kHostWriteSlots (4096) is far above any realistic
//     per-tick burst. Because there is exactly one producer and one consumer, the
//     head/tail need only release/acquire pairing (no CAS, no locks).
//   * ChangeSweeper holds its OWN last-seen epoch vector, so any number of
//     observers (each an independent UI frame tick) can watch the same store with
//     no cross-thread marshalling. Steady-state sweep() is wait-free.
//
namespace factory_params
{
    // A pending host-facing write, drained by the host driver. Value carries the
    // (snapped) real value; the gesture kinds carry only the index.
    struct HostWrite
    {
        enum class Kind { Value, GestureBegin, GestureEnd };
        int   index = 0;
        float value = 0.0f;
        Kind  kind  = Kind::Value;
    };

    class ParamStore
    {
    public:
        // Fixed ring capacity; one slot is always left empty to distinguish full
        // from empty, so the queue holds up to kHostWriteSlots - 1 pending events.
        static constexpr int kHostWriteSlots = 4096;

        explicit ParamStore (std::vector<ParamDesc> descs)
            : descriptors (std::move (descs)),
              values (descriptors.size()),
              epochs (descriptors.size()),
              queue  (static_cast<std::size_t> (kHostWriteSlots))
        {
            ranges.reserve (descriptors.size());
            for (std::size_t i = 0; i < descriptors.size(); ++i)
            {
                ranges.push_back (makeRange (descriptors[i]));
                values[i].store (descriptors[i].defaultValue, std::memory_order_relaxed);
                epochs[i].store (0, std::memory_order_relaxed);
            }
        }

        // --- lookup -----------------------------------------------------------
        int size() const noexcept { return static_cast<int> (descriptors.size()); }
        const ParamDesc& desc (int idx) const noexcept { return descriptors[static_cast<std::size_t> (idx)]; }

        int indexOf (std::string_view id) const noexcept
        {
            for (std::size_t i = 0; i < descriptors.size(); ++i)
                if (descriptors[i].id == id)
                    return static_cast<int> (i);
            return -1;
        }

        // --- any-thread reads -------------------------------------------------
        float value (int idx) const noexcept
        {
            return values[static_cast<std::size_t> (idx)].load (std::memory_order_relaxed);
        }

        std::uint32_t epoch (int idx) const noexcept
        {
            return epochs[static_cast<std::size_t> (idx)].load (std::memory_order_relaxed);
        }

        // --- host-side write (no queue) --------------------------------------
        void setFromHost (int idx, float real) noexcept
        {
            storeSnapped (idx, real);
        }

        // --- UI-side writes (store + enqueue for the host driver) -------------
        void setFromUi (int idx, float real) noexcept
        {
            const float snapped = storeSnapped (idx, real);
            pushHostWrite ({ idx, snapped, HostWrite::Kind::Value });
        }

        void beginGesture (int idx) noexcept
        {
            pushHostWrite ({ idx, 0.0f, HostWrite::Kind::GestureBegin });
        }

        void endGesture (int idx) noexcept
        {
            pushHostWrite ({ idx, 0.0f, HostWrite::Kind::GestureEnd });
        }

        // --- host-write queue drain (single consumer = host driver) ----------
        template <class Fn>
        void drainHostWrites (Fn&& fn)
        {
            std::size_t tail = queueTail.load (std::memory_order_relaxed);
            const std::size_t head = queueHead.load (std::memory_order_acquire);
            while (tail != head)
            {
                fn (queue[tail]);
                tail = (tail + 1) % queue.size();
            }
            queueTail.store (tail, std::memory_order_release);
        }

        std::uint32_t droppedHostWrites() const noexcept
        {
            return droppedCount.load (std::memory_order_relaxed);
        }

    private:
        // Snap + clamp via the parameter's range, store, bump epoch. Returns the
        // stored (snapped) value so the UI path can enqueue exactly what it stored.
        float storeSnapped (int idx, float real) noexcept
        {
            const float snapped = snapToLegalValue (ranges[static_cast<std::size_t> (idx)], real);
            values[static_cast<std::size_t> (idx)].store (snapped, std::memory_order_relaxed);
            epochs[static_cast<std::size_t> (idx)].fetch_add (1, std::memory_order_relaxed);
            return snapped;
        }

        // Single-producer enqueue. On a full ring, drop + count (see contract).
        void pushHostWrite (const HostWrite& w) noexcept
        {
            const std::size_t head = queueHead.load (std::memory_order_relaxed);
            const std::size_t next = (head + 1) % queue.size();
            if (next == queueTail.load (std::memory_order_acquire))
            {
                droppedCount.fetch_add (1, std::memory_order_relaxed);
                return;
            }
            queue[head] = w;
            queueHead.store (next, std::memory_order_release);
        }

        std::vector<ParamDesc>                   descriptors;
        std::vector<std::atomic<float>>          values;   // sized from descriptors
        std::vector<std::atomic<std::uint32_t>>  epochs;   // sized from descriptors
        std::vector<RangeSpec>                   ranges;   // one per parameter (snap/clamp)

        std::vector<HostWrite>       queue;                // fixed size == kHostWriteSlots
        std::atomic<std::size_t>     queueHead { 0 };      // producer (UI) index
        std::atomic<std::size_t>     queueTail { 0 };      // consumer (host) index
        std::atomic<std::uint32_t>   droppedCount { 0 };
    };

    //
    // ChangeSweeper — visit each parameter whose epoch advanced since this
    // sweeper's previous sweep, exactly once. Its last-seen epoch vector is
    // private to the instance, so observers are fully independent. The baseline is
    // the store's construction-time epoch (0), seeded lazily on the first sweep, so
    // a change made before the first sweep is still caught. Steady-state sweep() is
    // wait-free (relaxed loads + the callback); only the one-time lazy sizing
    // allocates, on the (UI) thread that owns the sweeper.
    //
    class ChangeSweeper
    {
    public:
        ChangeSweeper() = default;

        // Pre-size against a store so even the first sweep is wait-free (optional).
        explicit ChangeSweeper (const ParamStore& store)
            : lastEpochs (static_cast<std::size_t> (store.size()), 0u) {}

        template <class Fn>
        void sweep (const ParamStore& store, Fn&& fn)
        {
            const int n = store.size();
            if (static_cast<int> (lastEpochs.size()) != n)
                lastEpochs.assign (static_cast<std::size_t> (n), 0u); // baseline == construction-time epoch

            for (int i = 0; i < n; ++i)
            {
                const std::uint32_t e = store.epoch (i);
                if (e != lastEpochs[static_cast<std::size_t> (i)])
                {
                    lastEpochs[static_cast<std::size_t> (i)] = e;
                    fn (i);
                }
            }
        }

    private:
        std::vector<std::uint32_t> lastEpochs;
    };
}
