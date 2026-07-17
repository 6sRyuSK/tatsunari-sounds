#include "factory_ui_visage/SpectrumView.h"

#include <visage_graphics/path.h>

#include <algorithm>

namespace factory_ui_visage
{
    SpectrumView::SpectrumView (const Theme& theme, SpectrumModel& model, double sampleRate)
        : theme_ (theme), model_ (model), sampleRate_ (sampleRate)
    {
    }

    void SpectrumView::setFrozen (bool frozen)
    {
        frozen_ = frozen;
        if (! frozen_)
            redraw(); // resume the animation loop
    }

    void SpectrumView::draw (visage::Canvas& canvas)
    {
        const SpectrumMetrics& sp = theme_.spectrum;
        const Palette& p = theme_.palette;

        // Advance the model (feed a fresh frame) unless frozen.
        if (! frozen_ && onTick)
            onTick();

        const float w = width();
        const float h = height();

        // Plot card: a soft top-lit fill + hairline, like the analyser panels.
        canvas.setColor (visage::Brush::vertical (visage::Color (p.panel),
                                                  visage::Color (p.panelLo)));
        canvas.roundedRectangle (0.0f, 0.0f, w, h, sp.cornerRadius);
        canvas.setColor (visage::Color (p.track));
        canvas.roundedRectangleBorder (0.5f, 0.5f, w - 1.0f, h - 1.0f, sp.cornerRadius, 1.0f);

        // A little inset so the trace never touches the rounded border.
        const float inset = 6.0f;
        LogFreqAxis  axis  { inset, std::max (1.0f, w - 2.0f * inset) };
        VerticalAxis vaxis { inset, std::max (1.0f, h - 2.0f * inset), sp.topDb, sp.bottomDb };
        const float baselineY = inset + std::max (1.0f, h - 2.0f * inset);

        const int bins = model_.numBins();
        if (bins < 2)
            return;

        // Faint decade gridlines (100 / 1k / 10k) for a sense of scale.
        canvas.setColor (visage::Color (p.track).withAlpha (0.6f));
        for (float f : { 100.0f, 1000.0f, 10000.0f })
        {
            const float gx = axis.freqToX (f);
            canvas.rectangle (gx, inset, 1.0f, h - 2.0f * inset);
        }

        // Build the smoothed trace (open polyline) + a closed area to the baseline,
        // and the peak-hold outline — SpectrumTrace geometry from SpectrumDisplay.h.
        visage::Path line, area, peak;
        bool started = false;
        float lastX = 0.0f;
        for (int bin = 1; bin < bins; ++bin)
        {
            const float f = SpectrumModel::binFrequency (bin, sampleRate_, model_.size());
            if (f < LogFreqAxis::kMinHz)
                continue;
            if (f > LogFreqAxis::kMaxHz)
                break;

            const float x  = axis.freqToX (f);
            const float y  = vaxis.toY (model_.smoothedDb (bin));
            const float yp = vaxis.toY (model_.peakDb (bin));
            if (! started)
            {
                line.moveTo (x, y);
                area.moveTo (x, baselineY);
                area.lineTo (x, y);
                peak.moveTo (x, yp);
                started = true;
            }
            else
            {
                line.lineTo (x, y);
                area.lineTo (x, y);
                peak.lineTo (x, yp);
            }
            lastX = x;
        }

        if (! started)
            return;

        area.lineTo (lastX, baselineY);
        area.close();

        // Area fill: accent, fading from the trace down to the baseline.
        canvas.setColor (visage::Brush::vertical (visage::Color (p.accent).withAlpha (sp.fillTopAlpha),
                                                  visage::Color (p.accent).withAlpha (sp.fillBottomAlpha)));
        canvas.fill (area);

        // Peak-hold outline (dim), then the smoothed trace (solid accent).
        canvas.setColor (visage::Color (p.accentDim));
        canvas.fill (peak.stroke (sp.peakWidth, visage::Path::Join::Round, visage::Path::EndCap::Round));
        canvas.setColor (visage::Color (p.accent));
        canvas.fill (line.stroke (sp.traceWidth, visage::Path::Join::Round, visage::Path::EndCap::Round));

        // Keep the animation loop alive unless frozen (visage repaints on redraw()).
        if (! frozen_)
            redraw();
    }
}
