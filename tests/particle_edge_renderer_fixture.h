#pragma once
#include "GS/ParticleEdges.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

using u16 = uint16_t;
using u32 = uint32_t;
struct GSVector4i
{
    int x=0, y=0, z=0, w=0;
    GSVector4i() = default;
    GSVector4i(int a,int b,int c,int d):x(a),y(b),z(c),w(d){}
    GSVector4i runion(const GSVector4i& b) const {return {std::min(x,b.x),std::min(y,b.y),std::max(z,b.z),std::max(w,b.w)};}
    GSVector4i rintersect(const GSVector4i& b) const {return {std::max(x,b.x),std::max(y,b.y),std::min(z,b.z),std::min(w,b.w)};}
    template<int n> GSVector4i sra32() const {return {x>>n,y>>n,z>>n,w>>n};}
    bool operator==(const GSVector4i&) const = default;
};
struct GSVertex
{
    struct {u16 X=0,Y=0; u32 Z=0;} XYZ;
    struct {float Q=1.0f;} RGBAQ;
    struct {float S=0,T=0;} ST;
    u16 U=0,V=0;
};
namespace GSTextureCache
{
struct Source {bool m_target=false,m_from_target=false;};
struct Target {float scale=2; float GetScale() const{return scale;}};
}
enum class GSHalfPixelOffset {Off,Normal,Special,SpecialAggressive,Native,NativeWTexOffset};
constexpr int GS_SPRITE_CLASS=3;
constexpr int ZTST_NEVER=0, ZTST_ALWAYS=1;
struct FixtureConfig {GSHalfPixelOffset UserHacks_HalfPixelOffset=GSHalfPixelOffset::NativeWTexOffset;};
struct FixturePerf {int frame=7; int GetFrame() const{return frame;}};
extern FixtureConfig GSConfig;
extern FixturePerf g_perfmon;
extern int s_dod_ab_off_frame, s_dod_draw_last_frame;
extern std::vector<int> s_dod_draw_frames, s_dod_sprites_before;
extern std::string s_dod_draw_summary;

class GSRendererHW
{
public:
    struct Vertices {u32 next=2; std::array<GSVertex,4> buff{};} vertices;
    struct Context
    {
        struct {int OFX=0,OFY=0;} XYOFFSET;
        struct {u32 MMIN=0,MXL=0;} TEX1;
        struct {bool ZTE=true; int ZTST=2;} TEST;
        struct {int TW=8,TH=9;} TEX0;
        struct {GSVector4i in{-4096,-4096,4096,4096};} scissor;
    } context;
    struct Prim {bool FST=true;} prim;
    struct Trace {int m_primclass=GS_SPRITE_CLASS; bool linear=true; bool IsLinear()const{return linear;}} m_vt;
    Vertices* m_vertex=&vertices;
    Context* m_context=&context;
    Context m_cached_ctx{};
    Prim* PRIM=&prim;
    bool m_process_texture=true,m_texture_shuffle=false,m_channel_shuffle=false;
    GSVector4i m_r{0,0,4,4};
    const char* ApplyParticleEdges(const ParticleEdges::Profile&,float);
    const char* LegacyParticleEdges(const GSTextureCache::Source*,float);
    const char* Draw(const GSTextureCache::Target*,const GSTextureCache::Source*,bool skipped=false);
    static void ResetParticleEdgeSession();
};

int BridgeOpen(const char* options, bool backendSuccess=true, bool validWindow=true);
int BridgeNullOpen();
void BridgeClose();
bool BridgeIsOpen();
void QueueOldCapture();
bool HasPendingCapture();
