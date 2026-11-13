// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>

namespace
{
const juce::StringArray kTimecodeNames { "Serato CV02 Side A", "Serato CV02 Side B" };
const juce::StringArray kModeNames     { "Absolute", "Relative" };
const juce::StringArray kRpmNames      { "33 1/3", "45" };
} // namespace

//==============================================================================
VinylTracerProcessor::VinylTracerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Timecode", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",   juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMS", createLayout())
{
    tables[0] = std::make_unique<vt::LfsrTable> (vt::kSeratoSideA);
    tables[1] = std::make_unique<vt::LfsrTable> (vt::kSeratoSideB);

    pTimecode  = apvts.getRawParameterValue ("timecode");
    pMode      = apvts.getRawParameterValue ("mode");
    pRpm       = apvts.getRawParameterValue ("rpm");
    pThreshold = apvts.getRawParameterValue ("threshold");
    pGain      = apvts.getRawParameterValue ("gain");
    pThru      = apvts.getRawParameterValue ("thru");
    pLoop      = apvts.getRawParameterValue ("loop");

    for (auto& c : cuePoints) c.store (std::nan (""));

    formatManager.registerBasicFormats();
    startTimer (500);
}

VinylTracerProcessor::~VinylTracerProcessor()
{
    stopTimer();
}

juce::AudioProcessorValueTreeState::ParameterLayout VinylTracerProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "timecode", 1 }, "Timecode", kTimecodeNames, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "mode", 1 }, "Mode", kModeNames, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { "rpm", 1 }, "RPM", kRpmNames, 0));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "threshold", 1 }, "Threshold",
                                                        NormalisableRange<float> (-60.f, -12.f, 0.5f), -36.f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { "gain", 1 }, "Output Gain",
                                                        NormalisableRange<float> (-24.f, 12.f, 0.1f), 0.f,
                                                        AudioParameterFloatAttributes().withLabel ("dB")));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "thru", 1 }, "Timecode Thru", false));
    layout.add (std::make_unique<AudioParameterBool> (ParameterID { "loop", 1 }, "Loop Sample", false));
    return layout;
}

bool VinylTracerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

//==============================================================================
void VinylTracerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    decoder.prepare (sampleRate);
    currentTimecode = -1;
    if (savedConfig.load() >= 0)
        decoder.setPreferredConfig (savedConfig.load());
    if (savedNominalDb.load() > kNoNominal)
        decoder.setNominalDb (savedNominalDb.load());
    player.prepare (sampleRate);

    const size_t n = size_t (juce::jmax (64, samplesPerBlock));
    posAbs.assign (n, 0.0);
    posRel.assign (n, 0.0);
    vinyl.assign (n, 0.0);
    quality.assign (n, 1.f);
    speedBuf.assign (n, 0.f);
    lockErrBuf.assign (n, 0.f);
    offsetBuf.assign (n, 0.f);
    diagFrameLen = juce::jmax (1, juce::roundToInt (sampleRate / 1000.0));
    diagCount = diagOpen = 0;
    lastMode = -1;
}

void VinylTracerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    if (buffer.getNumChannels() < 2 || numSamples <= 0)
    {
        buffer.clear();
        return;
    }

    // Parameters
    const int tc = juce::jlimit (0, 1, int (pTimecode->load()));
    if (tc != currentTimecode)
    {
        decoder.setTable (tables[size_t (tc)].get());
        currentTimecode = tc;
    }
    decoder.setGateDb (pThreshold->load());
    if (nominalRequest.exchange (false))
        decoder.setNominalDb (savedNominalDb.load());

    const int  mode = int (pMode->load());          // 0 = absolute, 1 = relative
    const bool thru = pThru->load() > 0.5f;

    vt::ScratchPlayer::Params pp;
    pp.cyclesPerSecond = tables[size_t (tc)]->def().resolution * (int (pRpm->load()) == 1 ? 1.35 : 1.0);
    pp.gain            = juce::Decibels::decibelsToGain (pGain->load());
    pp.loop            = pLoop->load() > 0.5f;

    const vt::SampleData* sample = audioSample.load (std::memory_order_acquire);

    float* outL = buffer.getWritePointer (0);
    float* outR = buffer.getWritePointer (1);
    const float* inL = buffer.getReadPointer (0);
    const float* inR = buffer.getReadPointer (1);

    // input peaks for the level meter (before the output overwrites the input)
    {
        const float pl = buffer.getMagnitude (0, 0, numSamples);
        const float pr = buffer.getMagnitude (1, 0, numSamples);
        if (pl > inPeak[0].load (std::memory_order_relaxed)) inPeak[0].store (pl, std::memory_order_relaxed);
        if (pr > inPeak[1].load (std::memory_order_relaxed)) inPeak[1].store (pr, std::memory_order_relaxed);
    }

    const int chunkMax = int (vinyl.size());
    for (int offset = 0; offset < numSamples; offset += chunkMax)
    {
        const int n = juce::jmin (chunkMax, numSamples - offset);

        // 1. decode (reads the input before the output overwrites it)
        decoder.process (inL + offset, inR + offset, n, posAbs.data(), posRel.data(), speedBuf.data(), quality.data(),
                         lockErrBuf.data(), offsetBuf.data());

        // diagnostics: 1 ms frames for the editor (dropped while it does not read them)
        for (int i = 0; i < n; ++i)
        {
            const float v = speedBuf[size_t (i)], e = lockErrBuf[size_t (i)], o = offsetBuf[size_t (i)];
            if (diagCount == 0)
                diagAcc = { v, v, e, 0.f, 0.f };
            diagAcc.vMin = juce::jmin (diagAcc.vMin, v);
            diagAcc.vMax = juce::jmax (diagAcc.vMax, v);
            diagAcc.lockErr = juce::jmax (diagAcc.lockErr, e);
            if (o >= 0.f) { diagAcc.offset = juce::jmax (diagAcc.offset, o); ++diagOpen; }
            if (++diagCount >= diagFrameLen)
            {
                diagAcc.open = float (diagOpen) / float (diagCount);
                int s1, n1, s2, n2;
                diagFifo.prepareToWrite (1, s1, n1, s2, n2);
                if (n1 + n2 > 0)
                {
                    diagData[size_t (n1 > 0 ? s1 : s2)] = diagAcc;
                    diagFifo.finishedWrite (1);
                }
                diagCount = diagOpen = 0;
            }
        }

        // scope: raw input, normalised to the tracked nominal level
        {
            const float refLin = juce::Decibels::decibelsToGain (decoder.status().refDb, -120.f);
            const float norm = refLin > 1e-6f ? 1.f / refLin : 1.f;
            for (int i = 0; i < n; ++i)
            {
                if (++scopeDecim < 2) continue;
                scopeDecim = 0;
                int s1, n1, s2, n2;
                scopeFifo.prepareToWrite (1, s1, n1, s2, n2);
                if (n1 + n2 == 0) break;
                const int idx = n1 > 0 ? s1 : s2;
                scopeData[size_t (idx * 2)]     = inL[offset + i] * norm;
                scopeData[size_t (idx * 2 + 1)] = inR[offset + i] * norm;
                scopeFifo.finishedWrite (1);
            }
        }

        // 2. record position in the current mode's coordinates
        if (mode != lastMode)
        {
            if (mode == 1) relBase = lastVinyl - posRel[0]; // continue seamlessly
            lastMode = mode;
        }
        if (mode == 1)
            for (int i = 0; i < n; ++i) vinyl[size_t (i)] = posRel[size_t (i)] + relBase;
        else
            for (int i = 0; i < n; ++i) vinyl[size_t (i)] = posAbs[size_t (i)];
        lastVinyl = vinyl[size_t (n - 1)];

        // 3. sample start: automatically at the first absolute lock; a jump (a
        //    recalled cue point, a click in the overview) moves it so that the
        //    record plays from that point now (the player cross-fades the jump)
        const bool anchored = decoder.status().anchored;
        if (! cueValid.load() && (anchored || mode == 1))
        {
            cueCycles.store (lastVinyl);
            cueValid.store (true);
        }
        const double jump = jumpRequest.exchange (std::nan (""));
        if (! std::isnan (jump))
        {
            cueCycles.store (lastVinyl - jump * pp.cyclesPerSecond);
            cueValid.store (true);
        }
        pp.cueCycles = cueCycles.load();

        // 4. output
        if (thru)
            continue; // leave the timecode in the buffer for checking the routing

        player.render (sample, vinyl.data(), n, pp, outL + offset, outR + offset, quality.data());
    }

    for (int ch = 2; ch < buffer.getNumChannels(); ++ch)
        buffer.clear (ch, 0, numSamples);

    // Publish status for the editor
    const auto& st = decoder.status();
    stSignal.store (st.signal);
    stBitLock.store (st.bitLock);
    stAnchored.store (st.anchored);
    stSpeed.store (st.speed);
    stRecord.store (lastVinyl / pp.cyclesPerSecond);
    stSample.store (player.lastSourceSeconds());
    stLevel.store (st.levelDb);
    stRef.store (st.refDb);
    stGate.store (st.gateDb);
    stConfig.store (st.config);
    stReanchors.store (st.reanchors);
    if (st.config >= 0) savedConfig.store (st.config);
    if (st.refLearned)  savedNominalDb.store (st.refDb);
}

//==============================================================================
VinylTracerProcessor::Status VinylTracerProcessor::getStatus() const noexcept
{
    Status s;
    s.signal        = stSignal.load();
    s.bitLock       = stBitLock.load();
    s.anchored      = stAnchored.load();
    s.cueValid      = cueValid.load();
    s.speed         = stSpeed.load();
    s.recordSeconds = stRecord.load();
    s.sampleSeconds = stSample.load();
    s.levelDb       = stLevel.load();
    s.refDb         = stRef.load();
    s.gateDb        = stGate.load();
    s.config        = stConfig.load();
    s.reanchors     = stReanchors.load();
    return s;
}

void VinylTracerProcessor::storeCue (int index) noexcept
{
    if (index < 0 || index >= kNumCues || currentSample == nullptr || currentSample->length == 0)
        return;
    // Position of the last block, kept inside the sample (wrapped when looping).
    const double len = double (currentSample->length) / currentSample->sampleRate;
    double t = stSample.load();
    if (pLoop->load() > 0.5f) t -= len * std::floor (t / len);
    cuePoints[size_t (index)].store (juce::jlimit (0.0, len, t));
}

void VinylTracerProcessor::recallCue (int index) noexcept
{
    if (index >= 0 && index < kNumCues)
        jumpTo (cuePoints[size_t (index)].load()); // nothing when empty (NaN)
}

void VinylTracerProcessor::clearCue (int index) noexcept
{
    if (index >= 0 && index < kNumCues)
        cuePoints[size_t (index)].store (std::nan (""));
}

double VinylTracerProcessor::getCue (int index) const noexcept
{
    return (index >= 0 && index < kNumCues) ? cuePoints[size_t (index)].load() : std::nan ("");
}

int VinylTracerProcessor::readScope (float* xy, int maxPoints) noexcept
{
    int s1, n1, s2, n2;
    scopeFifo.prepareToRead (maxPoints, s1, n1, s2, n2);
    int k = 0;
    for (int i = 0; i < n1; ++i, ++k) { xy[k * 2] = scopeData[size_t ((s1 + i) * 2)]; xy[k * 2 + 1] = scopeData[size_t ((s1 + i) * 2 + 1)]; }
    for (int i = 0; i < n2; ++i, ++k) { xy[k * 2] = scopeData[size_t ((s2 + i) * 2)]; xy[k * 2 + 1] = scopeData[size_t ((s2 + i) * 2 + 1)]; }
    scopeFifo.finishedRead (n1 + n2);
    return k;
}

int VinylTracerProcessor::readDiagnostics (DiagFrame* out, int maxFrames) noexcept
{
    int s1, n1, s2, n2;
    diagFifo.prepareToRead (maxFrames, s1, n1, s2, n2);
    int k = 0;
    for (int i = 0; i < n1; ++i) out[k++] = diagData[size_t (s1 + i)];
    for (int i = 0; i < n2; ++i) out[k++] = diagData[size_t (s2 + i)];
    diagFifo.finishedRead (n1 + n2);
    return k;
}

//==============================================================================
bool VinylTracerProcessor::loadSample (const juce::File& file, juce::String& error)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
    {
        error = "Unsupported or unreadable file: " + file.getFileName();
        return false;
    }

    const double maxSeconds = 20.0 * 60.0;
    const auto frames = reader->lengthInSamples;
    if (frames <= 0 || double (frames) / reader->sampleRate > maxSeconds)
    {
        error = "File is empty or longer than 20 minutes.";
        return false;
    }

    const int numCh = juce::jlimit (1, 2, int (reader->numChannels));
    juce::AudioBuffer<float> buf (numCh, int (frames));
    reader->read (&buf, 0, int (frames), 0, true, numCh > 1);

    auto sd = std::make_shared<vt::SampleData>();
    const float* chans[2] = { buf.getReadPointer (0), buf.getReadPointer (numCh > 1 ? 1 : 0) };
    sd->build (chans, numCh, int64_t (frames), reader->sampleRate);
    sd->name = file.getFileName().toStdString();

    if (currentSample != nullptr)
        retired.push_back ({ currentSample, juce::Time::getMillisecondCounter() });
    currentSample = sd;
    audioSample.store (sd.get(), std::memory_order_release);
    sampleFile = file;
    for (int i = 0; i < kNumCues; ++i) clearCue (i); // cue points belong to the previous sample
    return true;
}

void VinylTracerProcessor::timerCallback()
{
    // Free replaced samples once the audio thread can no longer be using them.
    const auto now = juce::Time::getMillisecondCounter();
    retired.erase (std::remove_if (retired.begin(), retired.end(),
                                   [now] (const Retired& r) { return now - r.time > 2000; }),
                   retired.end());
}

//==============================================================================
void VinylTracerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.setProperty ("samplePath", sampleFile.getFullPathName(), nullptr);
    state.setProperty ("cueCycles", cueCycles.load(), nullptr);
    state.setProperty ("cueValid", cueValid.load(), nullptr);
    for (int i = 0; i < kNumCues; ++i)
    {
        const double t = cuePoints[size_t (i)].load();
        if (! std::isnan (t))
            state.setProperty ("cuePoint" + juce::String (i + 1), t, nullptr);
    }
    state.setProperty ("waveViewSeconds", waveViewSeconds.load(), nullptr);
    state.setProperty ("showDiagnostics", showDiagnostics.load(), nullptr);
    state.setProperty ("config", savedConfig.load(), nullptr);
    if (savedNominalDb.load() > kNoNominal)
        state.setProperty ("nominalDb", savedNominalDb.load(), nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void VinylTracerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    apvts.replaceState (state);

    cueCycles.store (double (state.getProperty ("cueCycles", 0.0)));
    cueValid.store (bool (state.getProperty ("cueValid", false)));
    setWaveViewSeconds (int (state.getProperty ("waveViewSeconds", 2)));
    showDiagnostics.store (bool (state.getProperty ("showDiagnostics", false)));
    const int cfg = int (state.getProperty ("config", -1));
    savedConfig.store (cfg);
    if (cfg >= 0)
        decoder.setPreferredConfig (cfg);
    const float nominal = float (state.getProperty ("nominalDb", double (kNoNominal)));
    if (nominal > kNoNominal)
    {
        savedNominalDb.store (nominal);
        nominalRequest.store (true); // applied on the audio thread
    }

    const juce::File f (state.getProperty ("samplePath", juce::String()).toString());
    if (f.existsAsFile())
    {
        juce::String err;
        loadSample (f, err);
    }

    // after loading the sample (which clears them)
    for (int i = 0; i < kNumCues; ++i)
    {
        const juce::Identifier id ("cuePoint" + juce::String (i + 1));
        cuePoints[size_t (i)].store (state.hasProperty (id) ? double (state.getProperty (id)) : std::nan (""));
    }
}

//==============================================================================
juce::AudioProcessorEditor* VinylTracerProcessor::createEditor()
{
    return new VinylTracerEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VinylTracerProcessor();
}
