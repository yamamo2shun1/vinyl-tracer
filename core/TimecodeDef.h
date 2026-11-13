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
    uint32_t    length;      // number of cycles on the side
    uint32_t    safe;        // last index considered safe (before run-out)
    double      resolution;  // carrier cycles per second at 33 1/3 rpm
};

inline constexpr TimecodeDef kSeratoSideA { "serato_2a", "Serato CV02 Side A", 20, 0x59017u, 0x361e4u, 712000u, 707000u, 1000.0 };
inline constexpr TimecodeDef kSeratoSideB { "serato_2b", "Serato CV02 Side B", 20, 0x8f3c6u, 0x4f0d8u, 922000u, 917000u, 1000.0 };

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

    // Index (cycle number) of a state, or -1 if the state is not on the record.
    int32_t lookup (uint32_t s) const noexcept { return lut_[s & mask_]; }

    // Bit that is pressed at cycle index n (the MSB of the state at n).
    // Only used by the test signal generator.
    static uint32_t parity (uint32_t v) noexcept
    {
        v ^= v >> 16; v ^= v >> 8; v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
        return v & 1u;
    }

private:
    TimecodeDef          def_;
    uint32_t             mask_;
    std::vector<int32_t> lut_;
};

} // namespace vt
