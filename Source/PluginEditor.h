#pragma once
#include "PluginProcessor.h"
#include "VegaUI.h"

class VegaAnalyzerEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit VegaAnalyzerEditor (VegaAnalyzerProcessor&);
    ~VegaAnalyzerEditor() override;
    void paint (juce::Graphics&) override {}
    void resized() override;
    static constexpr int W = 1100, H = 640;
    class Main;
private:
    void timerCallback() override;
    VegaAnalyzerProcessor& proc;
    VegaLookAndFeel lnf;
    std::unique_ptr<Main> main;
    juce::ComponentBoundsConstrainer constrainer;
};
