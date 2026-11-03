// SPDX-License-Identifier: GPL-3.0+
#pragma once

#include "ParticleEdgesTypes.h"
#include "ParticleEdgeProfiles/DoD_SLPM66275.h"
#include <array>
#include <cstddef>
#include <string_view>

// All access is on the GS API thread. No process-wide cached environment selection.
namespace ParticleEdges
{
struct Session
{
    const Profile* profile = nullptr;
    const char* reason = "profile-off";
    bool forceOff = false;
};
inline Session session;

inline void Reset() { session = {}; }

inline std::string_view Trim(std::string_view value)
{
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    value.remove_prefix(first);
    return value.substr(0, value.find_last_not_of(" \t\r\n") + 1);
}

inline const Profile* FindProfile(std::string_view id)
{
    // New games/revisions add independently reviewed entries; there is no fallback.
    const Profile& p = Profiles::DoD_SLPM66275;
    return id == p.id ? &p : nullptr;
}

inline void BeginSession(const char* options, const char* primaryOff, const char* legacyOff)
{
    Reset();
    const char* env = primaryOff ? primaryOff : legacyOff; // preserve PS2_* precedence
    session.forceOff = env && env[0] == '0';
    std::array<std::string_view, 3> values{};
    std::array<bool, 3> seen{};
    bool invalid = false;
    std::string_view rest = options ? options : "";
    while (!rest.empty())
    {
        const size_t end = rest.find(';');
        const std::string_view item = Trim(rest.substr(0, end));
        rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
        const size_t eq = item.find('=');
        const std::string_view key = Trim(item.substr(0, eq));
        const int index = key == "game_id" ? 0 : key == "game_revision" ? 1 : key == "particle_edges_profile" ? 2 : -1;
        if (index < 0) continue;
        if (eq == std::string_view::npos || seen[index]) { invalid = true; continue; }
        seen[index] = true;
        values[index] = Trim(item.substr(eq + 1));
        if (values[index].empty()) invalid = true;
    }
    if (invalid) { session.reason = "profile-invalid"; return; }
    if (!seen[2] || values[2] == "off") return;
    const Profile* selected = FindProfile(values[2]);
    if (!selected) { session.reason = "profile-unknown"; return; }
    if (!SupportedProfile(*selected)) { session.reason = "profile-unsupported"; return; }
    if (!seen[0] || !seen[1]) { session.reason = "profile-identity-missing"; return; }
    if (values[0] != selected->game || values[1] != selected->revision)
    { session.reason = "profile-identity-mismatch"; return; }
    session.profile = selected;
    session.reason = "profile-selected";
}

inline const char* SessionSkipReason()
{
    if (!session.profile) return session.reason;
    return session.forceOff ? "env-off" : nullptr;
}
}
