// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

#include "Synth.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <random>

namespace vt::test {

namespace {
constexpr double kTwoPi = 6.283185307179586;

struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double process (double x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    double gainAt (double f, double sr) const
    {
        const std::complex<double> z = std::polar (1.0, -kTwoPi * f / sr);
        const auto num = b0 + b1 * z + b2 * z * z;
        const auto den = 1.0 + a1 * z + a2 * z * z;
        return std::abs (num / den);
    }
};

// RIAA playback (de-emphasis): (1 + sT2) / ((1 + sT1)(1 + sT3)), bilinear.
Biquad makeRiaa (double sr)
{
    const double T1 = 3180e-6, T2 = 318e-6, T3 = 75e-6, K = 2.0 * sr;
    const double b0 = 1 + T2 * K, b1 = 2, b2 = 1 - T2 * K;
    const double a0 = 1 + (T1 + T3) * K + T1 * T3 * K * K;
    const double a1 = 2 - 2 * T1 * T3 * K * K;
    const double a2 = 1 - (T1 + T3) * K + T1 * T3 * K * K;
    Biquad q;
    q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
    const double g = q.gainAt (1000.0, sr);
    q.b0 /= g; q.b1 /= g; q.b2 /= g;
    return q;
}

// Low-frequency disturbance made of a few sinusoids with random frequency in
// [f0, f1] Hz and random phase; peak about 1.
struct SlowMix
{
    struct Part { double f, ph; } p[4];
    SlowMix (uint32_t seed, double f0, double f1)
    {
        std::mt19937 r (seed);
        std::uniform_real_distribution<double> U (0.0, 1.0);
        for (auto& q : p) q = { f0 + (f1 - f0) * U (r), kTwoPi * U (r) };
    }
    double at (double t) const
    {
        double v = 0.0;
        for (const auto& q : p) v += 0.25 * std::sin (kTwoPi * q.f * t + q.ph);
        return v;
    }
};

// Traktor Control Vinyl MK2 (TimecodeDef::Encoding::Offset). Built as the
// groove displacement, theta in carrier cycles:
//   L = sin (2 pi theta) + c (theta),  R = sin (2 pi theta - phi) + c (theta)
// where c is the bit offset: +-mk2Offset for bit n on [cpb n, cpb (n + 1)),
// joined by raised-cosine steps of width mk2Ramp centred on the boundaries
// (the decoder takes the boundary after the newest bit as cpb (n + 1)). The
// cartridge outputs the stylus velocity: d/dtheta of the displacement times
// the record speed (normalised so that the carrier is 1 at 1x), then the
// playback RIAA curve. Reversed playback flips the L / R phase order and the
// polarity by itself.
void synthesizeOffset (const LfsrTable& table, const std::vector<double>& theta,
                       const SynthOptions& opt, std::vector<float>& left, std::vector<float>& right)
{
    const auto& def = table.def();
    const double cpb = def.cyclesPerBit;

    // Bit n = MSB of the LFSR state at index n (as for Serato).
    std::vector<uint8_t> bits (def.length);
    uint32_t s = def.seed;
    for (uint32_t n = 0; n < def.length; ++n)
    {
        bits[n] = uint8_t ((s >> (def.bits - 1)) & 1u);
        s = table.forward (s);
    }
    auto level = [&] (long long n) -> double
    {
        const bool b = (n >= 0 && n < (long long) def.length) ? bits[size_t (n)] != 0
                                                              : (std::hash<long long> {} (n) & 1) != 0;
        return b ? 1.0 : -1.0;
    };
    // Offset slope d c / d theta (the level is flat inside a bit).
    const double ramp = std::max (opt.mk2Ramp, 1e-3);
    auto offsetSlope = [&] (double th) -> double
    {
        const long long k = (long long) std::floor (th / cpb + 0.5); // nearest boundary: between bit k-1 and k
        const double d = th - cpb * double (k);
        if (std::abs (d) >= 0.5 * ramp)
            return 0.0;
        const double step = level (k) - level (k - 1);
        // c = mean + step / 2 * sin (pi d / ramp) over |d| < ramp / 2
        return opt.mk2Offset * 0.5 * step * (kTwoPi * 0.5 / ramp) * std::cos (kTwoPi * 0.5 * d / ramp);
    };

    std::mt19937 rng (opt.seed);
    std::normal_distribution<double> gauss (0.0, 1.0);
    const double noise = opt.level * std::pow (10.0, opt.noiseDb / 20.0);
    const double hum   = opt.level * std::pow (10.0, opt.humDb / 20.0);
    const double skew  = opt.skewDeg * kTwoPi / 360.0;
    const double phi   = opt.mk2PhaseDeg * kTwoPi / 360.0;

    Biquad riL = makeRiaa (opt.sampleRate), riR = riL;
    const SlowMix slowL (opt.seed + 1, 3.0, 25.0), slowR (opt.seed + 2, 3.0, 25.0);
    const SlowMix vert (opt.seed + 3, 20.0, 150.0), comm (opt.seed + 4, 20.0, 150.0);
    // handOffset as for Serato (full effect at 20x per second); the vertical /
    // common components only while the hand changes the speed hard (full
    // effect at 200x per second: the start and end of a stroke)
    double handGain = 0.0, handGainHard = 0.0, handLastV = 0.0;
    const double kHand = 1.0 - std::exp (-1.0 / (0.010 * opt.sampleRate));

    const size_t N = theta.size();
    left.resize (N);
    right.resize (N);

    for (size_t i = 0; i < N; ++i)
    {
        const double th = theta[i];
        const size_t i0 = i > 0 ? i - 1 : 0;
        const size_t i1 = i + 1 < N ? i + 1 : N - 1;
        double v = (theta[i1] - theta[i0]) / double (i1 - i0 == 0 ? 1 : i1 - i0)
                 * opt.sampleRate / def.resolution;
        const bool jump = std::abs (v) > 50.0; // position discontinuity = needle drop

        double gl, gr;
        if (opt.cartridge)
        {
            const double c = offsetSlope (th) / kTwoPi;
            const double vv = jump ? 0.0 : v;
            gl = riL.process ((std::cos (kTwoPi * th) + c) * vv);
            gr = riR.process ((std::cos (kTwoPi * th - phi) + c) * vv);
        }
        else
        {
            // displacement itself (no pickup model)
            const long long n = (long long) std::floor (th / cpb);
            const double c = opt.mk2Offset * level (n);
            gl = std::sin (kTwoPi * th) + c;
            gr = std::sin (kTwoPi * th - phi) + c;
        }

        double l = gl * opt.level;
        double r = gr * opt.level * opt.gainR;
        r = r * std::cos (skew) + l * std::sin (skew);

        const double t = double (i) / opt.sampleRate;
        l += opt.dcL + hum * std::sin (kTwoPi * 50.0 * t) + noise * gauss (rng);
        r += opt.dcR + hum * std::sin (kTwoPi * 50.0 * t + 0.3) + noise * gauss (rng);

        if (opt.handOffset > 0.0 || opt.handVertical > 0.0 || opt.handCommon > 0.0)
        {
            const double vh = jump ? handLastV : v;
            const double acc = std::abs (vh - handLastV) * opt.sampleRate;
            handLastV = vh;
            handGain     += kHand * (std::min (acc / 20.0, 1.0) - handGain);
            handGainHard += kHand * (std::min (acc / 200.0, 1.0) - handGainHard);
            const double g = opt.level * handGain, gh = opt.level * handGainHard;
            const double ve = opt.handVertical * vert.at (t), co = opt.handCommon * comm.at (t);
            l += g * opt.handOffset * 2.0 * slowL.at (t) + gh * (ve + co);
            r += g * opt.handOffset * 2.0 * slowR.at (t) + gh * (co - ve);
        }
        if (opt.vibration > 0.0)
        {
            const double tb = std::fmod (t, 0.25);
            if (tb < 0.03)
            {
                const double w = 0.5 - 0.5 * std::cos (kTwoPi * tb / 0.03);
                l += opt.vibration * opt.level * w * std::sin (kTwoPi * 40.0 * t);
                r -= opt.vibration * opt.level * w * 0.8 * std::sin (kTwoPi * 40.0 * t + 0.2);
            }
        }

        if (opt.invertL) l = -l;
        if (opt.invertR) r = -r;
        if (opt.swapLR) std::swap (l, r);

        left[i]  = float (l);
        right[i] = float (r);
    }
}
} // namespace

void synthesize (const LfsrTable& table, const std::vector<double>& theta,
                 const SynthOptions& opt, std::vector<float>& left, std::vector<float>& right)
{
    const auto& def = table.def();
    if (def.encoding == TimecodeDef::Encoding::Offset)
    {
        synthesizeOffset (table, theta, opt, left, right);
        return;
    }

    // Bits pressed at each cycle index: MSB of the LFSR state at that index.
    std::vector<uint8_t> bits (def.length);
    uint32_t s = def.seed;
    for (uint32_t n = 0; n < def.length; ++n)
    {
        bits[n] = uint8_t ((s >> (def.bits - 1)) & 1u);
        s = table.forward (s);
    }

    std::mt19937 rng (opt.seed);
    std::normal_distribution<double> gauss (0.0, 1.0);
    const double noise = opt.level * std::pow (10.0, opt.noiseDb / 20.0);
    const double hum   = opt.level * std::pow (10.0, opt.humDb / 20.0);
    const double skew  = opt.skewDeg * kTwoPi / 360.0;

    Biquad riL = makeRiaa (opt.sampleRate), riR = riL;

    // Hand offset: a few slow sinusoids per channel with random frequency and
    // phase (separate generator so the other options keep their noise).
    struct Slow { double f, ph; };
    Slow slowL[4], slowR[4];
    {
        std::mt19937 r2 (opt.seed + 1);
        std::uniform_real_distribution<double> U (0.0, 1.0);
        for (int k = 0; k < 4; ++k)
        {
            slowL[k] = { 3.0 + 22.0 * U (r2), kTwoPi * U (r2) };
            slowR[k] = { 3.0 + 22.0 * U (r2), kTwoPi * U (r2) };
        }
    }
    double handGain = 0.0, handLastV = 0.0;                  // follows |acceleration|, 10 ms
    const double kHand = 1.0 - std::exp (-1.0 / (0.010 * opt.sampleRate));

    const size_t N = theta.size();
    left.resize (N);
    right.resize (N);

    for (size_t i = 0; i < N; ++i)
    {
        const double th = theta[i];

        // Amplitude window of a bit: [n - 0.25, n + 0.75) so that the step
        // happens at a zero crossing of the modulated channel.
        const long long n = (long long) std::floor (th + 0.25);
        double amp = 1.0;
        if (n >= 0 && n < (long long) def.length)
            amp = bits[size_t (n)] ? 1.0 : opt.amLow;
        else
            amp = (std::hash<long long> {} (n) & 1) ? 1.0 : opt.amLow;

        double gl = std::sin (kTwoPi * th);          // secondary
        double gr = std::cos (kTwoPi * th) * amp;    // primary, read when gl crosses 0

        if (opt.cartridge)
        {
            // stylus velocity factor (speed relative to nominal)
            const size_t i0 = i > 0 ? i - 1 : 0;
            const size_t i1 = i + 1 < N ? i + 1 : N - 1;
            double v = (theta[i1] - theta[i0]) / double (i1 - i0 == 0 ? 1 : i1 - i0)
                     * opt.sampleRate / def.resolution;
            if (std::abs (v) > 50.0) v = 0.0; // position discontinuity = needle drop
            gl = riL.process (gl * v);
            gr = riR.process (gr * v);
        }

        double l = gl * opt.level;
        double r = gr * opt.level * opt.gainR;
        r = r * std::cos (skew) + l * std::sin (skew);

        const double t = double (i) / opt.sampleRate;
        l += opt.dcL + hum * std::sin (kTwoPi * 50.0 * t) + noise * gauss (rng);
        r += opt.dcR + hum * std::sin (kTwoPi * 50.0 * t + 0.3) + noise * gauss (rng);

        if (opt.handOffset > 0.0)
        {
            const size_t i0 = i > 0 ? i - 1 : 0;
            const size_t i1 = i + 1 < N ? i + 1 : N - 1;
            double v = (theta[i1] - theta[i0]) / double (i1 - i0 == 0 ? 1 : i1 - i0)
                     * opt.sampleRate / def.resolution;
            if (std::abs (v) > 50.0) v = handLastV;
            // the push of the hand ~ acceleration (full effect at 20x per second)
            const double acc = std::abs (v - handLastV) * opt.sampleRate;
            handLastV = v;
            handGain += kHand * (std::min (acc / 20.0, 1.0) - handGain);
            double ol = 0.0, orr = 0.0;
            for (int k = 0; k < 4; ++k)
            {
                ol  += 0.5 * std::sin (kTwoPi * slowL[k].f * t + slowL[k].ph);
                orr += 0.5 * std::sin (kTwoPi * slowR[k].f * t + slowR[k].ph);
            }
            l += opt.handOffset * opt.level * handGain * ol;
            r += opt.handOffset * opt.level * handGain * orr;
        }
        if (opt.vibration > 0.0)
        {
            const double tb = std::fmod (t, 0.25);
            if (tb < 0.03)
            {
                const double w = 0.5 - 0.5 * std::cos (kTwoPi * tb / 0.03);
                // mostly vertical stylus motion: L and R in anti-phase
                l += opt.vibration * opt.level * w * std::sin (kTwoPi * 40.0 * t);
                r -= opt.vibration * opt.level * w * 0.8 * std::sin (kTwoPi * 40.0 * t + 0.2);
            }
        }

        if (opt.invertR) r = -r;
        if (opt.swapLR) std::swap (l, r);

        left[i]  = float (l);
        right[i] = float (r);
    }
}

std::vector<double> integrateSpeed (const std::vector<double>& speed, double startCycles,
                                    double sampleRate, double resolution)
{
    std::vector<double> th (speed.size());
    double p = startCycles;
    const double k = resolution / sampleRate;
    for (size_t i = 0; i < speed.size(); ++i)
    {
        th[i] = p;
        p += speed[i] * k;
    }
    return th;
}

// Random scratching: piecewise targets with fast but finite acceleration.
std::vector<double> scratchProfile (double seconds, double sr, uint32_t seed, double maxSpeed)
{
    std::mt19937 rng (seed);
    std::uniform_real_distribution<double> U (0.0, 1.0);
    const size_t n = size_t (seconds * sr);
    std::vector<double> v (n);
    double target = 0, cur = 0, pos = 0, segLeft = 0;
    const double tau = 0.006; // 6 ms velocity lag -> very high accelerations
    const double k   = 1.0 - std::exp (-1.0 / (tau * sr));
    for (size_t i = 0; i < n; ++i)
    {
        if (i < size_t (sr)) { v[i] = 1.0; pos += 1000.0 / sr; continue; } // 1 s normal play to lock
        if (segLeft <= 0)
        {
            segLeft = 0.03 + 0.17 * U (rng);
            const double u = U (rng);
            if (u < 0.15)       target = 0.0;                                   // hold
            else if (u < 0.30)  target = (U (rng) < 0.5 ? -1 : 1) * (0.005 + 0.1 * U (rng)); // slow
            else                target = (U (rng) < 0.5 ? -1 : 1) * (0.3 + (maxSpeed - 0.3) * U (rng));
            // stay within +-1.5 s of the cue point
            if (pos > 1500 && target > 0) target = -target;
            if (pos < -1500 && target < 0) target = -target;
        }
        segLeft -= 1.0 / sr;
        cur += k * (target - cur);
        v[i] = cur;
        pos += cur * 1000.0 / sr;
    }
    return v;
}

} // namespace vt::test
