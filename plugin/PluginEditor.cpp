#include "PluginEditor.h"

#include <cmath>

namespace
{
const juce::Colour kBg      { 0xff16181d };
const juce::Colour kPanel   { 0xff20232a };
const juce::Colour kAccent  { 0xff3ddc97 };
const juce::Colour kWarn    { 0xffffb020 };
const juce::Colour kText    { 0xffe6e6e6 };
const juce::Colour kDim     { 0xff8a8f98 };
// Cue point colours (buttons and waveform markers)
const juce::Colour kCueColours[VinylTracerProcessor::kNumCues] {
    juce::Colour (0xffff5c5c), juce::Colour (0xff4da3ff), juce::Colour (0xffffd84d), juce::Colour (0xffd36bff)
};

// Waveform colours by amplitude relative to the sample's peak: kWaveSteps steps,
// interpolated between these anchors (dark blue -> green -> yellow -> red)
constexpr int kWaveSteps = 16;
const juce::Colour kWaveAnchors[] {
    juce::Colour (0xff24507a), juce::Colour (0xff2a7fa8), juce::Colour (0xff2fb3b0), juce::Colour (0xff3ddc97),
    juce::Colour (0xff9be05a), juce::Colour (0xffe8d44d), juce::Colour (0xffff9a3d), juce::Colour (0xffff4f3d)
};

juce::Colour waveColour (float a) // a: amplitude relative to the sample's peak
{
    static const auto table = []
    {
        constexpr int nA = int (std::size (kWaveAnchors));
        std::array<juce::Colour, kWaveSteps> t;
        for (int i = 0; i < kWaveSteps; ++i)
        {
            const float p = float (i) / float (kWaveSteps - 1) * float (nA - 1);
            const int   j = juce::jmin (int (p), nA - 2);
            t[size_t (i)] = kWaveAnchors[j].interpolatedWith (kWaveAnchors[j + 1], p - float (j));
        }
        return t;
    }();
    return table[size_t (juce::jlimit (0, kWaveSteps - 1, int (a * float (kWaveSteps))))];
}

constexpr float kWaveHeight = 0.8f; // waveform height relative to its area (margin above / below)

bool sameCue (double a, double b) { return (std::isnan (a) && std::isnan (b)) || a == b; }

juce::String formatMinSec (double seconds) // m:ss
{
    const bool neg = seconds < 0;
    const int total = int (std::floor (std::abs (seconds)));
    return (neg ? "-" : "") + juce::String (total / 60) + ":" + juce::String (total % 60).paddedLeft ('0', 2);
}

juce::String formatTime (double seconds)
{
    const bool neg = seconds < 0;
    seconds = std::abs (seconds);
    const int m = int (seconds / 60.0);
    const double s = seconds - 60.0 * m;
    return (neg ? "-" : "") + juce::String (m) + ":" + juce::String (s, 3).paddedLeft ('0', 6);
}
} // namespace

//==============================================================================
void ScopeView::push (const float* xy, int numPoints)
{
    for (int i = 0; i < numPoints; ++i)
    {
        points[size_t (writePos)] = { xy[i * 2], xy[i * 2 + 1] };
        writePos = (writePos + 1) % capacity;
    }
}

void ScopeView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (r, 6.f);

    const auto c = r.getCentre();
    const float radius = 0.42f * juce::jmin (r.getWidth(), r.getHeight());
    g.setColour (kDim.withAlpha (0.4f));
    g.drawEllipse (c.x - radius, c.y - radius, 2 * radius, 2 * radius, 1.f);
    g.drawLine (c.x - radius, c.y, c.x + radius, c.y, 0.5f);
    g.drawLine (c.x, c.y - radius, c.x, c.y + radius, 0.5f);

    // Inside the guide circle: its edge is 1.4 x the nominal level, larger
    // points are pulled in to it. Colour by the distance from the centre
    // (16 steps).
    constexpr float kEdge = 1.4f;
    const float scale = radius / kEdge;
    for (const auto& p : points)
    {
        const float m = std::sqrt (p.x * p.x + p.y * p.y);
        const float k = m > kEdge ? kEdge / m : 1.f;
        g.setColour (waveColour (juce::jmin (m, kEdge) / kEdge));
        g.fillRect (c.x + p.x * k * scale - 0.75f, c.y - p.y * k * scale - 0.75f, 1.5f, 1.5f);
    }

    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (11.f)));
    g.drawText ("L / R", r.reduced (6.f), juce::Justification::topLeft);
}

//==============================================================================
void WaveView::setSample (std::shared_ptr<const vt::SampleData> s)
{
    mins.clear();
    maxs.clear();
    meanSq.clear();
    ovMins.clear();
    ovMaxs.clear();
    ovRms.clear();
    duration = 0.0;
    peak = 1.f;
    name = {};
    if (s == nullptr || s->levels.empty() || s->length == 0)
    {
        repaint();
        return;
    }

    // Fixed-size bins (not a fixed number for the whole sample): the view shows
    // a few seconds, so a long sample needs as much detail as a short one.
    const auto& lv = s->levels[0];
    const int64_t n = s->length;
    const size_t bins = size_t ((n + kBinFrames - 1) / kBinFrames);
    mins.assign (bins, 0.f);
    maxs.assign (bins, 0.f);
    meanSq.assign (bins, 0.f);
    float pk = 0.f;
    for (size_t b = 0; b < bins; ++b)
    {
        const int64_t a = int64_t (b) * kBinFrames;
        const int64_t e = juce::jmin (n, a + kBinFrames);
        float mn = 0.f, mx = 0.f;
        double sq = 0.0;
        for (const auto& ch : lv)
            for (int64_t i = a; i < e; ++i)
            {
                const float v = ch[size_t (i)];
                mn = juce::jmin (mn, v);
                mx = juce::jmax (mx, v);
                sq += double (v) * v;
            }
        mins[b] = mn;
        maxs[b] = mx;
        meanSq[b] = float (sq / double (juce::jmax<int64_t> (1, (e - a) * int64_t (lv.size()))));
        pk = juce::jmax (pk, mx, -mn);
    }
    peak = pk > 1e-6f ? pk : 1.f;
    binSeconds = double (kBinFrames) / s->sampleRate;
    duration = double (n) / s->sampleRate;
    name = juce::String (s->name);
    repaint();
}

void WaveView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (r, 6.f);

    if (mins.empty() || duration <= 0.0)
    {
        g.setColour (kDim);
        g.setFont (juce::Font (juce::FontOptions (14.f)));
        g.drawText ("Drop an audio file here or press \"Load Sample...\"", r, juce::Justification::centred);
        return;
    }

    juce::Rectangle<float> scrollArea, overviewArea;
    layout (scrollArea, overviewArea);

    paintScroll (g, scrollArea);
    g.setColour (kBg.withAlpha (0.6f));
    g.fillRect (r.getX() + 6.f, overviewArea.getY() - 2.5f, r.getWidth() - 12.f, 1.f);
    paintOverview (g, overviewArea);

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (12.f)));
    g.drawText (name, scrollArea.reduced (2.f, 0.f).withTrimmedLeft (16.f).withTrimmedRight (130.f),
                juce::Justification::topLeft);
    // playback position / total length
    g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.f, juce::Font::bold)));
    g.drawText (formatMinSec (wrapped (playhead)) + " / " + formatMinSec (duration), scrollArea.reduced (2.f, 0.f),
                juce::Justification::topRight);
}

void WaveView::layout (juce::Rectangle<float>& scroll, juce::Rectangle<float>& overview) const
{
    auto inner = getLocalBounds().toFloat().reduced (6.f, 6.f);
    scroll = inner.removeFromTop (inner.getHeight() * 2.f / 3.f);
    inner.removeFromTop (4.f);
    overview = inner;
}

void WaveView::mouseDown (const juce::MouseEvent& e)
{
    // A click in the overview plays from that point of the sample.
    juce::Rectangle<float> scroll, overview;
    layout (scroll, overview);
    if (duration <= 0.0 || ! overview.contains (e.position) || onSeek == nullptr)
        return;
    onSeek (duration * double ((e.position.x - overview.getX()) / overview.getWidth()));
}

void WaveView::paintScroll (juce::Graphics& g, juce::Rectangle<float> area)
{
    const float mid  = area.getCentreY();
    const float h    = area.getHeight() * 0.5f * kWaveHeight;
    const int   cols = juce::jmax (1, int (area.getWidth()));
    const double pxSeconds = viewSeconds / double (cols);

    // Playhead in the centre; with Loop on the sample repeats.
    const double tStart = wrapped (playhead) - 0.5 * viewSeconds;
    auto xOf = [&] (double t) { return area.getX() + float ((t - tStart) / viewSeconds) * area.getWidth(); };

    // Every place a sample time appears in the view (repeated when looping).
    auto forEachX = [&] (double t, auto&& fn)
    {
        if (! loop) { fn (xOf (t)); return; }
        for (double u = t + duration * std::floor ((tStart - t) / duration); u <= tStart + viewSeconds; u += duration)
            fn (xOf (u));
    };

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());

        // Columns sit on a grid fixed to the sample time, so a column always
        // shows the same bins while the view scrolls (no shimmering); x is
        // snapped to whole pixels so neighbouring columns touch without gaps.
        const int64_t n  = int64_t (mins.size());
        const int64_t k0 = int64_t (std::floor (tStart / pxSeconds));
        const int binsPerCol = juce::jmax (1, int (std::round (pxSeconds / binSeconds)));
        for (int64_t k = k0; k <= k0 + cols + 1; ++k)
        {
            double ts = double (k) * pxSeconds;
            if (loop) ts -= duration * std::floor (ts / duration);
            else if (ts < 0.0 || ts >= duration) continue;

            const int64_t b0 = int64_t (ts / binSeconds);
            float mn = 0.f, mx = 0.f;
            for (int j = 0; j < binsPerCol; ++j)
            {
                int64_t b = b0 + j;
                if (b >= n) { if (! loop) break; b -= n; }
                mn = juce::jmin (mn, mins[size_t (b)]);
                mx = juce::jmax (mx, maxs[size_t (b)]);
            }

            const float x0 = std::round (xOf (double (k) * pxSeconds));
            const float x1 = std::round (xOf (double (k + 1) * pxSeconds));
            g.setColour (waveColour (juce::jmax (mx, -mn) / peak));
            g.fillRect (juce::Rectangle<float> (x0, mid - mx / peak * h, juce::jmax (1.f, x1 - x0),
                                                juce::jmax (1.f, (mx - mn) / peak * h)));
        }

        // zero axis
        g.setColour (kText.withAlpha (0.3f));
        g.fillRect (area.getX(), std::round (mid) - 0.5f, area.getWidth(), 1.f);

        // sample start
        g.setColour (kWarn);
        forEachX (0.0, [&] (float x) { g.fillRect (x - 0.5f, area.getY(), 1.f, area.getHeight()); });
    }

    // cue point markers (over the waveform)
    g.setFont (juce::Font (juce::FontOptions (10.f, juce::Font::bold)));
    for (int k = 0; k < VinylTracerProcessor::kNumCues; ++k)
    {
        const double t = cues[size_t (k)];
        if (std::isnan (t)) continue;
        forEachX (t, [&] (float x)
        {
            if (x < area.getX() || x > area.getRight()) return;
            g.setColour (kCueColours[k].withAlpha (0.85f));
            g.fillRect (x - 0.75f, area.getY(), 1.5f, area.getHeight());
            const juce::Rectangle<float> flag (x, area.getY(), 13.f, 13.f); // number at the top
            g.setColour (kCueColours[k]);
            g.fillRect (flag);
            g.setColour (kBg);
            g.drawText (juce::String (k + 1), flag, juce::Justification::centred);
        });
    }

    // playhead (fixed in the centre)
    g.setColour (juce::Colours::white);
    g.fillRect (area.getCentreX() - 1.f, area.getY(), 2.f, area.getHeight());
}

void WaveView::paintOverview (juce::Graphics& g, juce::Rectangle<float> area)
{
    const int cols = juce::jmax (1, int (area.getWidth()));
    const float colW = area.getWidth() / float (cols);
    const float mid  = area.getCentreY();
    const float h    = area.getHeight() * 0.5f * kWaveHeight;

    // one min / max / RMS per pixel column, rebuilt only when the width changes
    if (int (ovMins.size()) != cols)
    {
        const int64_t n = int64_t (mins.size());
        ovMins.assign (size_t (cols), 0.f);
        ovMaxs.assign (size_t (cols), 0.f);
        ovRms.assign (size_t (cols), 0.f);
        ovRmsMax = 0.f;
        for (int c = 0; c < cols; ++c)
        {
            const int64_t b0 = n * c / cols;
            const int64_t b1 = juce::jmax (b0 + 1, n * (c + 1) / cols);
            float mn = 0.f, mx = 0.f;
            double sq = 0.0;
            int64_t cnt = 0;
            for (int64_t b = b0; b < b1 && b < n; ++b, ++cnt)
            {
                mn = juce::jmin (mn, mins[size_t (b)]);
                mx = juce::jmax (mx, maxs[size_t (b)]);
                sq += meanSq[size_t (b)];
            }
            ovMins[size_t (c)] = mn;
            ovMaxs[size_t (c)] = mx;
            ovRms[size_t (c)]  = float (std::sqrt (sq / double (juce::jmax<int64_t> (1, cnt))));
            ovRmsMax = juce::jmax (ovRmsMax, ovRms[size_t (c)]);
        }
        if (ovRmsMax <= 1e-9f) ovRmsMax = 1.f;
    }

    auto xOf = [&] (double t) { return area.getX() + area.getWidth() * float (t / duration); };

    // the part shown in the scrolling view
    {
        const double t = wrapped (playhead);
        const float x0 = xOf (t - 0.5 * viewSeconds), x1 = xOf (t + 0.5 * viewSeconds);
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (area.toNearestInt());
        g.setColour (kText.withAlpha (0.08f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x0, area.getY(), x1, area.getBottom()));
    }

    // A pixel column of a long sample spans a lot of audio, so its peak is
    // nearly always the maximum. The colour follows the loudness (RMS,
    // relative to the loudest column); the peak envelope is drawn faint
    // behind the RMS body.
    for (int c = 0; c < cols; ++c)
    {
        const float mn = ovMins[size_t (c)], mx = ovMaxs[size_t (c)];
        const float rms = ovRms[size_t (c)];
        const juce::Colour col = waveColour (rms / ovRmsMax);
        const float x = area.getX() + float (c) * colW;
        g.setColour (col.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (x, mid - mx / peak * h, colW, juce::jmax (1.f, (mx - mn) / peak * h)));
        const float body = juce::jmin (rms * 1.414f / peak, juce::jmax (mx, -mn) / peak) * h; // sine: peak = RMS * sqrt 2
        g.setColour (col);
        g.fillRect (juce::Rectangle<float> (x, mid - body, colW, juce::jmax (1.f, 2.f * body)));
    }

    // cue points
    for (int k = 0; k < VinylTracerProcessor::kNumCues; ++k)
    {
        const double t = cues[size_t (k)];
        if (std::isnan (t)) continue;
        g.setColour (kCueColours[k]);
        g.fillRect (xOf (juce::jlimit (0.0, duration, t)) - 0.75f, area.getY(), 1.5f, area.getHeight());
    }

    // playhead
    const double t = wrapped (playhead);
    if (t >= 0.0 && t <= duration)
    {
        g.setColour (juce::Colours::white);
        g.fillRect (xOf (t) - 1.f, area.getY(), 2.f, area.getHeight());
    }
}

//==============================================================================
VinylTracerEditor::VinylTracerEditor (VinylTracerProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    auto& vts = proc.apvts;

    auto setupLabel = [this] (juce::Label& l, const juce::String& text)
    {
        l.setText (text, juce::dontSendNotification);
        l.setColour (juce::Label::textColourId, kDim);
        l.setFont (juce::Font (juce::FontOptions (12.f)));
        addAndMakeVisible (l);
    };

    addAndMakeVisible (loadButton);
    loadButton.onClick = [this] { openChooser(); };
#if JUCE_MAC
    const juce::String storeKey = "Cmd", deleteKey = "Option";
#else
    const juce::String storeKey = "Ctrl", deleteKey = "Alt";
#endif
    for (int k = 0; k < VinylTracerProcessor::kNumCues; ++k)
    {
        auto& b = cueButtons[size_t (k)];
        b.setButtonText ("Cue " + juce::String (k + 1));
        b.setTooltip ("Click: play from cue " + juce::String (k + 1) + "\n"
                      + storeKey + "+click: store the current position\n"
                      + deleteKey + "+click: delete");
        b.onClick = [this, k] { cueClicked (k); };
        addAndMakeVisible (b);
        shownCues[size_t (k)] = 0.0; // differs from any stored state -> first update draws it
    }
    updateCueButtons();

    setupLabel (timecodeLabel, "Timecode");
    setupLabel (modeLabel, "Mode");
    setupLabel (rpmLabel, "RPM");
    setupLabel (thresholdLabel, "Threshold (dB)");
    setupLabel (gainLabel, "Output (dB)");

    fillChoices (timecodeBox, vts.getParameter ("timecode"));
    fillChoices (modeBox, vts.getParameter ("mode"));
    fillChoices (rpmBox, vts.getParameter ("rpm"));
    for (auto* b : { &timecodeBox, &modeBox, &rpmBox }) addAndMakeVisible (*b);

    for (auto* s : { &thresholdSlider, &gainSlider })
    {
        s->setSliderStyle (juce::Slider::LinearHorizontal);
        s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 20);
        addAndMakeVisible (*s);
    }
    addAndMakeVisible (thruButton);
    addAndMakeVisible (loopButton);

    setupLabel (viewLabel, "Wave view");
    for (int s : VinylTracerProcessor::kWaveViewChoices)
        viewBox.addItem (juce::String (s) + " s", s); // item id = seconds
    shownViewSeconds = proc.getWaveViewSeconds();
    viewBox.setSelectedId (shownViewSeconds, juce::dontSendNotification);
    viewBox.onChange = [this]
    {
        proc.setWaveViewSeconds (viewBox.getSelectedId());
        shownViewSeconds = proc.getWaveViewSeconds();
    };
    addAndMakeVisible (viewBox);
    wave.setViewSeconds (double (proc.getWaveViewSeconds()));
    wave.onSeek = [this] (double seconds) { proc.jumpTo (seconds); };

    timecodeAtt  = std::make_unique<ComboAttachment>  (vts, "timecode", timecodeBox);
    modeAtt      = std::make_unique<ComboAttachment>  (vts, "mode", modeBox);
    rpmAtt       = std::make_unique<ComboAttachment>  (vts, "rpm", rpmBox);
    thresholdAtt = std::make_unique<SliderAttachment> (vts, "threshold", thresholdSlider);
    gainAtt      = std::make_unique<SliderAttachment> (vts, "gain", gainSlider);
    thruAtt      = std::make_unique<ButtonAttachment> (vts, "thru", thruButton);
    loopAtt      = std::make_unique<ButtonAttachment> (vts, "loop", loopButton);

    sampleLabel.setColour (juce::Label::textColourId, kText);
    statusLabel.setColour (juce::Label::textColourId, kText);
    levelLabel.setColour (juce::Label::textColourId, kDim);
    statusLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.f, juce::Font::plain)));
    levelLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.f, juce::Font::plain)));
    addAndMakeVisible (sampleLabel);
    addAndMakeVisible (statusLabel);
    addAndMakeVisible (levelLabel);

    addAndMakeVisible (scope);
    addAndMakeVisible (wave);

    setSize (780, 460);
    startTimerHz (60); // smooth scrolling of the waveform
}

VinylTracerEditor::~VinylTracerEditor()
{
    stopTimer();
}

void VinylTracerEditor::fillChoices (juce::ComboBox& box, juce::RangedAudioParameter* param)
{
    if (auto* c = dynamic_cast<juce::AudioParameterChoice*> (param))
        box.addItemList (c->choices, 1);
}

void VinylTracerEditor::paint (juce::Graphics& g)
{
    g.fillAll (kBg);
    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (18.f, juce::Font::bold)));
    g.drawText ("Vinyl Tracer", 14, 8, 300, 26, juce::Justification::centredLeft);
    g.setColour (kDim);
    g.setFont (juce::Font (juce::FontOptions (11.f)));
    // Version from CMake (project VERSION), so a replaced build is recognisable.
    g.drawText ("v" JucePlugin_VersionString " prototype", 180, 8, 200, 26, juce::Justification::centredLeft);
}

void VinylTracerEditor::resized()
{
    auto r = getLocalBounds().reduced (14);
    r.removeFromTop (30);

    auto top = r.removeFromTop (28);
    loadButton.setBounds (top.removeFromLeft (130));
    top.removeFromLeft (8);
    for (auto& b : cueButtons)
    {
        b.setBounds (top.removeFromLeft (60));
        top.removeFromLeft (4);
    }
    top.removeFromLeft (8);
    sampleLabel.setBounds (top);

    r.removeFromTop (10);
    auto row = r.removeFromTop (44);
    auto col = [&row] (int w) { auto c = row.removeFromLeft (w); row.removeFromLeft (10); return c; };
    {
        auto c = col (170); timecodeLabel.setBounds (c.removeFromTop (18)); timecodeBox.setBounds (c.removeFromTop (24));
    }
    {
        auto c = col (110); modeLabel.setBounds (c.removeFromTop (18)); modeBox.setBounds (c.removeFromTop (24));
    }
    {
        auto c = col (80); rpmLabel.setBounds (c.removeFromTop (18)); rpmBox.setBounds (c.removeFromTop (24));
    }
    {
        auto c = col (170); thresholdLabel.setBounds (c.removeFromTop (18)); thresholdSlider.setBounds (c.removeFromTop (24));
    }
    {
        auto c = col (150); gainLabel.setBounds (c.removeFromTop (18)); gainSlider.setBounds (c.removeFromTop (24));
    }

    r.removeFromTop (6);
    auto toggles = r.removeFromTop (24);
    thruButton.setBounds (toggles.removeFromLeft (140));
    loopButton.setBounds (toggles.removeFromLeft (80));
    toggles.removeFromLeft (20);
    viewLabel.setBounds (toggles.removeFromLeft (70));
    viewBox.setBounds (toggles.removeFromLeft (80));

    r.removeFromTop (10);
    auto bottom = r;
    auto left = bottom.removeFromLeft (220);
    scope.setBounds (left.removeFromTop (220));
    left.removeFromTop (6);
    levelLabel.setBounds (left.removeFromTop (40));

    bottom.removeFromLeft (12);
    statusLabel.setBounds (bottom.removeFromBottom (48));
    bottom.removeFromBottom (6);
    wave.setBounds (bottom);
}

void VinylTracerEditor::timerCallback()
{
    const int n = proc.readScope (scopeBuf.data(), int (scopeBuf.size() / 2));
    if (n > 0)
    {
        scope.push (scopeBuf.data(), n);
        scope.repaint();
    }

    auto s = proc.getSampleForUI();
    if (s.get() != shownSample)
    {
        shownSample = s.get();
        wave.setSample (s);
        sampleLabel.setText (s != nullptr ? juce::String (s->name) : juce::String ("No sample loaded"),
                             juce::dontSendNotification);
    }

    updateCueButtons(); // also picks up cue points restored with a Live set

    const auto st = proc.getStatus();
    wave.setLoop (loopButton.getToggleState());
    {
        const int span = proc.getWaveViewSeconds();
        wave.setViewSeconds (double (span));
        // Follow the processor only when its value changed elsewhere (a restored
        // state). The box notifies onChange asynchronously; comparing with the
        // box itself here reset a fresh selection before onChange had run.
        if (span != shownViewSeconds)
        {
            shownViewSeconds = span;
            viewBox.setSelectedId (span, juce::dontSendNotification);
        }
    }
    wave.setPlayhead (st.sampleSeconds);

    auto flag = [] (bool b, const char* on, const char* off) { return juce::String (b ? on : off); };
    juce::String line1 = flag (st.signal, "SIGNAL ok ", "SIGNAL -- ")
                       + "| " + flag (st.bitLock, "CODE ok ", "CODE -- ")
                       + "| " + flag (st.anchored, "ABS ok ", "ABS -- ")
                       + "| speed " + juce::String (st.speed, 3) + "x";
    juce::String line2 = "record " + formatTime (st.recordSeconds)
                       + "   sample " + formatTime (st.sampleSeconds)
                       + "   corrections " + juce::String (st.reanchors);
    statusLabel.setText (line1 + "\n" + line2, juce::dontSendNotification);
    statusLabel.setColour (juce::Label::textColourId, st.signal ? kText : kWarn);

    levelLabel.setText ("input " + juce::String (st.levelDb, 1) + " dBFS\nnominal " + juce::String (st.refDb, 1)
                            + " dBFS  cfg " + juce::String (st.config),
                        juce::dontSendNotification);
}

void VinylTracerEditor::cueClicked (int index)
{
    // Ctrl (Cmd on macOS) + click stores / overwrites, Alt (Option) + click
    // deletes, a plain click plays from the cue (nothing if it is empty).
    const auto mods = juce::ModifierKeys::getCurrentModifiers();
    if (mods.isAltDown())          proc.clearCue (index);
    else if (mods.isCommandDown()) proc.storeCue (index);
    else                           proc.recallCue (index);
    updateCueButtons();
}

void VinylTracerEditor::updateCueButtons()
{
    std::array<double, VinylTracerProcessor::kNumCues> now {};
    bool changed = false;
    for (int k = 0; k < VinylTracerProcessor::kNumCues; ++k)
    {
        now[size_t (k)] = proc.getCue (k);
        changed |= ! sameCue (now[size_t (k)], shownCues[size_t (k)]);
    }
    if (! changed)
        return;
    shownCues = now;
    for (int k = 0; k < VinylTracerProcessor::kNumCues; ++k)
    {
        auto& b = cueButtons[size_t (k)];
        const bool set = ! std::isnan (now[size_t (k)]);
        b.setColour (juce::TextButton::buttonColourId, set ? kCueColours[k].withAlpha (0.75f) : kPanel);
        b.setColour (juce::TextButton::textColourOffId, set ? kBg : kDim);
    }
    wave.setCues (now);
    wave.repaint();
}

void VinylTracerEditor::openChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Load sample", juce::File(),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto f = fc.getResult();
                              if (f.existsAsFile())
                                  loadFile (f);
                          });
}

void VinylTracerEditor::loadFile (const juce::File& f)
{
    juce::String err;
    if (! proc.loadSample (f, err))
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Vinyl Tracer", err);
}

bool VinylTracerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (f.endsWithIgnoreCase (".wav") || f.endsWithIgnoreCase (".aif") || f.endsWithIgnoreCase (".aiff")
            || f.endsWithIgnoreCase (".flac") || f.endsWithIgnoreCase (".mp3") || f.endsWithIgnoreCase (".ogg"))
            return true;
    return false;
}

void VinylTracerEditor::filesDropped (const juce::StringArray& files, int, int)
{
    if (! files.isEmpty())
        loadFile (juce::File (files[0]));
}
