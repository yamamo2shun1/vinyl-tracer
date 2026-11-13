// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// tc_analyze - run the timecode decoder over a WAV recording of a turntable.
//
//   tc_analyze input.wav [--side A|B] [--gate -36] [--csv trace.csv] [--block 128]
//
// Record the timecode through JUMBLEQ (or any interface) at 44.1/48/96 kHz,
// stereo, without processing. The tool prints a summary and optionally writes
// a CSV trace (1 row per millisecond) for plotting.

#include "TimecodeDecoder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

bool readWav (const char* path, std::vector<float>& L, std::vector<float>& R, double& rate, std::string& err)
{
    FILE* f = std::fopen (path, "rb");
    if (! f) { err = "cannot open file"; return false; }
    auto rd = [&] (void* p, size_t n) { return std::fread (p, 1, n, f) == n; };

    char id[4]; uint32_t sz = 0;
    if (! rd (id, 4) || std::memcmp (id, "RIFF", 4) != 0 || ! rd (&sz, 4) || ! rd (id, 4) || std::memcmp (id, "WAVE", 4) != 0)
    { err = "not a RIFF/WAVE file"; std::fclose (f); return false; }

    uint16_t fmt = 0, ch = 0, bits = 0;
    uint32_t sr = 0;
    std::vector<uint8_t> data;
    while (rd (id, 4) && rd (&sz, 4))
    {
        if (std::memcmp (id, "fmt ", 4) == 0)
        {
            std::vector<uint8_t> b (sz);
            if (! rd (b.data(), sz)) break;
            std::memcpy (&fmt, &b[0], 2);
            std::memcpy (&ch, &b[2], 2);
            std::memcpy (&sr, &b[4], 4);
            std::memcpy (&bits, &b[14], 2);
            if (fmt == 0xFFFE && sz >= 26) std::memcpy (&fmt, &b[24], 2); // extensible: sub-format
        }
        else if (std::memcmp (id, "data", 4) == 0)
        {
            data.resize (sz);
            const size_t got = std::fread (data.data(), 1, sz, f);
            data.resize (got);
            break;
        }
        else
        {
            std::fseek (f, long (sz + (sz & 1)), SEEK_CUR);
        }
    }
    std::fclose (f);

    if (ch < 2) { err = "need a stereo recording (L/R of the turntable)"; return false; }
    if (data.empty()) { err = "no data chunk"; return false; }

    const int bps = bits / 8;
    const size_t frames = data.size() / size_t (bps * ch);
    L.resize (frames); R.resize (frames);
    for (size_t i = 0; i < frames; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            const uint8_t* p = &data[(i * ch + size_t (c)) * size_t (bps)];
            float v = 0.f;
            if (fmt == 3 && bits == 32)      { std::memcpy (&v, p, 4); }
            else if (fmt == 1 && bits == 16) { int16_t s; std::memcpy (&s, p, 2); v = float (s) / 32768.f; }
            else if (fmt == 1 && bits == 24) { int32_t s = (int32_t (p[2]) << 24) | (int32_t (p[1]) << 16) | (int32_t (p[0]) << 8); v = float (s) / 2147483648.f; }
            else if (fmt == 1 && bits == 32) { int32_t s; std::memcpy (&s, p, 4); v = float (s) / 2147483648.f; }
            else { err = "unsupported sample format (use 16/24/32-bit PCM or 32-bit float)"; return false; }
            (c == 0 ? L : R)[i] = v;
        }
    }
    rate = double (sr);
    return true;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: tc_analyze input.wav [--side A|B] [--gate dB] [--csv out.csv] [--block N] [--window t0 t1]...\n"
                     "                  [--nominal dB] [--scope out.bin t0 t1]\n");
        return 1;
    }
    // --nominal dB: start from a learned nominal level, as the plugin does
    // with the value saved in its state.
    // --scope out.bin t0 t1: write what the plugin's L / R scope shows for
    // that span (used for the web site's scope animation). Points as in the
    // plugin: raw input normalised to the nominal level, every 2nd sample,
    // pulled in to 1.4 x nominal. Little-endian:
    //   "VTS1", u32 point rate, u32 points, u32 status rate (100 Hz), u32 statuses,
    //   i8 x/y pairs (127 = 1.4 x nominal),
    //   per status: i16 speed x 1000, u8 flags (1 signal, 2 code lock, 4 anchored), u8 0
    const char* scopePath = nullptr;
    double scopeT0 = 0, scopeT1 = 0;
    float nominal = 0.f;
    bool haveNominal = false;
    // --window t0 t1: report how far the output position moved in that span
    // (e.g. a stretch where the record stood still) and its largest step.
    struct Window { double t0, t1, lo = 1e300, hi = -1e300, step = 0; };
    std::vector<Window> windows;
    const char* in = argv[1];
    const char* csv = nullptr;
    bool sideB = false;
    float gate = -36.f;
    int block = 128;
    for (int i = 2; i < argc; ++i)
    {
        if (! std::strcmp (argv[i], "--side") && i + 1 < argc)  sideB = (argv[++i][0] == 'B' || argv[i][0] == 'b');
        else if (! std::strcmp (argv[i], "--gate") && i + 1 < argc)  gate = float (std::atof (argv[++i]));
        else if (! std::strcmp (argv[i], "--csv") && i + 1 < argc)   csv = argv[++i];
        else if (! std::strcmp (argv[i], "--block") && i + 1 < argc) block = std::max (1, std::atoi (argv[++i]));
        else if (! std::strcmp (argv[i], "--window") && i + 2 < argc)
        {
            Window w;
            w.t0 = std::atof (argv[++i]);
            w.t1 = std::atof (argv[++i]);
            windows.push_back (w);
        }
        else if (! std::strcmp (argv[i], "--nominal") && i + 1 < argc) { nominal = float (std::atof (argv[++i])); haveNominal = true; }
        else if (! std::strcmp (argv[i], "--scope") && i + 3 < argc)
        {
            scopePath = argv[++i];
            scopeT0 = std::atof (argv[++i]);
            scopeT1 = std::atof (argv[++i]);
        }
    }

    std::vector<float> L, R;
    double sr = 0;
    std::string err;
    if (! readWav (in, L, R, sr, err)) { std::fprintf (stderr, "error: %s\n", err.c_str()); return 1; }

    const vt::LfsrTable table (sideB ? vt::kSeratoSideB : vt::kSeratoSideA);
    vt::TimecodeDecoder dec;
    dec.prepare (sr);
    dec.setTable (&table);
    dec.setGateDb (gate);
    if (haveNominal) dec.setNominalDb (nominal);

    std::vector<int8_t> scopePts;
    std::vector<uint8_t> scopeStatus;
    int scopeDecim = 0;
    double nextStatus = scopeT0;

    const size_t N = L.size();
    std::vector<double> pos (static_cast<size_t> (block)), rel (static_cast<size_t> (block));
    std::vector<float> spd (static_cast<size_t> (block));

    // Steps in the output: second difference of the tracked position (cycles
    // per sample^2). Smooth motion stays far below kStep even in scratches;
    // a jump of the tracker shows up as a click in the audio.
    constexpr double kStep = 0.005;
    double r1 = 0.0, r2 = 0.0, maxStep = 0.0;
    size_t steps = 0, seen = 0;

    FILE* out = csv ? std::fopen (csv, "w") : nullptr;
    if (out) std::fprintf (out, "time_s,record_s,speed,signal,code_lock,anchored,level_db\n");

    size_t sigSamples = 0, lockSamples = 0;
    double firstAnchor = -1.0;
    double lastCsv = -1.0;
    for (size_t i = 0; i < N; i += size_t (block))
    {
        const int n = int (std::min (size_t (block), N - i));
        // Steps are counted once the direction is known (the relative position
        // changes sign when it is detected).
        const bool count = dec.status().anchored;
        dec.process (L.data() + i, R.data() + i, n, pos.data(), rel.data(), spd.data());
        for (int k = 0; k < n; ++k)
        {
            const double r0 = rel[size_t (k)];
            const double tk = double (i + size_t (k)) / sr;
            const double d2 = (count && seen >= 2) ? std::abs (r0 - 2.0 * r1 + r2) : 0.0;
            if (d2 > kStep) ++steps;
            maxStep = std::max (maxStep, d2);
            for (auto& w : windows)
                if (tk >= w.t0 && tk < w.t1)
                {
                    w.lo = std::min (w.lo, r0);
                    w.hi = std::max (w.hi, r0);
                    w.step = std::max (w.step, d2);
                }
            r2 = r1; r1 = r0; ++seen;
        }
        const auto& st = dec.status();
        if (scopePath)
        {
            // Same as the plugin's processBlock: normalised with the level after the block.
            constexpr float kEdge = 1.4f;
            const float refLin = std::pow (10.f, st.refDb / 20.f);
            const float norm = refLin > 1e-6f ? 1.f / refLin : 1.f;
            for (int k = 0; k < n; ++k)
            {
                if (++scopeDecim < 2) continue;
                scopeDecim = 0;
                const double tk = double (i + size_t (k)) / sr;
                if (tk < scopeT0 || tk >= scopeT1) continue;
                float x = L[i + size_t (k)] * norm, y = R[i + size_t (k)] * norm;
                const float m = std::sqrt (x * x + y * y);
                if (m > kEdge) { x *= kEdge / m; y *= kEdge / m; }
                scopePts.push_back (int8_t (std::lround (std::clamp (x / kEdge, -1.f, 1.f) * 127.f)));
                scopePts.push_back (int8_t (std::lround (std::clamp (y / kEdge, -1.f, 1.f) * 127.f)));
            }
            const double tEnd = double (i + size_t (n)) / sr;
            while (nextStatus < scopeT1 && tEnd >= nextStatus)
            {
                if (nextStatus >= scopeT0)
                {
                    const auto sp = int16_t (std::lround (std::clamp (spd[size_t (n - 1)], -32.f, 32.f) * 1000.f));
                    scopeStatus.push_back (uint8_t (uint16_t (sp) & 0xff));
                    scopeStatus.push_back (uint8_t (uint16_t (sp) >> 8));
                    scopeStatus.push_back (uint8_t ((st.signal ? 1 : 0) | (st.bitLock ? 2 : 0) | (st.anchored ? 4 : 0)));
                    scopeStatus.push_back (0);
                }
                nextStatus += 0.01;
            }
        }
        if (st.signal)  sigSamples  += size_t (n);
        if (st.bitLock) lockSamples += size_t (n);
        const double t = double (i + size_t (n)) / sr;
        if (st.anchored && firstAnchor < 0) firstAnchor = t;
        if (out && t - lastCsv >= 0.001)
        {
            lastCsv = t;
            std::fprintf (out, "%.4f,%.5f,%.4f,%d,%d,%d,%.1f\n", t, pos[size_t (n - 1)] / table.def().resolution,
                          spd[size_t (n - 1)], int (st.signal), int (st.bitLock), int (st.anchored), st.levelDb);
        }
    }
    if (out) std::fclose (out);

    if (scopePath)
    {
        FILE* sf = std::fopen (scopePath, "wb");
        if (! sf) { std::fprintf (stderr, "error: cannot write %s\n", scopePath); return 1; }
        const uint32_t hdr[4] { uint32_t (std::lround (sr / 2.0)), uint32_t (scopePts.size() / 2),
                                100u, uint32_t (scopeStatus.size() / 4) };
        std::fwrite ("VTS1", 1, 4, sf);
        std::fwrite (hdr, sizeof (uint32_t), 4, sf);
        std::fwrite (scopePts.data(), 1, scopePts.size(), sf);
        std::fwrite (scopeStatus.data(), 1, scopeStatus.size(), sf);
        std::fclose (sf);
    }

    const auto& st = dec.status();
    std::printf ("file            : %s (%.1f s @ %.0f Hz)\n", in, double (N) / sr, sr);
    std::printf ("timecode        : %s\n", table.def().description);
    std::printf ("signal present  : %.1f %% of the time\n", 100.0 * double (sigSamples) / double (N));
    std::printf ("bitstream locked: %.1f %% of the time\n", 100.0 * double (lockSamples) / double (N));
    std::printf ("first abs. lock : %s\n", firstAnchor < 0 ? "never" : (std::to_string (firstAnchor) + " s").c_str());
    std::printf ("corrections     : %d\n", st.reanchors);
    std::printf ("detected config : %d (primary=%s, polarity=%s, direction=%s)\n", st.config,
                 st.config < 0 ? "-" : ((st.config >> 2) ? "L" : "R"),
                 st.config < 0 ? "-" : (((st.config >> 1) & 1) ? "neg" : "pos"),
                 st.config < 0 ? "-" : ((st.config & 1) ? "inverted" : "normal"));
    std::printf ("nominal level   : %.1f dBFS\n", st.refDb);
    std::printf ("output steps    : %zu (> %.3f cycles), largest %.4f cycles\n", steps, kStep, maxStep);
    for (const auto& w : windows)
        std::printf ("window %6.2f-%6.2f s: moved %.3f cycles, largest step %.4f\n",
                     w.t0, w.t1, w.hi >= w.lo ? w.hi - w.lo : 0.0, w.step);
    if (firstAnchor < 0)
        std::printf ("\nNo lock: check side A/B, that the input is stereo, and that the level is not clipping.\n");
    return 0;
}
