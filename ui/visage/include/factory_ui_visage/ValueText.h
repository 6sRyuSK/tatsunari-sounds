#pragma once

#include "factory_params/ParamStore.h"
#include "factory_params/Text.h"

#include <algorithm>
#include <functional>
#include <string>

//
// factory_ui_visage::ValueText — the visage-free half of the direct value-entry
// flow: the open-request contract a control hands the editor (ValueEntryRequest /
// ValueEntryOpener), the prefill reduction of a drawn read-out to a bare number,
// and the shared commit (parse -> revert-on-invalid -> gestured write). The
// overlay widget itself lives in ValueEntry.h; everything here compiles with a
// plain host compiler so the flow is headless-testable (value_text_test.cpp).
//
namespace factory_ui_visage
{
    // A request to open the shared overlay over a control's value read-out. The rect
    // is in WINDOW px; the editor that hosts the shared ValueEntry converts it to its
    // own frame-local coords. `commit` fires with the typed text on Enter / focus-loss
    // (never on Esc, never when the text is unchanged).
    struct ValueEntryRequest
    {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f; // value-label rect, WINDOW px
        std::string prefill;                          // pre-filled bare number (select-all on open)
        float fontPx = 12.0f;                         // value read-out font size
        std::function<void (const std::string&)> commit;
    };

    // The hook a control calls to open the shared overlay (set by the editor).
    using ValueEntryOpener = std::function<void (const ValueEntryRequest&)>;

    // Leading numeric run of `s` (optional sign, digits, one '.', digits) — the
    // JUCE-free analogue of rs::stripToLeadingNumber, so a read-out like "62%" /
    // "2.6kHz" / "-24.0dB" pre-fills the editor with a bare number. Everything from
    // the first non-numeric character on is dropped.
    inline std::string stripLeadingNumber (const std::string& s)
    {
        std::size_t b = 0;
        while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
        std::size_t i = b;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i < s.size() && s[i] == '.')
        {
            ++i;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        return s.substr (b, i - b);
    }

    // The drawn read-out reduced to a bare number for the entry prefill: formatValue
    // -> spaces stripped (matching the tight "100%" / "-24.0dB" draw) ->
    // stripLeadingNumber, so the user edits "62" not "62%".
    inline std::string entryPrefillText (const factory_params::ParamDesc& desc,
                                         float value, int decimals)
    {
        std::string disp = factory_params::formatValue (desc, value, decimals);
        disp.erase (std::remove (disp.begin(), disp.end(), ' '), disp.end());
        return stripLeadingNumber (disp);
    }

    // Shared entry commit. Round #4 follow-up: DELIBERATE deviation from the JUCE
    // oracle (getValueFromText("") == 0 -> clamp-to-min). Invalid / empty input
    // REVERTS — returns false with no gesture, no write, label unchanged — rather
    // than snapping to the minimum (user request). A valid-but-out-of-range number
    // still commits + clamps (setFromUi snaps to the legal range).
    inline bool commitEntryText (factory_params::ParamStore& store, int idx,
                                 const std::string& text)
    {
        float real = 0.0f;
        if (! factory_params::tryParseValue (store.desc (idx), text, real)) return false;
        store.setFromUiGestured (idx, real);
        return true;
    }
}
