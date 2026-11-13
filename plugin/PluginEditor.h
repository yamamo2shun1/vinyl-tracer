#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

#include <array>
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

// Scrolling waveform (a few seconds around the playhead, which stays in the
// centre) above an overview of the whole sample, coloured by the brightness
// of the sound (red = low, violet = high).
class WaveView : public juce::Component
{
public:
    void setSample (std::shared_ptr<const vt::SampleData> s);
    void setPlayhead (double seconds) { playhead = seconds; repaint(); }
    void setLoop (bool l) { loop = l; }
    // Cue points in seconds (NaN = empty), drawn as markers over the waveform.
    void setCues (const std::array<double, VinylTracerProcessor::kNumCues>& c) { cues = c; }
    void paint (juce::Graphics&) override;

    void setViewSeconds (double s) { viewSeconds = s; } // visible span of the scrolling view
    void mouseDown (const juce::MouseEvent&) override;

    std::function<void (double seconds)> onSeek; // click in the overview

private:
    // upper 2/3: scrolling waveform, lower 1/3: overview of the whole sample
    void layout (juce::Rectangle<float>& scroll, juce::Rectangle<float>& overview) const;
    void paintScroll (juce::Graphics&, juce::Rectangle<float> area);
    void paintOverview (juce::Graphics&, juce::Rectangle<float> area);
    double wrapped (double t) const { return loop ? t - duration * std::floor (t / duration) : t; }

    // min / max over both channels per kBinFrames frames of the sample
    static constexpr int kBinFrames = 64;
    std::vector<float> mins, maxs, meanSq, diffSq;      // per bin (diffSq: mean squared first difference)
    std::vector<float> ovMins, ovMaxs, ovRms;           // overview: one entry per pixel column (rebuilt on resize)
    std::vector<juce::Colour> ovCol;
    juce::Image scrollImg, ovImg;                       // waveform columns, redrawn every paint
    float ovRmsMax = 1.f;
    double binSeconds = 0.0;
    double sampleRate = 48000.0;
    float  peak = 1.f;
    double duration = 0.0;
    double playhead = 0.0;
    bool   loop = false;
    double viewSeconds = 2.0;
    std::array<double, VinylTracerProcessor::kNumCues> cues {};
    juce::String name;
};

// Colours for the standard widgets: black base, yellow accent.
class VtLookAndFeel : public juce::LookAndFeel_V4
{
public:
    VtLookAndFeel();
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
    void cueClicked (int index);
    void updateCueButtons();
    static void fillChoices (juce::ComboBox& box, juce::RangedAudioParameter* param);

    VinylTracerProcessor& proc;
    VtLookAndFeel lnf; // declared before the widgets so it outlives them

    juce::TextButton loadButton { "Load Sample..." };
    std::array<juce::TextButton, VinylTracerProcessor::kNumCues> cueButtons;
    juce::Label      cueLabel;
    std::array<double, VinylTracerProcessor::kNumCues> shownCues {};
    juce::TooltipWindow tooltips { this };
    juce::Label      statusLabel, levelLabel;

    juce::ComboBox   timecodeBox, modeBox, rpmBox;
    juce::Label      timecodeLabel, modeLabel, rpmLabel, thresholdLabel, gainLabel;
    juce::Slider     thresholdSlider, gainSlider;
    juce::ToggleButton thruButton { "Timecode Thru" }, loopButton { "Loop" };
    juce::Label      viewLabel;
    juce::ComboBox   viewBox;   // visible span of the scrolling waveform
    int              shownViewSeconds = 0; // the processor's value the box was last synced to

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
