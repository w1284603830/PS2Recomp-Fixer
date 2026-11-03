#include "particle_edge_renderer_fixture.h"
#include <cmath>
#include <cstdio>

FixtureConfig GSConfig;
FixturePerf g_perfmon;
int s_dod_ab_off_frame=-1,s_dod_draw_last_frame=0;
std::vector<int> s_dod_draw_frames,s_dod_sprites_before;
std::string s_dod_draw_summary;
// Host-service stand-in: production ResetParticleEdgeSession below must clear
// the actual session markers, even when no log file is open.
void DoDDrawLogClose() {}
#include "particle_renderer_current.inc"
#include "fixtures/particle_legacy_05807db.inc"

const char* GSRendererHW::Draw(const GSTextureCache::Target* rt,const GSTextureCache::Source* src,bool skip_draw)
{
#include "particle_draw_gate.inc"
    return dod_edges;
}
