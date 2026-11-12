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

    // Detected configuration (0..7) so it can be stored with the session.
    int  getConfig() const noexcept { return selected_; }
    void setPreferredConfig (int cfg) noexcept;

    // Decode a block. Any output pointer may be null.
    //  posAbs : absolute position in cycles (tracked phase + anchor)
    //  posRel : tracked phase only (cycles), never jumps
    //  speed  : 1.0 = nominal
    void process (const float* left, const float* right, int numSamples,
                  double* posAbs, double* posRel, float* speed) noexcept;

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
    double refAmp_ = 0.02, kRef_ = 0.0;
    double gateLin_ = 0.0158; // -36 dB
    double shortPow_ = 0.0, kShort_ = 0.0;
    double noiseFloor_ = 1e-7, kNfFall_ = 0.0, nfRise_ = 1.0;
    bool   gateOpen_ = false;
    float  peak_ = 0.f, kPeak_ = 0.f;

    // Envelope of the calibrated magnitude (for bit amplitude normalisation)
    double env_ = 0.0, kEnv_ = 0.0;

    // Phase tracker (doubled angle, in turns)
    double X_ = 0.0, V_ = 0.0, A_ = 0.0;
    double loopA_ = 0.0, loopB_ = 0.0, loopC_ = 0.0, coast_ = 0.0;
    double lastLoopSpeed_ = -1.0;

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
