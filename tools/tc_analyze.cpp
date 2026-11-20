// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// tc_analyze - run the timecode decoder over a WAV recording of a turntable.
//
//   tc_analyze input.wav [--side A|B|MK2A|MK2B] [--gate -36] [--csv trace.csv] [--block 128]
//   tc_analyze input.wav --side MK2B --eval [--eval-ref tests/mk2_eval_refs.txt] [--eval-sample s.wav]
//
// Record the timecode through JUMBLEQ (or any interface) at 44.1/48/96 kHz,
// stereo, without processing. The tool prints a summary and optionally writes
// a CSV trace (1 row per millisecond) for plotting.

#include "DropoutMetrics.h"
#include "TimecodeDecoder.h"

#include <algorithm>
#include <cctype>
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

// --------------------------------------------------------------------------
// --eval (Traktor MK2 recordings): drop-outs of the rendered output, needle
// drops, slips in strokes and sound at standstill, against a true motion that
// does not come from the decoder under test:
//  * |speed| from the phase of the analytic signal of S = (L - R) / 2
//    (120 Hz high-pass, no offset there);
//  * the direction from the sign of the correlation of M = (L + R) / 2 (same
//    high-pass; its carrier is in quadrature with S) with dS/dt over 2 ms;
//    windows where it is not clear are bridged only while they move 0.5 ms or
//    less, and from one reference only while all of them move 1 ms or less,
//    otherwise the reference stops there;
//  * a moving carrier only where S is loud for the recording and S / M are in
//    quadrature (or S carries on from such a window): a recording of noise
//    has no drop-outs to count;
//  * the absolute position from fixed reference points (time, position,
//    tolerance) in a file made once with a fixed decoder version (see
//    tests/mk2_eval_refs.txt), integrated forwards and backwards from the
//    nearest one, never across a needle drop; between two references that
//    do not agree nothing is evaluated.

struct EvalRefs
{
    struct Ref { double t, posMs, tolMs; };
    std::vector<Ref> refs;
    std::vector<double> drops;
    std::vector<std::pair<double, double>> still;
    int polarity = 0; // sign of M x dS/dt that is forward on the record (0 = find from the references)
};

bool loadRefs (const char* path, const std::string& name, EvalRefs& r, std::string& err)
{
    FILE* f = std::fopen (path, "r");
    if (! f) { err = "cannot open the reference file"; return false; }
    char line[512];
    while (std::fgets (line, sizeof line, f))
    {
        if (line[0] == '#') continue;
        char file[256], kind[32];
        double a = 0, b = 0, c = 0;
        const int n = std::sscanf (line, "%255s %31s %lf %lf %lf", file, kind, &a, &b, &c);
        if (n < 3 || name != file) continue;
        const std::string k (kind);
        if (k == "ref" && n >= 5)        r.refs.push_back ({ a, b, c });
        else if (k == "drop")            r.drops.push_back (a);
        else if (k == "still" && n >= 4) r.still.push_back ({ a, b });
        else if (k == "polarity")        r.polarity = a < 0 ? -1 : 1;
    }
    std::fclose (f);
    std::sort (r.refs.begin(), r.refs.end(), [] (const auto& x, const auto& y) { return x.t < y.t; });
    std::sort (r.drops.begin(), r.drops.end());
    return true;
}

// 2nd-order Butterworth high-pass, run forwards and backwards (no phase shift).
std::vector<double> highPassZeroPhase (const std::vector<double>& x, double fc, double sr)
{
    const double w0 = 2.0 * 3.141592653589793 * fc / sr, cw = std::cos (w0), al = std::sin (w0) / std::sqrt (2.0);
    const double a0 = 1.0 + al;
    const double b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0, b2 = b0, a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
    std::vector<double> y (x.size());
    auto pass = [&] (const std::vector<double>& in, std::vector<double>& out, bool rev)
    {
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        const size_t n = in.size();
        for (size_t j = 0; j < n; ++j)
        {
            const size_t i = rev ? n - 1 - j : j;
            const double v = b0 * in[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = in[i]; y2 = y1; y1 = v;
            out[i] = v;
        }
    };
    std::vector<double> tmp (x.size());
    pass (x, tmp, false);
    pass (tmp, y, true);
    return y;
}

// Hilbert transform: windowed (Blackman) FIR, 511 taps (flat above ~200 Hz).
std::vector<double> hilbert (const std::vector<double>& x)
{
    constexpr int M = 255;
    std::vector<double> h (2 * M + 1, 0.0);
    for (int n = -M; n <= M; ++n)
    {
        if ((n & 1) == 0) continue;
        const double w = 0.42 + 0.5 * std::cos (3.141592653589793 * n / (M + 1)) + 0.08 * std::cos (2.0 * 3.141592653589793 * n / (M + 1));
        h[size_t (n + M)] = 2.0 / (3.141592653589793 * n) * w;
    }
    const size_t N = x.size();
    std::vector<double> y (N, 0.0);
    for (size_t i = 0; i < N; ++i)
    {
        double s = 0.0;
        for (int n = -M; n <= M; n += 1)
        {
            const double hn = h[size_t (n + M)];
            if (hn == 0.0) continue;
            const long long j = (long long) i - n;
            if (j >= 0 && j < (long long) N) s += hn * x[size_t (j)];
        }
        y[i] = s;
    }
    return y;
}

int runEval (const std::vector<float>& L, const std::vector<float>& R, double sr, const vt::LfsrTable& table,
             float gate, int block, bool haveNominal, float nominal, const char* in,
             const char* refPath, const char* samplePath, const char* tracePath)
{
    const double res = table.def().resolution, toMs = 1000.0 / res;
    std::string name (in);
    {
        const size_t sl = name.find_last_of ("/\\");
        if (sl != std::string::npos) name = name.substr (sl + 1);
        const size_t dot = name.rfind ('.');
        if (dot != std::string::npos) name = name.substr (0, dot);
    }
    EvalRefs refs;
    if (refPath)
    {
        std::string err;
        if (! loadRefs (refPath, name, refs, err)) { std::fprintf (stderr, "error: %s\n", err.c_str()); return 1; }
    }
    std::printf ("\n--eval (%s): %zu reference points, %zu needle drops, %zu standstill spans\n", name.c_str(),
                 refs.refs.size(), refs.drops.size(), refs.still.size());

    const size_t N = L.size();
    const size_t w = size_t (std::lround (0.001 * sr));
    const size_t nw = N / w;
    const double wSec = double (w) / sr;

    // ---- true motion -------------------------------------------------------
    std::vector<double> sRaw (N), mRaw (N);
    for (size_t i = 0; i < N; ++i)
    {
        sRaw[i] = 0.5 * (double (L[i]) - double (R[i]));
        mRaw[i] = 0.5 * (double (L[i]) + double (R[i]));
    }
    const std::vector<double> s = highPassZeroPhase (sRaw, 120.0, sr);
    const std::vector<double> m = highPassZeroPhase (mRaw, 120.0, sr);
    const std::vector<double> hs = hilbert (s);
    std::vector<double> magMs (nw, 0.0); // |motion| per window, ms of record
    std::vector<int> dir (nw, 0);
    std::vector<char> carrier (nw, 0);   // S is loud enough to be a carrier
    std::vector<double> ncW (nw, 0.0);   // correlation of M with dS/dt (direction)
    std::vector<char> evidence (nw, 0);  // ... and S / M are in quadrature (a moving carrier, not noise)
    std::vector<char> moving;            // evidence, or S carrying on from it
    {
        // Without a carrier the phase is noise and |dphi| adds up to a fast
        // "motion": windows whose S level is below 5 % of the loud carrier (90th
        // percentile of the window RMS) count as no motion. The carrier level
        // goes with the speed, so this is a slow record (a few % of 1x) there.
        std::vector<double> rms (nw, 0.0);
        for (size_t k = 0; k < nw; ++k)
        {
            double e = 0.0;
            for (size_t i = k * w; i < (k + 1) * w; ++i) e += s[i] * s[i];
            rms[k] = std::sqrt (e / double (w));
        }
        std::vector<double> sorted (rms);
        std::sort (sorted.begin(), sorted.end());
        const double thr = 0.05 * (sorted.empty() ? 0.0 : sorted[sorted.size() * 9 / 10]);
        for (size_t k = 0; k < nw; ++k) carrier[k] = rms[k] >= thr ? 1 : 0;

        double prev = std::atan2 (hs[0], s[0]);
        for (size_t k = 0; k < nw; ++k)
        {
            double acc = 0.0;
            for (size_t i = std::max<size_t> (k * w, 1); i < (k + 1) * w; ++i)
            {
                const double ph = std::atan2 (hs[i], s[i]);
                double d = ph - prev;
                prev = ph;
                while (d > 3.141592653589793) d -= 6.283185307179586;
                while (d < -3.141592653589793) d += 6.283185307179586;
                acc += std::abs (d);
            }
            magMs[k] = carrier[k] ? acc / 6.283185307179586 * toMs : 0.0;
        }
        // direction over 2 ms centred on the window
        for (size_t k = 0; k < nw; ++k)
        {
            const size_t a = k * w >= w / 2 ? k * w - w / 2 : 1;
            const size_t b = std::min (N, (k + 1) * w + w / 2);
            double c = 0, em = 0, ed = 0;
            for (size_t i = std::max<size_t> (a, 1); i < b; ++i)
            {
                const double ds = s[i] - s[i - 1];
                c += m[i] * ds; em += m[i] * m[i]; ed += ds * ds;
            }
            const double nc = c / std::sqrt (std::max (em * ed, 1e-30));
            dir[k] = ! carrier[k] ? 0 : nc > 0.3 ? 1 : nc < -0.3 ? -1 : 0;
            ncW[k] = carrier[k] ? nc : 0.0;
        }
        // The level is relative to the recording, so in a recording of noise
        // (no carrier at all) most windows pass it. A moving carrier has M in
        // quadrature with S, with one sign while it moves one way: the signed
        // correlation averaged over 11 ms is 0.3 or more (above 0.5x: 98-100 %
        // of the windows of the scratch recordings), noise averages out
        // (b_needle_up_drop: 8 of 4233 windows above 0.5x).
        for (size_t k = 0; k < nw; ++k)
        {
            if (! carrier[k]) continue;
            double sum = 0.0; int n = 0;
            for (size_t j = k >= 5 ? k - 5 : 0; j <= std::min (nw - 1, k + 5); ++j) { sum += ncW[j]; ++n; }
            evidence[k] = std::abs (sum / double (n)) >= 0.3 ? 1 : 0;
        }
        // M alone can lose the quadrature while the record keeps moving: the
        // hand disturbs M (b_33_tear 2.484 s, 12 ms in a -1.4x pull with the
        // decoder stopped, a real drop-out). A window without the quadrature
        // still counts as moving where S carries on from a window with it, at
        // a speed within 25 % of the previous window's, for up to 30 ms.
        moving = evidence;
        for (int pass = 0; pass < 2; ++pass)
        {
            size_t since = 1000000;
            for (size_t j = 0; j < nw; ++j)
            {
                const size_t k = pass == 0 ? j : nw - 1 - j;
                if (evidence[k]) { since = 0; continue; }
                if (j == 0 || ! carrier[k]) { since = 1000000; continue; }
                const size_t kp = pass == 0 ? k - 1 : k + 1;
                const double sp = magMs[kp], sk = magMs[k];
                if (++since <= 30 && moving[kp] && std::abs (sk - sp) <= 0.25 * sp) moving[k] = 1;
                else since = 1000000;
            }
        }
    }
    auto winOf = [&] (double t) { return std::min (nw, size_t (std::max (0.0, t) / wSec)); };

    // polarity: from the file, or the sign that best joins consecutive references
    std::vector<double> dropK;
    for (double t : refs.drops) dropK.push_back (double (winOf (t)));
    auto crossesDrop = [&] (size_t a, size_t b)
    {
        for (double d : dropK) if (d > double (std::min (a, b)) && d <= double (std::max (a, b))) return true;
        return false;
    };
    // Integrate from window ka (position p) to kb; stops (stopAt) where a run
    // of windows without a direction moves more than 0.5 ms (the reference
    // ends there), or where all the runs bridged so far (their motion is not
    // counted, so the position may be off by it) moved more than cumLimit ms.
    // Windows without a carrier move 0 and are bridged.
    auto integrate = [&] (size_t ka, size_t kb, double p, int pol, std::vector<double>* out, std::vector<char>* ok, size_t* stopAt,
                          double cumLimit = 1e300)
    {
        double gapMove = 0.0, cum = 0.0;
        const int step = kb >= ka ? 1 : -1;
        for (size_t k = ka; k != kb; k = size_t (long long (k) + step))
        {
            const size_t kw = step > 0 ? k : k - 1; // window crossed
            if (dir[kw] == 0)
            {
                gapMove += magMs[kw]; cum += magMs[kw];
                if (gapMove > 0.5 || cum > cumLimit) { if (stopAt) *stopAt = k; return p; }
            }
            else
            {
                gapMove = 0.0;
                p += double (step) * double (pol * dir[kw]) * magMs[kw];
            }
            if (out) { const size_t kn = size_t (long long (k) + step); (*out)[kn] = p; (*ok)[kn] = 1; }
        }
        if (stopAt) *stopAt = kb;
        return p;
    };
    int pol = refs.polarity;
    if (pol == 0 && refs.refs.size() >= 2)
    {
        double e[2] = { 0, 0 };
        for (int c = 0; c < 2; ++c)
        {
            const int pc = c == 0 ? 1 : -1;
            for (size_t r = 0; r + 1 < refs.refs.size(); ++r)
            {
                const size_t ka = winOf (refs.refs[r].t), kb = winOf (refs.refs[r + 1].t);
                if (crossesDrop (ka, kb)) continue;
                size_t stop = 0;
                const double p = integrate (ka, kb, refs.refs[r].posMs, pc, nullptr, nullptr, &stop);
                if (stop == kb) e[c] += std::abs (p - refs.refs[r + 1].posMs);
            }
        }
        pol = e[0] <= e[1] ? 1 : -1;
        std::printf ("direction polarity: %+d (from the references)\n", pol);
    }
    if (pol == 0) pol = 1;

    // check the references against each other (each is within its tolerance,
    // so two may differ by the sum); between two that do not agree one of
    // them is wrong, and nothing there is evaluated
    std::vector<char> mismatch (refs.refs.size(), 0); // pair r -> r + 1
    for (size_t r = 0; r + 1 < refs.refs.size(); ++r)
    {
        const auto& a = refs.refs[r];
        const auto& b = refs.refs[r + 1];
        const size_t ka = winOf (a.t), kb = winOf (b.t);
        if (crossesDrop (ka, kb)) { std::printf ("reference %.3f -> %.3f s: needle drop between\n", a.t, b.t); continue; }
        size_t stop = 0;
        const double p = integrate (ka, kb, a.posMs, pol, nullptr, nullptr, &stop);
        if (stop != kb)
            std::printf ("reference %.3f -> %.3f s: direction gap at %.3f s, not joined\n", a.t, b.t, double (stop) * wSec);
        else
        {
            mismatch[r] = std::abs (p - b.posMs) > a.tolMs + b.tolMs ? 1 : 0;
            std::printf ("reference %.3f -> %.3f s: integrated %+.2f ms vs %+.2f ms (%s)\n", a.t, b.t, p - a.posMs,
                         b.posMs - a.posMs, mismatch[r] ? "MISMATCH, not evaluated" : "ok");
        }
    }

    // true position at the window starts: from the nearest reference, or
    // between two joined references adjusted to both
    std::vector<double> truth (nw + 1, 0.0);
    std::vector<char> valid (nw + 1, 0);
    {
        std::vector<double> bestDist (nw + 1, 1e300);
        for (size_t r = 0; r < refs.refs.size(); ++r)
        {
            const size_t k0 = winOf (refs.refs[r].t);
            // span up to the neighbouring references and needle drops
            size_t lo = 0, hi = nw;
            if (r > 0) lo = winOf (refs.refs[r - 1].t);
            if (r + 1 < refs.refs.size()) hi = winOf (refs.refs[r + 1].t);
            for (double d : dropK)
            {
                const size_t kd = size_t (d);
                if (kd <= k0) lo = std::max (lo, kd);
                if (kd > k0)  hi = std::min (hi, kd);
            }
            std::vector<double> tr (nw + 1, 0.0);
            std::vector<char> ok (nw + 1, 0);
            tr[k0] = refs.refs[r].posMs; ok[k0] = 1;
            // From one reference, the bridged runs may add up to 1 ms (the
            // reference's tolerance): no worse than two joined references.
            integrate (k0, hi, refs.refs[r].posMs, pol, &tr, &ok, nullptr, 1.0);
            integrate (k0, lo, refs.refs[r].posMs, pol, &tr, &ok, nullptr, 1.0);
            for (size_t k = lo; k <= hi && k <= nw; ++k)
            {
                const double dist = std::abs (double (k) - double (k0));
                if (ok[k] && dist < bestDist[k]) { bestDist[k] = dist; truth[k] = tr[k]; valid[k] = 1; }
            }
        }
        // Between two references that the integration joins within their
        // tolerances, spread the closing error over the way in proportion to
        // the distance moved (the phase errors come with the motion, mostly at
        // reversals), so that the reference meets both ends.
        for (size_t r = 0; r + 1 < refs.refs.size(); ++r)
        {
            const auto& a = refs.refs[r];
            const auto& b = refs.refs[r + 1];
            const size_t ka = winOf (a.t), kb = winOf (b.t);
            if (kb <= ka || crossesDrop (ka, kb)) continue;
            std::vector<double> tr (nw + 1, 0.0);
            std::vector<char> ok (nw + 1, 0);
            size_t stop = 0;
            const double p = integrate (ka, kb, a.posMs, pol, &tr, &ok, &stop);
            const double close = b.posMs - p;
            if (stop != kb || std::abs (close) > a.tolMs + b.tolMs) continue;
            double total = 0.0;
            for (size_t k = ka; k < kb; ++k) total += magMs[k];
            double run = 0.0;
            truth[ka] = a.posMs; valid[ka] = 1;
            for (size_t k = ka; k < kb; ++k)
            {
                run += magMs[k];
                truth[k + 1] = tr[k + 1] + (total > 0.0 ? close * run / total : 0.0);
                valid[k + 1] = 1;
            }
        }
        for (size_t r = 0; r + 1 < refs.refs.size(); ++r)
            if (mismatch[r])
                for (size_t k = winOf (refs.refs[r].t); k <= winOf (refs.refs[r + 1].t) && k <= nw; ++k) valid[k] = 0;
    }

    // ---- decoder and player ------------------------------------------------
    vt::TimecodeDecoder dec;
    dec.prepare (sr);
    dec.setTable (&table);
    dec.setGateDb (gate);
    if (haveNominal) dec.setNominalDb (nominal);
    vt::SampleData src;
    bool customSample = false;
    if (samplePath)
    {
        std::vector<float> a, b; double r = 0; std::string err;
        if (! readWav (samplePath, a, b, r, err)) { std::fprintf (stderr, "error: --eval-sample: %s\n", err.c_str()); return 1; }
        const float* ch[2] = { a.data(), b.data() };
        src.build (ch, 2, int64_t (a.size()), r);
        customSample = true;
    }
    else
        src = vt::test::makeQuadratureTone();
    vt::ScratchPlayer play, playNq;
    play.prepare (sr); playNq.prepare (sr);
    vt::ScratchPlayer::Params pp;
    pp.loop = true;

    std::vector<double> pos (N);
    std::vector<float> q (N), oL (N), oR (N), nL (N), nR (N);
    struct Blk { size_t end; bool signal, bitLock, anchored; int reanchors; };
    std::vector<Blk> blocks;
    for (size_t i = 0; i < N; i += size_t (block))
    {
        const int n = int (std::min (size_t (block), N - i));
        dec.process (L.data() + i, R.data() + i, n, pos.data() + i, nullptr, nullptr, q.data() + i);
        for (int k = 0; k < n; ++k) pos[i + size_t (k)] *= toMs;
        play.render (&src, pos.data() + i, n, pp, oL.data() + i, oR.data() + i, q.data() + i);
        playNq.render (&src, pos.data() + i, n, pp, nL.data() + i, nR.data() + i, nullptr);
        const auto& st = dec.status();
        blocks.push_back ({ i + size_t (n), st.signal, st.bitLock, st.anchored, st.reanchors });
    }
    std::vector<char> anchoredW (nw, 0);
    {
        size_t b = 0;
        for (size_t k = 0; k < nw; ++k)
        {
            while (b + 1 < blocks.size() && blocks[b].end <= k * w) ++b;
            anchoredW[k] = blocks[b].anchored ? 1 : 0;
        }
    }
    if (tracePath)
    {
        // per window: time, |motion| (ms), direction, reference position, valid, decoded position (ms)
        FILE* f = std::fopen (tracePath, "w");
        if (f)
        {
            std::fprintf (f, "time_s,move_ms,dir,ref_ms,ref_valid,decoded_ms,anchored\n");
            for (size_t k = 0; k < nw; ++k)
                std::fprintf (f, "%.4f,%.4f,%d,%.3f,%d,%.3f,%d\n", double (k) * wSec, magMs[k], dir[k] * pol, truth[k],
                              int (valid[k]), pos[k * w], int (anchoredW[k]));
            std::fclose (f);
        }
    }

    // ---- drop-outs -----------------------------------------------------------
    {
        std::vector<double> ts (nw), ds (nw);
        for (size_t k = 0; k < nw; ++k)
        {
            ts[k] = magMs[k] / (1000.0 * wSec);
            const size_t a = k * w, b = std::min (N - 1, (k + 1) * w);
            ds[k] = std::abs (pos[b] - pos[a]) / (1000.0 * double (b - a) / sr);
        }
        // the speed is known where S is a moving carrier (level and
        // quadrature); a record known to stand still is not moving whatever S
        // does (knocks)
        std::vector<char> v (moving), still (nw, 0);
        for (const auto& sp : refs.still)
            for (size_t k = winOf (sp.first); k < winOf (sp.second); ++k) { still[k] = 1; v[k] = 0; }
        std::printf ("source          : %s\n", customSample ? "--eval-sample (the drop-out count includes silence in the sample itself)"
                                                         : "looped quadrature tone");
        const auto dr = vt::test::measureDropouts (oL, oR, nL, nR, sr, ts, ds, v, still);
        double quadMs = 0.0; // windows above 0.5x with the quadrature itself
        for (size_t k = 0; k < nw; ++k) if (evidence[k] && ! still[k] && ts[k] > 0.5) quadMs += 1000.0 * wSec;
        if (quadMs >= 50.0)
            vt::test::printDropouts ("drop-outs", dr);
        else
        {
            // no moving carrier to measure against (a record that does not turn)
            std::printf ("%-34s drop-outs above 0.5x: n/a (S / M in quadrature for %.0f ms only)", "drop-outs", quadMs);
            if (dr.stillMs > 0)
                std::printf (" | standstill: %.0f of %.0f ms at -40 dBFS or above", dr.stillLoudMs, dr.stillMs);
            std::printf ("\n");
        }
    }

    // ---- needle drops --------------------------------------------------------
    // Each landing is measured up to the next one: the position and the
    // decoder's state after the next landing belong to that one.
    for (size_t di = 0; di < refs.drops.size(); ++di)
    {
        const double td = refs.drops[di];
        const double tNext = di + 1 < refs.drops.size() ? refs.drops[di + 1] : double (N) / sr;
        const size_t kd = winOf (td), kEnd = winOf (tNext);
        const size_t hold = size_t (0.2 / wSec);
        bool hasRef = false;
        for (const auto& rf : refs.refs) if (rf.t >= td && rf.t < tNext) hasRef = true;
        double anchorMs = -1;
        size_t good = 0;
        for (size_t k = kd + 1; hasRef && k < kEnd; ++k) // window kd may still be the previous landing's
        {
            if (! valid[k]) { good = 0; continue; }
            const double e = pos[k * w] - truth[k];
            good = std::abs (e) <= 1.0 ? good + 1 : 0;
            if (good >= hold) { anchorMs = 1000.0 * (double (k + 1 - hold) * wSec - td); break; }
        }
        // reference times from the decoder's state (per block)
        double gateMs = -1, bitMs = -1, countMs = -1;
        bool sig0 = false, bit0 = false, bitFell = false, prevSig = true, prevBit = false;
        int cnt0 = -1;
        for (const auto& b : blocks)
        {
            const double t = double (b.end) / sr;
            if (t <= td) { sig0 = b.signal; bit0 = b.bitLock; cnt0 = b.reanchors; prevSig = b.signal; prevBit = b.bitLock; continue; }
            if (t > tNext) break;
            const double ms = 1000.0 * (t - td);
            if (gateMs < 0 && b.signal && ! prevSig) gateMs = ms;
            if (! b.bitLock) bitFell = true;
            if (bitMs < 0 && b.bitLock && ! prevBit) bitMs = ms;
            if (countMs < 0 && b.reanchors > cnt0) countMs = ms;
            prevSig = b.signal; prevBit = b.bitLock;
        }
        char a[96], g[64], bl[64], c[64];
        if (anchorMs >= 0) std::snprintf (a, sizeof a, "%6.1f ms", anchorMs);
        else std::snprintf (a, sizeof a, "n/a (%s)", hasRef ? "not within 1 ms for 200 ms before the next landing" : "no reference in this landing");
        if (gateMs >= 0) std::snprintf (g, sizeof g, "%.1f ms", gateMs); else std::snprintf (g, sizeof g, "n/a (%s)", sig0 ? "stayed open" : "did not open");
        if (bitMs >= 0) std::snprintf (bl, sizeof bl, "%.1f ms", bitMs); else std::snprintf (bl, sizeof bl, "n/a (%s)", bit0 && ! bitFell ? "held across the lift" : "no new lock");
        if (countMs >= 0) std::snprintf (c, sizeof c, "%.1f ms", countMs); else std::snprintf (c, sizeof c, "n/a (none counted)");
        std::printf ("needle drop %8.3f s: re-anchor %s | ref: gate %s | bitLock %s | reanchors+1 %s\n", td, a, g, bl, c);
    }

    // ---- slips in strokes ------------------------------------------------------
    if (! refs.refs.empty())
    {
        // strokes: runs of one known direction, from 5 ms after a reversal
        struct Stroke { size_t a, b; };
        std::vector<Stroke> strokes;
        int cur = 0; size_t start = 0;
        for (size_t k = 0; k <= nw; ++k)
        {
            const int d = k < nw ? dir[k] : -cur;
            if (d == 0) continue;
            if (d != cur)
            {
                if (cur != 0 && k > start + 5) strokes.push_back ({ start + 5, k });
                cur = d; start = k;
            }
        }
        // Which strokes count depends on the recording and the references only;
        // a stroke the decoder is not anchored in is its failure, not excluded.
        int evaluated = 0, excluded = 0, slipped = 0;
        double overMs = 0.0, maxErr = 0.0;
        std::vector<double> errs, unanchored;
        struct Worst { double t, over, max; };
        std::vector<Worst> worst;
        for (const auto& st : strokes)
        {
            bool ok = true, anch = true; double moved = 0.0;
            for (size_t k = st.a; k < st.b; ++k) { if (! valid[k]) ok = false; if (! anchoredW[k]) anch = false; moved += magMs[k]; }
            if (moved < 2.0) continue; // too short to be a stroke
            if (! ok) { ++excluded; continue; }
            if (! anch) { unanchored.push_back (double (st.a) * wSec); continue; }
            ++evaluated;
            double over = 0.0, mx = 0.0;
            for (size_t k = st.a; k < st.b; ++k)
            {
                const double e = std::abs (pos[k * w] - truth[k]);
                errs.push_back (e);
                if (e > 2.0) over += 1000.0 * wSec;
                mx = std::max (mx, e);
            }
            if (over > 0.0) { ++slipped; overMs += over; worst.push_back ({ double (st.a) * wSec, over, mx }); }
            maxErr = std::max (maxErr, mx);
        }
        std::sort (errs.begin(), errs.end());
        const auto pct = [&] (double p) { return errs.empty() ? 0.0 : errs[std::min (errs.size() - 1, size_t (p * double (errs.size())))]; };
        std::printf ("strokes         : %d evaluated, %zu not anchored by the decoder, %d excluded (no reference / direction gap / mismatch)\n",
                     evaluated, unanchored.size(), excluded);
        if (! unanchored.empty())
        {
            std::printf ("  not anchored at:");
            for (size_t i = 0; i < std::min<size_t> (10, unanchored.size()); ++i) std::printf (" %.3f", unanchored[i]);
            std::printf ("%s s\n", unanchored.size() > 10 ? " ..." : "");
        }
        if (evaluated > 0)
            std::printf ("slips (>2 ms)   : %d strokes, %.0f ms in total, largest error %.1f ms | error p50 %.2f p90 %.2f p99 %.2f ms\n",
                         slipped, overMs, maxErr, pct (0.5), pct (0.9), pct (0.99));
        else
            std::printf ("slips (>2 ms)   : n/a (no stroke with a decoded position to compare)\n");
        std::sort (worst.begin(), worst.end(), [] (const Worst& x, const Worst& y) { return x.over > y.over; });
        for (size_t i = 0; i < std::min<size_t> (5, worst.size()); ++i)
            std::printf ("  stroke at %7.3f s: %.0f ms over 2 ms, max %.1f ms\n", worst[i].t, worst[i].over, worst[i].max);
        size_t val = 0; for (size_t k = 0; k < nw; ++k) val += valid[k] ? 1 : 0;
        std::printf ("reference covers: %.1f of %.1f s\n", double (val) * wSec, double (nw) * wSec);
    }
    return 0;
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: tc_analyze input.wav [--side A|B|MK2A|MK2B] [--gate dB] [--csv out.csv] [--block N] [--window t0 t1]...\n"
                     "                  [--nominal dB] [--scope out.bin t0 t1]\n"
                     "                  [--eval [--eval-ref refs.txt] [--eval-sample sample.wav] [--eval-csv out.csv]]  (Traktor MK2)\n");
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
    // --eval: extra measurements of the output against a reference motion
    // (see runEval); the summary and the CSV stay as they are.
    bool eval = false;
    const char* evalRef = nullptr;
    const char* evalSample = nullptr;
    const char* evalCsv = nullptr;
    const char* in = argv[1];
    const char* csv = nullptr;
    bool sideB = false, mk2 = false;
    float gate = -36.f;
    int block = 128;
    for (int i = 2; i < argc; ++i)
    {
        if (! std::strcmp (argv[i], "--side") && i + 1 < argc)
        {
            // A, B (Serato CV02) or MK2A, MK2B (Traktor Control Vinyl MK2)
            std::string v (argv[++i]);
            for (auto& c : v) c = char (std::toupper (static_cast<unsigned char> (c)));
            mk2   = v.rfind ("MK2", 0) == 0;
            sideB = ! v.empty() && v.back() == 'B';
        }
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
        else if (! std::strcmp (argv[i], "--eval"))                        eval = true;
        else if (! std::strcmp (argv[i], "--eval-ref") && i + 1 < argc)    evalRef = argv[++i];
        else if (! std::strcmp (argv[i], "--eval-sample") && i + 1 < argc) evalSample = argv[++i];
        else if (! std::strcmp (argv[i], "--eval-csv") && i + 1 < argc)    evalCsv = argv[++i];
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

    const vt::LfsrTable table (mk2 ? (sideB ? vt::kTraktorMk2SideB : vt::kTraktorMk2SideA)
                                   : (sideB ? vt::kSeratoSideB : vt::kSeratoSideA));
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
    std::vector<float> spd (static_cast<size_t> (block)), qual (static_cast<size_t> (block));
    // Signal quality while moving: the player turns the level down below 0.5
    size_t movSamples = 0, lowQ = 0, lowRuns = 0, lowRun = 0, lowLongest = 0;

    // Steps in the output: second difference of the tracked position (cycles
    // per sample^2). Smooth motion stays far below kStep even in scratches;
    // a jump of the tracker shows up as a click in the audio.
    constexpr double kStep = 0.005;
    double r1 = 0.0, r2 = 0.0, maxStep = 0.0;
    size_t steps = 0, seen = 0;

    // Position jitter above 100 Hz while playing near 1x (2nd-order high-pass
    // of the position: a steady ramp gives 0), in microseconds of record so
    // that carriers of different frequency compare. Counted after 0.2 s of
    // steady play (the filter settles).
    double hb0, hb1, hb2, ha1, ha2;
    {
        const double w0 = 2.0 * 3.141592653589793 * 100.0 / sr, cw = std::cos (w0), al = std::sin (w0) / std::sqrt (2.0);
        const double a0 = 1.0 + al;
        hb0 = (1.0 + cw) / 2.0 / a0; hb1 = -(1.0 + cw) / a0; hb2 = hb0;
        ha1 = -2.0 * cw / a0;        ha2 = (1.0 - al) / a0;
    }
    double hx1 = 0, hx2 = 0, hy1 = 0, hy2 = 0, jitSum = 0;
    size_t jitN = 0, steady = 0;

    FILE* out = csv ? std::fopen (csv, "w") : nullptr;
    if (out) std::fprintf (out, "time_s,record_s,speed,signal,code_lock,anchored,level_db,quality_min\n");

    size_t sigSamples = 0, lockSamples = 0;
    double firstAnchor = -1.0;
    double lastCsv = -1.0;
    for (size_t i = 0; i < N; i += size_t (block))
    {
        const int n = int (std::min (size_t (block), N - i));
        // Steps are counted once the direction is known (the relative position
        // changes sign when it is detected).
        const bool count = dec.status().anchored;
        dec.process (L.data() + i, R.data() + i, n, pos.data(), rel.data(), spd.data(), qual.data());
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

            if (std::abs (spd[size_t (k)]) > 0.05f && count)
            {
                ++movSamples;
                if (qual[size_t (k)] < 0.5f)
                {
                    ++lowQ;
                    if (++lowRun == size_t (0.002 * sr)) ++lowRuns;
                    lowLongest = std::max (lowLongest, lowRun);
                }
                else
                    lowRun = 0;
            }

            const double hy = hb0 * r0 + hb1 * hx1 + hb2 * hx2 - ha1 * hy1 - ha2 * hy2;
            hx2 = hx1; hx1 = r0; hy2 = hy1; hy1 = hy;
            const double av = std::abs (double (spd[size_t (k)]));
            steady = (count && av > 0.8 && av < 1.25) ? steady + 1 : 0;
            if (double (steady) > 0.2 * sr)
            {
                jitSum += hy * hy;
                ++jitN;
            }
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
            std::fprintf (out, "%.4f,%.5f,%.4f,%d,%d,%d,%.1f,%.3f\n", t, pos[size_t (n - 1)] / table.def().resolution,
                          spd[size_t (n - 1)], int (st.signal), int (st.bitLock), int (st.anchored), st.levelDb,
                          double (*std::min_element (qual.begin(), qual.begin() + n)));
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
                 (st.config < 0 || mk2) ? "-" : ((st.config >> 2) ? "L" : "R"), // MK2: no primary channel
                 st.config < 0 ? "-" : (((st.config >> 1) & 1) ? "neg" : "pos"),
                 st.config < 0 ? "-" : ((st.config & 1) ? "inverted" : "normal"));
    std::printf ("nominal level   : %.1f dBFS\n", st.refDb);
    std::printf ("output steps    : %zu (> %.3f cycles), largest %.4f cycles\n", steps, kStep, maxStep);
    if (movSamples > 0)
        std::printf ("quality < 0.5   : %.2f %% of moving time, %zu stretches > 2 ms, longest %.1f ms\n",
                     100.0 * double (lowQ) / double (movSamples), lowRuns, 1000.0 * double (lowLongest) / sr);
    if (jitN > 0)
        std::printf ("jitter > 100 Hz : %.1f us RMS (near 1x, %.1f s)\n",
                     std::sqrt (jitSum / double (jitN)) / table.def().resolution * 1e6, double (jitN) / sr);
    for (const auto& w : windows)
        std::printf ("window %6.2f-%6.2f s: moved %.3f cycles, largest step %.4f\n",
                     w.t0, w.t1, w.hi >= w.lo ? w.hi - w.lo : 0.0, w.step);
    if (firstAnchor < 0)
        std::printf ("\nNo lock: check side A/B, that the input is stereo, and that the level is not clipping.\n");
    if (eval)
    {
        if (! mk2) { std::fprintf (stderr, "error: --eval is for Traktor MK2 recordings (--side MK2A / MK2B)\n"); return 1; }
        return runEval (L, R, sr, table, gate, block, haveNominal, nominal, in, evalRef, evalSample, evalCsv);
    }
    return 0;
}
