#include "ScratchPlayer.h"

#include <algorithm>
#include <cmath>

namespace vt {

namespace {
constexpr double kPi = 3.141592653589793;

// Half-band low-pass (cut-off at a quarter of the sample rate), Blackman window.
std::vector<float> makeHalfbandKernel (int halfLen)
{
    const int n = 2 * halfLen + 1;
    std::vector<float> h (static_cast<size_t> (n));
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const int k = i - halfLen;
        const double x = 0.5 * double (k);
        const double sinc = (k == 0) ? 1.0 : std::sin (kPi * x) / (kPi * x);
        const double w = 0.42 - 0.5 * std::cos (2.0 * kPi * i / (n - 1)) + 0.08 * std::cos (4.0 * kPi * i / (n - 1));
        h[size_t (i)] = float (sinc * w);
        sum += sinc * w;
    }
    for (auto& v : h) v = float (v / sum);
    return h;
}

inline float hermite (float xm1, float x0, float x1, float x2, float t) noexcept
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}
} // namespace

void SampleData::build (const float* const* channels, int numCh, int64_t numFrames, double rate, int numLevels)
{
    numChannels = std::clamp (numCh, 1, 2);
    length      = numFrames;
    sampleRate  = rate;
    levels.assign (size_t (std::max (1, numLevels)), {});

    levels[0].resize (size_t (numChannels));
    for (int c = 0; c < numChannels; ++c)
        levels[0][size_t (c)].assign (channels[c], channels[c] + numFrames);

    const auto h = makeHalfbandKernel (15);
    const int half = 15;
    for (size_t k = 1; k < levels.size(); ++k)
    {
        const auto& prev = levels[k - 1];
        levels[k].resize (size_t (numChannels));
        for (int c = 0; c < numChannels; ++c)
        {
            const auto& x = prev[size_t (c)];
            const int64_t nIn  = int64_t (x.size());
            const int64_t nOut = (nIn + 1) / 2;
            auto& y = levels[k][size_t (c)];
            y.resize (size_t (nOut));
            for (int64_t m = 0; m < nOut; ++m)
            {
                double acc = 0.0;
                const int64_t centre = 2 * m; // centred: no delay between levels
                for (int j = -half; j <= half; ++j)
                {
                    const int64_t idx = centre + j;
                    if (idx >= 0 && idx < nIn)
                        acc += double (h[size_t (j + half)]) * double (x[size_t (idx)]);
                }
                y[size_t (m)] = float (acc);
            }
        }
    }
}

void ScratchPlayer::prepare (double hostSampleRate)
{
    hostSr_ = hostSampleRate > 0 ? hostSampleRate : 48000.0;
    kRate_  = 1.0 - std::exp (-1.0 / (0.002 * hostSr_));
    dcR_    = std::exp (-2.0 * kPi * 5.0 / hostSr_);
    reset();
}

void ScratchPlayer::reset()
{
    rateSmooth_ = 0.0;
    havePrev_ = false;
    dcX_[0] = dcX_[1] = dcY_[0] = dcY_[1] = 0.0;
}

float ScratchPlayer::readLevel (const SampleData& s, int level, int ch, double pos, bool loop) const noexcept
{
    const auto& x = s.levels[size_t (level)][size_t (ch)];
    const int64_t n = int64_t (x.size());
    if (n < 4) return 0.f;

    const double fl = std::floor (pos);
    const float  t  = float (pos - fl);
    const int64_t i0 = int64_t (fl);

    auto at = [&] (int64_t i) -> float
    {
        if (loop)
        {
            i %= n;
            if (i < 0) i += n;
            return x[size_t (i)];
        }
        return (i >= 0 && i < n) ? x[size_t (i)] : 0.f;
    };

    if (! loop && (i0 < -2 || i0 > n + 1))
        return 0.f;
    return hermite (at (i0 - 1), at (i0), at (i0 + 1), at (i0 + 2), t);
}

void ScratchPlayer::render (const SampleData* sample, const double* vinylPos, int numSamples,
                            const Params& p, float* outL, float* outR) noexcept
{
    if (sample == nullptr || sample->length < 4 || sample->levels.empty())
    {
        std::fill (outL, outL + numSamples, 0.f);
        std::fill (outR, outR + numSamples, 0.f);
        havePrev_ = false;
        return;
    }

    const SampleData& s = *sample;
    const double toFrames = s.sampleRate / p.cyclesPerSecond;
    const int maxLevel = int (s.levels.size()) - 1;
    const int chR = s.numChannels > 1 ? 1 : 0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double src = (vinylPos[i] - p.cueCycles) * toFrames; // level-0 frames

        // playback rate in source frames per output sample (for anti-aliasing)
        const double ratio = s.sampleRate / hostSr_;
        double rate = havePrev_ ? std::abs (src - prevSrc_) : ratio;
        rate = std::min (rate, 64.0);
        rateSmooth_ += kRate_ * (rate - rateSmooth_);
        prevSrc_ = src;
        havePrev_ = true;

        float l = 0.f, r = 0.f;
        const double lvl = std::clamp (std::log2 (std::max (rateSmooth_, 1.0)), 0.0, double (maxLevel));
        const int    k0  = std::min (int (lvl), maxLevel);
        const float  fr  = float (lvl - double (k0));
        {
            const double pk = src / double (int64_t (1) << k0);
            l = readLevel (s, k0, 0, pk, p.loop);
            r = readLevel (s, k0, chR, pk, p.loop);
        }
        if (fr > 0.f && k0 < maxLevel)
        {
            const double pk = src / double (int64_t (1) << (k0 + 1));
            const float l1 = readLevel (s, k0 + 1, 0, pk, p.loop);
            const float r1 = readLevel (s, k0 + 1, chR, pk, p.loop);
            l += fr * (l1 - l);
            r += fr * (r1 - r);
        }

        // DC blocker: a stopped record holds a constant value -> silence
        const double yl = double (l) - dcX_[0] + dcR_ * dcY_[0];
        const double yr = double (r) - dcX_[1] + dcR_ * dcY_[1];
        dcX_[0] = l; dcY_[0] = yl;
        dcX_[1] = r; dcY_[1] = yr;

        outL[i] = float (yl) * p.gain;
        outR[i] = float (yr) * p.gain;
    }

    lastSrcSeconds_ = prevSrc_ / s.sampleRate;
}

} // namespace vt
