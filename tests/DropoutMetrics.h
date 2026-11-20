// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// DropoutMetrics.h - drop-outs of the rendered output while the record moves.
// Shared by test_player (synthetic timecode) and tc_analyze --eval
// (recordings) so that both count the same thing:
//  * The source is a looped constant-amplitude stereo quadrature tone
//    (L = cos, R = sin, amplitude 1), whose L / R RMS is constant (-3 dBFS)
//    at any playback speed: a quiet output can only come from the playback.
//  * The output is cut into 1 ms windows. A window counts when the true
//    record speed is above 0.5x (and the reference is valid there); it is a
//    drop-out window when the L / R RMS sqrt ((sum L^2 + sum R^2) / 2N) is
//    below -20 dBFS.
//  * Consecutive drop-out windows make one drop-out (at least 1 ms; one
//    window that is not a drop-out separates two).
//  * Cause of each drop-out, by the majority of its windows:
//      quality - the same positions rendered without the quality input are
//                at -20 dBFS or above (the signal-quality mute);
//      speed   - otherwise, the decoded speed is below half the true speed;
//      other   - neither.
//  * Standstill: in windows known to be stopped, the time the output is at
//    -40 dBFS or above.
#pragma once

#include "ScratchPlayer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vt::test {

// The test source: 1 s at 44.1 kHz, 440 whole cycles (loops without a step).
inline SampleData makeQuadratureTone()
{
    const int64_t len = 44100;
    std::vector<float> a (static_cast<size_t> (len)), b (static_cast<size_t> (len));
    for (int64_t i = 0; i < len; ++i)
    {
        const double ph = 6.283185307179586 * 440.0 * double (i) / 44100.0;
        a[size_t (i)] = float (std::cos (ph));
        b[size_t (i)] = float (std::sin (ph));
    }
    const float* ch[2] = { a.data(), b.data() };
    SampleData s;
    s.build (ch, 2, len, 44100.0);
    return s;
}

struct DropoutResult
{
    double evaluatedMs  = 0;  // windows above 0.5x with a valid reference
    int    count        = 0;
    double totalMs      = 0;
    double medianMs     = 0, p90Ms = 0, maxMs = 0;
    int    byQuality    = 0, bySpeed = 0, byOther = 0;
    double totalNoQualityMs = 0; // drop-out time of the render without quality
    double stillMs      = 0;  // standstill windows evaluated
    double stillLoudMs  = 0;  // ... with the output at -40 dBFS or above
};

// Per 1 ms window (index k covers samples [k w, (k + 1) w), w = round (sampleRate / 1000)):
//   trueSpeed  |true record speed| (x), decSpeed |decoded speed| (x),
//   valid      reference usable, still  known standstill.
// out*, outNq*: renders with and without the quality input.
inline DropoutResult measureDropouts (const std::vector<float>& outL, const std::vector<float>& outR,
                                      const std::vector<float>& nqL, const std::vector<float>& nqR,
                                      double sampleRate,
                                      const std::vector<double>& trueSpeed, const std::vector<double>& decSpeed,
                                      const std::vector<char>& valid, const std::vector<char>& still)
{
    const double kDrop = 0.1, kLoud = 0.01; // -20 / -40 dBFS
    const size_t window = size_t (std::lround (0.001 * sampleRate));
    const double wMs = 1000.0 * double (window) / sampleRate;
    const size_t nw = std::min (outL.size() / window, trueSpeed.size());
    auto rms = [&] (const std::vector<float>& l, const std::vector<float>& r, size_t k)
    {
        double s = 0.0;
        for (size_t i = k * window; i < (k + 1) * window; ++i)
            s += double (l[i]) * l[i] + double (r[i]) * r[i];
        return std::sqrt (s / double (2 * window));
    };

    DropoutResult res;
    std::vector<double> lengths;
    int run = 0, runQ = 0, runS = 0;
    auto close = [&]
    {
        if (run == 0) return;
        lengths.push_back (double (run) * wMs);
        ++res.count;
        if (2 * runQ > run)      ++res.byQuality;
        else if (2 * runS > run) ++res.bySpeed;
        else                     ++res.byOther;
        run = runQ = runS = 0;
    };
    for (size_t k = 0; k < nw; ++k)
    {
        if (k < still.size() && still[k])
        {
            res.stillMs += wMs;
            if (rms (outL, outR, k) >= kLoud) res.stillLoudMs += wMs;
        }
        const bool eligible = (k >= valid.size() || valid[k]) && trueSpeed[k] > 0.5;
        if (! eligible) { close(); continue; }
        res.evaluatedMs += wMs;
        const bool drop = rms (outL, outR, k) < kDrop;
        const bool dropNq = rms (nqL, nqR, k) < kDrop;
        if (dropNq) res.totalNoQualityMs += wMs;
        if (! drop) { close(); continue; }
        ++run;
        res.totalMs += wMs;
        if (! dropNq) ++runQ;
        else if (k < decSpeed.size() && decSpeed[k] < 0.5 * trueSpeed[k]) ++runS;
    }
    close();
    if (! lengths.empty())
    {
        std::sort (lengths.begin(), lengths.end());
        res.medianMs = lengths[lengths.size() / 2];
        res.p90Ms    = lengths[std::min (lengths.size() - 1, lengths.size() * 9 / 10)];
        res.maxMs    = lengths.back();
    }
    return res;
}

inline void printDropouts (const char* name, const DropoutResult& r)
{
    std::printf ("%-34s drop-outs above 0.5x: %d in %.0f ms evaluated, total %.0f ms (no quality: %.0f ms), "
                 "length median %.0f p90 %.0f max %.0f ms | cause quality %d speed %d other %d",
                 name, r.count, r.evaluatedMs, r.totalMs, r.totalNoQualityMs, r.medianMs, r.p90Ms, r.maxMs,
                 r.byQuality, r.bySpeed, r.byOther);
    if (r.stillMs > 0)
        std::printf (" | standstill: %.0f of %.0f ms at -40 dBFS or above", r.stillLoudMs, r.stillMs);
    std::printf ("\n");
}

} // namespace vt::test
