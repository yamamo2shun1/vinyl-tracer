// test_player.cpp - end-to-end check: synthetic timecode -> decoder -> player.
// Compares the audio rendered from decoded positions with the audio rendered
// from the true record positions (same player), and measures CPU time.
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
    return 0;
}
