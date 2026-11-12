#include "TimecodeDecoder.h"

#include <algorithm>
#include <cmath>
#ifdef VT_DEBUG
#include <cstdio>
#endif

namespace vt {

namespace {
constexpr double kTwoPi        = 6.283185307179586;
constexpr int    kLockBits     = 24;    // consecutive verified bits for a lock
constexpr float  kBitRefAvg    = 16.f;  // bits averaged for the 0/1 threshold
constexpr double kMinBitSpeed  = 0.01;  // below this, direction is unreliable
constexpr int    kReanchorBits = 3;     // consecutive disagreeing bits before a correction
constexpr double kReanchorTol  = 0.25;  // cycles; tracker slips come in steps of 0.5
constexpr double kJumpCycles   = 4.0;   // larger corrections jump, smaller ones slew
constexpr double kSlewPerSec   = 25.0;  // cycles/s for small corrections (2.5 % pitch)
constexpr double kAnchorMinSpeed = 0.6; // anchor / verify only in the flat RIAA region
constexpr double kAnchorMaxSpeed = 2.0;
constexpr int    kSwitchAfterBits = 4000; // ~4 s of failing bits before re-selecting
constexpr double kCalMinSpeed  = 0.2;   // DC / L-R calibration only above this speed
constexpr double kNfCap        = 0.02;  // noise floor never above -34 dB re nominal
constexpr double kNfMargin     = 2.5;   // auto gate 8 dB above the noise floor
constexpr double kStaleSeconds = 0.03;  // carrier lost this long -> needle may have moved
constexpr double kLoopBaseHz     = 300.0;
constexpr double kLoopPerSpeedHz = 300.0;
constexpr double kLoopMaxHz      = 2500.0;

inline double onePole (double sr, double seconds) { return 1.0 - std::exp (-1.0 / (seconds * sr)); }

// atan2 in turns, [-0.5, 0.5)
inline double atan2Turns (double y, double x) noexcept
{
    return std::atan2 (y, x) * (1.0 / kTwoPi);
}
} // namespace

void TimecodeDecoder::prepare (double sampleRate)
{
    sr_    = sampleRate > 0 ? sampleRate : 48000.0;
    kDc_   = 1.0 - std::exp (-kTwoPi * 2.0 / sr_);
    kDcSlow_ = 1.0 - std::exp (-kTwoPi * 0.3 / sr_);
    kShort_  = onePole (sr_, 0.010);
    kNfFall_ = onePole (sr_, 0.030);
    nfRise_  = std::pow (10.0, 6.0 / 20.0 / sr_); // +6 dB per second
    kCal_  = onePole (sr_, 0.5);
    kRef_  = onePole (sr_, 1.0);
    kEnv_  = onePole (sr_, 0.003);
    kPeak_ = float (std::exp (-1.0 / (0.3 * sr_)));
    coast_ = std::exp (-1.0 / (0.010 * sr_));
    lastLoopSpeed_ = -1.0;
    updateLoopGains (0.0);
    reset();
}

void TimecodeDecoder::setTable (const LfsrTable* table)
{
    table_ = table;
    for (auto& h : hyp_) h = Hypothesis {};
    selected_ = -1;
    selectedBad_ = 0;
    anchored_ = false;
    stale_    = false;
    mismatch_ = 0;
    offset_ = offsetTarget_ = 0.0;
}

void TimecodeDecoder::reset()
{
    dcL_ = dcR_ = 0.0;
    eLL_ = eRR_ = 1e-9; eLR_ = 0.0;
    env_ = 0.0;
    shortPow_ = 0.0; noiseFloor_ = 1e-7;
    X_ = V_ = A_ = 0.0;
    gateOpen_ = false;
    posL_ = posR_ = false;
    for (auto& r : refLevel_) r[0] = r[1] = 1.f;
    setTable (table_);
    status_ = DecoderStatus {};
}

void TimecodeDecoder::setGateDb (float db) noexcept
{
    gateLin_ = std::pow (10.0, double (db) / 20.0);
}

void TimecodeDecoder::setPreferredConfig (int cfg) noexcept
{
    if (cfg >= 0 && cfg < 8)
    {
        preferred_ = cfg;
        if (selected_ < 0)
            dirSign_ = (cfg & 1) ? -1 : 1;
    }
}

void TimecodeDecoder::updateLoopGains (double absSpeed) noexcept
{
    // Loop bandwidth grows with speed: fast scratches need a fast loop, while
    // at low speed a narrow loop rejects noise.
    if (lastLoopSpeed_ >= 0.0 && std::abs (absSpeed - lastLoopSpeed_) < 0.05)
        return;
    lastLoopSpeed_ = absSpeed;

    // Type-3 loop (position, velocity, acceleration): tracks constant
    // acceleration without error, which matters around direction changes
    // where scratches accelerate hardest. Poles at -w (critically damped).
    const double fn = std::min (kLoopBaseHz + kLoopPerSpeedHz * absSpeed, kLoopMaxHz);
    const double w  = kTwoPi * fn / sr_;
    loopA_ = 3.0 * w;
    loopB_ = 3.0 * w * w;
    loopC_ = w * w * w;
}

void TimecodeDecoder::process (const float* left, const float* right, int numSamples,
                               double* posAbs, double* posRel, float* speed) noexcept
{
    const double speedScale = (table_ != nullptr) ? sr_ * 0.5 / table_->def().resolution
                                                  : sr_ * 0.5 / 1000.0;
    for (int i = 0; i < numSamples; ++i)
    {
        processSample (left[i], right[i]);

        const double rel = double (dirSign_) * X_ * 0.5;
        if (posRel != nullptr) posRel[i] = rel;
        if (posAbs != nullptr) posAbs[i] = anchored_ ? rel + offset_ : rel;
        if (speed  != nullptr) speed[i]  = float (double (dirSign_) * V_ * speedScale);
    }

    const double rel = double (dirSign_) * X_ * 0.5;
    status_.signal    = gateOpen_;
    status_.anchored  = anchored_;
    status_.absCycles = anchored_ ? rel + offset_ : rel;
    status_.speed     = double (dirSign_) * V_ * speedScale;
    status_.levelDb   = 20.f * std::log10 (std::max (peak_, 1e-6f));
    status_.refDb     = float (20.0 * std::log10 (std::max (refAmp_, 1e-6)));
    status_.config    = selected_;
    status_.reanchors = reanchors_;
    status_.validBits = selected_ >= 0 ? hyp_[selected_].valid : 0;
    status_.bitLock   = status_.validBits >= kLockBits;
}

void TimecodeDecoder::processSample (float xl, float xr) noexcept
{
    // 1. DC removal. A plain high-pass would shift the phase of a very slow
    //    carrier by up to 90 degrees with the sign of the direction (a
    //    half-cycle jump at every reversal), so the cut-off is very low
    //    (0.3 Hz) and only raised to 2 Hz while the carrier is fast.
    const double kdc = (gateOpen_ && std::abs (speedNow_) > kCalMinSpeed) ? kDc_ : kDcSlow_;
    dcL_ += kdc * (double (xl) - dcL_);
    dcR_ += kdc * (double (xr) - dcR_);
    const double l0 = double (xl) - dcL_;
    const double r0 = double (xr) - dcR_;

    const double rawMag = std::sqrt (l0 * l0 + r0 * r0);
    peak_ = std::max (peak_ * kPeak_, float (rawMag));

    // 2. Gate with hysteresis: the larger of the user threshold (relative to
    //    the nominal carrier level) and an automatic one 8 dB above the
    //    measured noise floor (hum, rumble, preamp noise while the record
    //    stands still).
    shortPow_ += kShort_ * (rawMag * rawMag - shortPow_);
    const double rms = std::sqrt (shortPow_);
    if (rms < noiseFloor_) noiseFloor_ += kNfFall_ * (rms - noiseFloor_);
    else                   noiseFloor_ *= nfRise_;
    noiseFloor_ = std::clamp (noiseFloor_, 1e-7, refAmp_ * kNfCap);
    const double openLevel  = std::max (refAmp_ * gateLin_, noiseFloor_ * kNfMargin);
    const double closeLevel = openLevel * 0.7;
    gateOpen_ = gateOpen_ ? (rawMag > closeLevel) : (rawMag > openLevel);

    // 3. L/R calibration statistics. Only updated while the carrier is fast
    //    enough for the averaging window to span many cycles; slow partial
    //    cycles would bias the estimate.
    if (gateOpen_ && std::abs (speedNow_) > kCalMinSpeed)
    {
        eLL_ += kCal_ * (l0 * l0 - eLL_);
        eRR_ += kCal_ * (r0 * r0 - eRR_);
        eLR_ += kCal_ * (l0 * r0 - eLR_);
    }

    const double gainR  = std::sqrt (eLL_ / std::max (eRR_, 1e-12));
    double rho          = eLR_ / std::sqrt (std::max (eLL_ * eRR_, 1e-24));
    rho                 = std::clamp (rho, -0.6, 0.6);
    const double invSk  = 1.0 / std::sqrt (1.0 - rho * rho);

    const double l = l0;
    const double r = (r0 * gainR - rho * l0) * invSk;
    const double mag = std::sqrt (l * l + r * r);
    env_ += kEnv_ * (mag - env_);

    status_.scopeL = float (l / std::max (refAmp_, 1e-6));
    status_.scopeR = float (r / std::max (refAmp_, 1e-6));

    // 4. Phase tracker on the doubled angle
    const double Xp = X_ + V_ + 0.5 * A_;
    const double Vp = V_ + A_;
    if (gateOpen_)
    {
        const double meas  = atan2Turns (2.0 * l * r, r * r - l * l); // 2*theta, turns
        double resid = meas - (Xp - std::floor (Xp));
        resid -= std::floor (resid + 0.5);                           // wrap to [-0.5, 0.5)
        X_ = Xp + loopA_ * resid;
        V_ = Vp + loopB_ * resid;
        A_ += loopC_ * resid;
    }
    else
    {
        // No usable carrier: the record is (nearly) stopped or the needle is up.
        X_ = X_ + V_;
        V_ *= coast_;
        A_ = 0.0;
    }

    const double speedNow = (table_ != nullptr ? sr_ * 0.5 / table_->def().resolution : sr_ * 0.5 / 1000.0) * V_;
    speedNow_ = speedNow;
    updateLoopGains (std::abs (speedNow));

    // Needle-up detection and slewing of small anchor corrections
    if (gateOpen_) closedSamples_ = 0;
    else if (++closedSamples_ > int (kStaleSeconds * sr_) && anchored_) stale_ = true;

    if (offset_ != offsetTarget_)
    {
        const double step = kSlewPerSec / sr_;
        const double d    = offsetTarget_ - offset_;
        offset_ = (std::abs (d) <= step) ? offsetTarget_ : offset_ + (d > 0 ? step : -step);
    }

    // Track the nominal carrier level while playing near normal speed
    if (gateOpen_ && std::abs (speedNow) > 0.8 && std::abs (speedNow) < 1.25)
        refAmp_ += kRef_ * (rawMag - refAmp_);
    else if (rawMag > refAmp_ * 2.0) // much louder input than expected: adapt quickly
        refAmp_ += kRef_ * 10.0 * (rawMag - refAmp_);

    // 5. Zero crossings of the calibrated channels -> bit reads
    if (gateOpen_ && table_ != nullptr && std::abs (speedNow) > kMinBitSpeed && env_ > 0.0)
    {
        const double h = 0.1 * env_;
        if (posL_ ? (l < -h) : (l > h))
        {
            posL_ = ! posL_;
            onCrossing (0, float (r)); // L crossed -> R is primary
        }
        if (posR_ ? (r < -h) : (r > h))
        {
            posR_ = ! posR_;
            onCrossing (1, float (l)); // R crossed -> L is primary
        }
    }
    else
    {
        posL_ = l > 0.0;
        posR_ = r > 0.0;
    }
}

void TimecodeDecoder::onCrossing (int primary, float primaryValue) noexcept
{
    const int   polarity = primaryValue >= 0.f ? 0 : 1;
    const float m        = std::abs (primaryValue) / float (std::max (env_, 1e-12));

    float& ref = refLevel_[primary][polarity];
    const uint32_t b = m > ref ? 1u : 0u;
    ref += (m - ref) / kBitRefAvg;

    const LfsrTable& t = *table_;
    const int nbits    = t.def().bits;
    const int trackDir = V_ >= 0.0 ? 1 : -1;

    for (int d = 0; d < 2; ++d)
    {
        const int idx = hypIndex (primary, polarity, d);
        Hypothesis& h = hyp_[idx];
        const int dir = trackDir * (d == 0 ? 1 : -1); // +1 = forward on the record

        // Keep the bitstream consistent across direction changes: the next bit
        // read after a reversal is the same physical bit as the last one.
        bool flipped = false;
        if (h.valid > 0 && h.lastDir != 0 && dir != h.lastDir)
        {
            uint32_t st = h.bits;
            if (dir < 0) { for (int k = 0; k < nbits; ++k) st = t.forward (st); }
            else         { for (int k = 0; k < nbits; ++k) st = t.reverse (st); }
            h.bits = h.expected = st;
            flipped = true;
        }

        if (dir > 0)
        {
            h.bits     = (h.bits >> 1) | (b << (nbits - 1));
            h.expected = t.forward (h.expected);
        }
        else
        {
            h.bits     = ((h.bits << 1) & t.mask()) | b;
            h.expected = t.reverse (h.expected);
        }
        h.lastDir = dir;

        if (h.bits == h.expected && t.lookup (h.bits) >= 0)
        {
            // A re-read right after a reversal confirms consistency but is not
            // new evidence (jitter over one crossing must not build a lock).
            if (! flipped) ++h.valid;
        }
        else
        {
            h.expected = h.bits;
            h.valid    = 0;
        }

        if (idx == selected_)
            selectedBad_ = (h.valid >= kLockBits) ? 0 : selectedBad_ + 1;

        // Selection and anchoring use forward motion only. A magnetic pickup
        // inverts the signal when the record runs backwards at higher speeds
        // but not in the mid range (RIAA integrates there), so in reverse the
        // bit is read half a cycle away depending on speed. Forward readings
        // are unambiguous; slips are corrected on the next forward stroke.
        if (h.valid < kLockBits || dir < 0)
            continue;

        // Hypothesis selection: the first one to lock wins. Several
        // hypotheses can be valid at once (e.g. both peak polarities carry the
        // bit, half a cycle apart), so only switch when the selected one has
        // been failing for a long time (wrong first guess, cables swapped).
        if (selected_ < 0 || (selectedBad_ > kSwitchAfterBits && h.valid >= 2 * kLockBits))
        {
            selectedBad_ = 0;
            if (selected_ != idx)
            {
                const int newDir = (d == 0) ? 1 : -1;
                if (selected_ < 0 || newDir != dirSign_)
                    anchored_ = false;
                selected_ = idx;
                dirSign_  = newDir;
            }
        }

        if (idx != selected_)
            continue;

        // Absolute index of the bit just read
        const int32_t n = t.lookup (h.bits);

        const double rel       = double (dirSign_) * X_ * 0.5;
        const double offsetNew = double (n) - rel;
        const double diff      = offsetNew - offsetTarget_;
        const double absSpeed  = std::abs (speedNow_);

        // Only use readings in the speed range where the pickup + RIAA phase
        // response is at its nominal value (flat part of the RIAA curve). In
        // the 50-500 Hz region the preamp integrates, which moves the signal
        // by a quarter cycle and the bit reading to a neighbouring half cycle.
        if (absSpeed < kAnchorMinSpeed || absSpeed > kAnchorMaxSpeed)
            continue;

        if (! anchored_)
        {
            offset_ = offsetTarget_ = offsetNew;
            anchored_ = true;
            mismatch_ = 0;
            stale_    = false;
        }
        else if (stale_ && std::abs (diff) > kReanchorTol)
        {
            // first lock after the needle was lifted: jump straight away
            offset_ = offsetTarget_ = offsetNew;
            mismatch_ = 0;
            stale_    = false;
            ++reanchors_;
        }
        else
        {
            stale_ = false;
            if (std::abs (diff) > kReanchorTol)
            {
                if (++mismatch_ >= kReanchorBits)
                {
#ifdef VT_DEBUG
                    std::fprintf (stderr, "reanchor diff=%+.3f V=%+.4f dir=%d valid=%d\n", diff, V_, dir, h.valid);
#endif
                    offsetTarget_ = offsetNew;
                    if (std::abs (diff) > kJumpCycles)
                        offset_ = offsetNew; // needle drop: jump
                    mismatch_ = 0;
                    ++reanchors_;
                }
            }
            else
            {
                mismatch_ = 0;
            }
        }
    }
}

} // namespace vt
