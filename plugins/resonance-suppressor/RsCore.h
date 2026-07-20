#pragma once
//
// RsCore.h — the resonance-suppressor block-processing core, extracted as a
// plain C++ unit with NO plugin-framework dependency so both a future CLAP shell
// and the equivalence oracle can drive the exact same DSP. It is a faithful CODE
// MOTION of PluginProcessor.cpp's prepareToPlay + processBlock math: it owns the
// same factory_core engine and the same output-trim ramp, composes the same core
// primitives (so every regression gate that already covers MultiResSuppressor
// still holds), and publishes the same lock-free display spectra with the same
// semantics the RsFeed contract expects. The shipping AudioProcessor is left
// UNCHANGED — this is a parallel unit the CLAP shell reuses, not a refactor of
// the wrapper.
//
// The caller SNAPSHOTS every per-block parameter into RsParamSnapshot (plain
// scalars / enums — no framework types, no atomics) and hands it to process().
// The listen-node and display-smoothing setters mirror the processor's
// non-parameter, lock-free GUI->audio side-channel (setListenNode /
// setDisplaySmoothMs), so an RsFeed can wrap an RsCore 1:1.
//
// Header-only, dependency-free at the framework level: it composes only
// factory_core primitives plus the shared, framework-free detail-macro math in
// Source/DetailParam.h (the SAME single definition the shipping processor uses,
// so the two can never drift). Real-time safe: prepare() sizes every buffer up
// front; process() never allocates, locks, or makes a syscall.
//
// FAITHFULNESS MAP (call-site line references are into PluginProcessor.cpp as of
// this extraction): prepare() mirrors prepareToPlay (l.122-149); process()
// mirrors processBlock's per-block engine configuration (l.248-303), the
// output-trim target (l.321-323), the per-sample loop (l.325-337), the Quality
// latency renegotiation (l.343-348), and the display-spectra publish (l.353-362).
// rasterizeProfile / rasterizeListenProfile / nodesFromSnapshot / slopeValue are
// transcribed from their processor counterparts (l.20-42, l.108-120, l.173-226).
//
#include "factory_core/MultiResSuppressor.h"
#include "factory_core/ReductionProfile.h"
#include "factory_core/StftResolution.h"
#include "factory_core/LinearRamp.h"

#include "Source/DetailParam.h" // shared, framework-free detail-macro + depth math

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

namespace rs_core
{
    // Buffer-sizing constants, transcribed from PluginProcessor.h (kBaseFftOrder /
    // kMaxFftOrder / kRefSampleRate / kMaxBins / kNumBands). Every buffer covers
    // the largest window High quality can reach (order+1, capped at 14), so a
    // Quality switch never reallocates.
    inline constexpr int    kBaseFftOrder  = 11;      // N = 2048 at the 48 kHz reference (Normal)
    inline constexpr int    kMaxFftOrder   = 14;      // N = 16384 (High quality at 176.4/192 kHz)
    inline constexpr double kRefSampleRate = 48000.0;
    inline constexpr int    kMaxBins       = (1 << kMaxFftOrder) / 2 + 1; // 8193
    inline constexpr int    kNumBands      = 8;       // + a low cut and a high cut

    // Every value processBlock pulls per block, snapshotted by the caller. Fields
    // are stored in the SAME post-load form the processor's atomic reads produce
    // (a float widened to double, a `> 0.5f` truth test, or an `(int)` cast), and
    // the downstream arithmetic (/100, pow(10,·/20), enum cast, slope lookup) is
    // reproduced verbatim inside RsCore, so a snapshot taken from the live
    // parameters feeds RsCore bit-for-bit what the processor feeds its engine.
    struct RsParamSnapshot
    {
        // (double) *Param->load()
        double depth   = 0.0;   // %, engineDepthForPct
        double detail  = 50.0;  // %, drives sharp/selectivity/smoothing macro
        double attack  = 20.0;  // ms
        double release = 65.0;  // ms
        double mix     = 100.0; // %, engine gets mix/100
        double tilt    = 0.0;   // %, engine gets tilt/100
        double linkAmt = 100.0; // %, engine gets linkAmt/100
        double out     = 0.0;   // dB, trim gain = pow(10, out/20)

        // *Param->load() > 0.5f
        bool delta    = false;
        bool link     = true;
        bool bypass   = false;
        bool scEnable = false;
        bool scListen = false;

        // (int) *Param->load()
        int mode        = 0; // 0 Soft, 1 Hard (hardMode == mode == 1)
        int quality     = 1; // 0 Fast, 1 Normal, 2 High
        int channelMode = 0; // 0 Stereo, 1 Mid/Side

        struct Cut  { bool on = false; double freq = 1000.0; int slope = 2; };   // slope: choice index 0..3
        struct Band { bool on = false; double freq = 1000.0; int type = 0;       // type: ReductionBandType index
                      double sens = 0.0; double width = 0.50; };

        Cut lowCut, highCut;
        std::array<Band, kNumBands> bands {};
    };

    class RsCore
    {
    public:
        RsCore() = default;

        // Mirror prepareToPlay (l.122-149): pick the Normal-quality order for this
        // rate (the engine allocates one order higher for High), prepare the
        // engine, latch the ~20 ms output-trim ramp length, seed the display
        // smoothing at 50 ms, and reset the published spectra + live bin/latency
        // read-outs. samplesPerBlock is accepted for wrapper symmetry but unused —
        // the engine sizes by FFT order, exactly like the processor (which ignores
        // its samplesPerBlock argument). Allocation happens here, never in process.
        void prepare (double sampleRate, int /*samplesPerBlock*/)
        {
            currentSampleRate = sampleRate;
            currentFftOrder   = factory_core::fftOrderForSampleRate (sampleRate, kBaseFftOrder, kRefSampleRate, kMaxFftOrder);
            suppressor.prepare (sampleRate, currentFftOrder);

            // Output-trim ramp length only (~20 ms, continuous-param rule); the
            // ramp is SEEDED on the first process() block from that block's own
            // bypass/out target (see outTrimPrimed) — the code-motion of
            // prepareToPlay's setCurrentAndTargetValue seed, which read the live
            // params prepare() deliberately does not carry. With params stable
            // across prepare -> first block (the wrapper's normal case) this
            // reproduces "playback never starts with a trim ramp" exactly.
            outTrim.reset (sampleRate, 0.02);
            outTrimPrimed = false;

            suppressor.setDisplaySmoothingMs (50.0);
            reportedLatency = suppressor.latencySamples();
            activeBins      = suppressor.numBins();
            for (auto& a : pubMag)    a.store (-120.0f, std::memory_order_relaxed);
            for (auto& a : pubRed)    a.store (   0.0f, std::memory_order_relaxed);
            for (auto& a : pubMagPre) a.store (-120.0f, std::memory_order_relaxed);
            // Invalidate the profile cache: the engine's profile is all-1.0 after
            // prepare and the grid may have changed, so the first block must bake.
            profileCacheValid = false;
        }

        // Mirror processBlock's inner work on caller-owned buffers. L/R is the main
        // signal (processed in place); scL/scR is the sidechain (nullptr == not
        // connected, exactly the processor's `scConnected = scChannels > 0`). The
        // caller handles bus extraction / output-channel clearing (the framework
        // housekeeping in processBlock l.232-240 / l.306-313) — RsCore only owns
        // the DSP. Real-time safe.
        void process (float* L, float* R, const float* scL, const float* scR,
                      int numSamples, const RsParamSnapshot& s) noexcept
        {
            const bool scConnected = (scL != nullptr); // processor: scChannels > 0 (l.242)

            // Listen solo, loaded once per block (l.248). >= 0 forces mix=1/delta=true
            // for THIS block and outranks SC Listen; < 0 is the normal path.
            const int listen = listenNode.load (std::memory_order_relaxed);

            // Editor-attached gate (perf): mirrors the processor's editorActive. The
            // display publish + display-time smoothing are GUI-only; with no editor
            // attached (CLAP GUI closed) both are skipped, DSP untouched. Read once.
            const bool showDisplay = displayActive.load (std::memory_order_relaxed);

            // Live display-smoothing override (l.253): atomic load only. Forced to 0
            // (the engine's cheap unsmoothed display path) while no editor is attached.
            suppressor.setDisplaySmoothingMs (showDisplay ? (double) displaySmoothMsUi.load (std::memory_order_relaxed) : 0.0);
            uiQuality = s.quality;

            // Mode read once (l.257): steers both the Depth mapping and setMode.
            const bool hardMode = (s.mode == 1);
            suppressor.setDepth          (resonance_suppressor_detail::engineDepthForPct (s.depth, hardMode)); // l.261
            const double detail = s.detail;                                                                    // l.265
            suppressor.setSharpness      (resonance_suppressor_detail::sharpOctForDetail (detail));            // l.266
            suppressor.setSelectivity    (resonance_suppressor_detail::selectivityForDetail (detail));         // l.267
            suppressor.setSmoothingWidth (resonance_suppressor_detail::gainSmoothOctForDetail (detail));       // l.268
            suppressor.setTilt        (s.tilt / 100.0);   // l.269 (-100..+100 % -> -1..+1)
            suppressor.setRange       (20.0, 20000.0);    // l.270 (cuts bound processing via the profile)
            suppressor.setTimes       (s.attack, s.release); // l.271
            suppressor.setMix         ((listen >= 0) ? 1.0 : s.mix / 100.0);       // l.275
            suppressor.setDelta       ((listen >= 0) || s.delta);                  // l.276
            suppressor.setStereoLink  (s.link);                                    // l.277
            suppressor.setLinkAmount  (s.linkAmt / 100.0);                         // l.279
            suppressor.setChannelMode (s.channelMode);                             // l.280
            const bool scEnable = s.scEnable;                                      // l.285
            const bool scListen = (listen < 0) && s.scListen;                     // l.286
            suppressor.setSidechain   (scEnable && scConnected);                   // l.287
            suppressor.setScListen    (scListen && scEnable && scConnected);       // l.288
            suppressor.setMode        (hardMode ? 1 : 0);                          // l.289
            suppressor.setQuality     (s.quality);                                 // l.293
            suppressor.setBypassed    (s.bypass);                                  // l.298
            updateProfile (listen, s);                                             // l.302-303 (cached; see below)

            // Output-trim target (l.321-323): unity while bypassed so bypass stays a
            // unity passthrough, else pow(10, out/20). Seed on the first block.
            const double outTarget = s.bypass ? 1.0 : std::pow (10.0, s.out / 20.0);
            if (! outTrimPrimed) { outTrim.setCurrentAndTargetValue (outTarget); outTrimPrimed = true; }
            else                   outTrim.setTargetValue (outTarget);

            for (int i = 0; i < numSamples; ++i) // l.325-337
            {
                double l = L[i];
                double r = (R != nullptr) ? R[i] : l;
                // Read the sidechain BEFORE writing the main output (the SC bus can
                // alias the main buffer): sample it first to key detection off the
                // true input. Absent SC -> fall back to (l, r) so the 4-arg call
                // matches the 2-arg one.
                const double sl = (scL != nullptr) ? (double) scL[i] : l;
                const double sr = (scR != nullptr) ? (double) scR[i] : r;
                suppressor.process (l, r, sl, sr);
                const double og = outTrim.getNextValue();
                L[i] = (float) (l * og);
                if (R != nullptr) R[i] = (float) (r * og);
            }

            // A Quality switch changes the engine's window length N, hence its
            // reported latency and bin grid; renegotiate them when they move
            // (l.343-348). The wrapper additionally calls setLatencySamples() for
            // host PDC — that lives in the shell, not here.
            const int lat = suppressor.latencySamples();
            if (lat != reportedLatency)
            {
                reportedLatency = lat;
                activeBins      = suppressor.numBins();
            }

            // Publish the latest display spectra on the now-current bin grid
            // (l.353-362). Scratch is preallocated at the top order, so this is
            // allocation-free. Skipped when no editor is attached (perf): the three
            // merged-dB reads + per-bin atomic stores are display-only work; the
            // snapshots resume one block after an editor attaches. Mirrors the
            // processor's showDisplay gate so the equivalence gate stays byte-exact.
            if (showDisplay)
            {
                const int bins = suppressor.numBins();
                const double* magDb    = suppressor.magnitudeDb    (magScratch.data());
                const double* redDb    = suppressor.reductionDb    (redScratch.data());
                const double* magPreDb = suppressor.magnitudePreDb (preScratch.data());
                for (int k = 0; k < bins; ++k)
                {
                    pubMag   [(size_t) k].store ((float) magDb   [k], std::memory_order_relaxed);
                    pubRed   [(size_t) k].store ((float) redDb   [k], std::memory_order_relaxed);
                    pubMagPre[(size_t) k].store ((float) magPreDb[k], std::memory_order_relaxed);
                }
            }
        }

        // --- published read-outs (mirror the processor's published semantics) ----
        // The live display bin count / latency track the engine across Quality
        // switches exactly like binsForDisplay() / getLatencySamples().
        int    numBins()        const noexcept { return activeBins; }
        int    latencySamples() const noexcept { return reportedLatency; }
        double sampleRate()     const noexcept { return currentSampleRate; }

        // Per-bin display spectra (length kMaxBins; valid over [0, numBins())),
        // element k read with [k].load(relaxed) — the same lock-free hand-off the
        // processor exposes via pubMag/pubRed/pubMagPre, so an RsFeed returns
        // pointers straight into these.
        const std::atomic<float>* magnitudeDb()    const noexcept { return pubMag.data();    } // post-suppression
        const std::atomic<float>* magnitudePreDb() const noexcept { return pubMagPre.data(); } // pre-gain (input)
        const std::atomic<float>* reductionDb()    const noexcept { return pubRed.data();    } // per-bin reduction, <= 0

        // --- non-parameter side-channel (mirror the processor) -------------------
        // Listen solo: 0 = low cut, 1 = high cut, 2..(1+kNumBands) = bands, -1 = off.
        void setListenNode (int id) noexcept { listenNode.store (id, std::memory_order_relaxed); }
        int  getListenNode() const noexcept  { return listenNode.load (std::memory_order_relaxed); }
        // Analyser display time smoothing (ms, >= 0), applied at the top of process().
        void setDisplaySmoothMs (float ms) noexcept
        {
            displaySmoothMsUi.store (ms < 0.0f ? 0.0f : ms, std::memory_order_relaxed);
        }

        // Editor-attached flag (perf), mirroring the processor's setEditorActive.
        // The CLAP editor sets it true on GUI create() and false on destroy() (via
        // the RsFeed seam). While false -- no GUI -- process() skips the display
        // publish + display-time smoothing (the DSP / audio are untouched, so the
        // byte-equivalence gate holds under both states). Defaults false so a
        // never-opened shell instance pays for no display. GUI thread sets, audio
        // thread reads (lock-free), same discipline as listenNode.
        void setDisplayActive (bool active) noexcept { displayActive.store (active, std::memory_order_relaxed); }
        bool getDisplayActive() const noexcept       { return displayActive.load (std::memory_order_relaxed); }

        // Optional read-out for a UI: the label of the last snapshot's Quality.
        const char* qualityLabel() const noexcept
        {
            switch (uiQuality) { case 0: return "Fast"; case 2: return "High"; default: return "Normal"; }
        }

    private:
        // Slope choice index (0..3) -> dB/oct (transcribed from slopeValue l.20-24).
        static double slopeValue (int index) noexcept
        {
            static constexpr double kSlopes[] = { 6.0, 12.0, 24.0, 48.0 };
            return kSlopes[(size_t) std::clamp (index, 0, 3)];
        }

        // Assemble the node config from the snapshot (transcribed from currentNodes
        // l.108-120: same `> 0.5f` / `(double)` / `(int)`enum / slopeValue casts,
        // now sourced from the caller's snapshot instead of the atomic pointers).
        factory_core::ReductionNodes nodesFromSnapshot (const RsParamSnapshot& s) const noexcept
        {
            factory_core::ReductionNodes n;
            n.lowCut  = { s.lowCut.on,  s.lowCut.freq,  slopeValue (s.lowCut.slope) };
            n.highCut = { s.highCut.on, s.highCut.freq, slopeValue (s.highCut.slope) };
            for (int b = 0; b < kNumBands; ++b)
                n.bands[(size_t) b] = { s.bands[(size_t) b].on,
                                        s.bands[(size_t) b].freq,
                                        (factory_core::ReductionBandType) s.bands[(size_t) b].type,
                                        s.bands[(size_t) b].sens,
                                        s.bands[(size_t) b].width };
            return n;
        }

        // Rebake the reduction profile ONLY when its inputs changed since the last
        // block (perf) -- the byte-identical mirror of the processor's updateProfile.
        // Key = (listen node, reduction nodes, both grids' bin counts); when
        // unchanged the engine retains the previous profile (setProfile copies) and
        // the transcendental sweep is skipped. Comparing the actual rasterise inputs
        // means the cache can never go stale, and using the SAME shared
        // reductionNodesIdentical / grid-bin key as the processor keeps the two
        // caches invalidating in lockstep (the equivalence gate's under-automation
        // case proves it). Bit-identical to rasterising every block.
        void updateProfile (int listen, const RsParamSnapshot& s) noexcept
        {
            const auto nodes  = nodesFromSnapshot (s);
            const int  nLow   = suppressor.numBins();
            const int  nHigh  = suppressor.highEngine().numBins();

            if (profileCacheValid
                && listen == cachedListenNode
                && nLow == cachedLowBins && nHigh == cachedHighBins
                && factory_core::reductionNodesIdentical (nodes, cachedProfileNodes))
                return;

            if (listen >= 0) rasterizeListenProfile (listen, nodes);
            else             rasterizeProfile (nodes);

            cachedListenNode  = listen;
            cachedLowBins     = nLow;
            cachedHighBins    = nHigh;
            cachedProfileNodes = nodes;
            profileCacheValid = true;
        }

        // Rasterise the full node graph onto BOTH engines' grids (transcribed from
        // rasterizeProfile l.173-197). The per-band centre log-frequency is hoisted
        // out of the bin sweep (prepareBandLogF) -- loop-invariant, bit-identical.
        void rasterizeProfile (const factory_core::ReductionNodes& nodes) noexcept
        {
            factory_core::BandLogF bandLogF;
            factory_core::prepareBandLogF (nodes, bandLogF);

            const int nLow = suppressor.numBins();
            profileBuf[0] = 1.0; // DC: nominal
            for (int k = 1; k < nLow; ++k)
                profileBuf[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (suppressor.binToHz (k), nodes, bandLogF);

            const auto& high = suppressor.highEngine();
            const int nHigh = high.numBins();
            profileBufHigh[0] = 1.0;
            for (int k = 1; k < nHigh; ++k)
                profileBufHigh[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (high.binToHz (k), nodes, bandLogF);

            suppressor.setProfile (profileBuf.data(), nLow, profileBufHigh.data(), nHigh);
        }

        // Solo one reduction node onto BOTH grids (transcribed from
        // rasterizeListenProfile l.199-226): same mapping, only that node contributes.
        void rasterizeListenProfile (int nodeId, const factory_core::ReductionNodes& nodes) noexcept
        {
            factory_core::ReductionNodes solo;
            if (nodeId == 0)      solo.lowCut  = nodes.lowCut;
            else if (nodeId == 1) solo.highCut = nodes.highCut;
            else if (nodeId >= 2 && nodeId - 2 < kNumBands)
                solo.bands[(size_t) (nodeId - 2)] = nodes.bands[(size_t) (nodeId - 2)];

            factory_core::BandLogF bandLogF;
            factory_core::prepareBandLogF (solo, bandLogF);

            const int nLow = suppressor.numBins();
            listenProfileLow[0] = 1.0;
            for (int k = 1; k < nLow; ++k)
                listenProfileLow[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (suppressor.binToHz (k), solo, bandLogF);

            const auto& high = suppressor.highEngine();
            const int nHigh = high.numBins();
            listenProfileHigh[0] = 1.0;
            for (int k = 1; k < nHigh; ++k)
                listenProfileHigh[(size_t) k] = factory_core::reductionProfileLinearAtPrepared (high.binToHz (k), solo, bandLogF);

            suppressor.setProfile (listenProfileLow.data(), nLow, listenProfileHigh.data(), nHigh);
        }

        factory_core::MultiResSuppressor suppressor;
        // Output-trim smoother (~20 ms linear ramp on the linear gain): a bit-exact
        // stand-in for the reference framework's linear SmoothedValue<double> the
        // shipping processor uses, so the trimmed output renders identically.
        factory_core::LinearRamp<double> outTrim { 1.0 };
        bool   outTrimPrimed  = false;

        double currentSampleRate = kRefSampleRate;
        int    currentFftOrder   = kBaseFftOrder;
        int    activeBins        = (1 << kBaseFftOrder) / 2 + 1;
        int    reportedLatency   = 0;

        std::array<double, kMaxBins> profileBuf {};        // reduction profile, low (display) grid
        std::array<double, kMaxBins> profileBufHigh {};    // reduction profile, high engine's grid
        std::array<double, kMaxBins> magScratch {};        // merged magnitude display scratch
        std::array<double, kMaxBins> redScratch {};        // merged reduction display scratch
        std::array<double, kMaxBins> preScratch {};        // merged pre-gain magnitude display scratch
        std::array<double, kMaxBins> listenProfileLow {};  // single-node profile, low grid
        std::array<double, kMaxBins> listenProfileHigh {}; // single-node profile, high grid

        std::array<std::atomic<float>, kMaxBins> pubMag {};    // post-suppression magnitude, dB
        std::array<std::atomic<float>, kMaxBins> pubRed {};    // per-bin reduction, dB (<= 0)
        std::array<std::atomic<float>, kMaxBins> pubMagPre {}; // pre-gain (input) magnitude, dB

        std::atomic<int>   listenNode        { -1 };    // -1 = normal processing
        std::atomic<float> displaySmoothMsUi { 50.0f }; // == prepareToPlay's fixed 50 ms until overridden
        std::atomic<bool>  displayActive     { false }; // editor attached? (see setDisplayActive)
        int                uiQuality         { 1 };     // last snapshot Quality (for qualityLabel)

        // Profile-rasterisation cache (perf), byte-identical to the processor's:
        // the key of the last rasterise (listen node / nodes / grid bin counts).
        // `valid` is reset false in prepare() so the first block bakes.
        bool                         profileCacheValid = false;
        int                          cachedListenNode  = -1;
        int                          cachedLowBins     = 0;
        int                          cachedHighBins    = 0;
        factory_core::ReductionNodes cachedProfileNodes {};
    };
} // namespace rs_core
