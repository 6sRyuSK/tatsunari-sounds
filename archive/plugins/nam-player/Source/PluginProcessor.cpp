#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "OfflineReamp.h"

#include "factory_core/PolyphaseResampler.h"

#include <algorithm>
#include <cmath>

namespace
{
    juce::NormalisableRange<float> logFreqRange (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi, 1.0f);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    juce::AudioParameterFloatAttributes dbAttr() { return juce::AudioParameterFloatAttributes().withLabel ("dB"); }

    // Band-limited resample of a mono IR from srcRate to dstRate, capped to maxOut
    // samples. Off the audio thread, so it reuses the anti-alias PolyphaseResampler
    // (linear interpolation would alias a bright cab IR). Its D-input-sample group
    // delay is trimmed off the front so the IR onset stays at sample 0 (the convolver
    // must remain zero-latency); its tail is flushed with D zeros so nothing is lost.
    std::vector<float> resampleIr (const float* src, int lenIn, double srcRate, double dstRate, int maxOut)
    {
        if (lenIn <= 0) return {};
        if (srcRate <= 0.0 || dstRate <= 0.0 || std::abs (srcRate - dstRate) < 1.0e-6)
            return std::vector<float> (src, src + std::min (lenIn, maxOut));

        factory_core::PolyphaseResampler rs;
        rs.prepare (srcRate, dstRate);
        const int D        = rs.groupDelayInputSamples();
        const int dropLead = (int) std::lround ((double) D * dstRate / srcRate);

        std::vector<float> in (src, src + lenIn);
        in.insert (in.end(), (size_t) (D + 2), 0.0f);                 // flush the group delay
        const int cap = (int) std::ceil (in.size() * dstRate / srcRate) + 64;
        std::vector<float> out ((size_t) std::max (1, cap), 0.0f);
        const int m = rs.process (in.data(), (int) in.size(), out.data(), cap);

        std::vector<float> trimmed;
        for (int i = std::min (m, dropLead); i < m && (int) trimmed.size() < maxOut; ++i)
            trimmed.push_back (out[(size_t) i]);
        return trimmed;
    }
}

juce::String NamPlayerAudioProcessor::slotPid (int s, const char* suffix)
{
    return "slot" + juce::String (s) + "_" + suffix;
}

juce::AudioProcessorValueTreeState::ParameterLayout
NamPlayerAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    for (int k = 0; k < kNumSlots; ++k)
    {
        const juce::String tag = "Slot " + juce::String (k + 1) + " ";
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { slotPid (k, "enable"), 1 }, tag + "Enable", k == 0));
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { slotPid (k, "mode"), 1 }, tag + "Mode",
            juce::StringArray { "Series", "Parallel" }, 0));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { slotPid (k, "ingain"), 1 }, tag + "In Gain",
            juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f, dbAttr()));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { slotPid (k, "out"), 1 }, tag + "Level",
            juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f, dbAttr()));
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { slotPid (k, "balance"), 1 }, tag + "Balance",
            juce::NormalisableRange<float> { -1.0f, 1.0f, 0.001f }, 0.0f));
    }

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "in_trim", 1 }, "Input Trim",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f, dbAttr()));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "ir_enable", 1 }, "IR Enable", true));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "ir_level", 1 }, "IR Level",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f, dbAttr()));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "out_gain", 1 }, "Output",
        juce::NormalisableRange<float> { -24.0f, 24.0f, 0.01f }, 0.0f, dbAttr()));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "mix", 1 }, "Mix",
        juce::NormalisableRange<float> { 0.0f, 100.0f, 0.1f }, 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tone_locut", 1 }, "Low Cut",
        logFreqRange (20.0f, 2000.0f), 20.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));
    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "tone_hicut", 1 }, "High Cut",
        logFreqRange (1000.0f, 20000.0f), 20000.0f,
        juce::AudioParameterFloatAttributes().withLabel ("Hz")));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "bypass", 1 }, "Bypass", false));

    return layout;
}

NamPlayerAudioProcessor::NamPlayerAudioProcessor()
    : AudioProcessor (BusesProperties()
          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createParameterLayout())
{
    for (int k = 0; k < kNumSlots; ++k)
    {
        slot[(size_t) k].enable  = apvts.getRawParameterValue (slotPid (k, "enable"));
        slot[(size_t) k].mode    = apvts.getRawParameterValue (slotPid (k, "mode"));
        slot[(size_t) k].ingain  = apvts.getRawParameterValue (slotPid (k, "ingain"));
        slot[(size_t) k].out     = apvts.getRawParameterValue (slotPid (k, "out"));
        slot[(size_t) k].balance = apvts.getRawParameterValue (slotPid (k, "balance"));
    }
    inTrimParam   = apvts.getRawParameterValue ("in_trim");
    irEnableParam = apvts.getRawParameterValue ("ir_enable");
    irLevelParam  = apvts.getRawParameterValue ("ir_level");
    outGainParam  = apvts.getRawParameterValue ("out_gain");
    mixParam      = apvts.getRawParameterValue ("mix");
    loCutParam    = apvts.getRawParameterValue ("tone_locut");
    hiCutParam    = apvts.getRawParameterValue ("tone_hicut");
    bypassParam   = apvts.getRawParameterValue ("bypass");
    bypassParamPtr = apvts.getParameter ("bypass");

    programs.configure (apvts, nam_player_presets::bank,
                        nam_player_presets::kExclude, nam_player_presets::kNumExclude);

    startTimerHz (20);   // message-thread retirement of handed-off models / kernels
}

NamPlayerAudioProcessor::~NamPlayerAudioProcessor()
{
    stopTimer();
}

void NamPlayerAudioProcessor::timerCallback()
{
    for (auto& perSlot : modelHandoff)
        for (auto& h : perSlot)
            h.drainRetired();
    for (auto& h : irHandoff)
        h.drainRetired();
}

juce::ValueTree NamPlayerAudioProcessor::filesTree()
{
    return apvts.state.getOrCreateChildWithName ("files", nullptr);
}

void NamPlayerAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    currentBlock      = juce::jmax (1, samplesPerBlock);

    // The host<->48k resampling bracket owns its FIFO, latency and prefill; the
    // NAM section runs at 48 kHz when resampling, otherwise at the host rate.
    bracket.prepare (sampleRate, kNamRate, currentBlock);
    resampling      = bracket.active();
    reportedLatency = bracket.latencySamples();
    namMaxBlk       = bracket.modelBlockCapacity();

    engine.prepare (resampling ? kNamRate : sampleRate, resampling ? namMaxBlk : currentBlock);
    engine.reset();

    // IR length cap is time-based (rate-independent), so the effective cabinet
    // response is the same duration at 44.1 kHz and 192 kHz.
    maxIrSamples = std::max (1, (int) std::lround (kMaxIrSeconds * sampleRate));
    for (auto& c : irConv) { c.prepare (currentBlock, maxIrSamples); c.reset(); }
    for (auto& b : loCut)  b.reset();
    for (auto& b : hiCut)  b.reset();
    for (auto& d : dryDelay) { d.prepare (reportedLatency + currentBlock + 4); d.reset(); }

    bypassSm.reset (sampleRate, 0.010);   // ~10 ms click-free bypass crossfade
    bypassSm.setCurrentAndTargetValue (bypassParam->load() > 0.5f ? 1.0f : 0.0f);

    // The convolver FFT size depends on the block size, so rebuild IR kernels here.
    if (irLoaded)
        buildAndPublishIrKernels();

    inTrimSm.reset  (sampleRate, 0.02);
    outGainSm.reset (sampleRate, 0.02);
    mixSm.reset     (sampleRate, 0.02);
    inTrimSm.setCurrentAndTargetValue  (juce::Decibels::decibelsToGain (inTrimParam->load()));
    outGainSm.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outGainParam->load()));
    mixSm.setCurrentAndTargetValue     (mixParam->load() * 0.01f);

    wL.assign   ((size_t) currentBlock, 0.0f);
    wR.assign   ((size_t) currentBlock, 0.0f);
    dryL.assign ((size_t) currentBlock, 0.0f);
    dryR.assign ((size_t) currentBlock, 0.0f);

    setLatencySamples (reportedLatency);
}

bool NamPlayerAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void NamPlayerAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int n      = buffer.getNumSamples();
    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, n);
    if (n <= 0)
        return;

    // Consume model / IR handoffs once per callback (so a swapped-out object is retired
    // to the message thread even while bypassed). updateLive() is a couple of atomics.
    for (int k = 0; k < kNumSlots; ++k)
    {
        engine.setModel (k, 0, modelHandoff[(size_t) k][0].updateLive());
        engine.setModel (k, 1, modelHandoff[(size_t) k][1].updateLive());
    }
    const ImpulseKernel* kernL = irHandoff[0].updateLive();
    const ImpulseKernel* kernR = irHandoff[1].updateLive();

    // A host block larger than the prepared size is processed in chunks (never dropped).
    for (int off = 0; off < n; off += currentBlock)
        processChunk (buffer, off, std::min (currentBlock, n - off), kernL, kernR);
}

void NamPlayerAudioProcessor::processChunk (juce::AudioBuffer<float>& buffer, int start, int m,
                                            const ImpulseKernel* kernL, const ImpulseKernel* kernR) noexcept
{
    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();

    const float* in0 = buffer.getReadPointer (0) + start;
    const float* in1 = numIn > 1 ? buffer.getReadPointer (1) + start : in0;

    // Latency-aligned dry path (always advanced so wet/dry stay in phase through bypass).
    for (int i = 0; i < m; ++i)
    {
        wL[(size_t) i] = in0[i];
        wR[(size_t) i] = in1[i];
        dryDelay[0].write ((double) in0[i]);
        dryDelay[1].write ((double) in1[i]);
        dryL[(size_t) i] = (float) dryDelay[0].readInterpolated ((double) reportedLatency);
        dryR[(size_t) i] = (float) dryDelay[1].readInterpolated ((double) reportedLatency);
    }

    float* o0 = buffer.getWritePointer (0) + start;
    float* o1 = numOut > 1 ? buffer.getWritePointer (1) + start : nullptr;

    const bool bypReq = bypassParam->load() > 0.5f;
    bypassSm.setTargetValue (bypReq ? 1.0f : 0.0f);

    // Fully settled at bypassed: emit the aligned dry signal and skip the (expensive)
    // wet chain. The NAM section resumes from stale state on release, but the crossfade
    // (the not-settled branch below) covers the transient.
    if (bypReq && ! bypassSm.isSmoothing())
    {
        for (int i = 0; i < m; ++i) o0[i] = dryL[(size_t) i];
        if (o1 != nullptr) for (int i = 0; i < m; ++i) o1[i] = dryR[(size_t) i];
        return;
    }

    // Input trim.
    inTrimSm.setTargetValue (juce::Decibels::decibelsToGain (inTrimParam->load()));
    for (int i = 0; i < m; ++i)
    {
        const float g = inTrimSm.getNextValue();
        wL[(size_t) i] *= g; wR[(size_t) i] *= g;
    }

    // Per-slot routing and gains.
    using Mode = factory_core::NamRoutingEngine::Mode;
    for (int k = 0; k < kNumSlots; ++k)
    {
        const bool  en  = slot[(size_t) k].enable->load()  > 0.5f;
        const Mode  md  = slot[(size_t) k].mode->load()    > 0.5f ? Mode::Parallel : Mode::Series;
        const float ing = juce::Decibels::decibelsToGain (slot[(size_t) k].ingain->load());
        const float og  = juce::Decibels::decibelsToGain (slot[(size_t) k].out->load());
        const float bal = slot[(size_t) k].balance->load();
        engine.setSlot (k, en, md, ing, og, bal);
    }

    // NAM section: the RateBracket resamples host<->48k around it (or runs it at the
    // host rate when they match) and delivers exactly m aligned host samples.
    bracket.process (wL.data(), wR.data(), wL.data(), wR.data(), m,
                     [this] (float* l, float* r, int mm) noexcept
                     { engine.process (l, r, l, r, mm); });

    // Cabinet IR (zero latency). IR level only applies when a kernel is loaded.
    if (irEnableParam->load() > 0.5f
        && kernL != nullptr && ! kernL->k.head.empty()
        && kernR != nullptr && ! kernR->k.head.empty())
    {
        irConv[0].process (wL.data(), m, kernL->k);
        irConv[1].process (wR.data(), m, kernR->k);
        const float irg = juce::Decibels::decibelsToGain (irLevelParam->load());
        for (int i = 0; i < m; ++i) { wL[(size_t) i] *= irg; wR[(size_t) i] *= irg; }
    }

    // Tone: low-cut (HPF) + high-cut (LPF), 12 dB/oct Butterworth, per channel.
    const double fs = currentSampleRate;
    const auto lc = factory_core::designFilter (factory_core::BandType::HighPass, loCutParam->load(), 0.0, 0.70710678, fs);
    const auto hc = factory_core::designFilter (factory_core::BandType::LowPass,  hiCutParam->load(), 0.0, 0.70710678, fs);
    loCut[0].setCoeffs (lc); loCut[1].setCoeffs (lc);
    hiCut[0].setCoeffs (hc); hiCut[1].setCoeffs (hc);
    loCut[0].process (wL.data(), m); loCut[1].process (wR.data(), m);
    hiCut[0].process (wL.data(), m); hiCut[1].process (wR.data(), m);

    // Output gain, dry/wet blend, then the bypass crossfade (b: 0 = wet, 1 = dry).
    outGainSm.setTargetValue (juce::Decibels::decibelsToGain (outGainParam->load()));
    mixSm.setTargetValue     (mixParam->load() * 0.01f);
    for (int i = 0; i < m; ++i)
    {
        const float og = outGainSm.getNextValue();
        const float mx = mixSm.getNextValue();
        const float b  = bypassSm.getNextValue();
        const float wetL = wL[(size_t) i] * og;
        const float wetR = wR[(size_t) i] * og;
        const float mixedL = dryL[(size_t) i] + mx * (wetL - dryL[(size_t) i]);
        const float mixedR = dryR[(size_t) i] + mx * (wetR - dryR[(size_t) i]);
        o0[i] = dryL[(size_t) i] * b + mixedL * (1.0f - b);
        if (o1 != nullptr) o1[i] = dryR[(size_t) i] * b + mixedR * (1.0f - b);
    }
}

juce::AudioProcessorEditor* NamPlayerAudioProcessor::createEditor()
{
    return new NamPlayerAudioProcessorEditor (*this);
}

void NamPlayerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // copyState() carries the "files" child tree (model / IR paths) intact; the
    // shared helper appends the selected program index as a root attribute
    // (append-only, so existing sessions without it read back as program 0).
    if (auto xml = factory_presets::stateToXml (apvts, programs))
        copyXmlToBinary (*xml, destData);
}

void NamPlayerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    factory_presets::applyStateXml (apvts, programs, getXmlFromBinary (data, sizeInBytes).get());

    // Re-load the referenced model / IR files (heavy) on the message thread.
    if (juce::MessageManager::getInstanceWithoutCreating() != nullptr
        && juce::MessageManager::getInstance()->isThisTheMessageThread())
        reloadModelsFromState();
    else
        juce::MessageManager::callAsync ([this] { reloadModelsFromState(); });
}

void NamPlayerAudioProcessor::reloadModelsFromState()
{
    auto ft = filesTree();
    for (int k = 0; k < kNumSlots; ++k)
    {
        const juce::String path = ft.getProperty (juce::Identifier (slotPid (k, "path"))).toString();
        const juce::File f (path);
        if (path.isNotEmpty() && f.existsAsFile())
            loadModel (k, f);
        else
            clearModel (k);
    }
    const juce::String irPath = ft.getProperty ("ir_path").toString();
    const juce::File irFile (irPath);
    if (irPath.isNotEmpty() && irFile.existsAsFile())
        loadIr (irFile);
    else
        clearIr();
}

void NamPlayerAudioProcessor::loadModel (int slotIndex, const juce::File& file)
{
    std::string err;
    const std::string path = file.getFullPathName().toStdString();
    auto m0 = std::make_unique<NamModel>();
    auto m1 = std::make_unique<NamModel>();
    if (! m0->load (path, kNamRate, kNamMaxBlock, err) || ! m1->load (path, kNamRate, kNamMaxBlock, err))
    {
        juce::Logger::writeToLog ("NAM Player: model load failed (" + file.getFileName() + "): " + juce::String (err));
        return;
    }

    // Display name; hint the model's trained rate when it isn't the 48 kHz the
    // section runs at (the model still plays, with a slight voicing shift).
    juce::String display (m0->name());
    const double sr = m0->expectedSampleRate();
    if (sr > 0.0 && std::abs (sr - kNamRate) > 1.0)
        display += " @" + juce::String (juce::roundToInt (sr / 1000.0)) + "k";

    modelHandoff[(size_t) slotIndex][0].publish (std::move (m0));
    modelHandoff[(size_t) slotIndex][1].publish (std::move (m1));

    auto ft = filesTree();
    ft.setProperty (juce::Identifier (slotPid (slotIndex, "path")), file.getFullPathName(), nullptr);
    ft.setProperty (juce::Identifier (slotPid (slotIndex, "name")), display, nullptr);
}

void NamPlayerAudioProcessor::clearModel (int slotIndex)
{
    modelHandoff[(size_t) slotIndex][0].publishEmpty();
    modelHandoff[(size_t) slotIndex][1].publishEmpty();

    auto ft = filesTree();
    ft.removeProperty (juce::Identifier (slotPid (slotIndex, "path")), nullptr);
    ft.removeProperty (juce::Identifier (slotPid (slotIndex, "name")), nullptr);
}

void NamPlayerAudioProcessor::loadIr (const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr)
    {
        juce::Logger::writeToLog ("NAM Player: IR load failed (" + file.getFileName() + ")");
        return;
    }

    // Read generously (a few times the time cap) at the source rate so truncation can
    // be detected and the resampler tail flushed; buildAndPublishIrKernels applies the
    // actual time-based cap for the current host rate.
    const int numCh = (int) juce::jmax ((unsigned int) 1, reader->numChannels);
    const juce::int64 readCap = (juce::int64) std::ceil (kMaxIrSeconds * reader->sampleRate * 4.0) + 64;
    const int lenIn = (int) juce::jmin (readCap, reader->lengthInSamples);
    if (lenIn <= 0) return;

    juce::AudioBuffer<float> buf (numCh, lenIn);
    reader->read (&buf, 0, lenIn, 0, true, numCh > 1);

    irRaw[0].assign (buf.getReadPointer (0), buf.getReadPointer (0) + lenIn);
    const int srcCh1 = numCh > 1 ? 1 : 0;
    irRaw[1].assign (buf.getReadPointer (srcCh1), buf.getReadPointer (srcCh1) + lenIn);
    irRawRate = reader->sampleRate;
    irLoaded  = true;

    buildAndPublishIrKernels();

    auto ft = filesTree();
    ft.setProperty ("ir_path", file.getFullPathName(), nullptr);
    ft.setProperty ("ir_name", file.getFileNameWithoutExtension(), nullptr);
}

void NamPlayerAudioProcessor::clearIr()
{
    irLoaded = false;
    for (auto& h : irHandoff) h.publishEmpty();

    auto ft = filesTree();
    ft.removeProperty ("ir_path", nullptr);
    ft.removeProperty ("ir_name", nullptr);
}

void NamPlayerAudioProcessor::buildAndPublishIrKernels()
{
    if (! irLoaded) return;
    const double dst = currentSampleRate;
    const int    cap = maxIrSamples > 0 ? maxIrSamples
                                        : std::max (1, (int) std::lround (kMaxIrSeconds * dst));
    for (int ch = 0; ch < 2; ++ch)
    {
        const int srcLen = (int) irRaw[(size_t) ch].size();
        auto rs = resampleIr (irRaw[(size_t) ch].data(), srcLen, irRawRate, dst, cap);

        // If the IR was truncated by the time cap, taper the cut with a 5 ms
        // raised-cosine fade so the hard edge isn't an audible click.
        const int naturalLen = (irRawRate > 0.0 && std::abs (irRawRate - dst) > 1.0e-6)
                             ? (int) std::floor (srcLen * dst / irRawRate) : srcLen;
        if (naturalLen > cap && ! rs.empty())
        {
            const int fade = std::min ((int) rs.size(), std::max (1, (int) std::lround (0.005 * dst)));
            for (int i = 0; i < fade; ++i)
            {
                const size_t idx = rs.size() - (size_t) fade + (size_t) i;
                const float  g   = 0.5f + 0.5f * std::cos (3.14159265358979323846f * (float) (i + 1) / (float) fade);
                rs[idx] *= g;
            }
        }

        auto kernel = std::make_unique<ImpulseKernel>();
        if (! rs.empty())
            irConv[0].buildKernel (rs.data(), (int) rs.size(), kernel->k);   // same geometry for both channels
        irHandoff[(size_t) ch].publish (std::move (kernel));
    }
}

NamPlayerAudioProcessor::ReampResult
NamPlayerAudioProcessor::renderReampToFile (const juce::File& inWav, const juce::File& outWav,
                                            bool includeIrTone, const std::function<void (float)>& onProgress)
{
    using Mode = factory_core::NamRoutingEngine::Mode;

    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (inWav));
    if (reader == nullptr)
        return { false, "入力 WAV を読み込めませんでした。" };
    if (std::abs (reader->sampleRate - kNamRate) > 1.0)
        return { false, "48 kHz の WAV を指定してください（トレーナー較正互換のためリサンプルしません）。" };

    const int N = (int) reader->lengthInSamples;
    if (N <= 0)
        return { false, "入力が空です。" };

    juce::AudioBuffer<float> inbuf ((int) juce::jmax ((unsigned int) 1, reader->numChannels), N);
    reader->read (&inbuf, 0, N, 0, true, reader->numChannels > 1);
    std::vector<float> input ((size_t) N);
    std::copy (inbuf.getReadPointer (0), inbuf.getReadPointer (0) + N, input.begin());   // ch0

    // Fresh models (the L-channel path) + a fresh engine set from the current params,
    // Balance forced centre. Never touch the live audio objects.
    const int chunk = 512;
    factory_core::NamRoutingEngine reEngine;
    reEngine.prepare (kNamRate, chunk);
    std::vector<std::unique_ptr<NamModel>> models ((size_t) kNumSlots);
    auto ft = filesTree();
    bool anyParallelOffCentre = false;
    for (int k = 0; k < kNumSlots; ++k)
    {
        const juce::String path = ft.getProperty (juce::Identifier (slotPid (k, "path"))).toString();
        const juce::File f (path);
        factory_core::MonoProcessor* mp = nullptr;
        if (path.isNotEmpty() && f.existsAsFile())
        {
            auto mm = std::make_unique<NamModel>(); std::string err;
            if (mm->load (path.toStdString(), kNamRate, chunk, err)) { mp = mm.get(); models[(size_t) k] = std::move (mm); }
        }
        reEngine.setModel (k, 0, mp);
        reEngine.setModel (k, 1, mp);

        const bool  en  = slot[(size_t) k].enable->load()  > 0.5f;
        const Mode  md  = slot[(size_t) k].mode->load()    > 0.5f ? Mode::Parallel : Mode::Series;
        const float ing = juce::Decibels::decibelsToGain (slot[(size_t) k].ingain->load());
        const float og  = juce::Decibels::decibelsToGain (slot[(size_t) k].out->load());
        if (en && md == Mode::Parallel && std::abs (slot[(size_t) k].balance->load()) > 0.001f)
            anyParallelOffCentre = true;
        reEngine.setSlot (k, en, md, ing, og, 0.0f);        // Balance centred for capture
    }

    // Optional cab IR + tone, all built fresh at 48 kHz.
    std::unique_ptr<factory_core::PartitionedConvolver> reIr;
    factory_core::PartitionedConvolver::Kernel reKernel;
    float irLevelLin = 1.0f;
    factory_core::BiquadCoeffs lc {}, hc {};
    bool haveLo = false, haveHi = false;
    if (includeIrTone)
    {
        if (irEnableParam->load() > 0.5f && irLoaded)
        {
            const int maxIr48 = std::max (1, (int) std::lround (kMaxIrSeconds * kNamRate));
            reIr = std::make_unique<factory_core::PartitionedConvolver>();
            reIr->prepare (chunk, maxIr48);
            auto rs = resampleIr (irRaw[0].data(), (int) irRaw[0].size(), irRawRate, kNamRate, maxIr48);
            if (! rs.empty())
                reIr->buildKernel (rs.data(), (int) rs.size(), reKernel);
            irLevelLin = juce::Decibels::decibelsToGain (irLevelParam->load());
        }
        lc = factory_core::designFilter (factory_core::BandType::HighPass, loCutParam->load(), 0.0, 0.70710678, kNamRate);
        hc = factory_core::designFilter (factory_core::BandType::LowPass,  hiCutParam->load(), 0.0, 0.70710678, kNamRate);
        haveLo = haveHi = true;
    }

    auto out = OfflineReamp::render (reEngine, chunk, input, includeIrTone,
                                     reIr.get(), reKernel.head.empty() ? nullptr : &reKernel, irLevelLin,
                                     haveLo ? &lc : nullptr, haveHi ? &hc : nullptr, onProgress);

    // Write 48 kHz / mono / 32-bit float.
    outWav.deleteFile();
    std::unique_ptr<juce::FileOutputStream> os (outWav.createOutputStream());
    if (os == nullptr)
        return { false, "出力ファイルを作成できませんでした。" };
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (os.get(), kNamRate, 1, 32, {}, 0));
    if (writer == nullptr)
        return { false, "WAV ライターを作成できませんでした。" };
    os.release();                                            // writer owns the stream now
    juce::AudioBuffer<float> outbuf (1, (int) out.size());
    std::copy (out.begin(), out.end(), outbuf.getWritePointer (0));
    writer->writeFromAudioSampleBuffer (outbuf, 0, (int) out.size());
    writer.reset();

    juce::String msg = "リアンプ WAV を書き出しました: " + outWav.getFileName();
    if (anyParallelOffCentre)
        msg += "\n（Balance は中央としてキャプチャされます）";
    return { true, msg };
}

juce::String NamPlayerAudioProcessor::slotName (int slotIndex) const
{
    return apvts.state.getChildWithName ("files")
                .getProperty (juce::Identifier (slotPid (slotIndex, "name"))).toString();
}

juce::String NamPlayerAudioProcessor::irName() const
{
    return apvts.state.getChildWithName ("files").getProperty ("ir_name").toString();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NamPlayerAudioProcessor();
}
