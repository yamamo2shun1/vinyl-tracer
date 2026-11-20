// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

#include "TimecodeDef.h"

#include <algorithm>

namespace vt {

namespace {
constexpr int kMaxDirectBits = 20;
}

LfsrTable::LfsrTable (const TimecodeDef& def)
    : def_ (def),
      mask_ ((def.bits >= 32) ? 0xffffffffu : ((1u << def.bits) - 1u)),
      hist_ (def.length / 64 + 2, 0)
{
    const bool direct = def_.bits <= kMaxDirectBits;
    if (direct)
        lut_.assign (size_t (1) << def_.bits, -1);
    else
        sorted_.reserve (def_.length);

    uint32_t s = def_.seed & mask_;
    for (uint32_t n = 0; n < def_.length; ++n)
    {
        if (direct)
        {
            if (lut_[s] != -1)
                break; // sequence wrapped (should not happen for valid definitions)
            lut_[s] = int32_t (n);
        }
        else
            sorted_.push_back ((uint64_t (s) << 32) | n);
        if ((s >> (def_.bits - 1)) & 1u) // the bit pressed at n is the MSB of the state
        {
            const uint32_t p = def_.length - 1u - n;
            hist_[p >> 6] |= uint64_t (1) << (p & 63u);
        }
        s = forward (s);
    }
    std::sort (sorted_.begin(), sorted_.end());
}

int32_t LfsrTable::lookupSorted (uint32_t s) const noexcept
{
    const uint64_t key = uint64_t (s) << 32;
    const auto it = std::lower_bound (sorted_.begin(), sorted_.end(), key);
    if (it == sorted_.end() || uint32_t (*it >> 32) != s)
        return -1;
    return int32_t (uint32_t (*it));
}

} // namespace vt
