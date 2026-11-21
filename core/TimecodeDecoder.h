// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// TimecodeDecoder.h
//
// Timecode vinyl decoder designed for drift-free, low-speed-capable tracking.
//
// How it differs from a zero-crossing decoder:
//  * Position comes from the continuous phase of the stereo quadrature carrier
//    (atan2), tracked by a type-3 PLL. The PLL measures position every sample,
//    so it keeps moving at very low speeds where zero crossings become rare.
//  * The doubled angle (2*theta) is tracked. It is insensitive to the polarity
//    flip a magnetic cartridge produces when the record changes direction, and
//    to overall level changes (cartridge output falls with speed).
//  * The LFSR bitstream is only used to anchor/verify the absolute position.
//    Playback position = tracked phase + anchor. It is a measurement, not an
//    integrated speed, so it cannot drift. Cycle slips are detected by the
//    bitstream and corrected.
//  * Channel order, bit polarity and direction are detected automatically by
//    running all hypotheses in parallel (cheap: one update per zero crossing).
//  * L/R gain mismatch and phase skew are calibrated continuously.
#pragma once

#include "TimecodeDef.h"
#include <cstdint>

namespace vt {

struct DecoderStatus
{
    bool   signal      = false; // carrier above the gate
    bool   bitLock     = false; // bitstream currently verified by the LFSR
    bool   anchored    = false; // absolute position known
    int    validBits   = 0;
    double absCycles   = 0.0;   // absolute position in carrier cycles
    double speed       = 0.0;   // 1.0 = nominal speed
    float  levelDb     = -120.f;// input peak level (dBFS)
    float  refDb       = -120.f;// tracked nominal carrier level (dBFS)
    float  gateDb      = -120.f;// level that opens the gate (dBFS, same scale as refDb)
    bool   refLearned  = false; // refDb was measured (or restored), not the default
    int    config      = -1;    // detected channel/polarity/direction config
    int    reanchors   = 0;     // number of absolute jumps (needle drops / slips)
    float  scopeL      = 0.f;   // calibrated, normalised scope sample
    float  scopeR      = 0.f;
};

class TimecodeDecoder
{
public:
    TimecodeDecoder() = default;

    void prepare (double sampleRate);
    void setTable (const LfsrTable* table); // also resets the lock
    void reset();

    // Gate threshold relative to the nominal carrier level (e.g. -36 dB).
    void setGateDb (float db) noexcept;

    // Nominal carrier level (dBFS) measured in an earlier session, so that the
    // gate is right before the record has been played at normal speed.
    void setNominalDb (float db) noexcept;

    // Detected configuration (0..7) so it can be stored with the session.
    int  getConfig() const noexcept { return selected_; }
    void setPreferredConfig (int cfg) noexcept;

    // Decode a block. Any output pointer may be null.
    //  posAbs : absolute position in cycles (tracked phase + anchor)
    //  posRel : tracked phase plus slip corrections (cycles); needle drops and
    //           groove skips are ignored, tracker slips are not left in (small
    //           ones slewed, large ones jump like posAbs)
    //  speed  : 1.0 = nominal
    //  quality: 1 = clean carrier, falling to 0 while the input is not one
    //           (stylus skating / skipping; the position follows noise there)
    // Diagnostics (for display, not used for the position):
    //  lockError   : mean |residual| of the phase loop (turns; clean ~0.005)
    //  centreOffset: distance of the L/R path's centre (~30 Hz low-pass) from
    //                the origin relative to its radius (1 = the path passes
    //                through the origin); -1 while the gate is closed
    void process (const float* left, const float* right, int numSamples,
                  double* posAbs, double* posRel, float* speed, float* quality = nullptr,
                  float* lockError = nullptr, float* centreOffset = nullptr) noexcept;

    const DecoderStatus& status() const noexcept { return status_; }

private:
    struct Hypothesis
    {
        uint32_t bits     = 0;
        uint32_t expected = 0;
        int      valid    = 0;
        int      lastDir  = 0;   // +1 forward, -1 backward, 0 unknown
    };

    void processSample (float xl, float xr) noexcept;
    void onCrossing (int primary, float primaryValue) noexcept;
    void fastRead (float amplitude, int dir, double rel, double absSpeed) noexcept;
    void anchor (double offsetNew, int valid) noexcept;
    void correct (double offsetNew, double diff) noexcept;

    // Traktor MK2 (offset encoding)
    void mk2FrontEnd (double& l0, double& r0) noexcept;
    double mk2Assist (double meas, double Xp, double residRaw, double weight, double residM) noexcept;
    void mk2Acquire() noexcept;
    void mk2Pulses() noexcept;
    void mk2Candidate() noexcept;
    void onTransition (int sign, double u, double rel, double absSpeed) noexcept;
    void updateLoopGains (double absSpeed) noexcept;

    static int hypIndex (int primary, int polarity, int dir) noexcept { return primary * 4 + polarity * 2 + dir; }

    const LfsrTable* table_ = nullptr;
    double sr_ = 48000.0;
    double unit_ = 1.0; // carrier cycles per ms of record (1 for Serato)

    // DC removal
    double dcL_ = 0.0, dcR_ = 0.0, kDc_ = 0.0, kDcSlow_ = 0.0;

    // L/R calibration (second-order statistics of the raw signal)
    double eLL_ = 1e-9, eRR_ = 1e-9, eLR_ = 0.0, kCal_ = 0.0;

    // Gate / level
    // Nominal carrier level. The default (-20 dBFS, a typical line-level
    // timecode) keeps the gate above vibration and hum of a stopped record
    // until the real level has been measured or restored.
    double refAmp_ = 0.1, kRef_ = 0.0;
    int    refLearnSamples_ = 0, refLearnNeeded_ = 1;
    double gateLin_ = 0.0158; // -36 dB
    double shortPow_ = 0.0, kShort_ = 0.0;
    double noiseFloor_ = 1e-7, kNfFall_ = 0.0, nfRise_ = 1.0;
    double gateMag_ = 0.0, kGateMag_ = 0.0;   // short magnitude average (closing)
    double gatePow_ = 0.0, kGatePow_ = 0.0;   // short power average (opening)
    bool   gateOpen_ = false;
    float  peak_ = 0.f, kPeak_ = 0.f;

    // Envelope of the calibrated magnitude (for bit amplitude normalisation)
    double env_ = 0.0, kEnv_ = 0.0;
    double cenL_ = 0.0, cenR_ = 0.0, kCentre_ = 0.0; // centre of the L/R path (diagnostics)

    // Phase tracker (doubled angle, in turns)
    double X_ = 0.0, V_ = 0.0, A_ = 0.0;
    double loopA_ = 0.0, loopB_ = 0.0, loopC_ = 0.0, coast_ = 0.0;
    double lastLoopSpeed_ = -1.0;
    double vMax_ = 1.0;                 // speed limit in X turns per sample
    double lockErr_ = 0.0, kLockErr_ = 0.0; // mean |residual| of the loop
    double relShift_ = 0.0;                 // keeps posRel continuous when the direction is detected
    double relCorr_ = 0.0, relCorrTarget_ = 0.0; // slip corrections applied to posRel (slewed)
    double lastOpenSpeed_ = 0.0;            // |speed| while the carrier was last present
    double slowSpeed_ = 0.0, kSlowSpeed_ = 0.0; // |speed| averaged over 10 ms

    // Frequency acquisition after the gate opens
    int    acqN_ = 0;
    double acqSum_ = 0.0, acqSq_ = 0.0, acqLast_ = 0.0;

    // Zero crossing state of the calibrated channels
    bool posL_ = false, posR_ = false;
    float lastL_ = 0.f, lastR_ = 0.f;

    // Bit decoding
    float      refLevel_[2][2] { { 1.f, 1.f }, { 1.f, 1.f } };
    Hypothesis hyp_[8];
    int        selected_  = -1;
    int        selectedBad_ = 0;
    int        preferred_ = 0;
    int        dirSign_   = 1;

    // Anchor
    bool   anchored_ = false;
    double offset_   = 0.0;
    double offsetTarget_ = 0.0;
    bool   stale_    = false;
    int    closedSamples_ = 0;
    double speedNow_ = 0.0;
    int    mismatch_ = 0;
    double mismatchRef_ = 0.0; // diff claimed by the pending correction candidate
    int    reanchors_ = 0;

    // Fast correction while anchored: bits of the selected peak type, each
    // compared with the mean of its 3 + 3 neighbours, searched for near the
    // predicted position with a few bit errors allowed.
    struct FastPeak { float a; double rel, speed; };
    FastPeak fastRing_[7] {};
    int      fastN_ = 0;
    int      fastDir_ = 0;
    uint64_t fastBits_ = 0;
    int      fastCount_ = 0;
    int      fastAgree_ = 0;
    double   fastDiff_ = 0.0;
    bool     fastSynced_ = false;
    bool     fastLost_ = false; // the input was not a carrier since the last match (search wide)

    // Traktor MK2: the bits are a common-mode offset (L = R) of the carrier,
    // as large as the carrier itself. L - R is a clean carrier; L + R carries
    // the other quadrature component plus the offset. The offset is estimated
    // by a tracking filter on what the predicted carrier does not explain, and
    // taken off before the phase tracker. Its level changes at the bit
    // boundaries; after undoing the playback RIAA curve each change is a pulse.
    bool   mk2_ = false;
    double mkC_ = 0.0, mkD_ = 0.0;            // offset estimate (level, slope per sample)
    bool   mkZcPos_ = false;                  // zero crossings of S -> carrier frequency
    int    mkZcCount_ = 0;
    double mkZcFreq_ = 0.0;
    int    mkZcSteady_ = 0;                   // consecutive crossings within 10 % of the mean
    int    mkHalfCount_ = 0, mkHalf1_ = 0, mkHalf2_ = 0; // last two half cycles of S (samples)
    double mkHalfFreq_ = 0.0;                 // ... their frequency, 0 when they disagree
    int    mkQuick_ = 0;                      // samples a quick start has been due
    double mkSPow_ = 0.0;                     // power of S, 120 Hz high-pass (3 ms)
    double mkHpB0_ = 1.0, mkHpB1_ = 0.0, mkHpA1_ = 0.0, mkHpA2_ = 0.0; // 120 Hz high-pass of S
    double mkHx1_ = 0.0, mkHx2_ = 0.0, mkHy1_ = 0.0, mkHy2_ = 0.0;
    double mkLx1_ = 0.0, mkLx2_ = 0.0, mkLy1_ = 0.0, mkLy2_ = 0.0; // high-pass of S for the tracker
    double mkS_ = 0.0, mkM_ = 0.0;            // S and M of the current sample
    double mkQ_ = 0.0, kMkQ_ = 0.0, mkLPrev_ = 0.0; // direction: M x change of S
    int    mkStuck_ = 0;                      // samples the tracker stayed far below the carrier
    bool   mkBehind_ = false;                 // ... it is now
    double mkGoodV_ = 0.0, mkGoodX_ = 0.0;    // last speed that matched the zero crossings, and position ...
    int    mkGoodAge_ = 1 << 30;              // ... this many samples ago
    double mkPol_ = 1.0, kMkPol_ = 0.0;       // carrier polarity (theta vs theta + pi)
    double mkShB0_ = 1.0, mkShB1_ = 0.0, mkShA1_ = 0.0; // RIAA 3180 / 318 us section
    double mkShX_ = 0.0, mkShY_ = 0.0, mkZPrev_ = 0.0, mkHpY_ = 0.0;
    double mkV_ = 0.0;                        // pulse signal
    double mkPulseLevel_ = 1.0;               // pulse peak / (envelope x speed), learned
    bool   mkCandOn_ = false;                 // pulse candidate (largest |v| so far)
    int    mkCandSign_ = 0;
    double mkCandV_ = 0.0, mkCandU_ = 0.0, mkCandRel_ = 0.0;
    double mkCandSpeed_ = 0.0, mkCandEnv_ = 0.0;
    bool   mkLastOn_ = false;                 // previous transition of the current run
    int    mkLastSign_ = 0;
    double mkLastU_ = 0.0;
    bool   mkReversed_ = false;               // the tracker reversed since the last transition
    int    mkVSign_ = 0;
    // Tracker assist from S (issue #2, step 2 continued, stage 1): a band-pass
    // pair on S gives its doubled angle with far less noise than a sample
    // difference. While the S and M measurements agree (correlation gate),
    // the S measurement is blended into the loop's phase update only;
    // lockErr_ (quality) and the bit path keep the M-based residual.
    double mkBpB0_ = 0.0, mkBpB2_ = 0.0, mkBpA1_ = 0.0, mkBpA2_ = 0.0; // band-pass of S (Q = 2)
    double mkBpX1_ = 0.0, mkBpX2_ = 0.0, mkBpY1_ = 0.0, mkBpY2_ = 0.0;
    double mkApA_ = 0.0;                      // quadrature all-pass (-90 deg at the centre frequency)
    double mkApX1_ = 0.0, mkApY1_ = 0.0;
    double mkBpF0_ = 0.0;                     // centre frequency the pair is tuned to
    double mkBpI_ = 0.0, mkBpQ_ = 0.0;        // pair outputs of the current sample
    double mkDelta_ = 0.0;                    // alignment of S's doubled angle to M's (turns)
    bool   mkDeltaOk_ = false;
    double mkCorr_ = 0.0;                     // smoothed agreement of the two measurements
    double mkDMean_ = 0.0;                    // smoothed signed S-M phase difference (turns)
    int    mkBitsSinceBreak_ = 0;             // bits read since the last rejected transition
    int    mkAnchorPg_ = 0;                   // verified bits behind the current reading, counted from after that break
    double kMkDelta_ = 0.0, kMkCorr_ = 0.0;   // one-pole coefficients (set in prepare)

    DecoderStatus status_;
};

} // namespace vt
