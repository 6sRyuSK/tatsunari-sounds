//
// ui/visage/tests/theme_scale_test.cpp — headless unit test for the runtime
// px-metric scaler factory_ui_visage::scaleThemeMetrics (ThemeScale.h). It is
// visage-free and JUCE-free (Theme.cpp only), same conventions as
// theme_roundtrip_test (accumulate failures, return 1 at the end).
//
// Two invariants:
//   * s == 1.0 is the exact IDENTITY (operator== holds — float * 1.0f is exact),
//     so scaling never perturbs a design-size (k = 1) editor;
//   * s == 2.0 DOUBLES every px-dimension field and leaves colours / ratios /
//     angles / alphas UNTOUCHED.
//
// Manual run:
//   c++ -std=c++17 -Iinclude tests/theme_scale_test.cpp src/Theme.cpp -o ts && ./ts
//
#include "factory_ui_visage/Theme.h"
#include "factory_ui_visage/ThemeScale.h"

#include <cmath>
#include <cstdio>
#include <string>

namespace
{
int g_failures = 0;

void check (bool cond, const std::string& msg)
{
    if (! cond) { ++g_failures; std::printf ("  FAIL: %s\n", msg.c_str()); }
}

void near (float a, float b, const std::string& msg)
{
    if (! (std::abs (a - b) <= 1.0e-4f))
    { ++g_failures; std::printf ("  FAIL: %s (%g vs %g)\n", msg.c_str(), (double) a, (double) b); }
}
} // namespace

int main()
{
    using factory_ui_visage::Theme;
    using factory_ui_visage::scaleThemeMetrics;
    std::printf ("factory_ui_visage scaleThemeMetrics — headless checks\n");

    const Theme base = Theme::defaults();

    // 1. identity at s == 1.0.
    check (scaleThemeMetrics (base, 1.0f) == base, "s=1.0 is the identity (operator==)");

    // 2. representative px fields double at s == 2.0.
    const Theme two = scaleThemeMetrics (base, 2.0f);
    near (two.font.caption,           base.font.caption           * 2.0f, "font.caption doubles");
    near (two.font.title,             base.font.title             * 2.0f, "font.title doubles");
    near (two.card.cornerRadius,      base.card.cornerRadius      * 2.0f, "card.cornerRadius doubles");
    near (two.knob.boundsInset,       base.knob.boundsInset       * 2.0f, "knob.boundsInset doubles");
    near (two.knob.needleWidthPx,     base.knob.needleWidthPx     * 2.0f, "knob.needleWidthPx doubles");
    near (two.linkSlider.captionColumn, base.linkSlider.captionColumn * 2.0f, "linkSlider.captionColumn doubles");
    near (two.linkSlider.trackHeight, base.linkSlider.trackHeight * 2.0f, "linkSlider.trackHeight doubles");
    near (two.segmented.height,       base.segmented.height       * 2.0f, "segmented.height doubles");
    near (two.dropdown.rowHeight,     base.dropdown.rowHeight     * 2.0f, "dropdown.rowHeight doubles");
    near (two.valueSetting.iconSize,  base.valueSetting.iconSize  * 2.0f, "valueSetting.iconSize doubles");
    near (two.spectrum.traceWidth,    base.spectrum.traceWidth    * 2.0f, "spectrum.traceWidth doubles");

    // 3. colours + ratios + angles + dB + alpha are INVARIANT under scaling.
    check (two.palette.accent      == base.palette.accent,      "palette.accent unchanged");
    check (two.palette.background  == base.palette.background,  "palette.background unchanged");
    near  (two.knob.lineWidthRatio,   base.knob.lineWidthRatio,   "knob.lineWidthRatio (ratio) unchanged");
    near  (two.knob.bodyInsetFactor,  base.knob.bodyInsetFactor,  "knob.bodyInsetFactor (ratio) unchanged");
    near  (two.knob.needleLengthRatio,base.knob.needleLengthRatio,"knob.needleLengthRatio (ratio) unchanged");
    near  (two.knob.arcStart,         base.knob.arcStart,         "knob.arcStart (radians) unchanged");
    near  (two.knob.arcEnd,           base.knob.arcEnd,           "knob.arcEnd (radians) unchanged");
    near  (two.toggle.widthFactor,    base.toggle.widthFactor,    "toggle.widthFactor (ratio) unchanged");
    near  (two.iconButton.glyphInsetFactor, base.iconButton.glyphInsetFactor, "iconButton.glyphInsetFactor (ratio) unchanged");
    near  (two.spectrum.topDb,        base.spectrum.topDb,        "spectrum.topDb (dB) unchanged");
    near  (two.spectrum.bottomDb,     base.spectrum.bottomDb,     "spectrum.bottomDb (dB) unchanged");
    near  (two.spectrum.fillTopAlpha, base.spectrum.fillTopAlpha, "spectrum.fillTopAlpha (alpha) unchanged");

    if (g_failures == 0) { std::printf ("OK: all checks passed.\n"); return 0; }
    std::printf ("FAILED: %d check(s).\n", g_failures);
    return 1;
}
