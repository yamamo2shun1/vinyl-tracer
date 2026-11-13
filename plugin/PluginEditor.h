// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

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

// Input level meter: L and R peak bars (-60..0 dBFS) with a peak hold, a clip
// lamp on top, and lines for the learned nominal level (accent) and the level
// that opens the gate (dim). The decoder's levels are the magnitude of the L/R
// pair, which on a clean carrier equals each channel's peak.
class LevelMeter : public juce::Component
{
public:
    // peaks: linear |sample| since the last update; called at the timer rate
    void update (float peakL, float peakR, float nominalDb, float gateDb, double dtSeconds);
    void paint (juce::Graphics&) override;

private:
    std::array<float, 2> levelDb { -120.f, -120.f }, holdDb { -120.f, -120.f };
    std::array<double, 2> holdAge { 0.0, 0.0 }, clipAge { 1e9, 1e9 };
    float nominal = -120.f, gate = -120.f;
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

// Diagnostics strip: the last few seconds (wall-clock time, now at the right)
// of the decoded speed, the phase loop's mean |residual| and the centre offset
// of the L/R path, to see what the stylus does when the output sounds rough.
class DiagView : public juce::Component
{
public:
    void push (const VinylTracerProcessor::DiagFrame* frames, int n);
    void setSpanSeconds (double s) { spanSeconds = s; }
    void paint (juce::Graphics&) override;

private:
    static constexpr int capacity = 16384; // frames (1 ms each)
    std::vector<VinylTracerProcessor::DiagFrame> hist = std::vector<VinylTracerProcessor::DiagFrame> (capacity);
    int64_t total = 0;      // frames pushed so far
    double spanSeconds = 2.0;
};

// Colours for the standard widgets (black base, yellow accent) and the
// embedded default font (JetBrains Mono Regular / Bold, also used for the
// monospaced text).
class VtLookAndFeel : public juce::LookAndFeel_V4
{
public:
    VtLookAndFeel();
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

private:
    juce::Typeface::Ptr regular, bold;
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
    // Shared by all open editors (it is also the default look-and-feel while
    // one is open, see the constructor); declared before the widgets so it
    // outlives them.
    juce::SharedResourcePointer<VtLookAndFeel> lnf;
    static inline int openEditors = 0; // message thread only
    std::unique_ptr<juce::Drawable> logo; // SVG, top right above the version (from logo/ via BinaryData)
    juce::Typeface::Ptr nameTypeface;     // Space Grotesk SemiBold (fonts/, OFL) for the plugin name

    juce::TextButton loadButton { "Load Sample..." };
    juce::ToggleButton diagButton { "Diagnostics" };
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

    ScopeView  scope;
    LevelMeter meter;
    WaveView   wave;
    DiagView   diag;
    std::vector<VinylTracerProcessor::DiagFrame> diagBuf = std::vector<VinylTracerProcessor::DiagFrame> (4096);
    void updateDiagnosticsVisibility();
    std::unique_ptr<juce::FileChooser> chooser;
    const vt::SampleData* shownSample = nullptr;
    std::vector<float> scopeBuf = std::vector<float> (4096 * 2);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VinylTracerEditor)
};
