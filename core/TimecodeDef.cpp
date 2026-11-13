// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Shunichi Yamamoto

#include "TimecodeDef.h"

namespace vt {

LfsrTable::LfsrTable (const TimecodeDef& def)
    : def_ (def),
      mask_ ((def.bits >= 32) ? 0xffffffffu : ((1u << def.bits) - 1u)),
      lut_ (size_t (1) << def.bits, -1)
{
    uint32_t s = def_.seed & mask_;
    for (uint32_t n = 0; n < def_.length; ++n)
    {
        if (lut_[s] != -1)
            break; // sequence wrapped (should not happen for valid definitions)
        lut_[s] = int32_t (n);
        s = forward (s);
    }
}

} // namespace vt
