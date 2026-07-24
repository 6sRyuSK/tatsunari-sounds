//
// deqcore_equiv_test.cpp — the byte-identical DRIFT GUARD between the shipping
// JUCE DynamicEqAudioProcessor and the extracted, framework-free deq_core::DeqCore.
//
// DeqCore is a faithful CODE MOTION of the processor's prepareToPlay + processBlock
// math, reused verbatim by the CLAP shell. This test proves it reproduces the
// shipping processor EXACTLY: it drives the REAL processor (the oracle) and a
// DeqCore side by side with IDENTICAL input (noise + impulses) and IDENTICAL
// parameters (the APVTS params set to the same values the DeqParamSnapshot carries),
// across the full sample-rate matrix and a spread of block sizes (incl. non-multiples
// of the 32-sample smoothing chunk, and 1), exercising: bands on/off, all 5 band
// types, all 5 channel modes (Stereo/L/R/M/S), all 8 HP/LP slopes, dynamics on/off
// (attack/release ballistics + the 32-sample control-rate coeff update), the
// exclusive Listen/solo, global bypass, per-block automation (so the 72 Freq/Gain/Q
// smoothers actually ramp and the sub-block boundary is crossed), and a mid-stream
// type/channel switch (which resets a band's filter state).
//
// The gate is BYTE-IDENTICAL, no tolerance: every output sample, every published
// per-band live gain, and the pre/post analyzer rings must match via a bitwise float
// compare (memcmp). Per CLAUDE.md this is a hard gate — if DeqCore ever diverges, fix
// DeqCore until bit-exact; NEVER loosen the compare.
//
// Heap-allocate both the processor and the DeqCore (they carry large inline analyzer
// rings + 24 bands of state; several on the 1 MB Windows thread stack would overflow),
// the same reason preset_test heap-allocates.
//
#include "PluginProcessor.h"
#include "DeqCore.h"

#include "factory_core/testing/DspInvariants.h" // kStandardSampleRates()

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using DEQ = DynamicEqAudioProcessor;
    constexpr int kNumBands = deq_core::kNumBands; // 24

    struct Ctx { long long failures = 0; int printed = 0; };
    void reportFail (Ctx& ctx, const std::string& m)
    {
        ++ctx.failures;
        if (ctx.printed < 30) { std::printf ("  FAIL: %s\n", m.c_str()); ++ctx.printed; }
    }

    // Bitwise equality (byte-identical, no tolerance) — the gate. Sidesteps
    // -Wfloat-equal exactly as preset_test's bitEqual does, and is stricter than ==
    // (it catches +0/-0 and NaN-payload drift too).
    bool bitEqual (float a, float b) noexcept { return std::memcmp (&a, &b, sizeof (float)) == 0; }

    // ---- deterministic input generator --------------------------------------
    // A continuous stream (running sample index t keeps the sines phase-coherent
    // across blocks): broadband noise + resonant tones + a periodic impulse. The
    // exact signal is irrelevant to the gate (both engines get the SAME floats); it
    // only has to EXERCISE the filters, the dynamics detectors, and the ballistics.
    struct SignalGen
    {
        std::uint32_t s = 0x9e3779b9u;
        long long t = 0;
        float noise() noexcept
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5; // xorshift32
            return (float) ((double) s / 4294967295.0 * 2.0 - 1.0);
        }
        void block (double fs, int n, std::vector<float>& L, std::vector<float>& R)
        {
            L.resize ((size_t) n); R.resize ((size_t) n);
            const double w = 2.0 * 3.14159265358979323846;
            for (int i = 0; i < n; ++i)
            {
                const double tt = (double) t;
                const float imp = ((t % 4096) == 0) ? 0.8f : 0.0f;
                L[(size_t) i] = 0.25f * noise() + 0.30f * (float) std::sin (w * 1000.0 * tt / fs) + imp;
                R[(size_t) i] = 0.25f * noise() + 0.30f * (float) std::sin (w * 1200.0 * tt / fs + 0.5) + imp;
                ++t;
            }
        }
    };

    // ---- parameter config ---------------------------------------------------
    struct BndCfg
    {
        bool  on = false, byp = false, lsn = false, dyn = false;
        int   chan = 0, type = 0, slope = 0;
        float freq = 1000.0f, gain = 0.0f, q = 0.707f,
              thr = -24.0f, rng = 0.0f, atk = 10.0f, rel = 120.0f, knee = 6.0f;
    };
    struct Cfg
    {
        std::array<BndCfg, (size_t) kNumBands> b {};
        bool bypass = false;
    };

    void setParam (DEQ& p, const juce::String& id, float real)
    {
        auto* rp = p.apvts.getParameter (id);
        if (rp != nullptr) rp->setValueNotifyingHost (rp->convertTo0to1 (real));
    }

    void applyCfg (DEQ& p, const Cfg& c)
    {
        for (int b = 0; b < kNumBands; ++b)
        {
            const auto& bc = c.b[(size_t) b];
            setParam (p, DEQ::pid (b, "on"),    bc.on  ? 1.0f : 0.0f);
            setParam (p, DEQ::pid (b, "byp"),   bc.byp ? 1.0f : 0.0f);
            setParam (p, DEQ::pid (b, "lsn"),   bc.lsn ? 1.0f : 0.0f);
            setParam (p, DEQ::pid (b, "dyn"),   bc.dyn ? 1.0f : 0.0f);
            setParam (p, DEQ::pid (b, "chan"),  (float) bc.chan);
            setParam (p, DEQ::pid (b, "type"),  (float) bc.type);
            setParam (p, DEQ::pid (b, "slope"), (float) bc.slope);
            setParam (p, DEQ::pid (b, "freq"),  bc.freq);
            setParam (p, DEQ::pid (b, "gain"),  bc.gain);
            setParam (p, DEQ::pid (b, "q"),     bc.q);
            setParam (p, DEQ::pid (b, "thr"),   bc.thr);
            setParam (p, DEQ::pid (b, "rng"),   bc.rng);
            setParam (p, DEQ::pid (b, "atk"),   bc.atk);
            setParam (p, DEQ::pid (b, "rel"),   bc.rel);
            setParam (p, DEQ::pid (b, "knee"),  bc.knee);
        }
        setParam (p, "bypass", c.bypass ? 1.0f : 0.0f);
    }

    // Snapshot every value processBlock pulls per band, read from the SAME atomics
    // the processor's cached pointers read, with the SAME post-load casts
    // (`> 0.5f` / `(int)` / `(double)`) — so DeqCore is fed bit-for-bit what the
    // bands are fed.
    deq_core::DeqParamSnapshot buildSnapshot (DEQ& p)
    {
        auto raw = [&] (const juce::String& id) { return p.apvts.getRawParameterValue (id)->load(); };
        deq_core::DeqParamSnapshot s;
        for (int b = 0; b < kNumBands; ++b)
        {
            auto& bs = s.bands[(size_t) b];
            bs.on    = raw (DEQ::pid (b, "on"))  > 0.5f;
            bs.byp   = raw (DEQ::pid (b, "byp")) > 0.5f;
            bs.lsn   = raw (DEQ::pid (b, "lsn")) > 0.5f;
            bs.dyn   = raw (DEQ::pid (b, "dyn")) > 0.5f;
            bs.chan  = (int) raw (DEQ::pid (b, "chan"));
            bs.type  = (int) raw (DEQ::pid (b, "type"));
            bs.slope = (int) raw (DEQ::pid (b, "slope"));
            bs.freq  = (double) raw (DEQ::pid (b, "freq"));
            bs.gain  = (double) raw (DEQ::pid (b, "gain"));
            bs.q     = (double) raw (DEQ::pid (b, "q"));
            bs.thr   = (double) raw (DEQ::pid (b, "thr"));
            bs.rng   = (double) raw (DEQ::pid (b, "rng"));
            bs.atk   = (double) raw (DEQ::pid (b, "atk"));
            bs.rel   = (double) raw (DEQ::pid (b, "rel"));
            bs.knee  = (double) raw (DEQ::pid (b, "knee"));
        }
        s.bypass = raw ("bypass") > 0.5f;
        return s;
    }

    // Compare the pre/post analyzer rings (a window of the most recent samples).
    void compareRings (Ctx& ctx, const DEQ& proc, const deq_core::DeqCore& core,
                       const std::string& where, int blk)
    {
        constexpr int kN = 2048;
        std::vector<float> pPre (kN), pPost (kN), cPre (kN), cPost (kN);
        proc.copyAnalyzerSamples (pPre.data(),  kN, false);
        proc.copyAnalyzerSamples (pPost.data(), kN, true);
        core.copyAnalyzerSamples (cPre.data(),  kN, false);
        core.copyAnalyzerSamples (cPost.data(), kN, true);
        for (int i = 0; i < kN; ++i)
        {
            if (! bitEqual (pPre[(size_t) i], cPre[(size_t) i]))
            { reportFail (ctx, where + " ringPre[" + std::to_string (i) + "] blk " + std::to_string (blk)); break; }
            if (! bitEqual (pPost[(size_t) i], cPost[(size_t) i]))
            { reportFail (ctx, where + " ringPost[" + std::to_string (i) + "] blk " + std::to_string (blk)); break; }
        }
    }

    void compareLiveGains (Ctx& ctx, const DEQ& proc, const deq_core::DeqCore& core,
                           const std::string& where, int blk)
    {
        for (int b = 0; b < kNumBands; ++b)
            if (! bitEqual (proc.getLiveGainDb (b), core.liveGainDb (b)))
            { reportFail (ctx, where + " liveGainDb[" + std::to_string (b) + "] blk " + std::to_string (blk)); break; }
    }

    // One processing step: process `numBlocks` blocks of `blockSize`, comparing
    // every output sample + published live gains + analyzer rings byte-for-byte.
    void runStep (Ctx& ctx, DEQ& proc, deq_core::DeqCore& core, const deq_core::DeqParamSnapshot& snap,
                  SignalGen& gen, double fs, int blockSize, int numBlocks, const std::string& where)
    {
        juce::MidiBuffer midi;
        std::vector<float> mL, mR, coreL, coreR;
        for (int blk = 0; blk < numBlocks; ++blk)
        {
            gen.block (fs, blockSize, mL, mR);

            juce::AudioBuffer<float> procBuf (2, blockSize);
            std::memcpy (procBuf.getWritePointer (0), mL.data(), sizeof (float) * (size_t) blockSize);
            std::memcpy (procBuf.getWritePointer (1), mR.data(), sizeof (float) * (size_t) blockSize);
            coreL = mL; coreR = mR;

            proc.processBlock (procBuf, midi);
            {
                juce::ScopedNoDenormals noDenormals; // match the processor's FP mode
                core.process (coreL.data(), coreR.data(), blockSize, snap);
            }

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
            compareLiveGains (ctx, proc, core, where, blk);
            compareRings (ctx, proc, core, where, blk);
        }
    }

    // Equivalence UNDER AUTOMATION: mutate parameters EVERY block so the 72 Freq/
    // Gain/Q smoothers ramp continuously and the 32-sample sub-block boundary is
    // crossed under changing targets, plus a moving band toggle / type / channel
    // (channel changes reset a band's filter state — both engines must reset in
    // lockstep). Same byte-exact compare as runStep.
    void runAutomation (Ctx& ctx, DEQ& proc, deq_core::DeqCore& core, SignalGen& gen, double fs,
                        int blockSize, int numBlocks, Cfg cfg, const std::string& where)
    {
        juce::MidiBuffer midi;
        std::vector<float> mL, mR, coreL, coreR;
        for (int blk = 0; blk < numBlocks; ++blk)
        {
            const int band = blk % kNumBands;
            auto& bc = cfg.b[(size_t) band];
            bc.on    = true;
            bc.freq  = 80.0f + (float) ((blk * 137) % 15000);
            bc.gain  = (float) (((blk * 7) % 41) - 20);
            bc.q     = 0.2f + (float) (blk % 12) * 1.4f;
            bc.type  = (blk / 2) % 5;                  // cycle all 5 types
            bc.chan  = (blk / 3) % 5;                  // cycle all 5 channel modes (resets filters)
            bc.slope = blk % 8;                        // cycle all 8 slopes
            bc.dyn   = ((blk % 2) == 0);
            bc.thr   = -40.0f + (float) (blk % 30);
            bc.rng   = (float) (((blk * 3) % 41) - 20);
            cfg.b[(size_t) ((band + 5) % kNumBands)].on = ((blk % 2) == 0); // moving toggle
            cfg.b[(size_t) ((band + 9) % kNumBands)].lsn = (blk % 17 == 16); // occasional solo
            applyCfg (proc, cfg);
            const auto snap = buildSnapshot (proc);

            gen.block (fs, blockSize, mL, mR);
            juce::AudioBuffer<float> procBuf (2, blockSize);
            std::memcpy (procBuf.getWritePointer (0), mL.data(), sizeof (float) * (size_t) blockSize);
            std::memcpy (procBuf.getWritePointer (1), mR.data(), sizeof (float) * (size_t) blockSize);
            coreL = mL; coreR = mR;

            proc.processBlock (procBuf, midi);
            {
                juce::ScopedNoDenormals noDenormals;
                core.process (coreL.data(), coreR.data(), blockSize, snap);
            }

            const float* pL = procBuf.getReadPointer (0);
            const float* pR = procBuf.getReadPointer (1);
            for (int i = 0; i < blockSize; ++i)
            {
                if (! bitEqual (pL[i], coreL[(size_t) i]))
                { reportFail (ctx, where + " L[" + std::to_string (i) + "] blk " + std::to_string (blk)); break; }
                if (! bitEqual (pR[i], coreR[(size_t) i]))
                { reportFail (ctx, where + " R[" + std::to_string (i) + "] blk " + std::to_string (blk)); break; }
            }
            compareLiveGains (ctx, proc, core, where, blk);
            compareRings (ctx, proc, core, where, blk);
        }
    }

    // ---- configs ------------------------------------------------------------
    Cfg cfgStatic()
    {
        Cfg c {};
        c.b[0] = { true, false, false, false, 0, 0, 0, 1000.0f, 4.0f, 1.5f,  -24, 0, 10, 120, 6 };   // Bell +4 stereo
        c.b[1] = { true, false, false, false, 0, 1, 0,  120.0f, -3.0f, 0.7f, -24, 0, 10, 120, 6 };   // Low shelf -3
        c.b[2] = { true, false, false, false, 0, 2, 0, 10000.0f, 5.0f, 0.7f, -24, 0, 10, 120, 6 };   // High shelf +5
        c.b[3] = { true, false, false, false, 0, 3, 3,   60.0f, 0.0f, 0.707f, -24, 0, 10, 120, 6 };  // HP 48 dB/oct
        c.b[4] = { true, false, false, false, 0, 4, 5, 16000.0f, 0.0f, 0.707f, -24, 0, 10, 120, 6 }; // LP 72 dB/oct
        return c;
    }

    // Dynamics + channel-mode coverage: dynamic bells on L/R/M/S, plus a bypassed
    // present band (byp) so the "present-but-bypassed" branch is exercised.
    Cfg cfgDynChannels()
    {
        Cfg c {};
        c.b[0] = { true, false, false, true, 1, 0, 0,  800.0f, 0.0f, 2.0f, -30, -6, 5,  80, 3 };  // dyn bell, Left
        c.b[1] = { true, false, false, true, 2, 0, 0, 2500.0f, 0.0f, 3.0f, -28,  4, 20, 200, 9 }; // dyn bell, Right
        c.b[2] = { true, false, false, true, 3, 0, 0, 5000.0f, 0.0f, 4.0f, -35, -8, 8, 150, 0 };  // dyn bell, Mid, hard knee
        c.b[3] = { true, false, false, true, 4, 0, 0, 7000.0f, 0.0f, 4.0f, -25,  3, 3, 300, 12 }; // dyn bell, Side
        c.b[4] = { true, true,  false, false, 0, 0, 0, 3000.0f, 6.0f, 1.0f, -24, 0, 10, 120, 6 }; // present-but-bypassed
        return c;
    }

    struct StepSpec { int blockSize, numBlocks; };
}

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit; // MessageManager for async param updates
    Ctx ctx;

    std::printf ("deqcore_equiv: byte-identical DeqCore vs DynamicEqAudioProcessor\n");

    for (double fs : factory_core::testing::kStandardSampleRates())
    {
        const std::string at = "fs" + std::to_string ((int) fs);

        auto proc = std::make_unique<DEQ>();
        auto core = std::make_unique<deq_core::DeqCore>();

        // Config BEFORE prepare so the smoother seed matches (no startup ramp): the
        // processor seeds its smoothers in prepareToPlay from the live params, DeqCore
        // seeds on its first process() block from the first snapshot.
        const Cfg base = cfgStatic();
        applyCfg (*proc, base);
        proc->prepareToPlay (fs, 1024);
        core->prepare (fs, 1024);

        SignalGen gen;

        // 1. static config: bells/shelves/HP/LP, multiple block sizes incl. non-32.
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 1024, 3, at + " static");
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 512,  2, at + " static-512");
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 300,  2, at + " static-300");
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 1,    8, at + " static-1sample");

        // 2. dynamics + all channel modes + a present-but-bypassed band.
        Cfg dyn = cfgDynChannels();
        applyCfg (*proc, dyn);
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 1024, 4, at + " dyn-channels");
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 33,   3, at + " dyn-channels-33");

        // 3. Listen/solo (band 0) — the exclusive solo path (processListen).
        Cfg solo = dyn; solo.b[0].lsn = true;
        applyCfg (*proc, solo);
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 512, 3, at + " solo");

        // 4. Global bypass (L/R untouched, live gains = static gain, ring frozen).
        Cfg byp = base; byp.bypass = true;
        applyCfg (*proc, byp);
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 512, 2, at + " bypass");
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 200, 2, at + " bypass-200");

        // 5. Un-bypass then automation: params move every block (smoothers ramp,
        //    32-sample boundary crossed, band toggles + type/channel switches).
        Cfg off = base; off.bypass = false;
        applyCfg (*proc, off);
        runStep (ctx, *proc, *core, buildSnapshot (*proc), gen, fs, 256, 1, at + " unbypass");
        runAutomation (ctx, *proc, *core, gen, fs, 256, 96, base, at + " automation");
        runAutomation (ctx, *proc, *core, gen, fs, 96,  40, base, at + " automation-odd");
        runAutomation (ctx, *proc, *core, gen, fs, 1,   32, base, at + " automation-1sample");
    }

    if (ctx.failures == 0) { std::printf ("OK: DeqCore is byte-identical to the shipping processor.\n"); return 0; }
    std::printf ("FAILED: %lld byte-level mismatch(es).\n", ctx.failures);
    return 1;
}
