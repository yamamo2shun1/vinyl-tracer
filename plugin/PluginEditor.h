#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

#include <vector>

// Lissajous display of the timecode input (like the scope in DVS software).
class ScopeView : public juce::Component
{
public:
    void push (const float* xy, int numPoints);
    void paint (juce::Graphics&) override;

private:
    static constexpr int capacity = 1024;
    std::vector<juce::Point<float>> points = std::vector<juce::Point<float>> (capacity);
    int writePos = 0;
};

// Waveform overview with playhead.
class WaveView : public juce::Component
{
public:
    void setSample (std::shared_ptr<const vt::SampleData> s);
    void setPlayhead (double seconds) { playhead = seconds; repaint(); }
    void paint (juce::Graphics&) override;

private:
    std::vector<float> mins, maxs;
    double duration = 0.0;
    double playhead = 0.0;
    juce::String name;
};

class VinylTracerEditor : public juce::AudioProcessorEditor,
                              public juce::FileDragAndDropTarget,
                              private juce::Timer
{
public:
    explicit VinylTracerEditor (VinylTracerProcessor&);
    ~VinylTracerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void loadFile (const juce::File& f);
    void openChooser();
    static void fillChoices (juce::ComboBox& box, juce::RangedAudioParameter* param);

    VinylTracerProcessor& proc;

    juce::TextButton loadButton { "Load Sample..." };
    juce::TextButton cueButton  { "Set Cue Here" };
    juce::Label      sampleLabel, statusLabel, levelLabel;

    juce::ComboBox   timecodeBox, modeBox, rpmBox;
    juce::Label      timecodeLabel, modeLabel, rpmLabel, thresholdLabel, gainLabel;
    juce::Slider     thresholdSlider, gainSlider;
    juce::ToggleButton thruButton { "Timecode Thru" }, loopButton { "Loop" };

    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    std::unique_ptr<ComboAttachment>  timecodeAtt, modeAtt, rpmAtt;
    std::unique_ptr<SliderAttachment> thresholdAtt, gainAtt;
    std::unique_ptr<ButtonAttachment> thruAtt, loopAtt;

    ScopeView scope;
    WaveView  wave;
    std::unique_ptr<juce::FileChooser> chooser;
    const vt::SampleData* shownSample = nullptr;
    std::vector<float> scopeBuf = std::vector<float> (4096 * 2);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VinylTracerEditor)
};
