// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// test_player.cpp - end-to-end check: synthetic timecode -> decoder -> player.
// Compares the audio rendered from decoded positions with the audio rendered
// from the true record positions (same player), and measures CPU time.
#include "DropoutMetrics.h"
#include "ScratchPlayer.h"
#include "Synth.h"
#include "TimecodeDecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

using namespace vt;
using namespace vt::test;

// --------------------------------------------------------------------------
// Traktor MK2: drop-outs of the rendered output while the record moves (see
// DropoutMetrics.h). Positions go to the player in ms (1000 Hz cycles), as
// the plugin does.
static void mk2Dropouts (const char* name, const LfsrTable& table, const std::vector<double>& theta,
                         const SynthOptions& opt, const std::vector<char>& still, const SampleData& tone)
{
    const double sr = opt.sampleRate, res = table.def().resolution, toMs = 1000.0 / res;
    std::vector<float> L, R;
    synthesize (table, theta, opt, L, R);
    TimecodeDecoder dec; dec.prepare (sr); dec.setTable (&table); dec.setGateDb (-36.f);
    ScratchPlayer play, playNq;
    play.prepare (sr); playNq.prepare (sr);
    ScratchPlayer::Params p;
    p.cueCycles = theta.front() * toMs;
    p.loop = true;

    const size_t N = theta.size();
    std::vector<double> pos (N);
    std::vector<float> q (N), oL (N), oR (N), nL (N), nR (N);
    for (size_t i = 0; i < N; i += 128)
    {
        const int n = int (std::min<size_t> (128, N - i));
        dec.process (L.data() + i, R.data() + i, n, pos.data() + i, nullptr, nullptr, q.data() + i);
        for (int k = 0; k < n; ++k) pos[i + size_t (k)] *= toMs;
        play.render (&tone, pos.data() + i, n, p, oL.data() + i, oR.data() + i, q.data() + i);
        playNq.render (&tone, pos.data() + i, n, p, nL.data() + i, nR.data() + i, nullptr);
    }
    const size_t w = size_t (std::lround (0.001 * sr)), nw = N / w;
    std::vector<double> ts (nw), ds (nw);
    for (size_t k = 0; k < nw; ++k)
    {
        const size_t a = k * w, b = std::min (N - 1, (k + 1) * w);
        ts[k] = std::abs (theta[b] - theta[a]) * toMs / (1000.0 * double (b - a) / sr);
        ds[k] = std::abs (pos[b] - pos[a]) / (1000.0 * double (b - a) / sr);
    }
    const std::vector<char> valid (nw, 1);
    printDropouts (name, measureDropouts (oL, oR, nL, nR, sr, ts, ds, valid, still));
}

static void runMk2()
{
    const LfsrTable table (kTraktorMk2SideB);
    const SampleData tone = makeQuadratureTone();
    SynthOptions opt;
    opt.level = 0.22; opt.gainR = 0.95; opt.skewDeg = 0.0; // as in test_decoder --mk2
    const double sr = opt.sampleRate, res = table.def().resolution, start = 2.5 * 400000.0;
    std::printf ("\nTraktor MK2: drop-outs of a looped quadrature tone (1 ms windows above 0.5x, < -20 dBFS)\n");

    const auto th = integrateSpeed (scratchProfile (30, sr, 13), start, sr, res);
    mk2Dropouts ("scratch 30 s", table, th, opt, {}, tone);
    SynthOptions o = opt; o.handVertical = 3.0; o.handCommon = 1.0;
    mk2Dropouts ("scratch 30 s, hand (tear)", table, th, o, {}, tone);
    o.handVertical = 6.0; o.handCommon = 2.0;
    mk2Dropouts ("scratch 30 s, hand (tear-hard)", table, th, o, {}, tone);

    // stopped record with knocks: 1 s play, stopped by hand in 0.2 s, 10 s still
    std::vector<double> v (size_t (1.0 * sr), 1.0);
    for (size_t i = 0; i < size_t (0.2 * sr); ++i) v.push_back (1.0 - double (i) / (0.2 * sr));
    v.resize (size_t (11.2 * sr), 0.0);
    std::vector<char> still (v.size() / size_t (std::lround (0.001 * sr)), 0);
    std::fill (still.begin() + 1500, still.end(), char (1)); // from 1.5 s
    o = opt; o.vibration = 0.0316;
    mk2Dropouts ("standstill 10 s, vibration -30 dB", table, integrateSpeed (v, start, sr, res), o, still, tone);
}

int main()
{
    const double sr = 48000.0;
    const LfsrTable table (kSeratoSideA);

    // Test sample: 3 s, harmonic tone with an envelope (like a vocal stab)
    const int64_t len = int64_t (3 * 44100);
    std::vector<float> a (static_cast<size_t> (len)), b (static_cast<size_t> (len));
    for (int64_t i = 0; i < len; ++i)
    {
        const double t = double (i) / 44100.0;
        double v = 0;
        for (int h = 1; h <= 8; ++h) v += std::sin (2 * 3.14159265 * 220.0 * h * t) / h;
        v *= 0.3 * std::exp (-1.5 * std::fmod (t, 0.75));
        a[size_t (i)] = float (v);
        b[size_t (i)] = float (v * 0.9);
    }
    const float* chans[2] = { a.data(), b.data() };
    SampleData sample;
    sample.build (chans, 2, len, 44100.0);

    // Motion: 1 s play, then back-and-forth scratches around the cue
    std::vector<double> speed;
    for (int i = 0; i < int (sr); ++i) speed.push_back (1.0);
    std::mt19937 rng (3);
    std::uniform_real_distribution<double> U (0.0, 1.0);
    double cur = 0, target = 0, left = 0;
    const double k = 1.0 - std::exp (-1.0 / (0.008 * sr));
    for (int i = 0; i < int (20 * sr); ++i)
    {
        if (left <= 0) { left = 0.05 + 0.15 * U (rng); target = (U (rng) - 0.5) * 6.0; }
        left -= 1.0 / sr;
        cur += k * (target - cur);
        speed.push_back (cur);
    }
    auto theta = integrateSpeed (speed, 10000, sr, 1000);

    std::vector<float> L, R;
    SynthOptions opt;
    synthesize (table, theta, opt, L, R);

    TimecodeDecoder dec; dec.prepare (sr); dec.setTable (&table); dec.setGateDb (-36.f);
    ScratchPlayer playDec, playRef;
    playDec.prepare (sr); playRef.prepare (sr);

    ScratchPlayer::Params p;
    p.cueCycles = 10000.0 + 300.0; // sample starts 0.3 s after the motion start

    const size_t N = theta.size();
    std::vector<double> pos (N);
    std::vector<float> oL (N), oR (N), rL (N), rR (N);

    double tDec = 0, tPlay = 0;
    for (size_t i = 0; i < N; i += 128)
    {
        const int n = int (std::min<size_t> (128, N - i));
        auto t0 = std::chrono::steady_clock::now();
        dec.process (L.data() + i, R.data() + i, n, pos.data() + i, nullptr, nullptr);
        auto t1 = std::chrono::steady_clock::now();
        // align: compensate the decoder's constant offset measured on the fly is
        // not needed for the comparison, the reference uses the true theta.
        playDec.render (&sample, pos.data() + i, n, p, oL.data() + i, oR.data() + i);
        auto t2 = std::chrono::steady_clock::now();
        playRef.render (&sample, theta.data() + i, n, p, rL.data() + i, rR.data() + i);
        tDec  += std::chrono::duration<double> (t1 - t0).count();
        tPlay += std::chrono::duration<double> (t2 - t1).count();
    }

    // The decoder has a constant sub-millisecond offset (pickup/RIAA phase at
    // 1x). It is equivalent to a fixed latency, so compare against the ideal
    // render shifted by the same offset.
    {
        std::vector<double> d;
        for (size_t i = size_t (1.5 * sr); i < N; i += 16) d.push_back (pos[i] - theta[i]);
        std::nth_element (d.begin(), d.begin() + long (d.size() / 2), d.end());
        const double off = d[d.size() / 2];
        std::vector<double> th2 (theta);
        for (auto& v : th2) v += off;
        playRef.reset();
        for (size_t i = 0; i < N; i += 128)
        {
            const int n = int (std::min<size_t> (128, N - i));
            playRef.render (&sample, th2.data() + i, n, p, rL.data() + i, rR.data() + i);
        }
        std::printf ("constant decoder offset: %+.3f ms of record time\n", off);
    }

    // Position jitter: error high-passed at 100 Hz (slow offset changes are
    // inaudible time shifts, fast ones would be heard as roughness/FM).
    {
        const double kh = std::exp (-2.0 * 3.14159265 * 100.0 / sr);
        double x1 = 0, y = 0, acc = 0; size_t cnt = 0;
        for (size_t i = 0; i < N; ++i)
        {
            const double e = pos[i] - theta[i];
            y = kh * (y + e - x1); x1 = e;
            if (i > size_t (1.5 * sr)) { acc += y * y; ++cnt; }
        }
        std::printf ("position jitter above 100 Hz: %.2f us RMS\n", 1000.0 * std::sqrt (acc / double (cnt)));
    }
    const double audioSec = double (N) / sr;
    std::printf ("CPU: decoder %.2f %% + player %.2f %% of one core (48 kHz, one deck)\n",
                 100.0 * tDec / audioSec, 100.0 * tPlay / audioSec);

    // Write a listening file: left = decoded playback, right = ideal playback
    FILE* f = std::fopen ("scratch_demo.wav", "wb");
    if (f)
    {
        auto w32 = [&] (uint32_t v) { std::fwrite (&v, 4, 1, f); };
        auto w16 = [&] (uint16_t v) { std::fwrite (&v, 2, 1, f); };
        const uint32_t frames = uint32_t (N), bytes = frames * 2 * 2;
        std::fwrite ("RIFF", 1, 4, f); w32 (36 + bytes); std::fwrite ("WAVEfmt ", 1, 8, f);
        w32 (16); w16 (1); w16 (2); w32 (48000); w32 (48000 * 4); w16 (4); w16 (16);
        std::fwrite ("data", 1, 4, f); w32 (bytes);
        for (size_t i = 0; i < N; ++i)
        {
            int16_t s0 = int16_t (std::clamp (oL[i], -1.f, 1.f) * 32767.f);
            int16_t s1 = int16_t (std::clamp (rL[i], -1.f, 1.f) * 32767.f);
            std::fwrite (&s0, 2, 1, f); std::fwrite (&s1, 2, 1, f);
        }
        std::fclose (f);
    }
    runMk2();
    return 0;
}
