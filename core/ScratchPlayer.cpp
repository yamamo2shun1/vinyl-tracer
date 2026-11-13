// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

#include "ScratchPlayer.h"

#include <algorithm>
#include <cmath>

namespace vt {

namespace {
constexpr double kPi = 3.141592653589793;

// Standstill mute (record positions in cycles = ms of record at 33 1/3 rpm)
constexpr double kCalmCycles      = 0.5;  // staying within this of one point ...
constexpr double kStillSeconds    = 0.05; // ... this long means the record is stopped
// Leaving the stopped state: either far away from the anchor, or a little
// away and moving at a real pace. A knock on the desk makes the decoded
// position creep ~2 cycles over ~25 ms (the stylus orbits like a slow
// rotation), which is neither.
constexpr double kBreakawayCycles = 4.0;   // this far from the anchor: moving again
constexpr double kBreakNearCycles = 1.5;   // this far ...
constexpr double kBreakPaceCycles = 1.5;   // ... and this much travel in the last
constexpr double kBreakPaceSeconds = 0.01; // 10 ms (0.15x): moving again
constexpr double kMuteRampSeconds = 0.003;
constexpr double kAnchorFollowSeconds = 0.3; // while stopped, the anchor follows slow creep

// Low-speed position smoothing: tau = kSmoothSeconds / (1 + speed / knee)
constexpr double kSmoothSeconds   = 0.008;
constexpr double kSmoothKneeSpeed = 0.03;  // x; added lag at speed -> 2 * 8 ms * 0.03 = 0.48 cycles
constexpr double kNetSeconds      = 0.002; // averaging for the net speed (cancels trembling)

// Vinyl-like level near zero speed
constexpr double kVinylKneeSpeed  = 0.2;      // x; full level above, proportional to speed below
constexpr double kGainSpeedSeconds = 0.001;

// Position jumps (absolute corrections after a needle skip / tracker slip,
// needle drops, cue changes): a step faster than any record motion is
// cross-faded instead of played as a click.
constexpr double kJumpSpeed       = 40.0;   // x; the decoder's speed is limited to 16x
constexpr double kJumpFadeSeconds = 0.005;

// Level following the decoder's signal quality (stylus skating / skipping)
constexpr double kQualityDownSeconds = 0.0005;
constexpr double kQualityUpSeconds   = 0.003;
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
    stillAfter_ = int (kStillSeconds * hostSr_);
    kMute_  = float (1.0 - std::exp (-1.0 / (kMuteRampSeconds * hostSr_)));
    kAnchorFollow_ = 1.0 - std::exp (-1.0 / (kAnchorFollowSeconds * hostSr_));
    kNet_      = 1.0 - std::exp (-1.0 / (kNetSeconds * hostSr_));
    kNetSpeed_ = kNet_;
    kGainSpeed_ = 1.0 - std::exp (-1.0 / (kGainSpeedSeconds * hostSr_));
    paceBuf_.assign (size_t (std::max (1, int (kBreakPaceSeconds * hostSr_))), 0.0);
    fadeLen_ = std::max (1, int (kJumpFadeSeconds * hostSr_));
    kQualityDown_ = float (1.0 - std::exp (-1.0 / (kQualityDownSeconds * hostSr_)));
    kQualityUp_   = float (1.0 - std::exp (-1.0 / (kQualityUpSeconds * hostSr_)));
    reset();
}

void ScratchPlayer::reset()
{
    rateSmooth_ = 0.0;
    havePrev_ = false;
    dcX_[0] = dcX_[1] = dcY_[0] = dcY_[1] = 0.0;
    still_ = false;
    calmSamples_ = 0;
    muteGain_ = 1.f;
    havePos_ = false;
    speedNet_ = 0.0;
    haveCue_ = false;
    fadeLeft_ = 0;
    oldShift_ = lastStep_ = 0.0;
    qualityGain_ = 1.f;
}

void ScratchPlayer::startFade (double shift) noexcept
{
    // shift: source frames from the new read position to the one that was
    // playing. If the previous fade is still mostly the old position, that
    // one keeps fading out.
    if (fadeLeft_ * 2 > fadeLen_) oldShift_ += shift;
    else                          oldShift_ = shift;
    fadeLeft_ = fadeLen_;
    prevSrc_ -= shift; // no rate spike for the anti-aliasing
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
                            const Params& p, float* outL, float* outR, const float* quality) noexcept
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

    const double toSpeed = hostSr_ / p.cyclesPerSecond; // cycles per sample -> x
    const double jumpStep = kJumpSpeed / toSpeed;        // cycles per sample

    if (haveCue_ && p.cueCycles != lastCue_ && havePrev_)
        startFade ((p.cueCycles - lastCue_) * toFrames);
    lastCue_ = p.cueCycles;
    haveCue_ = true;

    // Reads the sample at a source position (level-0 frames), mip levels
    // cross-faded by the playback rate.
    auto readAt = [&] (double src, double lvl, float& l, float& r)
    {
        const int   k0 = std::min (int (lvl), maxLevel);
        const float fr = float (lvl - double (k0));
        const double pk = src / double (int64_t (1) << k0);
        l = readLevel (s, k0, 0, pk, p.loop);
        r = readLevel (s, k0, chR, pk, p.loop);
        if (fr > 0.f && k0 < maxLevel)
        {
            const double pk1 = src / double (int64_t (1) << (k0 + 1));
            const float l1 = readLevel (s, k0 + 1, 0, pk1, p.loop);
            const float r1 = readLevel (s, k0 + 1, chR, pk1, p.loop);
            l += fr * (l1 - l);
            r += fr * (r1 - r);
        }
    };

    for (int i = 0; i < numSamples; ++i)
    {
        // Low-speed smoothing of the record position. At very low speed the
        // decoded position trembles around the true one (its speed changes
        // sign many times a second); played back, every tiny reversal is a
        // click. The time constant shrinks with the (net) speed, so scratches
        // keep their attack; the added lag stays below ~0.5 cycles.
        if (! havePos_)
        {
            posA_ = posB_ = posNet_ = prevPos_ = prevIn_ = vinylPos[i];
            std::fill (paceBuf_.begin(), paceBuf_.end(), vinylPos[i]);
            lastStep_ = 0.0;
            havePos_ = true;
        }
        const double in = vinylPos[i];

        // A jump of the record position: move the smoothing / standstill state
        // along (no speed spike, a stopped record stays muted) and cross-fade
        // from the old read position to the new one.
        {
            const double step = in - prevIn_;
            prevIn_ = in;
            if (std::abs (step) > jumpStep)
            {
                const double d = step - lastStep_; // keep this sample's motion
                posA_ += d; posB_ += d; posNet_ += d;
                calmAnchor_ += d; prevPos_ += d;
                for (auto& v : paceBuf_) v += d;
                if (havePrev_) startFade (-d * toFrames);
            }
            else
            {
                lastStep_ = step;
            }
        }

        const double prevNet = posNet_;
        posNet_ += kNet_ * (in - posNet_);
        speedNet_ += kNetSpeed_ * (std::abs (posNet_ - prevNet) * toSpeed - speedNet_);
        // a = 1 - exp(-1/(tau*sr)) ~= x / (1 + x) with x = 1/(tau*sr): no exp per sample
        const double x   = (1.0 + speedNet_ / kSmoothKneeSpeed) / (kSmoothSeconds * hostSr_);
        const double a   = x / (1.0 + x);
        posA_ += a * (in - posA_);
        posB_ += a * (posA_ - posB_);
        const double pos = posB_;

        const double src = (pos - p.cueCycles) * toFrames; // level-0 frames

        // Standstill detection on the record position. When the record is
        // held or stopped, the decoded position still trembles (vibration,
        // noise); played back, that is crackle. Real motion leaves the calm
        // zone quickly, scratch reversals never stay long enough to mute.
        {
            const double d = std::abs (pos - calmAnchor_);
            const double pace = std::abs (pos - paceBuf_[size_t (paceIdx_)]); // travel over the last 10 ms
            paceBuf_[size_t (paceIdx_)] = pos;
            if (++paceIdx_ >= int (paceBuf_.size())) paceIdx_ = 0;
            if (still_)
            {
                if (d > kBreakawayCycles || (d > kBreakNearCycles && pace > kBreakPaceCycles))
                    { still_ = false; calmAnchor_ = pos; calmSamples_ = 0; }
                else calmAnchor_ += kAnchorFollow_ * (pos - calmAnchor_); // absorb slow creep from knocks
            }
            else if (d > kCalmCycles)
            {
                calmAnchor_ = pos;
                calmSamples_ = 0;
            }
            else if (++calmSamples_ > stillAfter_)
            {
                still_ = true;
            }
            muteGain_ += kMute_ * ((still_ ? 0.f : 1.f) - muteGain_);
        }

        // playback rate in source frames per output sample (for anti-aliasing)
        const double ratio = s.sampleRate / hostSr_;
        double rate = havePrev_ ? std::abs (src - prevSrc_) : ratio;
        rate = std::min (rate, 64.0);
        rateSmooth_ += kRate_ * (rate - rateSmooth_);
        prevSrc_ = src;
        havePrev_ = true;

        float l = 0.f, r = 0.f;
        const double lvl = std::clamp (std::log2 (std::max (rateSmooth_, 1.0)), 0.0, double (maxLevel));
        readAt (src, lvl, l, r);
        if (fadeLeft_ > 0)
        {
            // equal-power cross-fade (the two positions are uncorrelated)
            float lo = 0.f, ro = 0.f;
            readAt (src + oldShift_, lvl, lo, ro);
            const float g  = float (fadeLeft_) / float (fadeLen_); // old: 1 -> 0
            const float wo = float (std::sin (0.5 * kPi * g)), wn = float (std::cos (0.5 * kPi * g));
            l = wn * l + wo * lo;
            r = wn * r + wo * ro;
            --fadeLeft_;
        }

        // DC blocker: a stopped record holds a constant value -> silence
        const double yl = double (l) - dcX_[0] + dcR_ * dcY_[0];
        const double yr = double (r) - dcX_[1] + dcR_ * dcY_[1];
        dcX_[0] = l; dcY_[0] = yl;
        dcX_[1] = r; dcY_[1] = yr;

        // Vinyl-like level at low speed: a cartridge's output falls to zero as
        // the stylus stops, so on a record every reversal passes through
        // silence. Played from a sample, the waveform instead folds back at
        // full level (a sharp corner = a click) and the held value thumps out
        // through the DC blocker. Below kVinylKneeSpeed the level follows the
        // speed. The speed comes from the smoothed position (1 ms).
        {
            const double v = std::abs (pos - prevPos_) * toSpeed;
            prevPos_ = pos;
            gainSpeed_ += kGainSpeed_ * (v - gainSpeed_);
            const double target = kVinylKneeSpeed > 0.0 ? std::min (1.0, gainSpeed_ / kVinylKneeSpeed) : 1.0;
            vinylGain_ += kGainSpeed_ * (target - vinylGain_);
        }

        // While the stylus skates / skips the decoded position follows noise
        // (crackle); turn the level down as the decoder's quality drops.
        if (quality != nullptr)
        {
            const float q = quality[i];
            qualityGain_ += (q < qualityGain_ ? kQualityDown_ : kQualityUp_) * (q - qualityGain_);
        }

        outL[i] = float (yl * vinylGain_) * p.gain * muteGain_ * qualityGain_;
        outR[i] = float (yr * vinylGain_) * p.gain * muteGain_ * qualityGain_;
    }

    lastSrcSeconds_ = prevSrc_ / s.sampleRate;
}

} // namespace vt
