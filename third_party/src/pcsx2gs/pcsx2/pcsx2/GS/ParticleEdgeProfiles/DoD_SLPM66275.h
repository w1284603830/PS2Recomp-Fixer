// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include "../ParticleEdgesTypes.h"

namespace ParticleEdges::Profiles
{
// Public reference runner: Onimusha: Dawn of Dreams, SLPM_662.75.
// Preserves the parameters in main 05807db, GSRendererHW::DoDNativeParticleEdges.
// Source compatibility is not a new visual qualification, and covers no other revision/disc.
inline constexpr Profile DoD_SLPM66275{
    "dod-slpm66275-native-grid-v1", "onimusha-dawn-of-dreams", "SLPM_662.75",
    Transform::NativeFirstLastSample,
    5,      // NativeWTexOffset: the native-grid transform depends on this relationship
    true,   // upscale only
    true,   // exclude texture/channel shuffles
    true,   // exclude render-target sources
    true,   // bilinear sampling only
    true,   // real depth test (not NEVER/ALWAYS)
    true, 0xFF00u, // reference HUD depth exclusion, not a universal PS2 rule
    true,   // STQ requires one nonzero Q for both vertices
    true, 6u, 0.5f // inset half a texel of the coarsest mip read; level zero without mip filtering
};
}
