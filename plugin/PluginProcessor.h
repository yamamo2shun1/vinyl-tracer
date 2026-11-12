#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "ScratchPlayer.h"
#include "TimecodeDecoder.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class VinylTracerProcessor : public juce::AudioProcessor,
                                 private juce::Timer
{
public:
    VinylTracerProcessor();
    ~VinylTracerProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Message-thread API for the editor
    bool loadSample (const juce::File& file, juce::String& error);
    juce::File getSampleFile() const { return sampleFile; }
    std::shared_ptr<const vt::SampleData> getSampleForUI() const { return currentSample; }

    // Sets the sample start to the current record position (done on the audio thread).
    void requestCue() noexcept { cueRequest.store (true); }

    struct Status
    {
        bool   signal = false, bitLock = false, anchored = false, cueValid = false;
        double speed = 0.0, recordSeconds = 0.0, sampleSeconds = 0.0;
        float  levelDb = -120.f, refDb = -120.f;
        int    config = -1, reanchors = 0;
    };
    Status getStatus() const noexcept;

    // Lissajous points (x, y interleaved), normalised to the nominal level.
    int readScope (float* xy, int maxPoints) noexcept;

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    void timerCallback() override;

    // Timecode tables (built once, ~8 MB)
    std::array<std::unique_ptr<vt::LfsrTable>, 2> tables;

    vt::TimecodeDecoder decoder;
    vt::ScratchPlayer   player;

    std::vector<double> posAbs, posRel, vinyl;
    int    currentTimecode = -1;
    int    lastMode = -1;
    double relBase = 0.0;
    double lastVinyl = 0.0;

    std::atomic<double> cueCycles { 0.0 };
    std::atomic<bool>   cueValid { false };
    std::atomic<bool>   cueRequest { false };
    std::atomic<int>    savedConfig { -1 };

    // Sample ownership: the message thread owns shared_ptrs; the audio thread
    // only sees a raw pointer. Replaced samples are kept alive for a while.
    std::shared_ptr<const vt::SampleData> currentSample;
    std::atomic<const vt::SampleData*> audioSample { nullptr };
    struct Retired { std::shared_ptr<const vt::SampleData> s; juce::uint32 time; };
    std::vector<Retired> retired;
    juce::File sampleFile;
    juce::AudioFormatManager formatManager;

    // Status published by the audio thread
    std::atomic<bool>   stSignal { false }, stBitLock { false }, stAnchored { false };
    std::atomic<double> stSpeed { 0.0 }, stRecord { 0.0 }, stSample { 0.0 };
    std::atomic<float>  stLevel { -120.f }, stRef { -120.f };
    std::atomic<int>    stConfig { -1 }, stReanchors { 0 };

    // Scope FIFO (single producer: audio thread, single consumer: editor)
    static constexpr int scopeSize = 4096;
    juce::AbstractFifo scopeFifo { scopeSize };
    std::array<float, scopeSize * 2> scopeData {};
    int scopeDecim = 0;

    // Parameters
    std::atomic<float>* pTimecode = nullptr;
    std::atomic<float>* pMode = nullptr;
    std::atomic<float>* pRpm = nullptr;
    std::atomic<float>* pThreshold = nullptr;
    std::atomic<float>* pGain = nullptr;
    std::atomic<float>* pThru = nullptr;
    std::atomic<float>* pLoop = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VinylTracerProcessor)
};
