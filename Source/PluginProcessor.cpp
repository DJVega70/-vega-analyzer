#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout VegaAnalyzerProcessor::layout()
{
    using namespace juce;
    std::vector<std::unique_ptr<RangedAudioParameter>> v;
    v.push_back (std::make_unique<AudioParameterBool> (ParameterID { "listenRef", 1 }, "Listen to Reference", false));
    v.push_back (std::make_unique<AudioParameterBool> (ParameterID { "levelMatch", 1 }, "Level Match", true));
    v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "slope", 1 }, "Slope", StringArray { "0 dB/oct", "3 dB/oct", "4.5 dB/oct" }, 2));
    v.push_back (std::make_unique<AudioParameterChoice> (ParameterID { "response", 1 }, "Response", StringArray { "Fast", "Medium", "Slow" }, 1));
    v.push_back (std::make_unique<AudioParameterBool> (ParameterID { "showDiff", 1 }, "Show Difference", false));
    return { v.begin(), v.end() };
}

VegaAnalyzerProcessor::VegaAnalyzerProcessor()
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true).withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", layout())
{
    listenRef = apvts.getRawParameterValue ("listenRef");
    levelMatch = apvts.getRawParameterValue ("levelMatch");
}
VegaAnalyzerProcessor::~VegaAnalyzerProcessor() { loader.removeAllJobs (true, 20000); if (auto* r = ref.exchange (nullptr)) oldRefs.emplace_back (r); }

bool VegaAnalyzerProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto o = l.getMainOutputChannelSet();
    return l.getMainInputChannelSet() == o && (o == juce::AudioChannelSet::stereo() || o == juce::AudioChannelSet::mono());
}

void VegaAnalyzerProcessor::prepareToPlay (double s, int)
{
    const bool rateChanged = std::abs (s - sr) > 1.0;
    sr = s;
    meter.prepare (s);
    tp.reset();
    truePeakMax.store (-100); samplePeakMax.store (-100);
    refPos = 0; abMix = listenRef->load() > 0.5f ? 1.0f : 0.0f;
    if (rateChanged && referenceFile.existsAsFile())
    {   // reference must match the host rate; reload in the background
        loader.addJob ([this] { juce::String e; loadReference (referenceFile, e); });
    }
}

void VegaAnalyzerProcessor::copyLatest (float* dest, int n) const
{
    int w = ringW.load(); 
    for (int i = 0; i < n; ++i) dest[i] = ring[(size_t) ((w - n + i + ringSize) & (ringSize - 1))];
}

void VegaAnalyzerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals nd;
    const int n = buffer.getNumSamples(), nch = std::min (2, buffer.getNumChannels());
    if (n == 0 || nch == 0) return;
    if (resetTp.exchange (false)) { truePeakMax.store (-100); samplePeakMax.store (-100); tp.reset(); }

    float* L = buffer.getWritePointer (0); float* R = buffer.getWritePointer (nch > 1 ? 1 : 0);
    // ---- measure the mix (always the mix, even while listening to the reference)
    meter.process (L, R, n);
    float tpk = 0, spk = 0;
    int w = ringW.load();
    for (int i = 0; i < n; ++i)
    {
        tpk = std::max (tpk, tp.push (L[i], R[i]));
        spk = std::max (spk, std::max (std::abs (L[i]), std::abs (R[i])));
        cLR += L[i] * R[i]; cLL += L[i] * L[i]; cRR += R[i] * R[i]; ++cN;
    }
    tpk = std::max (tpk, spk);
    if (vega::gainToDb (tpk) > truePeakMax.load()) truePeakMax.store (vega::gainToDb (tpk));
    if (vega::gainToDb (spk) > samplePeakMax.load()) samplePeakMax.store (vega::gainToDb (spk));
    if (cN > sr * 0.1) { const double d = std::sqrt (cLL * cRR); correlation.store (d > 1e-9 ? (float) (cLR / d) : 1.0f); cLR = cLL = cRR = 0; cN = 0; }

    // ---- A/B: crossfade to the (level matched) reference
    const auto* r = ref.load();
    const bool wantRef = listenRef->load() > 0.5f && r != nullptr && r->audio.getNumSamples() > 0;
    const float step = (float) (1.0 / (0.02 * sr));
    const float g = (levelMatch->load() > 0.5f) ? vega::dbToGain (juce::jlimit (-24.0f, 24.0f, matchGainDb.load())) : 1.0f;
    float pk[2] {};
    for (int i = 0; i < n; ++i)
    {
        abMix = wantRef ? std::min (1.0f, abMix + step) : std::max (0.0f, abMix - step);
        float l = L[i], rr = R[i];
        if (abMix > 0 && r != nullptr && r->audio.getNumSamples() > 0)
        {
            const int len = r->audio.getNumSamples();
            if (refPos >= len) refPos = 0;
            const float a = r->audio.getSample (0, refPos) * g, b = r->audio.getSample (std::min (1, r->audio.getNumChannels() - 1), refPos) * g;
            ++refPos;
            l += (a - l) * abMix; rr += (b - rr) * abMix;
        }
        // spectrum shows whatever you are hearing
        ring[(size_t) w] = 0.5f * (l + rr); w = (w + 1) & (ringSize - 1);
        L[i] = l; if (nch > 1) R[i] = rr;
        pk[0] = std::max (pk[0], std::abs (l)); pk[1] = std::max (pk[1], std::abs (rr));
    }
    ringW.store (w);
    outL.store (std::max (outL.load(), pk[0])); outR.store (std::max (outR.load(), pk[1]));
}

bool VegaAnalyzerProcessor::loadReference (const juce::File& file, juce::String& error)
{
    const juce::ScopedLock sl (loadLock);
    juce::AudioFormatManager fm; fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    if (reader == nullptr) { error = "Can't read " + file.getFileName() + " (use WAV, AIFF, FLAC, OGG or MP3)"; return false; }
    const auto maxLen = (juce::int64) (reader->sampleRate * 600.0);       // up to 10 minutes
    const int len = (int) std::min (reader->lengthInSamples, maxLen);
    if (len < (int) reader->sampleRate) { error = "File is too short"; return false; }
    juce::AudioBuffer<float> raw (2, len);
    reader->read (&raw, 0, len, 0, true, reader->numChannels > 1);
    if (reader->numChannels == 1) raw.copyFrom (1, 0, raw, 0, 0, len);

    auto track = std::make_unique<ReferenceTrack>();
    track->name = file.getFileNameWithoutExtension();
    const double ratio = reader->sampleRate / sr;
    if (std::abs (ratio - 1.0) < 1e-6) track->audio = std::move (raw);
    else
    {
        const int outLen = (int) (len / ratio) - 4;
        track->audio.setSize (2, outLen);
        for (int c = 0; c < 2; ++c) { juce::LagrangeInterpolator li; li.process (ratio, raw.getReadPointer (c), track->audio.getWritePointer (c), outLen); }
    }
    // loudness + true peak
    vega::LoudnessMeter m; m.prepare (sr);
    vega::TruePeakInterpolator t; float tpk = 0;
    const int total = track->audio.getNumSamples();
    m.process (track->audio.getReadPointer (0), track->audio.getReadPointer (1), total);
    for (int i = 0; i < total; ++i) tpk = std::max (tpk, t.push (track->audio.getSample (0, i), track->audio.getSample (1, i)));
    track->lufs = m.computeIntegrated(); track->truePeak = vega::gainToDb (tpk);
    // average spectrum
    SpectrumMaker sm; std::vector<double> acc ((size_t) SpectrumMaker::bins, 0.0); std::vector<float> pw, mono ((size_t) SpectrumMaker::size);
    int frames = 0;
    for (int start = 0; start + SpectrumMaker::size <= total; start += SpectrumMaker::size / 2)
    {
        for (int i = 0; i < SpectrumMaker::size; ++i) mono[(size_t) i] = 0.5f * (track->audio.getSample (0, start + i) + track->audio.getSample (1, start + i));
        sm.power (mono.data(), pw);
        for (int k = 0; k < SpectrumMaker::bins; ++k) acc[(size_t) k] += pw[(size_t) k];
        ++frames;
    }
    track->avgPowerDb.resize ((size_t) SpectrumMaker::bins);
    for (int k = 0; k < SpectrumMaker::bins; ++k) track->avgPowerDb[(size_t) k] = (float) (10.0 * std::log10 (acc[(size_t) k] / std::max (1, frames) + 1e-14));

    referenceFile = file;
    if (auto* old = ref.exchange (track.release())) oldRefs.emplace_back (old);
    return true;
}

void VegaAnalyzerProcessor::getStateInformation (juce::MemoryBlock& d)
{
    auto s = apvts.copyState(); s.setProperty ("uiScale", uiScale.load(), nullptr);
    s.setProperty ("referenceFile", referenceFile.getFullPathName(), nullptr);
    if (auto x = s.createXml()) copyXmlToBinary (*x, d);
}
void VegaAnalyzerProcessor::setStateInformation (const void* data, int size)
{
    if (auto x = getXmlFromBinary (data, size))
    {
        auto s = juce::ValueTree::fromXml (*x);
        if (! s.hasType (apvts.state.getType())) return;
        uiScale.store ((float) s.getProperty ("uiScale", 1.0f));
        const juce::File f (s.getProperty ("referenceFile").toString());
        apvts.replaceState (s);
        if (f.existsAsFile()) { referenceFile = f; loader.addJob ([this, f] { juce::String e; loadReference (f, e); }); }
    }
}
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new VegaAnalyzerProcessor(); }
