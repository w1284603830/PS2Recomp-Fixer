// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ParticleEdges
{
enum class Transform { NativeFirstLastSample };

// A profile is a reviewed game/revision policy, never inferred from HPO.
struct Profile
{
    const char* id;
    const char* game;
    const char* revision;
    Transform transform;
    int requiredHpo;
    bool upscaleOnly;
    bool rejectShuffle;
    bool rejectRenderTargets;
    bool requireLinear;
    bool requireDepthTest;
    bool excludeHud;
    uint32_t hudZMinimum;
    bool requireEqualQ;
    bool coarsestMipInset;
    uint32_t maximumMip;
    float insetTexels;
};

struct Draw
{
    bool skipped = false;
    bool sprite = false;
    bool target = false;
    float scale = 0.0f;
    bool shuffle = false;
    int hpo = 0;
    bool texture = false;
    bool renderTargetTexture = false;
    bool linear = false;
    bool depthEnabled = false;
    int depthFunction = 0; // GS ZTST: 0 NEVER, 1 ALWAYS, 2 GEQUAL, 3 GREATER.
};

inline bool SupportedProfile(const Profile& p)
{
    // This transform's relationship to DetermineVSConfig has only been
    // implemented for NativeWTexOffset. A different HPO needs its own transform.
    return p.id && p.game && p.revision && p.transform == Transform::NativeFirstLastSample &&
        p.requiredHpo == 5 && p.upscaleOnly && p.requireEqualQ && p.maximumMip <= 7u &&
        std::isfinite(p.insetTexels) && p.insetTexels >= 0.0f;
}

inline const char* DrawSkipReason(const Profile& p, const Draw& d)
{
    if (d.skipped) return "skipped";
    if (!d.sprite) return "not-sprite"; // the supported transform operates on sprite pairs
    if (!d.target || (p.upscaleOnly && d.scale <= 1.0f)) return "native-scale";
    if (!std::isfinite(d.scale) || d.scale <= 1.0f) return "bad-scale";
    if (p.rejectShuffle && d.shuffle) return "shuffle";
    if (d.hpo != p.requiredHpo) return "hpo";
    if (!d.texture) return "no-texture";
    if (p.rejectRenderTargets && d.renderTargetTexture) return "rt-source";
    if (p.requireLinear && !d.linear) return "nearest";
    if (p.requireDepthTest && (!d.depthEnabled || d.depthFunction < 2)) return "no-ztest";
    return nullptr;
}

inline const char* VertexSkipReason(const Profile& p, uint32_t z, bool fixedUv, bool second, float previousQ, float q)
{
    if (p.excludeHud && z >= p.hudZMinimum) return "hud-z";
    if (!fixedUv && second && p.requireEqualQ &&
        (!std::isfinite(q) || q == 0.0f || previousQ != q)) return "q-varies";
    return nullptr;
}

inline float Inset(const Profile& p, uint32_t minFilter, uint32_t maxLevel)
{
    const uint32_t level = p.coarsestMipInset && minFilter >= 2 ? std::min(maxLevel, p.maximumMip) : 0u;
    return p.insetTexels * static_cast<float>(1u << level);
}

// The existing native-grid transform, parameterized only at its sampling inset.
// p0/p1 are GS 1/16-pixel coordinates with XYOFFSET removed.
inline void TransformAxis(int p0, int p1, float c0, float c1, float scale, float inset,
                          int& q0, int& q1, float& d0, float& d1)
{
    const int n0 = (std::min(p0, p1) + 15) >> 4;
    const int n1 = (std::max(p0, p1) + 15) >> 4;
    q0 = (p0 <= p1 ? n0 : n1) * 16;
    q1 = (p0 <= p1 ? n1 : n0) * 16;
    d0 = c0;
    d1 = c1;
    if (n1 <= n0) return;
    const float slope = (c1 - c0) / static_cast<float>(p1 - p0);
    float a = c0 + slope * static_cast<float>(n0 * 16 - p0);
    float b = c0 + slope * static_cast<float>((n1 - 1) * 16 - p0);
    const float lo = std::min(c0, c1) + inset;
    const float hi = std::max(c0, c1) - inset;
    if (lo <= hi)
    {
        a = std::clamp(a, lo, hi);
        b = std::clamp(b, lo, hi);
    }
    const float n = static_cast<float>(n1 - n0);
    const float s1 = a + (b - a) * n / (n - 1.0f / scale);
    d0 = p0 <= p1 ? a : s1;
    d1 = p0 <= p1 ? s1 : a;
}
}
