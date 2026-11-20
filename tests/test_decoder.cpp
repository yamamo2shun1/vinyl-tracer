// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// test_decoder.cpp - accuracy tests for TimecodeDecoder on synthetic timecode.
// --mk2 runs the Traktor Control Vinyl MK2 part only (it has pass / fail
// checks and exits with 1 on a failure); --quick shortens the long scratches.
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
    // Traktor MK2 pass / fail checks (after the lock, 50 ms start-up excluded)
    double signOk    = 0;    // fraction of moving samples whose decoded speed has the true sign
    double absOk     = 0;    // fraction of samples with |absolute error| <= 1 ms (latency not removed)
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

    // Errors in ms of record time: carrier cycles x 1000 / resolution
    // (1.0 for Serato, whose cycles are ms at 33 rpm).
    const double u = 1000.0 / table.def().resolution;
    std::vector<double> err;
    std::vector<size_t> idx;
    // skip 50 ms after the first lock so that the start-up transient is excluded
    const size_t start = first + size_t (0.05 * opt.sampleRate);
    for (size_t i = start; i < N; ++i)
    {
        if (evalMask != nullptr && ! (*evalMask)[i]) continue;
        err.push_back ((pos[i] - theta[i]) * u); // ms of record time
        idx.push_back (i);
    }
    if (err.empty()) return r;

    {
        size_t moving = 0, sameSign = 0, within = 0;
        for (size_t k = 0; k < err.size(); ++k)
        {
            const size_t i = idx[k];
            if (std::abs (err[k]) <= 1.0) ++within;
            const size_t i1 = std::min (i + 1, N - 1), i0 = i1 - 1;
            const double v = (theta[i1] - theta[i0]) * opt.sampleRate / table.def().resolution;
            if (std::abs (v) > 0.05)
            {
                ++moving;
                if (double (spd[i]) * v > 0.0) ++sameSign;
            }
        }
        r.absOk  = double (within) / double (err.size());
        r.signOk = moving > 0 ? double (sameSign) / double (moving) : 1.0;
    }

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
        const double dt = (theta[i + w] - theta[i]) * u;
        const double dp = (pos[i + w] - pos[i]) * u;
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

int runSerato (bool quick)
{
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

// --------------------------------------------------------------------------
// Traktor Control Vinyl MK2 (2500 Hz carrier, bits as a common-mode offset)

void printMk2 (const char* name, const Result& r, const char* verdict)
{
    std::printf ("%-34s lock %7.1f ms | offset %+6.3f ms | p99 %6.3f ms | max %6.3f ms | drift %+7.3f ms | stuck %5.2f %% | reanchor %d | cfg %d | %5.1f ns/smp | sign %6.2f %% | |err|<=1ms %6.2f %%%s\n",
                 name, r.lockMs, r.offsetMs, r.p99Ms, r.maxMs, r.driftMs, r.stuckPct, r.reanchors, r.config, r.nsPerSample,
                 100.0 * r.signOk, 100.0 * r.absOk, verdict);
}

// Clean playback must anchor within 300 ms and then follow the record: the
// decoded speed has the true sign and the absolute position is within 1 ms
// (latency included) for at least 99 % of the samples.
bool mk2Pass (const Result& r)
{
    return r.lockMs >= 0.0 && r.lockMs <= 300.0 && r.signOk >= 0.99 && r.absOk >= 0.99;
}

SynthOptions mk2Options()
{
    SynthOptions o;
    o.level   = 0.22; // nominal about -20 dBFS as in the recordings
    o.gainR   = 0.95; // a small L / R mismatch leaks the offset into L - R
    o.skewDeg = 0.0;  // the L / R phase difference is mk2PhaseDeg
    return o;
}

// Needle drop: times from the known landing sample.
void mk2NeedleDrop (const LfsrTable& t, double fromBit, double toBit, double speed, const SynthOptions& opt)
{
    const double sr = opt.sampleRate, res = t.def().resolution, cpb = t.def().cyclesPerBit;
    const double u = 1000.0 / res;
    std::vector<double> th;
    double p = cpb * fromBit;
    for (size_t i = 0; i < size_t (3 * sr); ++i) { th.push_back (p); p += speed * res / sr; }
    for (size_t i = 0; i < size_t (0.3 * sr); ++i) th.push_back (p); // lifted (no motion)
    p = cpb * toBit;
    const size_t dropAt = th.size();
    for (size_t i = 0; i < size_t (3 * sr); ++i) { th.push_back (p); p += speed * res / sr; }

    std::vector<float> L, R;
    synthesize (t, th, opt, L, R);
    TimecodeDecoder dec; dec.prepare (sr); dec.setTable (&t); dec.setGateDb (-36.f);
    std::vector<double> pos (th.size());
    const int block = 128;
    double gateMs = -1, bitMs = -1, countMs = -1;
    bool signalAtDrop = false, bitAtDrop = false, bitFell = false;
    bool prevSignal = true, prevBit = false;
    int countAtDrop = -1;
    for (size_t i = 0; i < th.size(); i += size_t (block))
    {
        const int n = int (std::min<size_t> (size_t (block), th.size() - i));
        dec.process (L.data() + i, R.data() + i, n, pos.data() + i, nullptr, nullptr);
        const auto& st = dec.status();
        const size_t end = i + size_t (n);
        const double ms = 1000.0 * (double (end) - double (dropAt)) / sr;
        if (end <= dropAt)
        {
            signalAtDrop = st.signal; bitAtDrop = st.bitLock; countAtDrop = st.reanchors;
            prevSignal = st.signal; prevBit = st.bitLock;
            continue;
        }
        if (gateMs < 0 && st.signal && ! prevSignal) gateMs = ms;
        if (! st.bitLock) bitFell = true;
        if (bitMs < 0 && st.bitLock && ! prevBit) bitMs = ms;
        if (countMs < 0 && st.reanchors > countAtDrop) countMs = ms;
        prevSignal = st.signal; prevBit = st.bitLock;
    }
    // re-anchored: |error| <= 1 ms from here on for at least 200 ms
    double anchorMs = -1;
    const size_t hold = size_t (0.2 * sr);
    size_t good = 0;
    for (size_t i = dropAt; i < th.size(); ++i)
    {
        good = std::abs ((pos[i] - th[i]) * u) <= 1.0 ? good + 1 : 0;
        if (good >= hold) { anchorMs = 1000.0 * double (i + 1 - hold - dropAt) / sr; break; }
    }
    auto fmt = [] (char* b, size_t n, double v, const char* why)
    {
        if (v >= 0) std::snprintf (b, n, "%6.1f ms", v);
        else        std::snprintf (b, n, "n/a (%s)", why);
    };
    char a[64], g[64], b[64], c[64];
    fmt (a, sizeof a, anchorMs, "never within 1 ms for 200 ms");
    fmt (g, sizeof g, gateMs, signalAtDrop ? "gate stayed open" : "gate did not open");
    fmt (b, sizeof b, bitMs, bitAtDrop && ! bitFell ? "bitLock held across the lift" : "no new bitLock");
    fmt (c, sizeof c, countMs, "no reanchor counted");
    char name[64];
    std::snprintf (name, sizeof name, "needle drop %s %.2fx", t.def().id, speed);
    std::printf ("%-34s re-anchor %s | ref: gate %s | bitLock %s | reanchors+1 %s\n", name, a, g, b, c);
}

int runTraktorMk2 (bool quick)
{
    const LfsrTable tA (kTraktorMk2SideA), tB (kTraktorMk2SideB);

    // LFSR sanity on both sides (start and end of the record)
    for (const LfsrTable* t : { &tA, &tB })
    {
        const uint32_t len = t->def().length;
        uint32_t s = t->def().seed;
        int bad = 0;
        for (uint32_t n = 0; n < len; ++n)
        {
            if (n < 100000u || n + 100000u >= len)
            {
                if (t->reverse (t->forward (s)) != s) ++bad;
                if (t->lookup (s) != int32_t (n)) ++bad;
            }
            s = t->forward (s);
        }
        std::printf ("LFSR self-test %s: %s\n", t->def().description, bad == 0 ? "OK" : "FAILED");
        if (bad) return 1;
    }

    const SynthOptions opt = mk2Options();
    const double sr = opt.sampleRate;
    const double res = kTraktorMk2SideB.resolution, cpb = kTraktorMk2SideB.cyclesPerBit;
    std::printf ("\nTraktor MK2 (48 kHz, block 128, cartridge+RIAA, noise -60 dB, R gain 0.95, phase %.0f deg, offset %.2f)\n",
                 opt.mk2PhaseDeg, opt.mk2Offset);

    int failures = 0;
    auto check = [&] (const char* name, const Result& r)
    {
        const bool ok = mk2Pass (r);
        if (! ok) ++failures;
        printMk2 (name, r, ok ? " | PASS" : " | FAIL");
    };

    // 1. Clean playback, both sides, forwards and backwards, three places
    for (const LfsrTable* t : { &tA, &tB })
        for (int place = 0; place < 3; ++place)
            for (double sp : { 0.4, 1.0, 2.16 })
                for (int dir : { 1, -1 })
                {
                    const double secs = 3.0, span = secs * sp * res;
                    const double lo = cpb * 2000.0, hi = cpb * double (t->def().safe);
                    const double from = place == 0 ? lo : place == 1 ? 0.5 * (lo + hi - span) : hi - span;
                    auto th = integrateSpeed (constantSpeed (secs, sp * dir, sr), dir > 0 ? from : from + span, sr, res);
                    char name[64];
                    std::snprintf (name, sizeof name, "%s %-6s %+.2fx", t == &tA ? "A" : "B",
                                   place == 0 ? "start" : place == 1 ? "middle" : "end", sp * dir);
                    check (name, run (*t, th, opt));
                }

    // 2. Channel order / both channels inverted (one channel alone swaps the
    //    roles of L - R and L + R and is not a supported wiring)
    for (int c = 0; c < 2; ++c)
        for (int dir : { 1, -1 })
        {
            SynthOptions o = opt;
            if (c == 0) o.swapLR = true;
            else        o.invertL = o.invertR = true;
            auto th = integrateSpeed (constantSpeed (3, double (dir), sr), cpb * 400000.0, sr, res);
            char name[64];
            std::snprintf (name, sizeof name, "%s %+dx", c == 0 ? "swap L/R" : "invert L+R", dir);
            check (name, run (tB, th, o));
        }

    // Baselines (no pass / fail): the values are recorded in docs/results_decoder.txt
    const double mid = cpb * 400000.0;
    {
        const double secs = quick ? 60.0 : 300.0;
        char name[64];
        std::snprintf (name, sizeof name, "scratch %.0f s (<=4x)", secs);
        printMk2 (name, run (tB, integrateSpeed (scratchProfile (secs, sr, 7), mid, sr, res), opt), "");
        // (8x would put the carrier at 20 kHz, next to the Nyquist frequency)
        std::snprintf (name, sizeof name, "scratch %.0f s (<=6x flicks)", secs);
        printMk2 (name, run (tB, integrateSpeed (scratchProfile (secs, sr, 11, 6.0), mid, sr, res), opt), "");
    }
    {
        // a hand pushing / pulling the record (tear scratches): L - R and L + R
        // get a 20-150 Hz component. Sizes fitted to b_33_tear / b_33_tear-hard
        // (mean loop residual 0.03 / 0.05 there, 0.023 / 0.036 here; quality < 0.5
        // 0.3 / 4-5 % there, 0.4 / 1.1 % here: the hard case is milder)
        SynthOptions o = opt; o.handVertical = 3.0; o.handCommon = 1.0;
        printMk2 ("scratch 30 s, hand (tear)", run (tB, integrateSpeed (scratchProfile (30, sr, 13), mid, sr, res), o), "");
        o.handVertical = 6.0; o.handCommon = 2.0;
        printMk2 ("scratch 30 s, hand (tear-hard)", run (tB, integrateSpeed (scratchProfile (30, sr, 13), mid, sr, res), o), "");
    }
    for (double rate : { 44100.0, 96000.0 })
    {
        SynthOptions o = opt; o.sampleRate = rate;
        char name[64];
        std::snprintf (name, sizeof name, "scratch 30 s @ %.1f kHz", rate / 1000.0);
        printMk2 (name, run (tB, integrateSpeed (scratchProfile (30, rate, 3), mid, rate, res), o), "");
    }
    printMk2 ("scratch 30 s, block 1024", run (tB, integrateSpeed (scratchProfile (30, sr, 9), mid, sr, res), opt, 1024), "");

    mk2NeedleDrop (tB, 300000.0, 700000.0, 1.0, opt);
    mk2NeedleDrop (tA, 100000.0, 500000.0, 1.0, opt);

    {
        // stopped record with knocks
        std::vector<double> v (size_t (1.0 * sr), 1.0);
        for (size_t i = 0; i < size_t (0.2 * sr); ++i) v.push_back (1.0 - double (i) / (0.2 * sr));
        v.resize (size_t (11.2 * sr), 0.0);
        SynthOptions o = opt; o.vibration = 0.0316;
        std::vector<char> mask (v.size(), 0);
        std::fill (mask.begin() + long (1.5 * sr), mask.end(), char (1));
        printMk2 ("standstill 10 s, vibration -30 dB", run (tB, integrateSpeed (v, mid, sr, res), o, 128, &mask), "");
    }
    {
        // back-cueing in Relative mode (as for Serato)
        std::vector<double> v (size_t (sr), 1.0);
        const double k = 1.0 - std::exp (-1.0 / (0.006 * sr));
        double cur = 1.0, pos = 0.0;
        for (int rep = 0; rep < 40; ++rep)
        {
            for (size_t i = 0; i < size_t (0.5 * sr); ++i) { cur += k * (1.0 - cur); v.push_back (cur); pos += cur / sr; }
            while (pos > 0.0) { cur += k * (-2.0 - cur); v.push_back (cur); pos += cur / sr; }
        }
        for (size_t i = 0; i < size_t (0.2 * sr); ++i) { cur += k * (0.0 - cur); v.push_back (cur); }
        auto th = integrateSpeed (v, mid, sr, res);
        std::vector<float> L, R; synthesize (tB, th, opt, L, R);
        TimecodeDecoder dec; dec.prepare (sr); dec.setTable (&tB); dec.setGateDb (-36.f);
        std::vector<double> rel (th.size());
        for (size_t i = 0; i < th.size(); i += 128)
        {
            const int n = int (std::min<size_t> (128, th.size() - i));
            dec.process (L.data() + i, R.data() + i, n, nullptr, rel.data() + i, nullptr);
        }
        const double u = 1000.0 / res;
        const size_t i0 = size_t (sr);
        const double ref = rel[i0] - th[i0];
        double maxErr = 0.0;
        for (size_t i = i0; i < th.size(); ++i) maxErr = std::max (maxErr, std::abs (rel[i] - th[i] - ref) * u);
        const double endErr = (rel.back() - th.back() - ref) * u;
        std::printf ("%-34s relative position: end %+7.3f ms | max %6.3f ms | reanchor %d\n", "back-cue 40x (1x / -2x)",
                     endErr, maxErr, dec.status().reanchors);
    }

    std::printf ("\nTraktor MK2 checks: %s (%d failed)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main (int argc, char** argv)
{
    bool quick = false, mk2Only = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp (argv[i], "--quick") == 0) quick = true;
        if (std::strcmp (argv[i], "--mk2") == 0)   mk2Only = true;
    }
    if (! mk2Only)
    {
        const int rc = runSerato (quick);
        if (rc != 0) return rc;
    }
    return runTraktorMk2 (quick);
}
