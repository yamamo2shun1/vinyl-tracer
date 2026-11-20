// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto
//
// TimecodeDef.h
// Timecode format definitions and the LFSR position lookup table.
//
// The numeric parameters below (bit length, seed, taps, length) describe the
// pressed Serato CV02 / CV02.5 control signal. They are format facts; this file
// contains no third-party code.
#pragma once

#include <cstdint>
#include <vector>

namespace vt {

struct TimecodeDef
{
    const char* id;
    const char* description;
    int         bits;        // LFSR length
    uint32_t    seed;        // LFSR state at timecode index 0
    uint32_t    taps;        // feedback taps (excluding the end taps)
    uint32_t    length;      // number of bits on the side
    uint32_t    safe;        // last index considered safe (before run-out)
    double      resolution;  // carrier cycles per second at 33 1/3 rpm

    // How the bits are pressed.
    //  Amplitude: one bit per carrier cycle, in the peak level of one channel
    //             (Serato CV02).
    //  Offset   : one bit per cyclesPerBit carrier cycles, as a common-mode
    //             (L = R) offset of the carrier that switches level at the bit
    //             boundaries (Traktor Control Vinyl MK2).
    enum class Encoding { Amplitude, Offset };
    Encoding    encoding     = Encoding::Amplitude;
    double      cyclesPerBit = 1.0;
};

inline constexpr TimecodeDef kSeratoSideA { "serato_2a", "Serato CV02 Side A", 20, 0x59017u, 0x361e4u, 712000u, 707000u, 1000.0 };
inline constexpr TimecodeDef kSeratoSideB { "serato_2b", "Serato CV02 Side B", 20, 0x8f3c6u, 0x4f0d8u, 922000u, 917000u, 1000.0 };

// Traktor Control Vinyl MK2: 2500 Hz carrier, one bit every 2.5 cycles (1000
// bits/s at 33 1/3 rpm). Both sides are parts of one 22-bit maximal-length
// sequence (x^22 + x^15 + x^14 + x^8 + 1), side B ~17 min and side A ~12 min,
// each ending in a locked groove that carries the code (1800 bits per turn).
// Measured from recordings of the pressed records; index 0 is a little before
// the earliest bit read after dropping the needle at the outer edge.
inline constexpr TimecodeDef kTraktorMk2SideA { "traktor_mk2a", "Traktor MK2 Side A", 22, 0x2dff3cu, 0x4180u, 734300u, 732300u, 2500.0,
                                                TimecodeDef::Encoding::Offset, 2.5 };
inline constexpr TimecodeDef kTraktorMk2SideB { "traktor_mk2b", "Traktor MK2 Side B", 22, 0x124eabu, 0x4180u, 1030000u, 1028000u, 2500.0,
                                                TimecodeDef::Encoding::Offset, 2.5 };

// LFSR stepping and state -> index lookup for one timecode definition.
class LfsrTable
{
public:
    explicit LfsrTable (const TimecodeDef& def);

    const TimecodeDef& def() const noexcept { return def_; }
    uint32_t mask() const noexcept { return mask_; }

    // Next state when the record moves forwards (new bit enters at the MSB).
    uint32_t forward (uint32_t s) const noexcept
    {
        const uint32_t l = parity (s & (def_.taps | 1u));
        return (s >> 1) | (l << (def_.bits - 1));
    }

    // Previous state (record moving backwards, new bit enters at the LSB).
    uint32_t reverse (uint32_t s) const noexcept
    {
        const uint32_t l = parity (s & ((def_.taps >> 1) | (1u << (def_.bits - 1))));
        return ((s << 1) & mask_) | l;
    }

    // Index (bit number) of a state, or -1 if the state is not on the record.
    int32_t lookup (uint32_t s) const noexcept
    {
        return lut_.empty() ? lookupSorted (s & mask_) : lut_[s & mask_];
    }

    // The 64 bits pressed up to cycle n: bit k is the bit at cycle n - k
    // (0 outside the record). Used to search for a bit pattern near a
    // predicted position.
    uint64_t history (int32_t n) const noexcept
    {
        if (n < 0 || n >= int32_t (def_.length))
            return 0;
        const uint32_t p = def_.length - 1u - uint32_t (n); // bit k of the result is at p + k
        const uint32_t w = p >> 6, sh = p & 63u;
        uint64_t v = hist_[w] >> sh;
        if (sh != 0)
            v |= hist_[w + 1] << (64u - sh);
        return v;
    }

    // Bit that is pressed at cycle index n (the MSB of the state at n).
    // Only used by the test signal generator.
    static uint32_t parity (uint32_t v) noexcept
    {
        v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
        return v & 1u;
    }

private:
    int32_t lookupSorted (uint32_t s) const noexcept;

    TimecodeDef          def_;
    uint32_t             mask_;
    // Direct table for up to 20 bits (4 MB). Longer registers (Traktor MK2,
    // 22 bits: 16 MB) keep only the states on the record, sorted, as
    // (state << 32 | index) and use a binary search (one per bit read).
    std::vector<int32_t> lut_;
    std::vector<uint64_t> sorted_;
    std::vector<uint64_t> hist_; // bit (length - 1 - n) = bit pressed at index n
};

} // namespace vt
