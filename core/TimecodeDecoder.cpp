// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

#include "TimecodeDecoder.h"

#include <algorithm>
#include <cmath>
#if defined (VT_DEBUG) || defined (VT_DEBUG_MK)
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
// (these three: cycles of a 1000 Hz carrier, scaled by unit_ for others)
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
constexpr int    kFastBits       = 40;    // bits compared by the fast correction
constexpr int    kFastMaxErrors  = 3;     // ... of which this many may be misread
constexpr int    kFastRange      = 256;   // cycles searched either side of the predicted position
constexpr int    kFastWideRange  = 2048;  // ... after the input was not a carrier (skating, skipping)
constexpr double kFastMaxSpeed   = 6.0;   // bits are read up to this speed (whole cycles only above 2x)
constexpr double kFastMinSpeed   = 0.3;   // the bits must be read in one run in one direction above this
constexpr int    kFastConfirm    = 3;     // matches giving the same correction before it is applied

// Traktor MK2 (offset encoding)
constexpr double kPi             = 3.141592653589793;
constexpr double kMkCutRatio     = 0.45;   // offset filter bandwidth relative to the carrier frequency (0.35-0.5 work; wider loses the carrier at start-up)
constexpr double kMkCutMinHz     = 20.0;
constexpr double kMkLHpRatio     = 0.15;   // high-pass of S for the tracker, relative to the carrier (~12 deg lead) ...
constexpr double kMkLHpMinHz     = 5.0;
constexpr double kMkLHpMaxHz     = 400.0;
constexpr double kMkRoughScale   = 2.0;    // quality thresholds x this: the residual has a higher floor (offset left in M)
constexpr double kMkPolSeconds   = 0.001;  // carrier polarity smoothing
constexpr double kRiaaT1 = 3180e-6, kRiaaT2 = 318e-6, kRiaaT3 = 75e-6; // playback curve time constants
constexpr double kMkHpPerSpeedHz = 8.0;    // pulse high-pass (removes the slow drift of the offset)
constexpr double kMkHpMinHz      = 2.0;
constexpr double kMkPulseThr     = 0.6;    // pulse threshold relative to the learned pulse level (ringing lobes reach ~0.45)
constexpr double kMkPulseLearn   = 0.01;
constexpr double kMkPulseInit    = 15.0;   // measured (14.7-18.7 from 2x to 0.4x)
constexpr double kMkPulseSpan    = 1.0;    // cycles: a pulse peak is final this far behind
constexpr double kMkMinBitSpeed  = 0.1;    // bits are read above this
constexpr double kMkRevSpeed     = 0.02;   // tracker direction changes counted above this
constexpr double kMkGapTol       = 0.3;    // bits: gaps between transitions must be this close to whole bits
constexpr int    kMkMaxRunBits   = 40;     // longer gaps: a transition was missed
constexpr double kMkAcqMinSpeed  = 0.1;    // start-up help for carriers above this ...
constexpr double kMkAcqSeconds   = 0.01;   // ... when the tracker stayed this long far below it
constexpr double kMkQuickMinSpeed = 0.15;  // quick start for carriers above this ...
constexpr double kMkQuickSeconds = 0.001;  // ... after the tracker stayed this long far below them
constexpr double kMkRestoreSeconds = 0.02; // after a click: restore the speed the tracker had this recently
constexpr double kMkBpQ          = 2.0;    // band-pass pair on S for the tracker assist
constexpr double kMkCorrGate     = 0.9;    // blend only while the S and M measurements agree this well
constexpr double kMkDNowMax      = 0.02;   // ... and their mean phase difference stays this small (turns): a persistent offset passes the cosine gate (0.06 turns -> cos 0.93) yet pulls the tracker
constexpr double kMkCandTol      = 1.0;    // cycles: a reading claiming a different slip than the pending correction candidate restarts its confirmation count
constexpr double kMkAnchorMinSpeed = 0.3;  // the offset is read cleanly well below the Serato range
constexpr double kMkAnchorMaxSpeed = 2.5;

inline int popcount64 (uint64_t v) noexcept
{
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return int ((v * 0x0101010101010101ull) >> 56);
}
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
    kMkPol_ = onePole (sr_, kMkPolSeconds);
    kMkQ_   = onePole (sr_, 0.001);
    kMkDelta_ = onePole (sr_, 0.05);
    kMkCorr_  = 1.0 / 24.0; // agreement of the two measurements over 24 samples
    {
        // 2nd-order Butterworth high-pass, 120 Hz (zero crossings of S)
        const double w0 = kTwoPi * 120.0 / sr_, cw = std::cos (w0), al = std::sin (w0) / std::sqrt (2.0);
        const double a0 = 1.0 + al;
        mkHpB0_ = (1.0 + cw) / 2.0 / a0;
        mkHpB1_ = -(1.0 + cw) / a0;
        mkHpA1_ = -2.0 * cw / a0;
        mkHpA2_ = (1.0 - al) / a0;
    }
    {
        // (1 + s T1) / (1 + s T2), bilinear
        const double K  = 2.0 * sr_;
        const double a0 = 1.0 + K * kRiaaT2;
        mkShB0_ = (1.0 + K * kRiaaT1) / a0;
        mkShB1_ = (1.0 - K * kRiaaT1) / a0;
        mkShA1_ = (1.0 - K * kRiaaT2) / a0;
    }
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
    fastN_ = fastCount_ = fastAgree_ = 0;
    fastSynced_ = fastLost_ = false;
    mk2_ = table_ != nullptr && table_->def().encoding == TimecodeDef::Encoding::Offset;
    // The correction thresholds below are in cycles of a 1000 Hz carrier
    // (= ms of record); other carriers keep the same times.
    unit_ = table_ != nullptr ? table_->def().resolution / 1000.0 : 1.0;
    mkCandOn_ = mkLastOn_ = mkReversed_ = false;
    mkVSign_ = 0;
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
    mkC_ = mkD_ = 0.0;
    mkZcPos_ = false;
    mkZcCount_ = 0;
    mkZcFreq_ = 0.0;
    mkSPow_ = 0.0;
    mkHx1_ = mkHx2_ = mkHy1_ = mkHy2_ = mkLx1_ = mkLx2_ = mkLy1_ = mkLy2_ = 0.0;
    mkQ_ = mkLPrev_ = 0.0;
    mkStuck_ = 0;
    mkBehind_ = false;
    mkGoodV_ = mkGoodX_ = 0.0;
    mkGoodAge_ = 1 << 30;
    mkZcSteady_ = 0;
    mkHalfCount_ = mkHalf1_ = mkHalf2_ = mkQuick_ = 0;
    mkHalfFreq_ = 0.0;
    mkPol_ = 1.0;
    mkShX_ = mkShY_ = mkZPrev_ = mkHpY_ = mkV_ = 0.0;
    mkBpX1_ = mkBpX2_ = mkBpY1_ = mkBpY2_ = mkApX1_ = mkApY1_ = 0.0;
    mkBpF0_ = 0.0;
    mkBpI_ = mkBpQ_ = 0.0;
    mkDelta_ = 0.0;
    mkDeltaOk_ = false;
    mkCorr_ = 0.0;
    mkDMean_ = 0.0;
    mkBitsSinceBreak_ = 0;
    mkAnchorPg_ = 0;
    mismatchRef_ = 0.0;
    mkPulseLevel_ = kMkPulseInit;
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
        {
            // MK2: the offset left in M raises the residual while a hand moves
            // the record (0.05-0.15 with the tracking right to < 1 ms); at the
            // Serato thresholds the level went down at most reversals and
            // pull-backs for up to ~30 ms (heard as drop-outs).
            const double sc = mk2_ ? kMkRoughScale : 1.0;
            quality[i] = float (1.0 - std::clamp ((lockErr_ - kRoughLow * sc) / ((kRoughHigh - kRoughLow) * sc), 0.0, 1.0));
        }
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
    double l0 = double (xl) - dcL_;
    double r0 = double (xr) - dcR_;
    if (mk2_)
        mk2FrontEnd (l0, r0); // -> L - R and the offset-free quadrature component

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


    // MK2: start the tracker on a record that is already turning (see mk2Acquire)
    if (mk2_)
        mk2Acquire();

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
        double weight = 1.0;
        if (slowSpeed_ < kWeightMaxSpeed || std::abs (speedNow_) < kReversalWeightSpeed)
        {
            const double q = std::min (mag / std::max (env_, 1e-12), 1.0);
            weight = q * q;
        }
        const double residM = resid * weight;
        // Lock detector: on a real carrier the residual stays small; on noise
        // it is large and random. Without this the loop, whose bandwidth grows
        // with speed, can run away on noise that keeps the gate open.
        lockErr_ += kLockErr_ * (std::abs (residM) - lockErr_);
        // MK2: the phase update may blend in the S-derived measurement
        // (mk2Assist); lockErr_ above always uses the M-based residual.
        const double residUpd = mk2_ ? mk2Assist (meas, Xp, resid, weight, residM) : residM;
        X_ = Xp + loopA_ * residUpd;
        V_ = Vp + loopB_ * residUpd;
        A_ += loopC_ * residUpd;
        if (lockErr_ > kLockLost)
        {
            V_ *= coast_; // following noise: let the speed die away
            A_ = 0.0;
        }
        if (lockErr_ > kRoughHigh)
        {
            // Not a carrier (skating, skipping): the tracker may have lost any
            // amount, so search wide, and only with bits read after this.
            fastLost_ = true;
            fastN_ = fastCount_ = fastAgree_ = 0;
            fastSynced_ = false;
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
        const double step = kSlewPerSec * unit_ / sr_;
        const double d    = offsetTarget_ - offset_;
        offset_ = (std::abs (d) <= step) ? offsetTarget_ : offset_ + (d > 0 ? step : -step);
    }
    // Tracker slips also go into the relative position (small ones slewed,
    // large ones jump, like the absolute corrections), or they pile up in
    // Relative mode and the cue point wanders when the record is pulled back
    // over and over.
    if (relCorr_ != relCorrTarget_)
    {
        const double step = kSlewPerSec * unit_ / sr_;
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
    if (mk2_)
        mk2Pulses();
    else if (gateOpen_ && table_ != nullptr && std::abs (speedNow) > kMinBitSpeed && env_ > 0.0)
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

    if (selected_ >= 0 && primary == selected_ / 4 && polarity == ((selected_ >> 1) & 1))
        fastRead (std::abs (primaryValue), trackDir * ((selected_ & 1) == 0 ? 1 : -1),
                  double (dirSign_) * X_ * 0.5 + relShift_, std::abs (speedNow_));

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
        const double absSpeed  = std::abs (speedNow_);

        // Only use readings in the speed range where the pickup + RIAA phase
        // response is at its nominal value (flat part of the RIAA curve). In
        // the 50-500 Hz region the preamp integrates, which moves the signal
        // by a quarter cycle and the bit reading to a neighbouring half cycle.
        if (absSpeed < kAnchorMinSpeed || absSpeed > kAnchorMaxSpeed)
            continue;

        anchor (offsetNew, h.valid);
    }
}

// A verified reading of the absolute position (offsetNew = absolute - relative).
void TimecodeDecoder::anchor (double offsetNew, int valid) noexcept
{
    (void) valid;
    const double diff = offsetNew - offsetTarget_;
    if (! anchored_)
    {
        offset_ = offsetTarget_ = offsetNew;
        anchored_ = true;
        mismatch_ = 0;
        stale_    = false;
    }
    else if (stale_ && std::abs (diff) > kReanchorTol * unit_)
    {
        // first lock after the needle was lifted: jump straight away
#ifdef VT_DEBUG_MK
        std::fprintf (stderr, "stale-jump diff=%+.3f\n", diff);
#endif
        offset_ = offsetTarget_ = offsetNew;
        mismatch_ = 0;
        stale_    = false;
        ++reanchors_;
    }
    else
    {
        stale_ = false;
        if (std::abs (diff) > kReanchorTol * unit_)
        {
            // MK2: the confirmation count belongs to one candidate, held
            // to its first reading: a reading that claims a different
            // slip restarts the count instead of completing another
            // candidate's confirmation (a marginal pre-gap reading and a
            // post-gap misalignment otherwise add up to a correction
            // neither sequence confirmed, and a candidate that drifts a
            // step at a time would otherwise walk the reference with
            // it). The count itself restarts at every rejected
            // transition (onTransition), and the completing reading must
            // rest on bits verified after that transition, not on a
            // verification count carried over it.
            if (mk2_ && mismatch_ > 0 && std::abs (diff - mismatchRef_) > kMkCandTol)
                mismatch_ = 0;
            if (mk2_ && mismatch_ == 0)
                mismatchRef_ = diff;
            if (++mismatch_ >= kReanchorBits && (! mk2_ || mkAnchorPg_ >= kReanchorBits))
            {
#if defined (VT_DEBUG) || defined (VT_DEBUG_MK)
                std::fprintf (stderr, "reanchor diff=%+.3f V=%+.4f valid=%d\n", diff, V_, valid);
#endif
                correct (offsetNew, diff);
            }
        }
        else
        {
            mismatch_ = 0;
        }
    }
}

void TimecodeDecoder::correct (double offsetNew, double diff) noexcept
{
    // The needle stayed on the record, so this is a tracker slip, a groove
    // skip (whole revolutions) or both. Slips come from the stylus skating
    // when the record is caught and pulled back (5..90 cycles, nearly always
    // the same sign), so the relative position gets them too, except whole
    // revolutions: a groove skip does not turn the record. Needle drops are
    // the stale_ branch in onCrossing.
    const double rev  = table_->def().resolution * kSecondsPerRev;
    const double slip = diff - rev * std::round (diff / rev);
    relCorrTarget_ += slip;
    if (std::abs (slip) > kJumpCycles * unit_)
        relCorr_ = relCorrTarget_; // large: jump (the player cross-fades it)
    offsetTarget_ = offsetNew;
    if (std::abs (diff) > kJumpCycles * unit_)
        offset_ = offsetNew; // large: jump the absolute position
    mismatch_ = 0;
    fastAgree_ = 0;
    ++reanchors_;
}

// Fast correction of slips while the position is known (anchored).
//
// After the record is caught and pulled back, the LFSR check needs about 45
// consecutive correctly read bits (20 to flush a misread bit out of the
// register, 24 to verify). While the hand moves the record the bit amplitude
// is misread 10-15 % of the time (the 3 ms envelope lags the level changes),
// so that took 50-250 ms into the forward stroke, and the stroke started from
// the wrong position.
//
// Here each bit is decided against the mean of its neighbours of the same
// peak type 3 cycles either side (3-5 % errors), and the last kFastBits bits
// are looked for within kFastRange cycles of the predicted position, allowing
// kFastMaxErrors misread bits. A match must be unambiguous and give the same
// correction kFastConfirm times in a row. The bits are only used from one
// unbroken run in one direction, so that consecutive peaks are consecutive
// cycles.
//
// The slip happens when the record is caught, so the backward pull is
// searched too and the forward stroke starts from the right place (only whole
// cycles there, see below). The LFSR check above stays as it is: it makes the
// first anchor, finds groove skips (outside kFastRange) and confirms these.
void TimecodeDecoder::fastRead (float amplitude, int dir, double rel, double absSpeed) noexcept
{
    const bool usable = anchored_ && ! stale_ && absSpeed >= kFastMinSpeed;
    if (! usable || (fastN_ > 0 && dir != fastDir_))
    {
        fastN_ = fastCount_ = fastAgree_ = 0;
        fastSynced_ = false;
        if (! usable)
            return;
    }
    fastDir_ = dir;

    for (int k = 6; k > 0; --k)
        fastRing_[k] = fastRing_[k - 1];
    fastRing_[0] = { amplitude, rel, absSpeed };
    if (fastN_ < 7)
    {
        ++fastN_;
        if (fastN_ < 7)
            return;
    }

    const FastPeak& mid = fastRing_[3];
    float sum = 0.f;
    for (int k = 0; k < 7; ++k)
        if (k != 3)
            sum += fastRing_[k].a;
    fastBits_ = (fastBits_ << 1) | (mid.a * 6.f > sum ? 1u : 0u);
    if (fastCount_ < kFastBits)
        ++fastCount_;
    if (fastCount_ < kFastBits || mid.speed > kFastMaxSpeed)
        return;
    // Readings give the exact position only forward in the flat RIAA region
    // (see onCrossing); elsewhere they can be half a cycle off.
    const bool exact = dir > 0 && mid.speed >= kAnchorMinSpeed && mid.speed <= kAnchorMaxSpeed;

    const uint64_t mask = (kFastBits >= 64) ? ~uint64_t (0) : ((uint64_t (1) << kFastBits) - 1u);
    // Forward, bit k of the word is the bit of cycle n - k (n = the newest);
    // backward it is the bit of cycle n + k, so the word is mirrored and
    // compared with the history ending kFastBits - 1 cycles later.
    uint64_t word = fastBits_ & mask;
    int32_t  shift = 0;
    if (dir < 0)
    {
        uint64_t r = 0;
        for (int k = 0; k < kFastBits; ++k)
            r |= ((word >> k) & 1u) << (kFastBits - 1 - k);
        word  = r;
        shift = kFastBits - 1;
    }
    const int32_t pred = int32_t (std::lround (mid.rel + offsetTarget_));

    // In step: only check the predicted position.
    if (fastSynced_ && popcount64 ((word ^ table_->history (pred + shift)) & mask) <= kFastMaxErrors)
        return;
    fastSynced_ = false;

    const int32_t range = fastLost_ ? kFastWideRange : kFastRange;
    int best = kFastBits + 1, second = kFastBits + 1;
    int32_t bestN = 0;
    for (int32_t n = pred - range; n <= pred + range; ++n)
    {
        const int e = popcount64 ((word ^ table_->history (n + shift)) & mask);
        if (e < best) { second = best; best = e; bestN = n; }
        else if (e < second) second = e;
    }
    if (best > kFastMaxErrors || second <= best + 2)
    {
        fastAgree_ = 0;
        return;
    }

    double offsetNew = double (bestN) - mid.rel;
    double diff      = offsetNew - offsetTarget_;
    if (! exact)
    {
        // Only whole cycles here (a half cycle may be the pickup / RIAA
        // phase); the next forward stroke in the flat region settles the rest.
        if (std::abs (diff) <= 0.5 + kReanchorTol)
            diff = 0.0;
        diff = std::round (diff);
        offsetNew = offsetTarget_ + diff;
    }
    if (std::abs (diff) <= kReanchorTol)
    {
        fastAgree_ = 0;
        fastSynced_ = true;
        fastLost_ = false;
        return;
    }
    if (fastAgree_ > 0 && std::abs (diff - fastDiff_) <= kReanchorTol)
        ++fastAgree_;
    else
    {
        fastAgree_ = 1;
        fastDiff_  = diff;
    }
    if (fastAgree_ >= kFastConfirm)
    {
#ifdef VT_DEBUG
        std::fprintf (stderr, "fast diff=%+.3f errors=%d\n", diff, best);
#endif
        correct (offsetNew, diff);
        fastSynced_ = true;
        fastLost_ = false;
    }
}

//==============================================================================
// Traktor MK2
//
// The carrier is a stereo quadrature pair like Serato's, but each bit is a
// common-mode offset (the same on L and R) about as large as the carrier,
// changing level at the bit boundaries every 2.5 cycles. The L / R path is a
// circle whose centre runs up and down the L = R diagonal.
//  * S = (L - R) / 2 has no offset: one clean component of the carrier.
//  * M = (L + R) / 2 is the other component (in quadrature, give or take the
//    L / R calibration) plus the offset. The offset is estimated by a
//    tracking filter on M minus the carrier the tracker predicts for this
//    sample, and M minus that estimate goes to the tracker as the second
//    component. The filter's bandwidth follows the carrier frequency, so the
//    separation is the same at any speed (the offset's spectrum ends at
//    ~0.4 x the carrier).
//  * The offset estimate, with the playback RIAA curve undone, is a pulse at
//    every bit boundary (positive for 0 -> 1 in time). The distance between
//    pulses, measured with the tracked position, gives the number of bits.
void TimecodeDecoder::mk2FrontEnd (double& l0, double& r0) noexcept
{
    const double s = 0.5 * (l0 - r0);
    const double m = 0.5 * (l0 + r0);
    mkS_ = s;
    mkM_ = m;
    // A fixed 120 Hz high-pass of S for its zero crossings (they only matter
    // above 0.15x = 375 Hz) and the carrier amplitude. A hand holding and
    // pushing the record moves the stylus up and down, and L - R then carries
    // a component below ~150 Hz up to 2-3 x the carrier (tear scratches): it
    // shifted the zero crossings, the start-up helps did not see the carrier
    // and the tracker stayed at rest for tens of ms (drop-outs). (The
    // amplitude is not taken from the calibrated pair: that contains the
    // predicted carrier itself and fed back on it.)
    const double sz = mkHpB0_ * s + mkHpB1_ * mkHx1_ + mkHpB0_ * mkHx2_ - mkHpA1_ * mkHy1_ - mkHpA2_ * mkHy2_;
    mkHx2_ = mkHx1_; mkHx1_ = s; mkHy2_ = mkHy1_; mkHy1_ = sz;
    mkSPow_ += kEnv_ * (sz * sz - mkSPow_);

    // Carrier frequency from the zero crossings of S (no offset there): the
    // tracker's own speed is not usable while it is still at rest after the
    // gate opened on a turning record.
    {
        const double h = 0.1 * std::sqrt (2.0 * mkSPow_);
        ++mkZcCount_;
        ++mkHalfCount_;
        if (mkZcPos_ ? (sz < -h) : (sz > h))
        {
            mkZcPos_ = ! mkZcPos_;
            mkHalf2_ = mkHalf1_;
            mkHalf1_ = mkHalfCount_;
            mkHalfCount_ = 0;
            if (mkZcPos_)
            {
                const double f = sr_ / double (mkZcCount_);
                mkZcSteady_ = (mkZcFreq_ > 0.0 && std::abs (f - mkZcFreq_) < 0.1 * mkZcFreq_) ? mkZcSteady_ + 1 : 0;
                mkZcFreq_ = (mkZcFreq_ > 0.0) ? mkZcFreq_ + 0.3 * (f - mkZcFreq_) : f;
                mkZcCount_ = 0;
            }
        }
        if (double (mkZcCount_) > 0.05 * sr_)
        {
            mkZcFreq_ = 0.0;
            mkZcSteady_ = 0;
        }
        // Frequency from the last two half cycles only (follows a record that
        // is speeding up): valid while they agree within 30 % and the next
        // crossing is not overdue.
        const int hMax = std::max (mkHalf1_, mkHalf2_);
        mkHalfFreq_ = (mkHalf1_ > 0 && mkHalf2_ > 0 && std::abs (mkHalf1_ - mkHalf2_) < 0.3 * hMax
                       && mkHalfCount_ < hMax + hMax / 2)
                          ? sr_ / double (mkHalf1_ + mkHalf2_) : 0.0;
    }
    // The tracker is far off a steadily crossing carrier: it has not picked
    // the record up yet, or sits on a false lock (see mk2Acquire). Steady
    // means 8 crossings in a row within 10 %, which a scratch never is.
    {
        const bool fresh = mkZcFreq_ > 0.0 && double (mkZcCount_) < 2.0 * sr_ / mkZcFreq_;
        const double ft  = std::abs (V_) * 0.5 * sr_; // tracker's carrier frequency
        mkBehind_ = fresh && mkZcSteady_ >= 8 && mkZcFreq_ > kMkAcqMinSpeed * table_->def().resolution
                    && std::abs (ft - mkZcFreq_) > 0.3 * mkZcFreq_;
    }

    // Carrier predicted for this sample in the calibrated frame. The tracker
    // follows the doubled angle 0.5 - theta / pi; theta itself is known up to
    // a half turn, settled by comparing with S (which has no offset). Not
    // while the tracker is behind: the prediction stands still then, and the
    // half-turn decision would follow every half cycle of S.
    double mCar = 0.0;
    if (gateOpen_ && ! mkBehind_)
    {
        const double Xp = X_ + V_ + 0.5 * A_;
        const double th = kPi * (0.5 - Xp);
        double cs = std::cos (th), sn = std::sin (th);
        if (std::abs (cs) > 0.5)
            mkPol_ += kMkPol_ * ((s * cs >= 0.0 ? 1.0 : -1.0) - mkPol_);
        if (mkPol_ < 0.0) { cs = -cs; sn = -sn; }
        // back from the calibrated frame (see step 3 of processSample)
        const double gainR = std::sqrt (eLL_ / std::max (eRR_, 1e-12));
        const double rho   = std::clamp (eLR_ / std::sqrt (std::max (eLL_ * eRR_, 1e-24)), -0.6, 0.6);
        const double invSk = 1.0 / std::sqrt (1.0 - rho * rho);
        mCar = (std::sqrt (2.0 * mkSPow_) * sn / invSk + rho * s) / gainR;
    }

    // Offset: a type-2 tracking filter (level and slope, critically damped)
    // on M minus the predicted carrier. Unlike a low-pass it follows the
    // offset's ramps without lag; its bandwidth follows the carrier frequency
    // so that carrier prediction errors stay mostly out of it.
    const double res = table_->def().resolution;
    const double fcar = std::max (std::abs (speedNow_) * res, mkZcFreq_);
    const double fn   = std::max (kMkCutMinHz, kMkCutRatio * fcar);
    const double w    = std::min (kTwoPi * fn / sr_, 0.6);
    const double cp   = mkC_ + mkD_;
    const double e    = (m - mCar) - cp;

    mkC_ = cp + 2.0 * w * e;
    mkD_ += w * w * e;

    // Band-pass pair on S for the tracker assist: a Q = 2 band-pass at the
    // carrier frequency (0 dB, 0 deg at the centre) and a first-order
    // all-pass tuned to -90 deg at the same frequency. Together they give
    // S's phase directly; on a clean constant-speed recording the phase
    // steps of the pair are ~150x steadier than a 2-sample difference of S.
    {
        const double f0 = std::clamp (fcar, 150.0, 4200.0);
        if (mkBpF0_ <= 0.0 || std::abs (f0 - mkBpF0_) > 0.05 * mkBpF0_)
        {
            mkBpF0_ = f0;
            const double w0 = kTwoPi * f0 / sr_, cw = std::cos (w0), al = std::sin (w0) / (2.0 * kMkBpQ);
            const double a0 = 1.0 + al;
            mkBpB0_ = al / a0;
            mkBpB2_ = -al / a0;
            mkBpA1_ = -2.0 * cw / a0;
            mkBpA2_ = (1.0 - al) / a0;
            const double t = std::tan (0.5 * w0);
            mkApA_ = (t - 1.0) / (t + 1.0);
        }
        const double bi = mkBpB0_ * s + mkBpB2_ * mkBpX2_ - mkBpA1_ * mkBpY1_ - mkBpA2_ * mkBpY2_;
        mkBpX2_ = mkBpX1_; mkBpX1_ = s; mkBpY2_ = mkBpY1_; mkBpY1_ = bi;
        mkBpI_ = bi;
        mkBpQ_ = mkApA_ * bi + mkApX1_ - mkApA_ * mkApY1_;
        mkApX1_ = bi; mkApY1_ = mkBpQ_;
    }

    // S for the tracker: a high-pass at a fixed fraction of the carrier
    // frequency (15 %, 5-400 Hz). A hand pushing the record puts a component
    // below ~150 Hz up to 2-3 x the carrier on S; in fast strokes it moved the
    // circle off centre and the tracker lost its way (crackle). Tied to the
    // carrier the phase lead stays the same small amount (~12 deg) at any
    // speed, and near a reversal the cut-off goes down to 5 Hz (a fixed
    // high-pass would shift a slow carrier by up to 90 deg with the sign of
    // the direction).
    {
        const double fh = std::clamp (kMkLHpRatio * fcar, kMkLHpMinHz, kMkLHpMaxHz);
        const double w0 = kTwoPi * fh / sr_, cw = std::cos (w0), al = std::sin (w0) / std::sqrt (2.0);
        const double a0 = 1.0 + al;
        const double b0 = (1.0 + cw) / 2.0 / a0, b1 = -(1.0 + cw) / a0;
        const double a1 = -2.0 * cw / a0, a2 = (1.0 - al) / a0;
        const double y = b0 * s + b1 * mkLx1_ + b0 * mkLx2_ - a1 * mkLy1_ - a2 * mkLy2_;
        mkLx2_ = mkLx1_; mkLx1_ = s; mkLy2_ = mkLy1_; mkLy1_ = y;
        l0 = y;
    }
    r0 = m - mkC_;

    // Pulses: playback RIAA undone ((1 + s T1)(1 + s T3) / (1 + s T2)),
    // then a high-pass that follows the speed.
    const double y = mkShB0_ * mkC_ + mkShB1_ * mkShX_ - mkShA1_ * mkShY_;
    mkShX_ = mkC_;
    const double z = y + kRiaaT3 * sr_ * (y - mkShY_);
    mkShY_ = y;
    const double fhp = std::max (kMkHpMinHz, kMkHpPerSpeedHz * std::abs (speedNow_));
    const double a   = 1.0 / (1.0 + kTwoPi * fhp / sr_);
    mkHpY_ = a * (mkHpY_ + z - mkZPrev_);
    mkZPrev_ = z;
    mkV_ = mkHpY_;
}

// Tracker assist from S (issue #2, step 2 continued, stage 1). The M-based
// measurement degrades while a hand disturbs the offset estimate; S carries
// the same carrier with no offset. The band-pass pair in mk2FrontEnd gives
// S's doubled angle; a slowly learned constant aligns it to the M frame
// (the pair's phase response and the S/M phase relation are constant at a
// given speed). While the two measurements agree (smoothed cosine of their
// difference >= kMkCorrGate over 24 samples), the S measurement is blended
// into the phase update with weight min (1, |speed| / 0.5). Nothing else
// changes: lockErr_ (quality), the bit path and the anchor logic all keep
// the M-based values, and Serato never reaches this code.
double TimecodeDecoder::mk2Assist (double meas, double Xp, double residRaw, double weight, double residM) noexcept
{
    const double amp = std::sqrt (mkBpI_ * mkBpI_ + mkBpQ_ * mkBpQ_);
    const double ref = std::sqrt (2.0 * mkSPow_);
    if (! (ref > 0.0) || amp < 0.3 * ref)
    {
        mkCorr_ += kMkCorr_ * (0.0 - mkCorr_);
        mkDMean_ += kMkCorr_ * (0.0 - mkDMean_);
        return residM;
    }
    const double measS = atan2Turns (2.0 * mkBpI_ * mkBpQ_, mkBpQ_ * mkBpQ_ - mkBpI_ * mkBpI_);
    double dAlign = (meas - measS) - mkDelta_;
    dAlign -= std::floor (dAlign + 0.5);
    if (! mkDeltaOk_)
    {
        mkDelta_  = meas - measS;
        mkDeltaOk_ = true;
    }
    else if (std::abs (residRaw) < 0.05)
    {
        // Learn the alignment only while the M measurement itself tracks
        // well; a disturbed M must not drag the S frame with it.
        mkDelta_ += kMkDelta_ * dAlign;
    }
    double dNow = (meas - measS) - mkDelta_;
    dNow -= std::floor (dNow + 0.5);
    mkCorr_ += kMkCorr_ * (std::cos (kTwoPi * dNow) - mkCorr_);
    mkDMean_ += kMkCorr_ * (dNow - mkDMean_);
    // The cosine gate alone passes a persistent offset (0.06 turns still
    // gives cos 0.93), and the blend then pulls the phase update towards
    // the offset for as long as it lasts. Require the mean difference
    // itself to be small as well. The agreement must be a positive
    // correlation: near the wrap boundary the difference can alternate
    // around +-0.5 turns, which averages to a strong negative correlation
    // with a near-zero mean -- an absolute-value gate would pass it and
    // the equal-weight blend would nearly cancel.
    const bool gatePass = mkCorr_ >= kMkCorrGate && std::abs (mkDMean_) < kMkDNowMax;
    if (! gatePass)
        return residM;
    const double w = std::min (1.0, std::abs (speedNow_) / 0.5);
    if (w <= 0.0)
        return residM;
    const double ms = measS + mkDelta_;
    const double bx = std::cos (kTwoPi * meas) + w * std::cos (kTwoPi * ms);
    const double by = std::sin (kTwoPi * meas) + w * std::sin (kTwoPi * ms);
    double residB = atan2Turns (by, bx) - (Xp - std::floor (Xp));
    residB -= std::floor (residB + 0.5);
    return residB * weight;
}

// The tracker can stay at rest when the record starts under the needle (or
// the gate opens on a turning record): while it does, the offset filter
// removes part of the carrier from M, and S alone does not pull the tracker
// (it cannot tell the two directions apart). If S has been crossing zero at a
// steady rate for a while and the tracker is still far slower, start it at
// that frequency. The direction is the sign of the correlation of M with the
// change of S (M leads or lags S by about a quarter turn; its offset changes
// far slower than S and averages out). The offset filter's output is no use
// here: while the tracker is behind it turns M's carrier towards S.
void TimecodeDecoder::mk2Acquire() noexcept
{
    mkQ_ += kMkQ_ * (mkM_ * (mkS_ - mkLPrev_) - mkQ_);
    mkLPrev_ = mkS_;

    const double vz    = 2.0 * mkZcFreq_ / sr_; // doubled-angle turns per sample
    const double qRef  = 2.0 * mkSPow_ * kPi * mkZcFreq_ / sr_;

    // A click (dust) lands in M and can pull the tracker to a stop within a
    // few ms while S keeps crossing zero as before. If the tracker ran at
    // that rate a moment ago, put it straight back (each click lost ~13 ms
    // of record before).
    const double ft = std::abs (V_) * 0.5 * sr_;
    if (gateOpen_ && ! mkBehind_ && mkZcFreq_ > 0.0 && std::abs (ft - mkZcFreq_) < 0.1 * mkZcFreq_ && lockErr_ < 0.05)
    {
        mkGoodV_   = V_;
        mkGoodX_   = X_;
        mkGoodAge_ = 0;
    }
    else if (mkGoodAge_ < int (sr_))
        ++mkGoodAge_;
    if (gateOpen_ && mkBehind_ && mkGoodAge_ < int (kMkRestoreSeconds * sr_)
        && std::abs (std::abs (mkGoodV_) * 0.5 * sr_ - mkZcFreq_) < 0.2 * mkZcFreq_)
    {
        // ... and the position where that speed has taken the record since
        X_ = mkGoodX_ + mkGoodV_ * double (mkGoodAge_);
        V_ = mkGoodV_;
        A_ = 0.0;
        mkGoodAge_ = int (sr_);
        mkStuck_ = 0;
        return;
    }

    // Quick start: the record moves again after it was held or turned round
    // (tear scratches) and the tracker has not followed. Two half cycles of S
    // that agree are enough here (the start-up help below needs a steady
    // carrier, which an accelerating stroke is not).
    {
        const double fi = mkHalfFreq_;
        const double qi = 2.0 * mkSPow_ * kPi * fi / sr_;
        if (gateOpen_ && fi > kMkQuickMinSpeed * table_->def().resolution && (ft < 0.6 * fi || ft > 1.6 * fi)
            && std::abs (mkQ_) > 0.3 * qi)
        {
            if (++mkQuick_ >= int (kMkQuickSeconds * sr_))
            {
                V_ = (mkQ_ > 0.0 ? 2.0 : -2.0) * fi / sr_;
                A_ = 0.0;
                mkQuick_ = 0;
                mkStuck_ = 0;
                return;
            }
        }
        else
            mkQuick_ = 0;
    }

    if (gateOpen_ && mkBehind_ && std::abs (mkQ_) > 0.3 * qRef)
    {
        if (++mkStuck_ >= int (kMkAcqSeconds * sr_))
        {
            V_ = mkQ_ > 0.0 ? vz : -vz;
            A_ = 0.0;
            mkStuck_ = 0;
        }
    }
    else
        mkStuck_ = 0;
}

void TimecodeDecoder::mk2Pulses() noexcept
{
    const double absSpeed = std::abs (speedNow_);
    if (absSpeed > kMkRevSpeed)
    {
        const int vs = V_ >= 0.0 ? 1 : -1;
        if (mkVSign_ != 0 && vs != mkVSign_)
            mkReversed_ = true;
        mkVSign_ = vs;
    }
    if (! gateOpen_ || table_ == nullptr || absSpeed < kMkMinBitSpeed || env_ <= 0.0)
    {
        mkCandOn_ = mkLastOn_ = false;
        return;
    }

    const double u   = X_ * 0.5; // tracker position, cycles
    const double v   = mkV_;
    const double thr = kMkPulseThr * mkPulseLevel_ * env_ * absSpeed;
    if (std::abs (v) > thr)
    {
        const int sg = v > 0.0 ? 1 : 0;
        if (mkCandOn_ && sg != mkCandSign_)
            mk2Candidate();
        if (! mkCandOn_ || std::abs (v) > mkCandV_)
        {
            mkCandOn_     = true;
            mkCandSign_   = sg;
            mkCandV_      = std::abs (v);
            mkCandU_      = u;
            mkCandRel_    = double (dirSign_) * u + relShift_;
            mkCandSpeed_  = absSpeed;
            mkCandEnv_    = env_;
        }
    }
    if (mkCandOn_ && std::abs (u - mkCandU_) > kMkPulseSpan)
        mk2Candidate();
}

void TimecodeDecoder::mk2Candidate() noexcept
{
    mkCandOn_ = false;
#ifdef VT_DEBUG_MK
    const double level = mkCandV_ / std::max (mkCandEnv_ * mkCandSpeed_, 1e-12);
    std::fprintf (stderr, "pulse sign=%d u=%.3f level=%.3f learned=%.3f speed=%.3f\n",
                  mkCandSign_, mkCandU_, level, mkPulseLevel_, mkCandSpeed_);
#endif
    // The next boundary is at least a bit away: closer pulses are the
    // opposite-sign lobe of the last one (the filters ring a little).
    if (mkLastOn_ && ! mkReversed_ && std::abs (mkCandU_ - mkLastU_) < 0.75 * table_->def().cyclesPerBit)
        return;
    onTransition (mkCandSign_, mkCandU_, mkCandRel_, mkCandSpeed_);
}

void TimecodeDecoder::onTransition (int sign, double u, double rel, double absSpeed) noexcept
{
    const double cpb = table_->def().cyclesPerBit;
    const bool   run = mkLastOn_ && ! mkReversed_ && sign != mkLastSign_;
    const double gap = std::abs (u - mkLastU_) / cpb;
    const int    nb  = int (std::lround (gap));
    const int    trackDir = u >= mkLastU_ ? 1 : -1;
    const uint32_t level  = uint32_t (mkLastSign_);
    mkLastOn_   = true;
    mkLastSign_ = sign;
    mkLastU_    = u;
    mkReversed_ = false;
    if (! run || nb < 1 || nb > kMkMaxRunBits || std::abs (gap - double (nb)) > kMkGapTol)
    {
#ifdef VT_DEBUG_MK
        std::fprintf (stderr, "break run=%d nb=%d gap=%.3f speed=%.3f\n", int (run), nb, gap, absSpeed);
#endif
        // A rejected transition breaks the bit sequence: the verification
        // behind later readings is counted from here (mkBitsSinceBreak_),
        // and the correction candidate's confirmation count restarts too,
        // so a correction must be confirmed by post-break readings alone
        // (bit counts and confirmation counts are different quantities;
        // either one carried over the break lets pre-break evidence
        // complete a post-break correction).
        mkBitsSinceBreak_ = 0;
        mismatch_ = 0;
        return; // not a measured run of bits (start, reversal, missed pulse)
    }

    // Pulse level, learned from pulses that fit the bit grid only (noise read
    // while the tracker is lost, divided by a near-zero speed, would drive it
    // up and the threshold above the real pulses).
    {
        const double pulse = mkCandV_ / std::max (mkCandEnv_ * mkCandSpeed_, 1e-12);
        mkPulseLevel_ += kMkPulseLearn * (std::clamp (pulse, 0.5 * mkPulseLevel_, 2.0 * mkPulseLevel_) - mkPulseLevel_);
        mkPulseLevel_ = std::clamp (mkPulseLevel_, kMkPulseInit / 3.0, kMkPulseInit * 3.0);
    }

    mkBitsSinceBreak_ += nb;

    const LfsrTable& t = *table_;
    const int nbits = t.def().bits;

    // Hypotheses: bit polarity (an inverting preamp / cable) x which tracker
    // direction is forward on the record (L / R order). Bits come in time
    // order; backwards they enter the register from the other end.
    for (int idx = 0; idx < 4; ++idx)
    {
        Hypothesis& h = hyp_[idx];
        const uint32_t b = level ^ uint32_t (idx >> 1);
        const int dir = trackDir * ((idx & 1) == 0 ? 1 : -1); // +1 = forward on the record
        for (int j = 0; j < nb; ++j)
        {
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
            if (h.bits == h.expected && t.lookup (h.bits) >= 0)
                ++h.valid;
            else
            {
#ifdef VT_DEBUG_MK
                if (idx == selected_ && h.valid >= kLockBits)
                    std::fprintf (stderr, "biterr valid=%d nb=%d j=%d speed=%.3f\n", h.valid, nb, j, absSpeed);
#endif
                h.expected = h.bits;
                h.valid    = 0;
            }
        }
        h.lastDir = dir;

        if (idx == selected_)
            selectedBad_ = (h.valid >= kLockBits) ? 0 : selectedBad_ + nb;

        // Unlike Serato's amplitude bits, the offset reads the same in both
        // directions (no half-cycle shift backwards), so backward readings
        // lock and anchor too.
        if (h.valid < kLockBits)
            continue;

        if (selected_ < 0 || (selectedBad_ > kSwitchAfterBits && h.valid >= 2 * kLockBits))
        {
            selectedBad_ = 0;
            if (selected_ != idx)
            {
                const int newDir = (idx & 1) == 0 ? 1 : -1;
                if (selected_ < 0 || newDir != dirSign_)
                    anchored_ = false;
                selected_ = idx;
                relShift_ += double (dirSign_ - newDir) * X_ * 0.5;
                dirSign_  = newDir;
                rel = double (dirSign_) * u + relShift_;
            }
        }
        if (idx != selected_)
            continue;
        if (absSpeed < kMkAnchorMinSpeed || absSpeed > kMkAnchorMaxSpeed)
            continue;

        // The pulse is the boundary next to the newest bit in the register:
        // its end going forward, its start going backward (the newest bit is
        // then the oldest of the state). The filters' delay to the pulse peak
        // is a few microseconds and is ignored.
        const int32_t n = t.lookup (h.bits);
        const double boundary  = dir > 0 ? cpb * double (n + 1) : cpb * double (n - nbits + 1);
        const double offsetNew = boundary - rel;
#ifdef VT_DEBUG_MK
        {
            // phase of the bit grid against the tracker's own coordinate (without relShift_)
            const double g = boundary - double (dirSign_) * u;
            std::fprintf (stderr, "anchor n=%d dir=%d speed=%.3f raw=%.4f frac=%.4f\n", n, dir, absSpeed, offsetNew,
                          g - 0.5 * std::floor (g / 0.5));
        }
#endif
        mkAnchorPg_ = std::min (h.valid, mkBitsSinceBreak_);
        anchor (offsetNew, h.valid);
    }
}

} // namespace vt
