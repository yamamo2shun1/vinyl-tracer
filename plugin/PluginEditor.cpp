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

    g.setColour (kAccent);
    for (const auto& p : points)
    {
        const float x = c.x + juce::jlimit (-1.4f, 1.4f, p.x) * radius;
        const float y = c.y - juce::jlimit (-1.4f, 1.4f, p.y) * radius;
        g.fillRect (x, y, 1.5f, 1.5f);
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
    duration = 0.0;
    name = {};
    if (s == nullptr || s->levels.empty() || s->length == 0)
    {
        repaint();
        return;
    }

    const int bins = 4096; // aggregated per pixel column in paint()
    const auto& x = s->levels[0][0];
    mins.assign (bins, 0.f);
    maxs.assign (bins, 0.f);
    for (int b = 0; b < bins; ++b)
    {
        const int64_t a = int64_t (x.size()) * b / bins;
        const int64_t e = juce::jmax (a + 1, int64_t (x.size()) * (b + 1) / bins);
        float mn = 0.f, mx = 0.f;
        for (int64_t i = a; i < e && i < int64_t (x.size()); ++i)
        {
            mn = juce::jmin (mn, x[size_t (i)]);
            mx = juce::jmax (mx, x[size_t (i)]);
        }
        mins[size_t (b)] = mn;
        maxs[size_t (b)] = mx;
    }
    duration = double (s->length) / s->sampleRate;
    name = juce::String (s->name);
    repaint();
}

void WaveView::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (r, 6.f);

    if (mins.empty())
    {
        g.setColour (kDim);
        g.setFont (juce::Font (juce::FontOptions (14.f)));
        g.drawText ("Drop an audio file here or press \"Load Sample...\"", r, juce::Justification::centred);
        return;
    }

    auto inner = r.reduced (6.f, 10.f);
    const float mid = inner.getCentreY();
    const float h = inner.getHeight() * 0.5f;
    // One opaque column per pixel, touching its neighbours: drawing the bins as
    // semi-transparent lines at rounded positions left periodic gaps/overlaps
    // that looked like discontinuities in the sample.
    g.setColour (kPanel.interpolatedWith (kAccent, 0.55f));
    const int n    = int (mins.size());
    const int cols = juce::jmax (1, int (inner.getWidth()));
    const float colW = inner.getWidth() / float (cols);
    for (int c = 0; c < cols; ++c)
    {
        const int b0 = int (int64_t (c) * n / cols);
        const int b1 = juce::jmax (b0 + 1, int (int64_t (c + 1) * n / cols));
        float mn = 0.f, mx = 0.f;
        for (int b = b0; b < b1 && b < n; ++b)
        {
            mn = juce::jmin (mn, mins[size_t (b)]);
            mx = juce::jmax (mx, maxs[size_t (b)]);
        }
        g.fillRect (juce::Rectangle<float> (inner.getX() + float (c) * colW, mid - mx * h,
                                            colW, juce::jmax (1.f, (mx - mn) * h)));
    }

    // sample start (cue) and playhead
    g.setColour (kWarn);
    g.drawVerticalLine (int (inner.getX()), inner.getY(), inner.getBottom());
    if (duration > 0.0 && playhead >= 0.0 && playhead <= duration)
    {
        const float px = inner.getX() + inner.getWidth() * float (playhead / duration);
        g.setColour (juce::Colours::white);
        g.fillRect (px - 1.f, inner.getY(), 2.f, inner.getHeight());
    }

    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (12.f)));
    g.drawText (name + "   (" + juce::String (duration, 2) + " s)", r.reduced (8.f, 4.f), juce::Justification::topLeft);
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
    addAndMakeVisible (cueButton);
    loadButton.onClick = [this] { openChooser(); };
    cueButton.onClick  = [this] { proc.requestCue(); };

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
    startTimerHz (30);
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
    cueButton.setBounds (top.removeFromLeft (120));
    top.removeFromLeft (12);
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

    const auto st = proc.getStatus();
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
