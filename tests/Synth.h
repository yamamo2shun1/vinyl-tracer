// Synth.h - test signal generator for Serato-style timecode.
//
// Models the playback chain of a real turntable:
//   groove signal a(theta)  (quadrature 1 kHz carrier, AM bits on one channel)
//   x magnetic cartridge   (output proportional to stylus velocity -> * speed,
//                           and the polarity inverts when the record reverses)
//   x RIAA de-emphasis     (normalised to 0 dB at 1 kHz)
//   + L/R gain mismatch, phase skew, DC offset, hum and noise.
#pragma once

#include "TimecodeDef.h"
#include <cstdint>
#include <random>
#include <vector>

namespace vt::test {

struct SynthOptions
{
    double sampleRate  = 48000.0;
    double level       = 0.25;   // nominal carrier amplitude (linear FS)
    double amLow       = 0.75;   // relative amplitude of a "0" bit
    bool   cartridge   = true;   // velocity-sensitive pickup + RIAA
    double noiseDb     = -60.0;  // relative to level
    double humDb       = -66.0;  // 50 Hz hum relative to level
    double dcL         = 0.004;
    double dcR         = -0.003;
    double gainR       = 0.85;   // channel imbalance
    double skewDeg     = 8.0;    // quadrature error
    bool   swapLR      = false;  // test auto-detection
    bool   invertR     = false;
    uint32_t seed      = 1234;
};

// theta: record position in carrier cycles, one value per output sample.
void synthesize (const LfsrTable& table, const std::vector<double>& theta,
                 const SynthOptions& opt, std::vector<float>& left, std::vector<float>& right);

// Motion helpers ---------------------------------------------------------
// Integrate a speed profile (1.0 = nominal) into positions (cycles).
std::vector<double> integrateSpeed (const std::vector<double>& speed, double startCycles,
                                    double sampleRate, double resolution);

} // namespace vt::test
