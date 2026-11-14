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
    void updateLoopGains (double absSpeed) noexcept;

    static int hypIndex (int primary, int polarity, int dir) noexcept { return primary * 4 + polarity * 2 + dir; }

    const LfsrTable* table_ = nullptr;
    double sr_ = 48000.0;

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
    int    reanchors_ = 0;

    DecoderStatus status_;
};

} // namespace vt
