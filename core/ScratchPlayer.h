// ScratchPlayer.h
//
// Plays a sample from a record position given for every output sample.
// Position-driven (not speed-driven) playback: the source position is a
// direct function of the decoded record position, so the sample can never
// drift away from the record.
//
// Anti-aliasing for fast movements uses pre-filtered half-rate copies of the
// sample (mip levels) that are cross-faded according to the playback rate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vt {

struct SampleData
{
    // levels[k][ch] : sample data decimated by 2^k (level 0 = original)
    std::vector<std::vector<std::vector<float>>> levels;
    int     numChannels = 0;
    int64_t length      = 0;   // frames at level 0
    double  sampleRate  = 44100.0;
    std::string name;

    // Build from planar float data (1 or 2 channels). Creates the mip levels.
    void build (const float* const* channels, int numCh, int64_t numFrames, double rate,
                int numLevels = 4);
};

class ScratchPlayer
{
public:
    void prepare (double hostSampleRate);
    void reset();

    struct Params
    {
        double cueCycles       = 0.0;    // record position of the sample start
        double cyclesPerSecond = 1000.0; // 1000 at 33 1/3, 1350 at 45 rpm
        float  gain            = 1.0f;
        bool   loop            = false;
    };

    // vinylPos: record position in cycles for each output sample.
    void render (const SampleData* sample, const double* vinylPos, int numSamples,
                 const Params& p, float* outL, float* outR) noexcept;

    double lastSourceSeconds() const noexcept { return lastSrcSeconds_; }

private:
    float readLevel (const SampleData& s, int level, int ch, double pos, bool loop) const noexcept;

    double hostSr_ = 48000.0;
    double rateSmooth_ = 0.0, kRate_ = 0.0;
    double prevSrc_ = 0.0;
    bool   havePrev_ = false;
    double lastSrcSeconds_ = 0.0;

    // output DC blocker (a held sample when the record stops must fade to silence)
    double dcX_[2] { 0, 0 }, dcY_[2] { 0, 0 }, dcR_ = 0.9995;
};

} // namespace vt
