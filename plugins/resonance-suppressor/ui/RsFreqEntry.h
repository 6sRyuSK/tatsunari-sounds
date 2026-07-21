#pragma once

#include "factory_params/Text.h"

#include <cctype>
#include <string>

//
// rs_ui::parseFreqEntry — the FREQ text-entry parser (Hz/kHz), shared between the
// node panel's mini FREQ read-out and the headless test (rs_ui_pure_test.cpp).
// visage-free and JUCE-free.
//
namespace rs_ui
{
    // Parse a FREQ text entry to raw Hz, mirroring the shipped JUCE NodePanel
    // freq valueFromTextFunction: the display is Hz/kHz, so a trailing k/kHz
    // (case/space tolerant) means kHz, and a bare number below the parameter
    // minimum whose *1000 lands in range is treated as kHz ("2.6" against a
    // "2.6kHz" display). Returns false (REVERT) when there is no leading number
    // after trimming + stripping the k/kHz suffix ("abc", "", "kHz") — the round
    // #4 follow-up (invalid reverts, not clamp-to-min). setFromUi clamps a valid
    // result to the range.
    inline bool parseFreqEntry (const std::string& text, double lo, double hi, float& out)
    {
        std::string t = text;
        auto isSpace = [] (char c) { return std::isspace ((unsigned char) c) != 0; };
        while (! t.empty() && isSpace (t.front())) t.erase (t.begin());
        while (! t.empty() && isSpace (t.back()))  t.pop_back();
        std::string low = t;
        for (char& c : low) c = (char) std::tolower ((unsigned char) c);
        bool asK = false;
        if (low.size() >= 3 && low.compare (low.size() - 3, 3, "khz") == 0) { t.resize (t.size() - 3); asK = true; }
        else if (! low.empty() && low.back() == 'k')                        { t.resize (t.size() - 1); asK = true; }
        double v = 0.0;
        if (! factory_params::tryParseNumber (t, v)) return false; // no leading number -> revert
        if (asK) { out = (float) (v * 1000.0); return true; }
        if (v < lo && v * 1000.0 >= lo && v * 1000.0 <= hi) { out = (float) (v * 1000.0); return true; }
        out = (float) v;
        return true;
    }
}
