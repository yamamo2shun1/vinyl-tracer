#include "Synth.h"

#include <cmath>
#include <complex>

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
} // namespace

void synthesize (const LfsrTable& table, const std::vector<double>& theta,
                 const SynthOptions& opt, std::vector<float>& left, std::vector<float>& right)
{
    const auto& def = table.def();

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

} // namespace vt::test
