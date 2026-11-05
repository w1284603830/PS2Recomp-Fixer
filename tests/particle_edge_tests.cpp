#include "particle_edge_renderer_fixture.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

void DirtyPortState();bool PortStateClean();

static const char* referenceOptions =
    "game_id=onimusha-dawn-of-dreams;game_revision=SLPM_662.75;particle_edges_profile=dod-slpm66275-native-grid-v1";

static void environment(const char* key,const char* value)
{
#ifdef _WIN32
    _putenv_s(key,value?value:"");
#else
    if(value) setenv(key,value,1); else unsetenv(key);
#endif
}
static void prepare(GSRendererHW& r)
{
    r.vertices.buff[0].XYZ={17,33,1000};
    r.vertices.buff[1].XYZ={81,113,1000};
    r.vertices.buff[0].U=160;r.vertices.buff[0].V=320;
    r.vertices.buff[1].U=1280;r.vertices.buff[1].V=1920;
    for(int i=0;i<2;++i)
    {
        r.vertices.buff[i].ST.S=r.vertices.buff[i].U/16.0f/256.0f;
        r.vertices.buff[i].ST.T=r.vertices.buff[i].V/16.0f/512.0f;
    }
}
static void reasonIs(const char* actual,const char* expected)
{
    if(!actual||std::strcmp(actual,expected))
        std::fprintf(stderr,"reason: expected=%s actual=%s\n",expected,actual?actual:"<null>");
    assert(actual&&std::strcmp(actual,expected)==0);
}
static void unchanged(GSRendererHW& r,const GSTextureCache::Target* rt,const GSTextureCache::Source* tex,const char* why,bool skip=false)
{
    const auto vertices=r.vertices.buff;
    const auto rect=r.m_r;
    reasonIs(r.Draw(rt,tex,skip),why);
    assert(std::memcmp(vertices.data(),r.vertices.buff.data(),sizeof(vertices))==0);
    assert(rect==r.m_r);
}

int main(int argc,char** argv)
{
    assert(argc==2);
    environment("PS2_PARTICLE_EDGES",nullptr);environment("DOD_PARTICLE_EDGES",nullptr);
    const std::string mode=argv[1];
    GSTextureCache::Target rt;
    GSTextureCache::Source texture;
    GSRendererHW renderer;
    prepare(renderer);
    if(mode=="default")
    {
        assert(!ParticleEdges::session.profile);
        unchanged(renderer,&rt,&texture,"profile-off");
        for(const char* text:{static_cast<const char*>(nullptr),"", "half_pixel_offset=5", "particle_edges=1", "game_id=other;game_revision=v1"})
        {
            ParticleEdges::BeginSession(text,"1","1");
            unchanged(renderer,&rt,&texture,"profile-off");
        }
    }
    else if(mode=="valid")
    {
        ParticleEdges::BeginSession(referenceOptions,nullptr,nullptr);
        assert(ParticleEdges::session.profile==&ParticleEdges::Profiles::DoD_SLPM66275);
        const auto vertices=renderer.vertices.buff;
        reasonIs(renderer.Draw(&rt,&texture),"applied");
        assert(std::memcmp(vertices.data(),renderer.vertices.buff.data(),sizeof(vertices))!=0);
        assert(renderer.vertices.buff[0].XYZ.X==32&&renderer.vertices.buff[1].XYZ.X==96);
    }
    else if(mode=="invalid")
    {
        for(const char* text:{"particle_edges_profile", "particle_edges_profile=", "game_id=;particle_edges_profile=off",
            "particle_edges_profile=off;particle_edges_profile=dod-slpm66275-native-grid-v1"})
        {
            ParticleEdges::BeginSession(referenceOptions,nullptr,nullptr);
            ParticleEdges::BeginSession(text,nullptr,nullptr);
            assert(!ParticleEdges::session.profile);
            unchanged(renderer,&rt,&texture,"profile-invalid");
        }
        ParticleEdges::BeginSession("particle_edges_profile=unknown",nullptr,nullptr);
        unchanged(renderer,&rt,&texture,"profile-unknown");
        ParticleEdges::BeginSession("particle_edges_profile=off",nullptr,nullptr);
        unchanged(renderer,&rt,&texture,"profile-off");
    }
    else if(mode=="identity")
    {
        for(const char* text:{"particle_edges_profile=dod-slpm66275-native-grid-v1",
            "game_id=onimusha-dawn-of-dreams;particle_edges_profile=dod-slpm66275-native-grid-v1"})
        {
            ParticleEdges::BeginSession(text,nullptr,nullptr);
            unchanged(renderer,&rt,&texture,"profile-identity-missing");
        }
        for(const char* text:{
            "game_id=onimusha3;game_revision=SLPM_662.75;particle_edges_profile=dod-slpm66275-native-grid-v1",
            "game_id=onimusha-dawn-of-dreams;game_revision=other-revision;particle_edges_profile=dod-slpm66275-native-grid-v1",
            "game_id=ONIMUSHA-DAWN-OF-DREAMS;game_revision=SLPM_662.75;particle_edges_profile=dod-slpm66275-native-grid-v1"})
        {
            ParticleEdges::BeginSession(referenceOptions,nullptr,nullptr);
            ParticleEdges::BeginSession(text,nullptr,nullptr);
            unchanged(renderer,&rt,&texture,"profile-identity-mismatch");
        }
    }
    else if(mode=="parsing")
    {
        ParticleEdges::BeginSession(" other=4 ; particle_edges_profile = dod-slpm66275-native-grid-v1 ; game_revision=SLPM_662.75; game_id=onimusha-dawn-of-dreams ",nullptr,nullptr);
        assert(!ParticleEdges::SessionSkipReason());
        const std::string duplicate=std::string(referenceOptions)+";game_revision=other";
        ParticleEdges::BeginSession(duplicate.c_str(),nullptr,nullptr);
        unchanged(renderer,&rt,&texture,"profile-invalid");
    }
    else if(mode=="env")
    {
        for(auto values:{std::pair{"0",static_cast<const char*>(nullptr)},std::pair{static_cast<const char*>(nullptr),"0"},std::pair{"0","1"}})
        {
            ParticleEdges::BeginSession(referenceOptions,values.first,values.second);
            unchanged(renderer,&rt,&texture,"env-off");
        }
        ParticleEdges::BeginSession(referenceOptions,"1","0"); // documented PS2_* precedence
        assert(!ParticleEdges::SessionSkipReason());
        ParticleEdges::BeginSession(nullptr,"1","1");
        unchanged(renderer,&rt,&texture,"profile-off");
        environment("PS2_PARTICLE_EDGES","0");
        assert(BridgeOpen(referenceOptions));
        unchanged(renderer,&rt,&texture,"env-off");
        BridgeClose();
        environment("PS2_PARTICLE_EDGES",nullptr);
        assert(BridgeOpen(referenceOptions));
        reasonIs(renderer.Draw(&rt,&texture),"applied");
        BridgeClose();
    }
    else if(mode=="lifecycle")
    {
        assert(BridgeOpen(referenceOptions));
        QueueOldCapture();
        unchanged(renderer,&rt,&texture,"ab-off");
        assert(!BridgeOpen("particle_edges_profile=off")); // duplicate open cannot clear a live game
        assert(ParticleEdges::session.profile&&HasPendingCapture());
        BridgeClose();
        assert(!BridgeIsOpen()&&!ParticleEdges::session.profile&&!HasPendingCapture());
        assert(s_dod_ab_off_frame==-1&&s_dod_draw_last_frame==0&&s_dod_draw_frames.empty()&&s_dod_sprites_before.empty()&&s_dod_draw_summary.empty());
        QueueOldCapture();
        assert(!BridgeOpen(referenceOptions,false));
        assert(!ParticleEdges::session.profile&&!HasPendingCapture()&&s_dod_ab_off_frame==-1);
        assert(!BridgeNullOpen());
        assert(!BridgeOpen(referenceOptions,true,false));
        assert(!ParticleEdges::session.profile);
        assert(BridgeOpen(referenceOptions));
        reasonIs(renderer.Draw(&rt,&texture),"applied");
        BridgeClose();BridgeClose();
    }
    else if(mode=="switches")
    {
        for(int i=0;i<20;++i)
        {
            assert(BridgeOpen(referenceOptions));
            assert(ParticleEdges::session.profile);
            QueueOldCapture();BridgeClose();
            assert(BridgeOpen("game_id=onimusha3;game_revision=own-revision;particle_edges_profile=dod-slpm66275-native-grid-v1"));
            unchanged(renderer,&rt,&texture,"profile-identity-mismatch");
            assert(!HasPendingCapture());BridgeClose();
            assert(BridgeOpen(nullptr));
            unchanged(renderer,&rt,&texture,"profile-off");BridgeClose();
        }
    }
    else if(mode=="port-lifecycle")
    {
        DirtyPortState();assert(!BridgeNullOpen());assert(PortStateClean());
        DirtyPortState();assert(!BridgeOpen(nullptr,false));assert(PortStateClean());
        DirtyPortState();assert(BridgeOpen(nullptr));assert(PortStateClean());
        DirtyPortState();assert(!BridgeOpen(nullptr));assert(!PortStateClean());
        BridgeClose();assert(PortStateClean());DirtyPortState();BridgeClose();assert(PortStateClean());
    }
    else if(mode=="capture-window")
    {
        ParticleEdges::BeginSession(referenceOptions,nullptr,nullptr);
        s_dod_ab_off_frame=11;s_dod_draw_last_frame=14;
        for(int f=7;f<=16;++f){g_perfmon.frame=f;prepare(renderer);
            reasonIs(renderer.Draw(&rt,&texture),(f>=11&&f<=14)?"ab-off":"applied");}
        GSRendererHW::ResetParticleEdgeSession();assert(s_dod_ab_off_frame==-1);
    }
    else if(mode=="guards")
    {
        ParticleEdges::BeginSession(referenceOptions,nullptr,nullptr);
        unchanged(renderer,&rt,&texture,"skipped",true);
        renderer.m_vt.m_primclass=0;unchanged(renderer,&rt,&texture,"not-sprite");renderer.m_vt.m_primclass=GS_SPRITE_CLASS;
        rt.scale=1;unchanged(renderer,&rt,&texture,"native-scale");rt.scale=2;
        rt.scale=std::numeric_limits<float>::quiet_NaN();unchanged(renderer,&rt,&texture,"bad-scale");rt.scale=2;
        unchanged(renderer,nullptr,&texture,"native-scale");
        renderer.m_texture_shuffle=true;unchanged(renderer,&rt,&texture,"shuffle");renderer.m_texture_shuffle=false;
        renderer.m_channel_shuffle=true;unchanged(renderer,&rt,&texture,"shuffle");renderer.m_channel_shuffle=false;
        GSConfig.UserHacks_HalfPixelOffset=GSHalfPixelOffset::Native;
        unchanged(renderer,&rt,&texture,"hpo");GSConfig.UserHacks_HalfPixelOffset=GSHalfPixelOffset::NativeWTexOffset;
        unchanged(renderer,&rt,nullptr,"no-texture");
        texture.m_target=true;unchanged(renderer,&rt,&texture,"rt-source");texture.m_target=false;
        texture.m_from_target=true;unchanged(renderer,&rt,&texture,"rt-source");texture.m_from_target=false;
        renderer.m_vt.linear=false;unchanged(renderer,&rt,&texture,"nearest");renderer.m_vt.linear=true;
        renderer.m_cached_ctx.TEST.ZTE=false;unchanged(renderer,&rt,&texture,"no-ztest");renderer.m_cached_ctx.TEST.ZTE=true;
        for(int f:{0,1}){renderer.m_cached_ctx.TEST.ZTST=f;unchanged(renderer,&rt,&texture,"no-ztest");}renderer.m_cached_ctx.TEST.ZTST=2;
        renderer.vertices.buff[0].XYZ.Z=0xFF00;unchanged(renderer,&rt,&texture,"hud-z");renderer.vertices.buff[0].XYZ.Z=0xFEFF;
        renderer.prim.FST=false;renderer.vertices.buff[1].RGBAQ.Q=0;unchanged(renderer,&rt,&texture,"q-varies");
        renderer.vertices.buff[1].RGBAQ.Q=2;unchanged(renderer,&rt,&texture,"q-varies");
        renderer.vertices.buff[1].RGBAQ.Q=std::numeric_limits<float>::infinity();unchanged(renderer,&rt,&texture,"q-varies");
        renderer.prim.FST=true;renderer.vertices.next=1;unchanged(renderer,&rt,&texture,"bad-vertices");renderer.vertices.next=0;unchanged(renderer,&rt,&texture,"bad-vertices");
    }
    else if(mode=="sampling")
    {
        const auto& p=ParticleEdges::Profiles::DoD_SLPM66275;
        assert(ParticleEdges::Inset(p,0,6)==0.5f&&ParticleEdges::Inset(p,1,6)==0.5f);
        assert(ParticleEdges::Inset(p,2,3)==4.0f&&ParticleEdges::Inset(p,2,7)==32.0f);
        int x0,x1;float u0,u1;
        ParticleEdges::TransformAxis(0,64,0,64,2,0.5f,x0,x1,u0,u1);
        assert(x0==0&&x1==64&&u0==0.5f&&std::abs(u1-(0.5f+47.5f*4/3.5f))<0.00001f);
        ParticleEdges::TransformAxis(64,0,64,0,2,0.5f,x0,x1,u0,u1);
        assert(x0==64&&x1==0&&u1==0.5f);
        ParticleEdges::TransformAxis(-17,-1,0,16,2,0.5f,x0,x1,u0,u1);
        assert(x0==-16&&x1==0);
        ParticleEdges::TransformAxis(7,7,1,2,2,0.5f,x0,x1,u0,u1);
        assert(x0==16&&x1==16&&u0==1&&u1==2);
    }
    else if(mode=="profile-data")
    {
        ParticleEdges::Draw absentFacts;
        absentFacts.hpo=5;
        reasonIs(ParticleEdges::DrawSkipReason(ParticleEdges::Profiles::DoD_SLPM66275,absentFacts),"not-sprite");
        auto own=ParticleEdges::Profiles::DoD_SLPM66275;
        own.id="synthetic-only";own.game="synthetic-game";own.revision="test-v2";
        own.hudZMinimum=0x1000;own.maximumMip=2;own.insetTexels=0.25f;
        assert(ParticleEdges::SupportedProfile(own));
        reasonIs(ParticleEdges::VertexSkipReason(own,0x2000,true,false,1,1),"hud-z");
        assert(ParticleEdges::VertexSkipReason(ParticleEdges::Profiles::DoD_SLPM66275,0x2000,true,false,1,1)==nullptr);
        assert(ParticleEdges::Inset(own,2,6)==1.0f);
        own.requiredHpo=4;assert(!ParticleEdges::SupportedProfile(own));
        own.requiredHpo=5;own.requireEqualQ=false;assert(!ParticleEdges::SupportedProfile(own));
        own.requireEqualQ=true;own.maximumMip=32;assert(!ParticleEdges::SupportedProfile(own));
        own.maximumMip=2;own.insetTexels=std::numeric_limits<float>::quiet_NaN();assert(!ParticleEdges::SupportedProfile(own));
        ParticleEdges::BeginSession("game_id=synthetic-game;game_revision=test-v2;particle_edges_profile=synthetic-only",nullptr,nullptr);
        unchanged(renderer,&rt,&texture,"profile-unknown"); // no inferred/registered profile
    }
    else if(mode=="legacy")
    {
        ParticleEdges::BeginSession(referenceOptions,nullptr,nullptr);
        uint32_t random=0x12345678u;
        auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
        int applied=0,fixedUv=0,stq=0,mipped=0,baseOnly=0;
        for(int n=0;n<12000;++n)
        {
            GSRendererHW old,current;
            std::array<GSVertex,4> input{};
            for(auto& v:input)
            {
                v.XYZ={static_cast<u16>(32000+next()%2048),static_cast<u16>(32000+next()%2048),next()%0x10000};
                v.U=static_cast<u16>(next()%16384);v.V=static_cast<u16>(next()%16384);
                v.ST={v.U/16.0f/256.0f,v.V/16.0f/512.0f};
            }
            const bool fst=next()&1,linear=(next()%4)!=0,zEnabled=(next()%5)!=0,fromTarget=(next()%9)==0;
            const int zFunction=next()%4,filter=next()%6,maxLevel=next()%8;
            rt.scale=static_cast<float>(2+next()%7);
            texture.m_from_target=fromTarget;
            for(auto* r:{&old,&current})
            {
                r->vertices.buff=input;r->vertices.next=(n&1)?2:4;
                r->prim.FST=fst;r->m_vt.linear=linear;
                r->context.XYOFFSET={32768,32768};r->context.TEX1={static_cast<u32>(filter),static_cast<u32>(maxLevel)};
                r->m_cached_ctx.TEST={zEnabled,zFunction};
            }
            const char* previous=old.LegacyParticleEdges(&texture,rt.scale);
            reasonIs(current.Draw(&rt,&texture),previous);
            if(std::strcmp(previous,"applied")==0)
            {
                ++applied;fixedUv+=fst;stq+=!fst;mipped+=filter>=2;baseOnly+=filter<2;
            }
            assert(std::memcmp(old.vertices.buff.data(),current.vertices.buff.data(),sizeof(input))==0);
            assert(old.m_r==current.m_r);
        }
        assert(applied>1000&&fixedUv>100&&stq>100&&mipped>100&&baseOnly>100);
        std::printf("legacy differential: 12000 draws, %d applied (%d FST, %d STQ, %d mip, %d base-only)\n",applied,fixedUv,stq,mipped,baseOnly);
    }
    else if(mode=="trace")
    {
        std::puts("SYNTHETIC ONLY: no game, GPU or visual acceptance");
        assert(BridgeOpen(nullptr));
        std::printf("unconfigured/HPO5: %s\n",renderer.Draw(&rt,&texture));BridgeClose();
        assert(BridgeOpen(referenceOptions));
        std::printf("explicit reference: %s\n",renderer.Draw(&rt,&texture));
        QueueOldCapture();BridgeClose();
        assert(BridgeOpen("game_id=onimusha3;game_revision=own-revision;particle_edges_profile=dod-slpm66275-native-grid-v1"));
        std::printf("other game/same HPO: %s; stale capture=%d\n",renderer.Draw(&rt,&texture),HasPendingCapture());BridgeClose();
        assert(BridgeOpen(referenceOptions,false)==0);
        std::printf("failed GS open: %s\n",ParticleEdges::SessionSkipReason());
    }
    else return 2;
}
