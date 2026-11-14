// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

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
constexpr double kCoastMinSpeed  = 0.5;   // below this a lost carrier means "stopped"
constexpr double kGateSeconds    = 0.0005; // averaging of the magnitude for closing the gate
constexpr double kGateRmsSeconds = 0.005;  // RMS window for opening the gate
constexpr double kRefLearnSeconds = 2.0;  // playback near 1x needed before the level is "learned"
constexpr double kMaxSpeed       = 16.0;  // physical limit of a hand-driven record
constexpr int    kAcqSamples     = 24;    // ~0.5 ms of angle steps for frequency acquisition
constexpr double kAcqMinStep     = 0.01;  // turns/sample (~0.25x) - smaller offsets the loop pulls in itself
constexpr double kLockLost       = 0.15;  // mean |residual| (turns) above which the loop follows noise
constexpr double kLiftMinSpeed   = 0.3;   // carrier lost above this speed -> needle lifted
constexpr double kWeightMaxSpeed = 0.1;     // magnitude weighting only below this (10 ms average speed)
constexpr double kReversalWeightSpeed = 0.2; // ... and at the turn of a reversal (instantaneous speed)
constexpr double kSecondsPerRev  = 1.8;   // one revolution at 33 1/3 rpm (a groove skip, ignored in relative mode)
#ifndef VT_ROUGH_LOW
#define VT_ROUGH_LOW 0.05
#endif
#ifndef VT_ROUGH_HIGH
#define VT_ROUGH_HIGH 0.10
#endif
constexpr double kRoughLow       = VT_ROUGH_LOW;  // mean |residual| (turns): quality 1 up to here ...
constexpr double kRoughHigh      = VT_ROUGH_HIGH; // ... falling to 0 here (clean carrier ~0.005)

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
    kGateMag_ = onePole (sr_, kGateSeconds);
    kGatePow_ = onePole (sr_, kGateRmsSeconds);
    kLockErr_ = onePole (sr_, 0.002);
    kSlowSpeed_ = onePole (sr_, 0.010);
    refLearnNeeded_ = int (kRefLearnSeconds * sr_);
    vMax_ = kMaxSpeed * 2.0 * 1000.0 / sr_; // doubled angle: 2 turns per carrier cycle (re-set in setTable)
    kNfFall_ = onePole (sr_, 0.030);
    nfRise_  = std::pow (10.0, 6.0 / 20.0 / sr_); // +6 dB per second
    kCal_  = onePole (sr_, 0.5);
    kRef_  = onePole (sr_, 1.0);
    kEnv_  = onePole (sr_, 0.003);
    kCentre_ = onePole (sr_, 0.005);
    kPeak_ = float (std::exp (-1.0 / (0.3 * sr_)));
    coast_ = std::exp (-1.0 / (0.010 * sr_));
    lastLoopSpeed_ = -1.0;
    updateLoopGains (0.0);
    reset();
}

void TimecodeDecoder::setTable (const LfsrTable* table)
{
    table_ = table;
    if (table_ != nullptr)
        vMax_ = kMaxSpeed * 2.0 * table_->def().resolution / sr_;
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
    cenL_ = cenR_ = 0.0;
    shortPow_ = 0.0; noiseFloor_ = 1e-7; gateMag_ = gatePow_ = 0.0;
    X_ = V_ = A_ = lockErr_ = relShift_ = relCorr_ = relCorrTarget_ = 0.0;
    slowSpeed_ = 0.0;
    acqN_ = 0;
    acqSum_ = acqSq_ = 0.0;
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

void TimecodeDecoder::setNominalDb (float db) noexcept
{
    if (db > -80.f && db < 6.f)
    {
        refAmp_ = std::pow (10.0, double (db) / 20.0);
        refLearnSamples_ = refLearnNeeded_; // a stored measurement counts as learned
    }
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
                               double* posAbs, double* posRel, float* speed, float* quality,
                               float* lockError, float* centreOffset) noexcept
{
    const double speedScale = (table_ != nullptr) ? sr_ * 0.5 / table_->def().resolution
                                                  : sr_ * 0.5 / 1000.0;
    for (int i = 0; i < numSamples; ++i)
    {
        processSample (left[i], right[i]);

        const double rel = double (dirSign_) * X_ * 0.5 + relShift_;
        if (posRel != nullptr) posRel[i] = rel + relCorr_;
        if (posAbs != nullptr) posAbs[i] = anchored_ ? rel + offset_ : rel;
        if (speed  != nullptr) speed[i]  = float (double (dirSign_) * V_ * speedScale);
        // Signal quality: while the input is not a clean carrier (stylus
        // skating / skipping in hard scratches) the loop follows noise and the
        // position shakes back and forth within a millisecond. The true motion
        // is unknown there; the player turns the level down instead.
        if (quality != nullptr)
            quality[i] = float (1.0 - std::clamp ((lockErr_ - kRoughLow) / (kRoughHigh - kRoughLow), 0.0, 1.0));
        // diagnostics
        if (lockError != nullptr)
            lockError[i] = float (lockErr_);
        if (centreOffset != nullptr)
            centreOffset[i] = gateOpen_ ? float (std::sqrt (cenL_ * cenL_ + cenR_ * cenR_) / std::max (env_, 1e-12))
                                        : -1.f;
    }

    const double rel = double (dirSign_) * X_ * 0.5 + relShift_;
    status_.signal    = gateOpen_;
    status_.anchored  = anchored_;
    status_.absCycles = anchored_ ? rel + offset_ : rel;
    status_.speed     = double (dirSign_) * V_ * speedScale;
    status_.levelDb   = 20.f * std::log10 (std::max (peak_, 1e-6f));
    status_.refDb     = float (20.0 * std::log10 (std::max (refAmp_, 1e-6)));
    status_.gateDb    = float (20.0 * std::log10 (std::max (std::max (refAmp_ * gateLin_, noiseFloor_ * kNfMargin), 1e-6)));
    status_.refLearned = refLearnSamples_ >= refLearnNeeded_;
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
    // Open on the RMS over a few ms, not on the magnitude: noise / vibration
    // peaks of a stopped record must not open the gate. Close on a short
    // magnitude average (holding it open longer let vibration bursts and
    // noise drive the loop in the synthetic tests).
    gateMag_ += kGateMag_ * (rawMag - gateMag_);
    gatePow_ += kGatePow_ * (rawMag * rawMag - gatePow_);
    gateOpen_ = gateOpen_ ? (gateMag_ > closeLevel) : (std::sqrt (gatePow_) > openLevel);

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

    // Centre of the L/R path, for the diagnostics only (the tracker does not
    // use it): a ~30 Hz low-pass of the calibrated pair. A stylus pushed
    // sideways or shaken moves the whole path off the origin at a few to a
    // few tens of Hz; a carrier above ~0.3x is well above the cut-off.
    cenL_ += kCentre_ * (l - cenL_);
    cenR_ += kCentre_ * (r - cenR_);

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
        // Magnitude weighting, only while the record is (nearly) stopped. A
        // knock on the desk swings the stylus through the origin on a thin
        // ellipse, always the same way round; its angle turns mostly where
        // the magnitude is small, so unweighted it reads as slow rotation and
        // the position creeps a cycle per swing ("ratchet"). Weighting while
        // moving made the loop lose cycles at every pull-back, so it stops
        // above kWeightMaxSpeed (10 ms average speed - quick scratch reversals
        // stay above it).
        // (While moving, the angle still swings when a hard push drives the
        // path close to the origin; the resulting position steps are removed
        // by the acceleration limit in ScratchPlayer, not here.)
        // ... and at the turning point of a reversal (instantaneous speed below
        // kReversalWeightSpeed): the cartridge output collapses there, the path
        // passes the origin and the angle swings (a 0.4-cycle jump and a click
        // in the output). Limited to the instant of the turn, the loop does not
        // lose cycles on pull-backs.
        if (slowSpeed_ < kWeightMaxSpeed || std::abs (speedNow_) < kReversalWeightSpeed)
        {
            const double q = std::min (mag / std::max (env_, 1e-12), 1.0);
            resid *= q * q;
        }
        // Lock detector: on a real carrier the residual stays small; on noise
        // it is large and random. Without this the loop, whose bandwidth grows
        // with speed, can run away on noise that keeps the gate open.
        lockErr_ += kLockErr_ * (std::abs (resid) - lockErr_);
        X_ = Xp + loopA_ * resid;
        V_ = Vp + loopB_ * resid;
        A_ += loopC_ * resid;
        if (lockErr_ > kLockLost)
        {
            V_ *= coast_; // following noise: let the speed die away
            A_ = 0.0;
        }

        // Frequency acquisition right after the gate opens (needle drop, record
        // starting under the needle): the loop alone cannot pull in from rest
        // to a fast carrier and may settle on a false lock near zero speed.
        // If the angle advanced steadily over the first samples, start the
        // loop at that frequency. Noise (random steps) is left to the loop.
        if (acqN_ < kAcqSamples)
        {
            if (acqN_ > 0)
            {
                double d = meas - acqLast_;
                d -= std::floor (d + 0.5);
                acqSum_ += d;
                acqSq_  += d * d;
            }
            acqLast_ = meas;
            if (++acqN_ == kAcqSamples)
            {
                const double n    = double (kAcqSamples - 1);
                const double mean = acqSum_ / n;
                const double sd   = std::sqrt (std::max (acqSq_ / n - mean * mean, 0.0));
                if (sd < 0.25 * std::abs (mean) && std::abs (mean - V_) > kAcqMinStep)
                {
                    V_ = mean;
                    A_ = 0.0;
                }
            }
        }
    }
    else if (std::abs (speedNow_) < kCoastMinSpeed)
    {
        // No usable carrier at low speed: the record has stopped. Coasting on
        // here would stretch a short vibration into a long movement.
        V_ = 0.0;
        A_ = 0.0;
    }
    else
    {
        // Short drop-out while the record is moving: keep going for a moment.
        X_ = X_ + V_;
        V_ *= coast_;
        A_ = 0.0;
    }
    if (! gateOpen_)
    {
        acqN_ = 0;
        acqSum_ = acqSq_ = 0.0;
    }

    // A turntable cannot go faster than this; without the limit, noise that
    // keeps the gate open could drive the loop (whose bandwidth grows with
    // speed) to ever higher speeds.
    V_ = std::clamp (V_, -vMax_, vMax_);

    const double speedNow = (table_ != nullptr ? sr_ * 0.5 / table_->def().resolution : sr_ * 0.5 / 1000.0) * V_;
    speedNow_ = speedNow;
    slowSpeed_ += kSlowSpeed_ * (std::abs (speedNow) - slowSpeed_); // a swing's brief spike stays small here
    updateLoopGains (std::abs (speedNow));

    // Needle-up detection and slewing of small anchor corrections
    // The needle counts as lifted only if the carrier vanished while the
    // record was moving. A record held still by the hand (between baby
    // scratches) also loses the carrier, but after slowing down to ~0; the
    // correction that follows is then a tracker slip, not a needle drop.
    if (gateOpen_)
    {
        closedSamples_ = 0;
        lastOpenSpeed_ = std::abs (speedNow);
    }
    else if (++closedSamples_ > int (kStaleSeconds * sr_) && anchored_ && lastOpenSpeed_ > kLiftMinSpeed)
        stale_ = true;

    if (offset_ != offsetTarget_)
    {
        const double step = kSlewPerSec / sr_;
        const double d    = offsetTarget_ - offset_;
        offset_ = (std::abs (d) <= step) ? offsetTarget_ : offset_ + (d > 0 ? step : -step);
    }
    // Tracker slips also go into the relative position (small ones slewed,
    // large ones jump, like the absolute corrections), or they pile up in
    // Relative mode and the cue point wanders when the record is pulled back
    // over and over.
    if (relCorr_ != relCorrTarget_)
    {
        const double step = kSlewPerSec / sr_;
        const double d    = relCorrTarget_ - relCorr_;
        relCorr_ = (std::abs (d) <= step) ? relCorrTarget_ : relCorr_ + (d > 0 ? step : -step);
    }

    // Track the nominal carrier level while playing near normal speed
    if (gateOpen_ && std::abs (speedNow) > 0.8 && std::abs (speedNow) < 1.25)
    {
        refAmp_ += kRef_ * (rawMag - refAmp_);
        if (refLearnSamples_ < refLearnNeeded_) ++refLearnSamples_;
    }
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
                // Keep the relative position continuous: flipping the sign
                // would mirror it (a jump in Relative mode and a loud click).
                relShift_ += double (dirSign_ - newDir) * X_ * 0.5;
                dirSign_  = newDir;
            }
        }

        if (idx != selected_)
            continue;

        // Absolute index of the bit just read
        const int32_t n = t.lookup (h.bits);

        const double rel       = double (dirSign_) * X_ * 0.5 + relShift_;
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
                    // The needle stayed on the record, so this is a tracker
                    // slip, a groove skip (whole revolutions) or both. Slips
                    // come from the stylus skating when the record is caught
                    // and pulled back (5..90 cycles, nearly always the same
                    // sign), so the relative position gets them too, except
                    // whole revolutions: a groove skip does not turn the
                    // record. Needle drops are the stale_ branch above.
                    const double rev  = t.def().resolution * kSecondsPerRev;
                    const double slip = diff - rev * std::round (diff / rev);
                    relCorrTarget_ += slip;
                    if (std::abs (slip) > kJumpCycles)
                        relCorr_ = relCorrTarget_; // large: jump (the player cross-fades it)
                    offsetTarget_ = offsetNew;
                    if (std::abs (diff) > kJumpCycles)
                        offset_ = offsetNew; // large: jump the absolute position
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
