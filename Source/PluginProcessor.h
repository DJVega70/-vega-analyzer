#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_dsp/juce_dsp.h>
#include "Meters.h"

/** Shared FFT helper (Hann window, power spectrum in dB per bin) */
struct SpectrumMaker
{
    static constexpr int order = 13, size = 1 << order, bins = size / 2;
    juce::dsp::FFT fft { order };
    std::vector<float> window, work;
    SpectrumMaker() : window ((size_t) size), work ((size_t) size * 2)
    {
        for (int i = 0; i < size; ++i) window[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / (size - 1));
    }
    /** input: `size` samples. output: power (linear, normalised so a full-scale sine reads ~0 dB) per bin */
    void power (const float* in, std::vector<float>& out)
    {
        for (int i = 0; i < size; ++i) work[(size_t) i] = in[i] * window[(size_t) i];
        std::fill (work.begin() + size, work.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (work.data(), true);
        out.resize ((size_t) bins);
        const float norm = 4.0f / (float) size;                 // Hann coherent gain 0.5 -> amplitude
        for (int k = 0; k < bins; ++k) { const float a = work[(size_t) k] * norm; out[(size_t) k] = a * a; }
    }
};

struct ReferenceTrack
{
    juce::String name;
    juce::AudioBuffer<float> audio;          // at host sample rate, stereo
    float lufs = -100, truePeak = -100;
    std::vector<float> avgPowerDb;           // per FFT bin
};

class VegaAnalyzerProcessor : public juce::AudioProcessor
{
public:
    VegaAnalyzerProcessor();
    ~VegaAnalyzerProcessor() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "Vega Productions Analyzer"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout layout();
    juce::AudioProcessorValueTreeState apvts;

    /** Decode, resample and analyse an audio file. Safe to call from a background thread. */
    bool loadReference (const juce::File&, juce::String& error);
    const ReferenceTrack* reference() const { return ref.load(); }
    juce::File referenceFile;

    // meters
    vega::LoudnessMeter meter;
    std::atomic<float> truePeakMax { -100 }, samplePeakMax { -100 }, correlation { 1 }, outL { 0 }, outR { 0 };
    std::atomic<float> matchGainDb { 0 };    // set by the UI from integrated loudness difference
    void resetMeters() { meter.requestReset(); resetTp.store (true); }

    // spectrum feed (mono mix of what is being measured)
    static constexpr int ringSize = 1 << 15;
    std::array<float, ringSize> ring {};
    std::atomic<int> ringW { 0 };
    void copyLatest (float* dest, int n) const;

    std::atomic<float> uiScale { 1.0f };
    double getSr() const { return sr; }

private:
    double sr = 48000;
    vega::TruePeakInterpolator tp;
    std::atomic<ReferenceTrack*> ref { nullptr };
    std::vector<std::unique_ptr<ReferenceTrack>> oldRefs;     // kept alive: the audio thread may still read them
    juce::CriticalSection loadLock;
public:
    juce::ThreadPool loader { 1 };
private:
    int refPos = 0; float abMix = 0;
    std::atomic<bool> resetTp { false };
    double cLR = 0, cLL = 0, cRR = 0; int cN = 0;
    std::atomic<float>* listenRef = nullptr; std::atomic<float>* levelMatch = nullptr;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VegaAnalyzerProcessor)
};
