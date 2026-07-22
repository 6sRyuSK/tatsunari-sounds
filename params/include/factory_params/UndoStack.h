#pragma once

#include <cstddef>
#include <utility>
#include <vector>

//
// factory_params::UndoStack — a JUCE-free undo/redo history for the future
// parameter shell. Spec'd and unit-tested now, WIRED LATER: it is intentionally
// not connected to any processor or editor in this phase.
//
// A snapshot is the full vector<float> of real parameter values. The history is a
// linear timeline with a cursor; history[cursor-1] is the current committed state
// and history[cursor..] is the redo tail.
//
//   * push(snapshot, nowSeconds): commit `snapshot` as the new current state,
//     first discarding any redo tail. COALESCING: a push within `coalesceWindow`
//     (0.5 s) of the previous push REPLACES the pending top instead of adding
//     depth — mirroring the editor's 500 ms beginNewTransaction() grouping, so a
//     knob drag collapses into one undo step. Depth is bounded at `maxDepth` (64);
//     past that the oldest entry is dropped.
//   * undo(current) / redo(current): return the snapshot to APPLY, recording the
//     passed-in live `current` values at the position being left (so a subsequent
//     redo/undo restores exactly what was on screen).
//   * clear() empties the history.
//
// Time is INJECTED via nowSeconds so tests drive it with a fake clock — there is
// no std::chrono in the logic.
//
namespace factory_params
{
    class UndoStack
    {
    public:
        using Snapshot = std::vector<float>;

        static constexpr double      coalesceWindow = 0.5; // seconds
        static constexpr std::size_t maxDepth       = 64;

        void push (Snapshot snapshot, double nowSeconds)
        {
            // A new push invalidates the redo branch.
            if (cursor < history.size())
                history.resize (cursor);

            const bool coalesce = haveLastPush
                                  && cursor >= 1
                                  && (nowSeconds - lastPushSeconds) < coalesceWindow;

            if (coalesce)
            {
                history[cursor - 1] = std::move (snapshot); // replace the pending top
            }
            else
            {
                history.push_back (std::move (snapshot));
                if (history.size() > maxDepth)
                    history.erase (history.begin());        // drop the oldest
            }

            cursor         = history.size();
            lastPushSeconds = nowSeconds;
            haveLastPush   = true;
        }

        bool canUndo() const noexcept { return cursor > 1; }
        bool canRedo() const noexcept { return cursor < history.size(); }

        // Returns the snapshot to apply. `current` (the live values being left) is
        // stored at the departing position so redo restores it exactly. If there is
        // nothing to undo, returns `current` unchanged.
        Snapshot undo (Snapshot current)
        {
            if (! canUndo())
                return current;
            history[cursor - 1] = std::move (current);
            --cursor;
            return history[cursor - 1];
        }

        Snapshot redo (Snapshot current)
        {
            if (! canRedo())
                return current;
            history[cursor - 1] = std::move (current);
            ++cursor;
            return history[cursor - 1];
        }

        void clear()
        {
            history.clear();
            cursor        = 0;
            haveLastPush  = false;
        }

        // Introspection (for tests / a future status line).
        std::size_t depth() const noexcept { return history.size(); }

    private:
        std::vector<Snapshot> history;
        std::size_t cursor          = 0;      // history[cursor-1] == current state
        double      lastPushSeconds = 0.0;
        bool        haveLastPush    = false;
    };
}
