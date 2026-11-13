// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// test_decoder.cpp - accuracy tests for TimecodeDecoder on synthetic timecode.
//
// Each scenario generates a known record motion, renders it through the
// cartridge/RIAA model, decodes it and compares the decoded position with the
// true one. Errors are reported in milliseconds of record time
// (1 carrier cycle = 1 ms at 33 1/3 rpm).

#include "Synth.h"
#include "TimecodeDecoder.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace vt;
using namespace vt::test;

namespace {

struct Result
{
    double lockMs    = -1;   // time until the absolute position is known
    double offsetMs  = 0;    // median error (constant latency-like offset)
    double p99Ms     = 0;    // 99th percentile |error - median|
    double maxMs     = 0;    // max |error - median|
    double driftMs   = 0;    // median error over the last 5 s minus first 5 s
    double stuckPct  = 0;    // % of 10 ms windows where the record moved but output did not follow
    double clickMs   = 0;    // max |second difference of the error| per sample (a step in the output = click)
    double nsPerSample = 0;
    int    reanchors = 0;
    int    config    = -1;
};

double median (std::vector<double> v)
{
    if (v.empty()) return 0;
    std::nth_element (v.begin(), v.begin() + long (v.size() / 2), v.end());
    return v[v.size() / 2];
}

double percentileAbs (std::vector<double> v, double p)
{
    if (v.empty()) return 0;
    for (auto& x : v) x = std::abs (x);
    const size_t k = std::min (v.size() - 1, size_t (p * double (v.size())));
    std::nth_element (v.begin(), v.begin() + long (k), v.end());
    return v[k];
}

Result run (const LfsrTable& table, const std::vector<double>& theta, const SynthOptions& opt,
            int blockSize = 128, const std::vector<char>* evalMask = nullptr, bool verbose = false)
{
    std::vector<float> L, R;
    synthesize (table, theta, opt, L, R);

    TimecodeDecoder dec;
    dec.prepare (opt.sampleRate);
    dec.setTable (&table);
    dec.setGateDb (-36.f);

    const size_t N = theta.size();
    std::vector<double> pos (N), rel (N);
    std::vector<float> spd (N);
    std::vector<char> anchored (N, 0);

    const auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < N; i += size_t (blockSize))
    {
        const int n = int (std::min (size_t (blockSize), N - i));
        dec.process (L.data() + i, R.data() + i, n, pos.data() + i, rel.data() + i, spd.data() + i);
        const char a = dec.status().anchored ? 1 : 0;
        std::fill (anchored.begin() + long (i), anchored.begin() + long (i) + n, a);
    }
    const auto t1 = std::chrono::steady_clock::now();

    Result r;
    r.nsPerSample = std::chrono::duration<double, std::nano> (t1 - t0).count() / double (N);
    r.reanchors   = dec.status().reanchors;
    r.config      = dec.status().config;

    size_t first = N;
    for (size_t i = 0; i < N; ++i) if (anchored[i]) { first = i; break; }
    if (first == N) return r;
    r.lockMs = 1000.0 * double (first) / opt.sampleRate;

    std::vector<double> err;
    std::vector<size_t> idx;
    // skip 50 ms after the first lock so that the start-up transient is excluded
    const size_t start = first + size_t (0.05 * opt.sampleRate);
    for (size_t i = start; i < N; ++i)
    {
        if (evalMask != nullptr && ! (*evalMask)[i]) continue;
        err.push_back (pos[i] - theta[i]); // cycles == ms at 33 rpm
        idx.push_back (i);
    }
    if (err.empty()) return r;

    r.offsetMs = median (err);
    for (size_t k = 2; k < err.size(); ++k)
        if (idx[k] == idx[k - 2] + 2)
            r.clickMs = std::max (r.clickMs, std::abs (err[k] - 2.0 * err[k - 1] + err[k - 2]));
    std::vector<double> dev (err.size());
    for (size_t k = 0; k < err.size(); ++k) dev[k] = err[k] - r.offsetMs;
    r.p99Ms = percentileAbs (dev, 0.99);
    r.maxMs = percentileAbs (dev, 1.0);

    const size_t fiveSec = size_t (5.0 * opt.sampleRate);
    if (N - start > 3 * fiveSec)
    {
        std::vector<double> a, b;
        for (size_t k = 0; k < err.size(); ++k)
        {
            if (idx[k] < start + fiveSec) a.push_back (err[k]);
            if (idx[k] >= N - fiveSec)    b.push_back (err[k]);
        }
        r.driftMs = median (b) - median (a);
    }

    // "Stuck" windows: record moved >= 0.1 cycle in 10 ms but output < 30 % of it
    const size_t w = size_t (0.01 * opt.sampleRate);
    int moving = 0, stuck = 0;
    for (size_t i = start; i + w < N; i += w)
    {
        if (evalMask != nullptr && ! (*evalMask)[i]) continue;
        const double dt = theta[i + w] - theta[i];
        const double dp = pos[i + w] - pos[i];
        if (std::abs (dt) >= 0.1)
        {
            ++moving;
            if (dp * dt < 0.3 * dt * dt) ++stuck;
        }
    }
    r.stuckPct = moving > 0 ? 100.0 * stuck / moving : 0.0;

    if (verbose)
    {
        FILE* f = std::fopen ("trace.csv", "w");
        if (f)
        {
            std::fprintf (f, "t,true,decoded,speed\n");
            for (size_t i = 0; i < N; i += 8)
                std::fprintf (f, "%.6f,%.4f,%.4f,%.4f\n", double (i) / opt.sampleRate, theta[i], pos[i], spd[i]);
            std::fclose (f);
        }
    }
    return r;
}

void print (const char* name, const Result& r)
{
    std::printf ("%-34s lock %7.1f ms | offset %+6.3f ms | p99 %6.3f ms | max %6.3f ms | drift %+7.3f ms | stuck %5.2f %% | reanchor %d | cfg %d | %5.1f ns/smp | click %.4f\n",
                 name, r.lockMs, r.offsetMs, r.p99Ms, r.maxMs, r.driftMs, r.stuckPct, r.reanchors, r.config, r.nsPerSample, r.clickMs);
}

// --------------------------------------------------------------------------
// Motion profiles

std::vector<double> constantSpeed (double seconds, double speed, double sr)
{
    return std::vector<double> (size_t (seconds * sr), speed);
}

// Slow movements: sinusoidal speed with small peaks plus ultra slow drags.
std::vector<double> slowProfile (double sr)
{
    std::vector<double> v;
    auto add = [&] (double seconds, std::function<double (double)> f)
    {
        const size_t n = size_t (seconds * sr);
        for (size_t i = 0; i < n; ++i) v.push_back (f (double (i) / sr));
    };
    add (1.0, [] (double) { return 1.0; });                                   // lock first
    add (8.0, [] (double t) { return 0.20 * std::sin (6.2831853 * 0.25 * t); });
    add (8.0, [] (double t) { return 0.05 * std::sin (6.2831853 * 0.25 * t); });
    add (6.0, [] (double t) { return t < 3 ? 0.02 : -0.02; });
    add (6.0, [] (double t) { return t < 3 ? 0.01 : -0.01; });
    return v;
}

// Random scratching: piecewise targets with fast but finite acceleration.
std::vector<double> scratchProfile (double seconds, double sr, uint32_t seed, double maxSpeed = 4.0)
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

} // namespace

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp (argv[1], "--quick") == 0;
    const LfsrTable tableA (kSeratoSideA);
    const LfsrTable tableB (kSeratoSideB);

    // LFSR sanity: reverse(forward(x)) == x and lookup consistency
    {
        uint32_t s = kSeratoSideA.seed;
        int bad = 0;
        for (int n = 0; n < 100000; ++n)
        {
            if (tableA.reverse (tableA.forward (s)) != s) ++bad;
            if (tableA.lookup (s) != n) ++bad;
            s = tableA.forward (s);
        }
        std::printf ("LFSR self-test: %s\n", bad == 0 ? "OK" : "FAILED");
        if (bad) return 1;
    }

    SynthOptions opt;
    const double sr = opt.sampleRate;
    std::printf ("\nScenario (48 kHz, block 128, cartridge+RIAA, noise -60 dB, R gain 0.85, skew 8 deg)\n");

    // 1. Normal play
    print ("1x play 20 s", run (tableA, integrateSpeed (constantSpeed (20, 1.0, sr), 5000, sr, 1000), opt));

    // 2. Slow movements
    print ("slow (0.2x..0.01x)", run (tableA, integrateSpeed (slowProfile (sr), 5000, sr, 1000), opt));

    // 3. Scratching, 5 minutes (drift test)
    {
        const double secs = quick ? 60.0 : 300.0;
        auto th = integrateSpeed (scratchProfile (secs, sr, 7), 20000, sr, 1000);
        char name[64]; std::snprintf (name, sizeof name, "scratch %.0f s (<=4x)", secs);
        print (name, run (tableA, th, opt, 128, nullptr, true));
        auto th2 = integrateSpeed (scratchProfile (secs, sr, 11, 8.0), 20000, sr, 1000);
        std::snprintf (name, sizeof name, "scratch %.0f s (<=8x flicks)", secs);
        print (name, run (tableA, th2, opt));
    }

    // 4. Needle drop: play, lift (silence), drop elsewhere
    {
        std::vector<double> th;
        double p = 5000;
        for (size_t i = 0; i < size_t (3 * sr); ++i) { th.push_back (p); p += 1000.0 / sr; }
        for (size_t i = 0; i < size_t (0.3 * sr); ++i) th.push_back (p);           // lifted (no motion)
        p = 300000;
        const size_t dropAt = th.size();
        for (size_t i = 0; i < size_t (3 * sr); ++i) { th.push_back (p); p += 1000.0 / sr; }
        std::vector<char> mask (th.size(), 1);
        for (size_t i = 0; i < dropAt + size_t (0.1 * sr); ++i) mask[i] = (i < size_t (3 * sr)) ? 1 : 0;
        // measure re-lock time separately
        std::vector<float> L, R; synthesize (tableA, th, opt, L, R);
        TimecodeDecoder dec; dec.prepare (sr); dec.setTable (&tableA); dec.setGateDb (-36.f);
        std::vector<double> pos (th.size());
        double relockMs = -1;
        for (size_t i = 0; i < th.size(); i += 128)
        {
            const int n = int (std::min<size_t> (128, th.size() - i));
            dec.process (L.data() + i, R.data() + i, n, pos.data() + i, nullptr, nullptr);
            if (i >= dropAt && relockMs < 0 && std::abs (pos[i + size_t (n) - 1] - th[i + size_t (n) - 1]) < 1.0)
                relockMs = 1000.0 * double (i + size_t (n) - dropAt) / sr;
        }
        Result r = run (tableA, th, opt, 128, &mask);
        print ("needle drop (excl. 100 ms after)", r);
        std::printf ("%-34s re-lock after drop: %.1f ms\n", "", relockMs);
    }

    // 5. Auto detection of channel order / polarity / direction
    {
        auto th = integrateSpeed (constantSpeed (5, 1.0, sr), 5000, sr, 1000);
        for (int c = 0; c < 4; ++c)
        {
            SynthOptions o = opt;
            o.swapLR  = (c & 1) != 0;
            o.invertR = (c & 2) != 0;
            char name[64]; std::snprintf (name, sizeof name, "autodetect swap=%d invert=%d", int (o.swapLR), int (o.invertR));
            print (name, run (tableA, th, o));
        }
    }

    // 6. Side B, other sample rates, larger blocks, noisy input
    print ("side B 1x play", run (tableB, integrateSpeed (constantSpeed (5, 1.0, sr), 400000, sr, 1000), opt));
    for (double rate : { 44100.0, 96000.0 })
    {
        SynthOptions o = opt; o.sampleRate = rate;
        char name[64]; std::snprintf (name, sizeof name, "scratch 30 s @ %.1f kHz", rate / 1000.0);
        print (name, run (tableA, integrateSpeed (scratchProfile (30, rate, 3), 20000, rate, 1000), o));
    }
    {
        SynthOptions o = opt; o.noiseDb = -40.0; o.humDb = -40.0;
        print ("scratch 30 s, noise/hum -40 dB", run (tableA, integrateSpeed (scratchProfile (30, sr, 5), 20000, sr, 1000), o));
    }
    print ("scratch 30 s, block 1024", run (tableA, integrateSpeed (scratchProfile (30, sr, 9), 20000, sr, 1000), opt, 1024));

    // 8. Back-cueing in Relative mode: play 0.5 s at 1x, pull back to the cue at
    //    about -2x, 40 times. Slips must not pile up in the relative position.
    {
        std::vector<double> v (size_t (sr), 1.0); // 1 s to lock
        const double k = 1.0 - std::exp (-1.0 / (0.006 * sr));
        double cur = 1.0, pos = 0.0;
        for (int rep = 0; rep < 40; ++rep)
        {
            for (size_t i = 0; i < size_t (0.5 * sr); ++i) { cur += k * (1.0 - cur); v.push_back (cur); pos += cur / sr; }
            while (pos > 0.0) { cur += k * (-2.0 - cur); v.push_back (cur); pos += cur / sr; }
        }
        for (size_t i = 0; i < size_t (0.2 * sr); ++i) { cur += k * (0.0 - cur); v.push_back (cur); }
        auto th = integrateSpeed (v, 20000, sr, 1000);
        std::vector<float> L, R; synthesize (tableA, th, opt, L, R);
        TimecodeDecoder dec; dec.prepare (sr); dec.setTable (&tableA); dec.setGateDb (-36.f);
        std::vector<double> rel (th.size());
        for (size_t i = 0; i < th.size(); i += 128)
        {
            const int n = int (std::min<size_t> (128, th.size() - i));
            dec.process (L.data() + i, R.data() + i, n, nullptr, rel.data() + i, nullptr);
        }
        // relative error referred to the moment the first stroke starts
        const size_t i0 = size_t (sr);
        const double ref = rel[i0] - th[i0];
        double maxErr = 0.0;
        for (size_t i = i0; i < th.size(); ++i) maxErr = std::max (maxErr, std::abs (rel[i] - th[i] - ref));
        const double endErr = rel.back() - th.back() - ref;
        std::printf ("%-34s relative position: end %+7.3f ms | max %6.3f ms | reanchor %d\n", "back-cue 40x (1x / -2x)",
                     endErr, maxErr, dec.status().reanchors);
    }

    // 7. Disturbances seen on real recordings (Issue #1)
    {
        // Pushing / dragging the record moves the stylus at low frequencies,
        // the L/R circle leaves the origin and reversals used to click.
        SynthOptions o = opt; o.handOffset = 0.25;
        print ("scratch 30 s, hand offset -12 dB", run (tableA, integrateSpeed (scratchProfile (30, sr, 13), 20000, sr, 1000), o));
        print ("slow, hand offset -12 dB", run (tableA, integrateSpeed (slowProfile (sr), 5000, sr, 1000), o));
    }
    {
        // Stopped record with knocks: p99 / max are the wander of the output.
        std::vector<double> v (size_t (1.0 * sr), 1.0);
        for (size_t i = 0; i < size_t (0.2 * sr); ++i) v.push_back (1.0 - double (i) / (0.2 * sr)); // hand stops it
        v.resize (size_t (11.2 * sr), 0.0);
        SynthOptions o = opt; o.vibration = 0.0316; // -30 dB
        std::vector<char> mask (v.size(), 0);               // evaluate the stopped part only
        std::fill (mask.begin() + long (1.5 * sr), mask.end(), char (1));
        print ("standstill 10 s, vibration -30 dB", run (tableA, integrateSpeed (v, 5000, sr, 1000), o, 128, &mask));
    }
    return 0;
}
