#include "../Source/PluginProcessor.h"
#include <iostream>
static int fails = 0;
static void check (bool ok, const juce::String& m) { std::cout << (ok ? "PASS " : "FAIL ") << m << "\n"; if (! ok) ++fails; }
static void set (VegaAnalyzerProcessor& p, const char* id, float v) { auto* q = p.apvts.getParameter (id); q->setValueNotifyingHost (q->convertTo0to1 (v)); }
const double fs = 48000; const int blk = 512;
static float sn (double f, int i, double amp, double ph = 0) { return (float) (amp * std::sin (2 * juce::MathConstants<double>::pi * f * i / fs + ph)); }

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    auto make = [&] { auto* p = new VegaAnalyzerProcessor(); p->setPlayConfigDetails (2, 2, fs, blk); p->prepareToPlay (fs, blk); return std::unique_ptr<VegaAnalyzerProcessor> (p); };
    {   // 1 kHz sine at -20 dBFS on both channels = -20 LUFS (BS.1770 reference)
        auto p = make(); juce::AudioBuffer<float> b (2, blk); juce::MidiBuffer m; double err = 0;
        for (int pos = 0; pos < (int) (fs * 5); pos += blk)
        {
            for (int i = 0; i < blk; ++i) { const float x = sn (997, pos + i, 0.1); b.setSample (0, i, x); b.setSample (1, i, x); }
            p->processBlock (b, m);
            for (int i = 0; i < blk; ++i) err = std::max (err, (double) std::abs (b.getSample (0, i) - sn (997, pos + i, 0.1)));
        }
        const float I = p->meter.computeIntegrated();
        check (std::abs (I + 20.0f) < 0.15f, "997 Hz at -20 dBFS stereo reads " + juce::String (I, 2) + " LUFS (expect -20.0)");
        check (std::abs (p->meter.shortTerm.load() + 20.0f) < 0.15f, "short-term " + juce::String (p->meter.shortTerm.load(), 2) + " LUFS");
        check (err < 1e-7, "audio passes through untouched");
    }
    {   // true peak catches inter-sample overs: fs/4 sine at 45 degrees -> samples at 0.707, true peak 1.0
        auto p = make(); juce::AudioBuffer<float> b (2, blk); juce::MidiBuffer m;
        for (int pos = 0; pos < 48000; pos += blk)
        {
            for (int i = 0; i < blk; ++i) { const float x = sn (12000, pos + i, 1.0, juce::MathConstants<double>::pi / 4); b.setSample (0, i, x); b.setSample (1, i, x); }
            p->processBlock (b, m);
        }
        check (std::abs (p->samplePeakMax.load() + 3.01f) < 0.1f && std::abs (p->truePeakMax.load()) < 0.3f,
               "sample peak " + juce::String (p->samplePeakMax.load(), 2) + " dBFS, true peak " + juce::String (p->truePeakMax.load(), 2) + " dBTP");
    }
    {   // spectrum maker: a 1 kHz full-scale sine peaks at the right bin near 0 dB
        SpectrumMaker sm; std::vector<float> in ((size_t) SpectrumMaker::size), pw;
        for (int i = 0; i < SpectrumMaker::size; ++i) in[(size_t) i] = sn (1000, i, 1.0);
        sm.power (in.data(), pw);
        int best = 0; for (int k = 1; k < SpectrumMaker::bins; ++k) if (pw[(size_t) k] > pw[(size_t) best]) best = k;
        const double hz = best * fs / SpectrumMaker::size, db = 10 * std::log10 (pw[(size_t) best]);
        check (std::abs (hz - 1000) < 6 && std::abs (db) < 1.6, "spectrum: peak at " + juce::String (hz, 1) + " Hz, " + juce::String (db, 2) + " dB");
    }
    {   // reference: write a wav (44.1k, -14 dBFS-ish pink-ish noise), load, check loudness, resampling, A/B playback + level match
        auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("vega_ref_test.wav");
        {
            file.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (file), 44100, 2, 24, {}, 0));
            juce::AudioBuffer<float> b (2, 44100 * 6);
            for (int i = 0; i < b.getNumSamples(); ++i) { const float x = (float) (0.2 * std::sin (2 * juce::MathConstants<double>::pi * 997 * i / 44100.0)); b.setSample (0, i, x); b.setSample (1, i, x); }
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }
        auto p = make(); juce::String err;
        const bool ok = p->loadReference (file, err);
        const auto* r = p->reference();
        check (ok && r != nullptr, "reference loads " + err);
        if (r != nullptr)
        {
            const float expect = 20 * std::log10 (0.2f);
            check (std::abs (r->lufs - expect) < 0.2f, "reference loudness " + juce::String (r->lufs, 2) + " LUFS (expect " + juce::String (expect, 2) + ")");
            check (std::abs (r->audio.getNumSamples() - 6 * 48000) < 100, "reference resampled 44.1k -> 48k (" + juce::String (r->audio.getNumSamples()) + " samples)");
            // play mix at -20 LUFS, listen to reference with level match -> output loudness ~ -20
            set (*p, "listenRef", 1); set (*p, "levelMatch", 1);
            p->matchGainDb.store (-20.0f - r->lufs);
            juce::AudioBuffer<float> b (2, blk); juce::MidiBuffer m; double e = 0; int cnt = 0;
            for (int pos = 0; pos < 48000 * 2; pos += blk)
            {
                for (int i = 0; i < blk; ++i) { const float x = sn (440, pos + i, 0.1); b.setSample (0, i, x); b.setSample (1, i, x); }
                p->processBlock (b, m);
                if (pos > 48000) for (int i = 0; i < blk; ++i) { e += b.getSample (0, i) * b.getSample (0, i); ++cnt; }
            }
            const double outDb = 10 * std::log10 (e / cnt) + 3.01;    // sine rms -> peak dB
            check (std::abs (outDb - 20 * std::log10 (0.1)) < 0.3, "A/B: reference plays level-matched (" + juce::String (outDb, 2) + " dB peak, mix is -20)");
        }
        file.deleteFile();
    }
    std::cout << (fails == 0 ? "ALL PASSED" : "FAILURES: " + std::to_string (fails)) << "\n";
    return fails == 0 ? 0 : 1;
}
