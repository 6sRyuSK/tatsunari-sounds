//
// rscore_equiv_test.cpp — the byte-identical DRIFT GUARD between the shipping
// JUCE ResonanceSuppressorAudioProcessor and the extracted, framework-free
// rs_core::RsCore (P4 coexistence, chunk 2).
//
// RsCore is a faithful CODE MOTION of the processor's prepareToPlay + processBlock
// math, meant to be reused verbatim by the future CLAP shell. This test proves it
// reproduces the shipping processor EXACTLY: it drives the REAL processor (the
// oracle) and an RsCore side by side with IDENTICAL input (noise + impulses + a
// sidechain signal) and IDENTICAL parameters (the APVTS params set to the same
// values the RsParamSnapshot carries), across the full sample-rate matrix and a
// spread of block sizes (incl. 1024 and 1), exercising several band/cut configs,
// Soft/Hard, Stereo/Mid-Side, link on/off, delta on/off, bypass on/off, a Quality
// switch mid-stream, sidechain connected / not, external detection, SC Listen, a
// Listen-node solo, and a display-smoothing change.
//
// The gate is BYTE-IDENTICAL, no tolerance: every output sample, and the published
// numBins() / latencySamples() / magnitude+reduction+pre dB, must match via a
// bitwise float compare (memcmp) and integer ==. Per CLAUDE.md this comparison is
// a hard gate — if RsCore ever diverges, fix RsCore until bit-exact; NEVER loosen
// the compare. (A genuinely unavoidable FP-ordering divergence would be reported,
// not papered over.)
//
// The RsCore side-channels (Listen node, display smoothing) are driven ONLY
// through an rs_ui::RsFeedFromCore adapter, so a wrong adapter mapping breaks the
// equivalence — the adapter is thereby part of the gate. Its pointer/value
// read-out mapping is additionally asserted directly.
//
// Heap-allocate both the processor and the RsCore (they carry large inline STFT /
// display buffers; several on the 1 MB Windows thread stack would overflow) — the
// same reason preset_test heap-allocates.
//
#include "PluginProcessor.h"
#include "RsCore.h"
#include "ui/RsFeed.h"
#include "ui/RsFeedFromCore.h"

#include "factory_core/testing/DspInvariants.h" // kStandardSampleRates()

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using RS = ResonanceSuppressorAudioProcessor;

    struct Ctx { long long failures = 0; int printed = 0; };
    void reportFail (Ctx& ctx, const std::string& m)
    {
        ++ctx.failures;
        if (ctx.printed < 30) { std::printf ("  FAIL: %s\n", m.c_str()); ++ctx.printed; }
    }

    // Bitwise equality (byte-identical, no tolerance) — the gate. Bit compares
    // sidestep -Wfloat-equal exactly as preset_test's bitEqual does, and are
    // stricter than == (they catch +0/-0 and NaN-payload drift too).
    bool bitEqual  (float a,  float b)  noexcept { return std::memcmp (&a, &b, sizeof (float))  == 0; }
    bool bitEqualD (double a, double b) noexcept { return std::memcmp (&a, &b, sizeof (double)) == 0; }

    // ---- deterministic input generator --------------------------------------
    // A continuous stream (running sample index t keeps the sines phase-coherent
    // across blocks): broadband noise + resonant tones (main: 1.0/1.2 kHz; SC:
    // 3.0/3.5 kHz) + a periodic impulse. The exact signal is irrelevant to the
    // gate (both engines get the SAME floats); it only has to EXERCISE detection,
    // suppression, delta, ballistics, the crossover and the sidechain path.
    struct SignalGen
    {
        std::uint32_t s = 0x9e3779b9u;
        long long t = 0;
        float noise() noexcept
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;           // xorshift32
            return (float) ((double) s / 4294967295.0 * 2.0 - 1.0);
        }
        void block (double fs, int n,
                    std::vector<float>& mL, std::vector<float>& mR,
                    std::vector<float>& sL, std::vector<float>& sR)
        {
            mL.resize ((size_t) n); mR.resize ((size_t) n);
            sL.resize ((size_t) n); sR.resize ((size_t) n);
            const double w = 2.0 * 3.14159265358979323846;
            for (int i = 0; i < n; ++i)
            {
                const double tt = (double) t;
                const float imp = ((t % 4096) == 0) ? 0.8f : 0.0f;
                mL[(size_t) i] = 0.25f * noise() + 0.30f * (float) std::sin (w * 1000.0 * tt / fs) + imp;
                mR[(size_t) i] = 0.25f * noise() + 0.30f * (float) std::sin (w * 1200.0 * tt / fs + 0.5) + imp;
                sL[(size_t) i] = 0.30f * noise() + 0.20f * (float) std::sin (w * 3000.0 * tt / fs);
                sR[(size_t) i] = 0.30f * noise() + 0.20f * (float) std::sin (w * 3500.0 * tt / fs + 0.3);
                ++t;
            }
        }
    };

    // ---- parameter config ---------------------------------------------------
    struct Cfg
    {
        float depth, detail, attack, release, mix, out, tilt, linkAmt;
        bool  delta, link, bypass, scEnable, scListen;
        int   mode, quality, channelMode;
        struct Cut { bool on; float freq; int slope; } lc, hc;
        struct Bnd { bool on; float freq; int type; float sens; float width; } b[8];
        int   listen;   // -1 = off
        float smoothMs; // display smoothing
    };

    void setParam (RS& p, const juce::String& id, float real)
    {
        auto* rp = p.apvts.getParameter (id);
        if (rp != nullptr) rp->setValueNotifyingHost (rp->convertTo0to1 (real));
    }

    // Apply a config: params onto the processor; the two side-channels onto the
    // processor directly AND onto the core THROUGH the feed (so the feed's write
    // mapping is exercised by the equivalence, not just asserted).
    void applyCfg (RS& p, rs_ui::RsFeed& feed, const Cfg& c)
    {
        setParam (p, "depth",   c.depth);
        setParam (p, "detail",  c.detail);
        setParam (p, "attack",  c.attack);
        setParam (p, "release", c.release);
        setParam (p, "mix",     c.mix);
        setParam (p, "out",     c.out);
        setParam (p, "tilt",    c.tilt);
        setParam (p, "linkAmt", c.linkAmt);
        setParam (p, "delta",       c.delta    ? 1.0f : 0.0f);
        setParam (p, "link",        c.link     ? 1.0f : 0.0f);
        setParam (p, "bypass",      c.bypass   ? 1.0f : 0.0f);
        setParam (p, "scEnable",    c.scEnable ? 1.0f : 0.0f);
        setParam (p, "scListen",    c.scListen ? 1.0f : 0.0f);
        setParam (p, "mode",        (float) c.mode);
        setParam (p, "quality",     (float) c.quality);
        setParam (p, "channelMode", (float) c.channelMode);

        setParam (p, RS::cutPid (0, "on"),    c.lc.on ? 1.0f : 0.0f);
        setParam (p, RS::cutPid (0, "freq"),  c.lc.freq);
        setParam (p, RS::cutPid (0, "slope"), (float) c.lc.slope);
        setParam (p, RS::cutPid (1, "on"),    c.hc.on ? 1.0f : 0.0f);
        setParam (p, RS::cutPid (1, "freq"),  c.hc.freq);
        setParam (p, RS::cutPid (1, "slope"), (float) c.hc.slope);
        for (int b = 0; b < 8; ++b)
        {
            setParam (p, RS::bandPid (b, "on"),    c.b[b].on ? 1.0f : 0.0f);
            setParam (p, RS::bandPid (b, "freq"),  c.b[b].freq);
            setParam (p, RS::bandPid (b, "type"),  (float) c.b[b].type);
            setParam (p, RS::bandPid (b, "sens"),  c.b[b].sens);
            setParam (p, RS::bandPid (b, "width"), c.b[b].width);
        }

        p.setListenNode (c.listen);
        feed.setListenNode (c.listen);
        p.setDisplaySmoothMs (c.smoothMs);
        feed.setDisplaySmoothMs (c.smoothMs);
    }

    // Snapshot every value the processor pulls per block, read from the SAME
    // atomics the processor's cached pointers read, with the SAME post-load casts
    // (`(double)` / `> 0.5f` / `(int)`) — so RsCore is fed bit-for-bit what the
    // engine is fed. (Mirrors currentNodes()/processBlock's reads.)
    rs_core::RsParamSnapshot buildSnapshot (RS& p)
    {
        auto raw = [&] (const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); };
        rs_core::RsParamSnapshot s;
        s.depth   = (double) raw ("depth");
        s.detail  = (double) raw ("detail");
        s.attack  = (double) raw ("attack");
        s.release = (double) raw ("release");
        s.mix     = (double) raw ("mix");
        s.tilt    = (double) raw ("tilt");
        s.linkAmt = (double) raw ("linkAmt");
        s.out     = (double) raw ("out");
        s.delta    = raw ("delta")    > 0.5f;
        s.link     = raw ("link")     > 0.5f;
        s.bypass   = raw ("bypass")   > 0.5f;
        s.scEnable = raw ("scEnable") > 0.5f;
        s.scListen = raw ("scListen") > 0.5f;
        s.mode        = (int) raw ("mode");
        s.quality     = (int) raw ("quality");
        s.channelMode = (int) raw ("channelMode");
        s.lowCut.on    = raw (RS::cutPid (0, "on")) > 0.5f;
        s.lowCut.freq  = (double) raw (RS::cutPid (0, "freq"));
        s.lowCut.slope = (int) raw (RS::cutPid (0, "slope"));
        s.highCut.on    = raw (RS::cutPid (1, "on")) > 0.5f;
        s.highCut.freq  = (double) raw (RS::cutPid (1, "freq"));
        s.highCut.slope = (int) raw (RS::cutPid (1, "slope"));
        for (int b = 0; b < 8; ++b)
        {
            s.bands[(size_t) b].on    = raw (RS::bandPid (b, "on")) > 0.5f;
            s.bands[(size_t) b].freq  = (double) raw (RS::bandPid (b, "freq"));
            s.bands[(size_t) b].type  = (int) raw (RS::bandPid (b, "type"));
            s.bands[(size_t) b].sens  = (double) raw (RS::bandPid (b, "sens"));
            s.bands[(size_t) b].width = (double) raw (RS::bandPid (b, "width"));
        }
        return s;
    }

    bool setupBuses (RS& p, bool scConnected)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (juce::AudioChannelSet::stereo()); // main in
        layout.inputBuses.add (scConnected ? juce::AudioChannelSet::stereo()
                                            : juce::AudioChannelSet::disabled()); // sidechain
        layout.outputBuses.add (juce::AudioChannelSet::stereo()); // main out
        return p.setBusesLayout (layout);
    }

    // One processing step: process `numBlocks` blocks of `blockSize`, comparing
    // every output sample + published read-out byte-for-byte.
    void runStep (Ctx& ctx, RS& proc, rs_core::RsCore& core, bool scConnected,
                  const rs_core::RsParamSnapshot& snap, SignalGen& gen, double fs,
                  int blockSize, int numBlocks, const std::string& where)
    {
        const int totalIn  = proc.getTotalNumInputChannels();
        const int totalOut = proc.getTotalNumOutputChannels();
        const int nch = std::max (totalIn, totalOut);
        juce::MidiBuffer midi;
        std::vector<float> mL, mR, sL, sR, coreL, coreR, coreScL, coreScR;

        for (int blk = 0; blk < numBlocks; ++blk)
        {
            gen.block (fs, blockSize, mL, mR, sL, sR);

            // Processor buffer: main in [0,1], sidechain in [2,3] when connected.
            juce::AudioBuffer<float> procBuf (nch, blockSize);
            procBuf.clear();
            std::memcpy (procBuf.getWritePointer (0), mL.data(), sizeof (float) * (size_t) blockSize);
            std::memcpy (procBuf.getWritePointer (1), mR.data(), sizeof (float) * (size_t) blockSize);
            if (scConnected && nch >= 4)
            {
                std::memcpy (procBuf.getWritePointer (2), sL.data(), sizeof (float) * (size_t) blockSize);
                std::memcpy (procBuf.getWritePointer (3), sR.data(), sizeof (float) * (size_t) blockSize);
            }

            // Core buffers: pristine copies of the same input (processBlock is in place).
            coreL = mL; coreR = mR; coreScL = sL; coreScR = sR;

            proc.processBlock (procBuf, midi);
            {
                // Match the processor's internal ScopedNoDenormals FP mode so the
                // comparison is under identical denormal handling. In the CLAP
                // shell this scope lives in the shell, exactly as it lives in the
                // AudioProcessor wrapper here.
                juce::ScopedNoDenormals noDenormals;
                core.process (coreL.data(), coreR.data(),
                              scConnected ? coreScL.data() : nullptr,
                              scConnected ? coreScR.data() : nullptr,
                              blockSize, snap);
            }

            // Output samples: byte-identical.
            const float* pL = procBuf.getReadPointer (0);
            const float* pR = procBuf.getReadPointer (1);
            for (int i = 0; i < blockSize; ++i)
            {
                if (! bitEqual (pL[i], coreL[(size_t) i]))
                    reportFail (ctx, where + " L[" + std::to_string (i) + "] blk " + std::to_string (blk)
                                     + ": proc " + std::to_string (pL[i]) + " vs core " + std::to_string (coreL[(size_t) i]));
                if (! bitEqual (pR[i], coreR[(size_t) i]))
                    reportFail (ctx, where + " R[" + std::to_string (i) + "] blk " + std::to_string (blk)
                                     + ": proc " + std::to_string (pR[i]) + " vs core " + std::to_string (coreR[(size_t) i]));
            }

            // Published read-outs: latency, live bin count, and the three dB spectra.
            if (proc.getLatencySamples() != core.latencySamples())
                reportFail (ctx, where + " latency: proc " + std::to_string (proc.getLatencySamples())
                                 + " vs core " + std::to_string (core.latencySamples()));
            if (proc.binsForDisplay() != core.numBins())
                reportFail (ctx, where + " numBins: proc " + std::to_string (proc.binsForDisplay())
                                 + " vs core " + std::to_string (core.numBins()));

            const int bins = core.numBins();
            const std::atomic<float>* cMag = core.magnitudeDb();
            const std::atomic<float>* cPre = core.magnitudePreDb();
            const std::atomic<float>* cRed = core.reductionDb();
            for (int k = 0; k < bins; ++k)
            {
                if (! bitEqual (proc.displayMagDb (k),    cMag[k].load (std::memory_order_relaxed)))
                    reportFail (ctx, where + " magDb[" + std::to_string (k) + "]");
                if (! bitEqual (proc.displayMagPreDb (k), cPre[k].load (std::memory_order_relaxed)))
                    reportFail (ctx, where + " magPreDb[" + std::to_string (k) + "]");
                if (! bitEqual (proc.displayRedDb (k),    cRed[k].load (std::memory_order_relaxed)))
                    reportFail (ctx, where + " redDb[" + std::to_string (k) + "]");
            }
        }
    }

    // ---- configs ------------------------------------------------------------
    Cfg cfgBase()
    {
        Cfg c {};
        c.depth = 40; c.detail = 60; c.attack = 15; c.release = 80; c.mix = 90;
        c.out = 0; c.tilt = 20; c.linkAmt = 80;
        c.delta = false; c.link = true; c.bypass = false; c.scEnable = false; c.scListen = false;
        c.mode = 0; c.quality = 1; c.channelMode = 0;
        c.lc = { true, 400.0f, 2 }; c.hc = { true, 15000.0f, 2 };
        for (auto& b : c.b) b = { false, 1000.0f, 0, 0.0f, 0.50f };
        c.b[0] = { true, 1000.0f, 0, 3.0f, 0.50f };   // bell +3
        c.b[2] = { true, 5000.0f, 0, 6.0f, 0.75f };   // bell +6, wider
        c.listen = -1; c.smoothMs = 50.0f;
        return c;
    }

    // Hard mode, Mid/Side, link off, external sidechain, output trim, different nodes.
    Cfg cfgHardMS (const Cfg& base)
    {
        Cfg c = base;
        c.depth = 70; c.detail = 30; c.attack = 5; c.release = 150; c.mix = 100;
        c.out = 6; c.tilt = -40; c.linkAmt = 100;
        c.link = false; c.mode = 1; c.channelMode = 1; c.scEnable = true;
        c.lc = { true, 200.0f, 3 }; c.hc = { true, 18000.0f, 1 };
        for (auto& b : c.b) b = { false, 1000.0f, 0, 0.0f, 0.50f };
        c.b[1] = { true, 2500.0f, 1, -6.0f, 0.60f };  // low shelf
        c.b[3] = { true, 8000.0f, 2,  4.0f, 0.90f };  // high shelf
        c.b[5] = { true,  500.0f, 4,  8.0f, 1.20f };  // band reject, wide
        return c;
    }

    struct StepSpec { int blockSize, numBlocks; };
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager for async param updates
    Ctx ctx;

    std::printf ("rscore_equiv: byte-identical RsCore vs ResonanceSuppressorAudioProcessor\n");

    for (double fs : factory_core::testing::kStandardSampleRates())
    {
        // ------------------------------------------------------------------ SC connected
        {
            auto proc = std::make_unique<RS>();
            auto core = std::make_unique<rs_core::RsCore>();
            rs_ui::RsFeedFromCore feed (*core);

            if (! setupBuses (*proc, /*scConnected*/ true))
                reportFail (ctx, "setBusesLayout(sc on) failed @ " + std::to_string ((int) fs));

            const Cfg base = cfgBase();
            applyCfg (*proc, feed, base);            // step-1 params BEFORE prepare so the
            proc->prepareToPlay (fs, 2048);          // out-trim seed matches (no startup ramp)
            core->prepare (fs, 2048);

            SignalGen gen;
            const std::string at = "fs" + std::to_string ((int) fs) + " sc-on";

            // 1. base (Normal / Soft / Stereo / internal detect)
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 1024, 3, at + " base");
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 512,  2, at + " base");

            // 2. Hard / Mid-Side / link off / external sidechain / out=+6 ramp
            Cfg hard = cfgHardMS (base);
            applyCfg (*proc, feed, hard);
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 1024, 3, at + " hardMS");
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 300,  2, at + " hardMS-odd");

            // 3. Quality -> High (mid-stream switch) + Delta on
            Cfg hi = hard; hi.quality = 2; hi.delta = true;
            applyCfg (*proc, feed, hi);
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 1024, 4, at + " highQ+delta");

            // 4. Bypass on (latency held, engine keeps running)
            Cfg byp = hi; byp.bypass = true;
            applyCfg (*proc, feed, byp);
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 1024, 2, at + " bypass");
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 256,  2, at + " bypass");

            // 5. Bypass off + Listen solo (band 0) + display smoothing change (via feed)
            Cfg lis = hi; lis.bypass = false; lis.listen = 2; lis.smoothMs = 120.0f;
            applyCfg (*proc, feed, lis);
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 512, 3, at + " listen+smooth");

            // 6. Quality -> Fast (switch down) + SC Listen + tilt extreme + cuts off + tiny blocks
            Cfg fast = base; fast.quality = 0; fast.tilt = 100; fast.scEnable = true; fast.scListen = true;
            fast.listen = -1; fast.smoothMs = 30.0f; fast.lc.on = false; fast.hc.on = false;
            fast.b[4] = { true, 150.0f, 0, -5.0f, 0.40f };
            fast.b[6] = { true, 3000.0f, 5, 7.0f, 1.50f }; // tilt-type band
            applyCfg (*proc, feed, fast);
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 1024, 3, at + " fastQ+scListen");
            runStep (ctx, *proc, *core, true, buildSnapshot (*proc), gen, fs, 1, 4, at + " tiny-blocks");

            // ---- RsFeedFromCore read-out mapping (pointers + values) ----
            if (feed.bins() != core->numBins())            reportFail (ctx, at + " feed.bins mismatch");
            if (! bitEqualD (feed.sampleRate(), core->sampleRate())) reportFail (ctx, at + " feed.sampleRate mismatch");
            if (feed.magDb()    != core->magnitudeDb())    reportFail (ctx, at + " feed.magDb ptr mismatch");
            if (feed.magPreDb() != core->magnitudePreDb()) reportFail (ctx, at + " feed.magPreDb ptr mismatch");
            if (feed.redDb()    != core->reductionDb())    reportFail (ctx, at + " feed.redDb ptr mismatch");
            if (feed.latencySamples() != core->latencySamples()) reportFail (ctx, at + " feed.latency mismatch");
            // Transitive byte-identity through the feed for a few bins.
            for (int k = 0; k < core->numBins(); k += std::max (1, core->numBins() / 8))
                if (! bitEqual (feed.magDb()[k].load(), proc->displayMagDb (k)))
                    reportFail (ctx, at + " feed.magDb[" + std::to_string (k) + "] != processor");
            // Listen-node write hook round-trips through the feed.
            feed.setListenNode (3);
            if (feed.getListenNode() != 3 || core->getListenNode() != 3)
                reportFail (ctx, at + " feed.setListenNode did not reach the core");
            feed.setListenNode (-1);
        }

        // ------------------------------------------------------------------ SC not connected
        {
            auto proc = std::make_unique<RS>();
            auto core = std::make_unique<rs_core::RsCore>();
            rs_ui::RsFeedFromCore feed (*core);

            if (! setupBuses (*proc, /*scConnected*/ false))
                reportFail (ctx, "setBusesLayout(sc off) failed @ " + std::to_string ((int) fs));

            const Cfg base = cfgBase();
            applyCfg (*proc, feed, base);
            proc->prepareToPlay (fs, 2048);
            core->prepare (fs, 2048);

            SignalGen gen;
            const std::string at = "fs" + std::to_string ((int) fs) + " sc-off";

            runStep (ctx, *proc, *core, false, buildSnapshot (*proc), gen, fs, 1024, 2, at + " base");
            runStep (ctx, *proc, *core, false, buildSnapshot (*proc), gen, fs, 512,  2, at + " base");

            // scEnable=true but the bus is DISABLED -> internal-detection fallback.
            Cfg hard = cfgHardMS (base); // scEnable = true here
            applyCfg (*proc, feed, hard);
            runStep (ctx, *proc, *core, false, buildSnapshot (*proc), gen, fs, 1024, 2, at + " sc-fallback");
            runStep (ctx, *proc, *core, false, buildSnapshot (*proc), gen, fs, 384,  2, at + " sc-fallback-odd");

            // Quality High + Listen + smoothing
            Cfg lis = hard; lis.quality = 2; lis.listen = 1; lis.smoothMs = 90.0f;
            applyCfg (*proc, feed, lis);
            runStep (ctx, *proc, *core, false, buildSnapshot (*proc), gen, fs, 512, 2, at + " highQ+listen");

            // Quality Fast + Delta
            Cfg fast = base; fast.quality = 0; fast.delta = true; fast.listen = -1;
            applyCfg (*proc, feed, fast);
            runStep (ctx, *proc, *core, false, buildSnapshot (*proc), gen, fs, 1024, 2, at + " fastQ+delta");
        }
    }

    if (ctx.failures == 0) { std::printf ("OK: RsCore is byte-identical to the shipping processor.\n"); return 0; }
    std::printf ("FAILED: %lld byte-level mismatch(es).\n", ctx.failures);
    return 1;
}
