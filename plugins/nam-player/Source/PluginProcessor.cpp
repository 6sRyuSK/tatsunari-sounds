#include "PluginProcessor.h"
#include "PluginEditor.h"

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

    // Simple linear resample of a mono IR from srcRate to dstRate, capped to maxOut
    // samples. Off the audio thread; adequate for a smooth cabinet impulse.
    std::vector<float> resampleIr (const float* src, int lenIn, double srcRate, double dstRate, int maxOut)
    {
        if (lenIn <= 0) return {};
        if (srcRate <= 0.0 || dstRate <= 0.0 || std::abs (srcRate - dstRate) < 1.0e-6)
        {
            const int n = std::min (lenIn, maxOut);
            return std::vector<float> (src, src + n);
        }
        const double ratio = srcRate / dstRate;                       // src samples per dst sample
        const int outLen = std::min (maxOut, (int) std::floor (lenIn / ratio));
        std::vector<float> out ((size_t) std::max (0, outLen), 0.0f);
        for (int i = 0; i < outLen; ++i)
        {
            const double pos = i * ratio;
            const int    i0  = (int) pos;
            const double fr  = pos - i0;
            const float  a   = src[i0];
            const float  b   = (i0 + 1 < lenIn) ? src[i0 + 1] : src[i0];
            out[(size_t) i] = a + (float) fr * (b - a);
        }
        return out;
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

    resampling = std::abs (sampleRate - kNamRate) > 1.0e-6;

    if (resampling)
    {
        // NAM section runs at 48 kHz; size its block for the worst-case ratio.
        namMaxBlk       = (int) std::ceil (currentBlock * kNamRate / sampleRate) + 4;
        reportedLatency = factory_core::resamplerRoundTripLatency (sampleRate, kNamRate, 2) + 16;
        engine.prepare (kNamRate, namMaxBlk);
        for (int ch = 0; ch < 2; ++ch)
        {
            downSamp[(size_t) ch].prepare (sampleRate, kNamRate);
            upSamp[(size_t) ch].prepare (kNamRate, sampleRate);
            namBuf[(size_t) ch].assign    ((size_t) namMaxBlk, 0.0f);
            upScratch[(size_t) ch].assign ((size_t) (currentBlock + 8), 0.0f);
            outFifo[(size_t) ch].prepare (currentBlock * 4 + 64);
            outFifo[(size_t) ch].reset();
            outFifo[(size_t) ch].pushZeros (reportedLatency);   // wet delayed by exactly reportedLatency
        }
    }
    else
    {
        namMaxBlk       = currentBlock;
        reportedLatency = 0;
        engine.prepare (sampleRate, currentBlock);
    }
    engine.reset();

    for (auto& c : irConv) { c.prepare (currentBlock, kMaxIrSamples); c.reset(); }
    for (auto& b : loCut)  b.reset();
    for (auto& b : hiCut)  b.reset();
    for (auto& d : dryDelay) { d.prepare (reportedLatency + currentBlock + 4); d.reset(); }

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
    if (n <= 0 || n > currentBlock)
        return;

    const float* in0 = buffer.getReadPointer (0);
    const float* in1 = numIn > 1 ? buffer.getReadPointer (1) : in0;
    for (int i = 0; i < n; ++i)
    {
        wL[(size_t) i] = in0[i];
        wR[(size_t) i] = in1[i];
        dryDelay[0].write ((double) in0[i]);
        dryDelay[1].write ((double) in1[i]);
        dryL[(size_t) i] = (float) dryDelay[0].readInterpolated ((double) reportedLatency);
        dryR[(size_t) i] = (float) dryDelay[1].readInterpolated ((double) reportedLatency);
    }

    // Consume model / IR handoffs every block (so a swapped-out object is retired to
    // the message thread even while bypassed). updateLive() is a couple of atomics.
    for (int k = 0; k < kNumSlots; ++k)
    {
        engine.setModel (k, 0, modelHandoff[(size_t) k][0].updateLive());
        engine.setModel (k, 1, modelHandoff[(size_t) k][1].updateLive());
    }
    const ImpulseKernel* kernL = irHandoff[0].updateLive();
    const ImpulseKernel* kernR = irHandoff[1].updateLive();

    // Bypass outputs the latency-aligned dry signal.
    if (bypassParam->load() > 0.5f)
    {
        float* b0 = buffer.getWritePointer (0);
        for (int i = 0; i < n; ++i) b0[i] = dryL[(size_t) i];
        if (numOut > 1)
        {
            float* b1 = buffer.getWritePointer (1);
            for (int i = 0; i < n; ++i) b1[i] = dryR[(size_t) i];
        }
        return;
    }

    // Input trim.
    inTrimSm.setTargetValue (juce::Decibels::decibelsToGain (inTrimParam->load()));
    for (int i = 0; i < n; ++i)
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

    // NAM section at 48 kHz, bracketed by resampling when the host rate differs.
    if (resampling)
    {
        const int namN0 = downSamp[0].process (wL.data(), n, namBuf[0].data(), namMaxBlk);
        const int namN1 = downSamp[1].process (wR.data(), n, namBuf[1].data(), namMaxBlk);
        const int namN  = std::min (namN0, namN1);
        engine.process (namBuf[0].data(), namBuf[1].data(), namBuf[0].data(), namBuf[1].data(), namN);
        for (int ch = 0; ch < 2; ++ch)
        {
            const int hostM = upSamp[(size_t) ch].process (namBuf[(size_t) ch].data(), namN,
                                                           upScratch[(size_t) ch].data(),
                                                           (int) upScratch[(size_t) ch].size());
            outFifo[(size_t) ch].push (upScratch[(size_t) ch].data(), hostM);
        }
        outFifo[0].pull (wL.data(), n);
        outFifo[1].pull (wR.data(), n);
    }
    else
    {
        engine.process (wL.data(), wR.data(), wL.data(), wR.data(), n);
    }

    // Cabinet IR (zero latency). IR level only applies when a kernel is loaded.
    if (irEnableParam->load() > 0.5f
        && kernL != nullptr && ! kernL->H.empty()
        && kernR != nullptr && ! kernR->H.empty())
    {
        irConv[0].process (wL.data(), n, kernL->H);
        irConv[1].process (wR.data(), n, kernR->H);
        const float irg = juce::Decibels::decibelsToGain (irLevelParam->load());
        for (int i = 0; i < n; ++i) { wL[(size_t) i] *= irg; wR[(size_t) i] *= irg; }
    }

    // Tone: low-cut (HPF) + high-cut (LPF), 12 dB/oct Butterworth, per channel.
    const double fs = currentSampleRate;
    const auto lc = factory_core::designFilter (factory_core::BandType::HighPass, loCutParam->load(), 0.0, 0.70710678, fs);
    const auto hc = factory_core::designFilter (factory_core::BandType::LowPass,  hiCutParam->load(), 0.0, 0.70710678, fs);
    loCut[0].setCoeffs (lc); loCut[1].setCoeffs (lc);
    hiCut[0].setCoeffs (hc); hiCut[1].setCoeffs (hc);
    loCut[0].process (wL.data(), n); loCut[1].process (wR.data(), n);
    hiCut[0].process (wL.data(), n); hiCut[1].process (wR.data(), n);

    // Output gain (wet chain) then dry/wet blend.
    outGainSm.setTargetValue (juce::Decibels::decibelsToGain (outGainParam->load()));
    mixSm.setTargetValue     (mixParam->load() * 0.01f);
    for (int i = 0; i < n; ++i)
    {
        const float og = outGainSm.getNextValue();
        const float m  = mixSm.getNextValue();
        const float wetL = wL[(size_t) i] * og;
        const float wetR = wR[(size_t) i] * og;
        wL[(size_t) i] = dryL[(size_t) i] + m * (wetL - dryL[(size_t) i]);
        wR[(size_t) i] = dryR[(size_t) i] + m * (wetR - dryR[(size_t) i]);
    }

    float* o0 = buffer.getWritePointer (0);
    for (int i = 0; i < n; ++i) o0[i] = wL[(size_t) i];
    if (numOut > 1)
    {
        float* o1 = buffer.getWritePointer (1);
        for (int i = 0; i < n; ++i) o1[i] = wR[(size_t) i];
    }
}

juce::AudioProcessorEditor* NamPlayerAudioProcessor::createEditor()
{
    return new NamPlayerAudioProcessorEditor (*this);
}

void NamPlayerAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void NamPlayerAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));

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

    const int numCh = (int) juce::jmax ((unsigned int) 1, reader->numChannels);
    const int lenIn = (int) juce::jmin ((juce::int64) (kMaxIrSamples * 8), reader->lengthInSamples);
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
    for (int ch = 0; ch < 2; ++ch)
    {
        auto rs = resampleIr (irRaw[(size_t) ch].data(), (int) irRaw[(size_t) ch].size(),
                              irRawRate, dst, kMaxIrSamples);
        auto kernel = std::make_unique<ImpulseKernel>();
        if (! rs.empty())
            irConv[0].buildKernel (rs.data(), (int) rs.size(), kernel->H);   // same FFT size for both channels
        irHandoff[(size_t) ch].publish (std::move (kernel));
    }
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
