#include "PluginEditor.h"

namespace
{
class SpectrumView : public juce::Component
{
public:
    explicit SpectrumView (VegaAnalyzerProcessor& p) : proc (p), smooth ((size_t) SpectrumMaker::bins, -120.0f), hold ((size_t) SpectrumMaker::bins, -120.0f), buf ((size_t) SpectrumMaker::size) {}

    void update()
    {
        proc.copyLatest (buf.data(), SpectrumMaker::size);
        maker.power (buf.data(), pw);
        const int resp = (int) proc.apvts.getRawParameterValue ("response")->load();
        const float a = resp == 0 ? 0.6f : resp == 1 ? 0.25f : 0.08f;
        for (int k = 0; k < SpectrumMaker::bins; ++k)
        {
            const float db = 10.0f * std::log10 (pw[(size_t) k] + 1e-14f);
            smooth[(size_t) k] += a * (db - smooth[(size_t) k]);
            hold[(size_t) k] = std::max (hold[(size_t) k] - 0.4f, smooth[(size_t) k]);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        vegaui::drawScreen (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().toFloat().reduced (36.0f, 14.0f).withTrimmedBottom (6.0f);
        auto& a = proc.apvts;
        const int slopeIdx = (int) a.getRawParameterValue ("slope")->load();
        const float slope = slopeIdx == 0 ? 0.0f : slopeIdx == 1 ? 3.0f : 4.5f;
        const double fs = proc.getSr(), binHz = fs / SpectrumMaker::size;
        const float top = 0, bottom = -90;
        auto fx = [&] (double f) { return (float) (r.getX() + r.getWidth() * std::log (f / 20.0) / std::log (1000.0)); };
        auto dy = [&] (float db) { return r.getY() + r.getHeight() * (top - db) / (top - bottom); };
        g.setFont (VegaFonts::ui (9.0f));
        for (double f : { 30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            g.setColour (juce::Colour (0xff232323)); g.drawVerticalLine ((int) fx (f), r.getY(), r.getBottom());
            g.setColour (VegaColours::textDim.withAlpha (0.6f));
            g.drawText (f >= 1000 ? juce::String ((int) f / 1000) + "k" : juce::String ((int) f), (int) fx (f) - 15, (int) r.getBottom() + 2, 30, 11, juce::Justification::centred);
        }
        for (int db = 0; db >= -90; db -= 10)
        {
            g.setColour (juce::Colour (0xff232323)); g.drawHorizontalLine ((int) dy ((float) db), r.getX(), r.getRight());
            g.setColour (VegaColours::textDim.withAlpha (0.6f)); g.drawText (juce::String (db), (int) r.getX() - 34, (int) dy ((float) db) - 6, 30, 12, juce::Justification::right);
        }
        // build a curve: per pixel column, max of bins it covers (interpolate at the low end)
        auto curve = [&] (const std::vector<float>& dbs, float offset)
        {
            juce::Path p; const int W = (int) r.getWidth();
            for (int x = 0; x <= W; x += 2)
            {
                const double f0 = 20.0 * std::pow (1000.0, x / r.getWidth()), f1 = 20.0 * std::pow (1000.0, (x + 2) / r.getWidth());
                const double b0 = f0 / binHz, b1 = f1 / binHz;
                float v;
                if (b1 - b0 < 1.0)
                {
                    const int k = juce::jlimit (0, SpectrumMaker::bins - 2, (int) b0); const float fr = (float) (b0 - k);
                    v = dbs[(size_t) k] + (dbs[(size_t) k + 1] - dbs[(size_t) k]) * fr;
                }
                else { v = -200; for (int k = (int) b0; k <= std::min ((int) b1, SpectrumMaker::bins - 1); ++k) v = std::max (v, dbs[(size_t) k]); }
                v += slope * (float) std::log2 (f0 / 1000.0) + offset;
                const float y = juce::jlimit (r.getY(), r.getBottom(), dy (v));
                if (x == 0) p.startNewSubPath (r.getX() + x, y); else p.lineTo (r.getX() + x, y);
            }
            return p;
        };
        g.saveState(); g.reduceClipRegion (r.toNearestInt());
        const auto* ref = proc.reference();
        const bool listening = a.getRawParameterValue ("listenRef")->load() > 0.5f;
        if (ref != nullptr && ! ref->avgPowerDb.empty())
        {
            const float off = a.getRawParameterValue ("levelMatch")->load() > 0.5f ? proc.matchGainDb.load() : 0.0f;
            g.setColour (VegaColours::gold.withAlpha (0.9f)); g.strokePath (curve (ref->avgPowerDb, off), juce::PathStrokeType (1.6f));
        }
        g.setColour (juce::Colours::white.withAlpha (0.18f)); g.strokePath (curve (hold, 0), juce::PathStrokeType (1.0f));
        auto main = curve (smooth, 0);
        juce::Path fill (main); fill.lineTo (r.getRight(), r.getBottom()); fill.lineTo (r.getX(), r.getBottom()); fill.closeSubPath();
        const auto col = listening ? VegaColours::gold : VegaColours::accent;
        g.setColour (col.withAlpha (0.13f)); g.fillPath (fill);
        g.setColour (col); g.strokePath (main, juce::PathStrokeType (1.8f));

        // difference (reference - mix), 1/3-octave smoothed, ±12 dB around the centre line
        if (ref != nullptr && a.getRawParameterValue ("showDiff")->load() > 0.5f && ! listening)
        {
            const float off = a.getRawParameterValue ("levelMatch")->load() > 0.5f ? proc.matchGainDb.load() : 0.0f;
            juce::Path d; const float mid = r.getCentreY();
            for (int x = 0; x <= (int) r.getWidth(); x += 3)
            {
                const double fc = 20.0 * std::pow (1000.0, x / r.getWidth());
                const int k0 = std::max (1, (int) (fc / 1.12 / binHz)), k1 = std::min (SpectrumMaker::bins - 1, std::max (k0, (int) (fc * 1.12 / binHz)));
                double sr = 0, sm = 0; for (int k = k0; k <= k1; ++k) { sr += std::pow (10.0, ref->avgPowerDb[(size_t) k] / 10.0); sm += std::pow (10.0, smooth[(size_t) k] / 10.0); }
                const float diff = juce::jlimit (-12.0f, 12.0f, (float) (10.0 * std::log10 ((sr + 1e-14) / (sm + 1e-14))) + off);
                const float y = mid - diff / 12.0f * r.getHeight() * 0.45f;
                if (x == 0) d.startNewSubPath (r.getX() + x, y); else d.lineTo (r.getX() + x, y);
            }
            g.setColour (juce::Colours::white.withAlpha (0.25f)); g.drawHorizontalLine ((int) mid, r.getX(), r.getRight());
            g.setColour (juce::Colours::white); g.strokePath (d, juce::PathStrokeType (2.0f));
            g.setFont (VegaFonts::ui (10.0f, true));
            g.drawText ("DIFFERENCE: ABOVE THE LINE = THE REFERENCE HAS MORE HERE", r.toNearestInt().removeFromTop (16).reduced (6, 0), juce::Justification::right);
        }
        g.restoreState();
        g.setFont (VegaFonts::ui (10.0f, true));
        g.setColour (col); g.drawText (listening ? "REFERENCE (LISTENING)" : "YOUR MIX", (int) r.getX() + 8, (int) r.getY() + 4, 200, 14, juce::Justification::left);
        if (ref != nullptr) { g.setColour (VegaColours::gold); g.drawText ("REFERENCE AVERAGE: " + ref->name, (int) r.getX() + 8, (int) r.getY() + 20, 400, 14, juce::Justification::left); }
    }
private:
    VegaAnalyzerProcessor& proc; SpectrumMaker maker;
    std::vector<float> smooth, hold, buf, pw;
};

class LoudnessHistory : public juce::Component
{
public:
    void push (float st, float m) { if (hist.size() >= 300) hist.erase (hist.begin()); hist.push_back ({ st, m }); repaint(); }
    void clear() { hist.clear(); repaint(); }
    float target = -100;
    void paint (juce::Graphics& g) override
    {
        vegaui::drawScreen (g, getLocalBounds().toFloat());
        auto r = getLocalBounds().toFloat().reduced (8.0f, 6.0f);
        auto dy = [&] (float l) { return r.getY() + r.getHeight() * juce::jlimit (0.0f, 1.0f, (-4.0f - l) / 36.0f); };
        g.setFont (VegaFonts::ui (9.0f));
        for (int l = -8; l >= -40; l -= 8) { g.setColour (juce::Colour (0xff232323)); g.drawHorizontalLine ((int) dy ((float) l), r.getX(), r.getRight()); g.setColour (VegaColours::textDim.withAlpha (0.5f)); g.drawText (juce::String (l), (int) r.getRight() - 24, (int) dy ((float) l) - 6, 24, 12, juce::Justification::right); }
        if (target > -60) { g.setColour (VegaColours::gold.withAlpha (0.7f)); g.drawHorizontalLine ((int) dy (target), r.getX(), r.getRight()); }
        juce::Path s, m;
        for (size_t i = 0; i < hist.size(); ++i)
        {
            const float x = r.getX() + r.getWidth() * i / 300.0f;
            if (i == 0) { s.startNewSubPath (x, dy (hist[i].first)); m.startNewSubPath (x, dy (hist[i].second)); }
            else { s.lineTo (x, dy (hist[i].first)); m.lineTo (x, dy (hist[i].second)); }
        }
        g.setColour (VegaColours::accent.withAlpha (0.3f)); g.strokePath (m, juce::PathStrokeType (1.0f));
        g.setColour (VegaColours::accent); g.strokePath (s, juce::PathStrokeType (2.0f));
        g.setColour (VegaColours::textDim); g.drawText ("SHORT-TERM LOUDNESS - 30 s  (gold = reference)", r.toNearestInt().removeFromTop (12), juce::Justification::left);
    }
private:
    std::vector<std::pair<float, float>> hist;
};
}

class VegaAnalyzerEditor::Main : public juce::Component
{
public:
    explicit Main (VegaAnalyzerProcessor& p) : proc (p), a (p.apvts), spectrum (p),
        slope (a, "slope", "Slope"), response (a, "response", "Response"), levelMatch (a, "levelMatch", "Level Match", "Off|Match"), showDiff (a, "showDiff", "Difference"),
        listenAtt (a, "listenRef", abButton)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &spectrum, &history, &slope, &response, &levelMatch, &showDiff, &meter, &oMom, &oShort, &oInt, &oTp, &oRef, &oStereo })
            addAndMakeVisible (c);
        auto btn = [this] (juce::TextButton& b, const char* t, std::function<void()> f) { b.setButtonText (t); b.onClick = std::move (f); addAndMakeVisible (b); };
        btn (loadBtn, "LOAD REFERENCE...", [this] { chooseReference(); });
        btn (resetBtn, "RESET", [this] { proc.resetMeters(); history.clear(); });
        abButton.setButtonText ("A / B"); abButton.setClickingTogglesState (true);
        abButton.setColour (juce::TextButton::buttonOnColourId, VegaColours::gold);
        addAndMakeVisible (abButton);
    }
    void refresh()
    {
        spectrum.update();
        meter.setLevels (proc.outL.exchange (0.0f), proc.outR.exchange (0.0f));
        const float m = proc.meter.momentary.load(), s = proc.meter.shortTerm.load();
        if (++tick % 3 == 0) { history.push (s, m); integrated = proc.meter.computeIntegrated(); }
        auto f = [] (float v) { return v <= -99 ? juce::String ("--") : juce::String (v, 1); };
        oMom.setLines ("MOMENTARY", f (m) + " LUFS");
        oShort.setLines ("SHORT-TERM", f (s) + " LUFS");
        const float tp = proc.truePeakMax.load();
        oInt.setLines ("INTEGRATED", f (integrated) + " LUFS", "PLR", integrated > -99 ? juce::String (tp - integrated, 1) : "--", "PEAK", f (proc.samplePeakMax.load()));
        oTp.setLines ("TRUE PEAK MAX", f (tp) + " dBTP");
        oStereo.setLines ("CORRELATION", juce::String (proc.correlation.load(), 2));
        if (const auto* r = proc.reference())
        {
            oRef.setLines ("REFERENCE", r->name.substring (0, 18), "LUFS", f (r->lufs), "TP", f (r->truePeak));
            history.target = r->lufs;
            proc.matchGainDb.store (integrated > -70 && r->lufs > -70 ? integrated - r->lufs : 0.0f);
        }
        else oRef.setLines ("REFERENCE", "none loaded", "", "", "", "");
        abButton.setEnabled (proc.reference() != nullptr);
        if (status.isNotEmpty() && juce::Time::getMillisecondCounter() > statusUntil) { status.clear(); repaint(); }
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (VegaColours::background);
        vegaui::drawTopBar (g, W, "ANALYZER  VP-17");
        vegaui::drawBody (g, { 0, 35, W, H - 65 });
        vegaui::drawSection (g, { 0, 440, 560, 170 }, "LOUDNESS HISTORY", false);
        vegaui::drawSection (g, { 560, 440, 300, 170 }, "REFERENCE  A / B");
        vegaui::drawSection (g, { 860, 440, W - 860, 170 }, "DISPLAY");
        if (status.isNotEmpty()) { g.setColour (VegaColours::gold); g.setFont (VegaFonts::ui (11.0f, true)); g.drawText (status, 470, 0, 420, 34, juce::Justification::centredRight); }
        vegaui::drawBottomBar (g, { 0, H - 30, W, 30 }, "LOAD A COMMERCIAL TRACK YOU LOVE, PRESS A/B TO COMPARE AT THE SAME LOUDNESS, AND MATCH THE CURVES");
    }
    void resized() override
    {
        spectrum.setBounds (10, 44, 820, 390);
        oMom.setBounds (840, 44, 180, 58); oShort.setBounds (840, 106, 180, 58); oInt.setBounds (840, 168, 180, 92); oTp.setBounds (840, 264, 180, 58);
        oStereo.setBounds (840, 326, 180, 52); meter.setBounds (1026, 44, 64, 334);
        resetBtn.setBounds (840, 384, 180, 26);
        history.setBounds (10, 458, 540, 146);
        loadBtn.setBounds (572, 460, 276, 26); oRef.setBounds (572, 492, 160, 108);
        abButton.setBounds (740, 492, 108, 46); levelMatch.setBounds (740, 544, 108, 58);
        slope.setBounds (872, 460, 110, 46); response.setBounds (984, 460, 106, 46); showDiff.setBounds (900, 520, 90, 80);
    }
private:
    void chooseReference()
    {
        chooser = std::make_unique<juce::FileChooser> ("Load a reference track", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
        {
            const auto f = fc.getResult(); if (! f.existsAsFile()) return;
            status = "Analysing " + f.getFileName() + "..."; statusUntil = juce::Time::getMillisecondCounter() + 60000; repaint();
            juce::Component::SafePointer<Main> safe (this);
            proc.loader.addJob ([this, f, safe]
            {
                juce::String err; const bool ok = proc.loadReference (f, err);
                juce::MessageManager::callAsync ([safe, ok, err]
                {
                    if (safe == nullptr) return;
                    safe->status = ok ? juce::String ("Reference loaded") : err; safe->statusUntil = juce::Time::getMillisecondCounter() + 4000; safe->repaint();
                });
            });
        });
    }
    VegaAnalyzerProcessor& proc; juce::AudioProcessorValueTreeState& a;
    SpectrumView spectrum; LoudnessHistory history;
    vegaui::Choice slope, response; vegaui::Toggle levelMatch, showDiff;
    juce::TextButton abButton; juce::AudioProcessorValueTreeState::ButtonAttachment listenAtt;
    juce::TextButton loadBtn, resetBtn;
    vegaui::Oled oMom, oShort, oInt, oTp, oRef, oStereo; vegaui::StereoMeter meter;
    std::unique_ptr<juce::FileChooser> chooser; juce::String status; juce::uint32 statusUntil = 0;
    int tick = 0; float integrated = -100;
};

VegaAnalyzerEditor::VegaAnalyzerEditor (VegaAnalyzerProcessor& p) : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);
    main = std::make_unique<Main> (p);
    addAndMakeVisible (*main);
    constrainer.setFixedAspectRatio ((double) W / H);
    constrainer.setSizeLimits (W / 2, H / 2, W * 2, H * 2);
    setConstrainer (&constrainer);
    setResizable (true, true);
    const float s = juce::jlimit (0.5f, 2.0f, proc.uiScale.load());
    setSize (juce::roundToInt (W * s), juce::roundToInt (H * s));
    startTimerHz (30);
}
VegaAnalyzerEditor::~VegaAnalyzerEditor() { stopTimer(); setConstrainer (nullptr); main.reset(); setLookAndFeel (nullptr); }
void VegaAnalyzerEditor::timerCallback() { main->refresh(); }
void VegaAnalyzerEditor::resized()
{
    main->setBounds (0, 0, W, H);
    const float s = (float) getWidth() / (float) W;
    main->setTransform (juce::AffineTransform::scale (s));
    proc.uiScale.store (s);
}
juce::AudioProcessorEditor* VegaAnalyzerProcessor::createEditor() { return new VegaAnalyzerEditor (*this); }
