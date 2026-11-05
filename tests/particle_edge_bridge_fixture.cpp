// Compiles the production Bridge Open/Close/reset bodies against recording GS
// services. No window, GPU, game data or commercial assets are loaded.
#include "particle_edge_renderer_fixture.h"
#include "pcsx2gs_api.h"
#include "GS/HostTuning.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <utility>

struct WindowInfo
{
    enum class Type {Win32};
    Type type{}; void* window_handle=nullptr;
    uint32_t surface_width=0,surface_height=0;
};
enum class GSRendererType {DX11};
enum class AspectRatioType {Stretch,R4_3,R16_9};
enum class GSVSyncMode {FIFO,Mailbox,Disabled};
struct Pcsx2Config
{
    struct GSOptions {GSRendererType Renderer{};float UpscaleMultiplier=1;AspectRatioType AspectRatio{};int MSAASamples=1;} GS;
    AspectRatioType CurrentAspectRatio{};
};
Pcsx2Config EmuConfig;
namespace pcsx2gs {std::string folder,title;void SetTextureFolder(const std::string& v){folder=v;} void SetGameTitle(const std::string& v){title=v;} void SetRenderWindow(WindowInfo){} void SetFolders(const char*,const char*){}}
namespace DoDHdStatic {void Configure(){} void Clear(){}}
namespace DoDHdCg {void Clear(){}}
struct FakePointer {void* release(){return nullptr;}};
FakePointer g_gs_renderer,g_texture_cache,g_gs_device;
bool g_open=false,g_dodWidescreen=false,backendSucceeds=true,g_dodExclusiveFullscreen=false,g_dodDumpPng=false;
long long g_dumpField=-1,g_fields=0;int g_dumpFrames=1;unsigned long long g_dodGsField=0;
WindowInfo g_window;
unsigned char g_regs[0x2000]{};
std::mutex g_dodDumpMutex;
std::string g_dodDumpDir;
bool g_dodDumpPending=false;
uint32_t g_dodDumpFlags=0;
int g_dodDumpFrames=1;
void ApplyGameFixes(Pcsx2Config::GSOptions&){}
void ApplyOptions(Pcsx2Config::GSOptions&,const char*){} // unrelated image option parser
bool GSopen(Pcsx2Config::GSOptions,GSRendererType,unsigned char*,GSVSyncMode,bool){return backendSucceeds;}
void GSclose(){}
void GSgetInternalResolution(int* w,int* h){*w=640;*h=448;}
template<class F> class ScopedGuard
{
    F function; bool enabled=true;
public:
    explicit ScopedGuard(F f):function(std::move(f)){}
    ~ScopedGuard(){if(enabled)function();}
    void Cancel(){enabled=false;}
};
#include "particle_bridge_current.inc"

int BridgeOpen(const char* text,bool succeeds,bool validWindow)
{
    backendSucceeds=succeeds;
    Pcsx2GsOpenParams p{};
    p.hwnd=validWindow?reinterpret_cast<void*>(1):nullptr;
    p.width=640;p.height=448;p.scale=2;p.options=text;
    return Open(&p);
}
int BridgeNullOpen(){return Open(nullptr);}
void BridgeClose(){Close();}
bool BridgeIsOpen(){return g_open;}
void QueueOldCapture()
{
    g_dodDumpPending=true;g_dodDumpFlags=2;g_dodDumpDir="synthetic";g_dodDumpFrames=2;
    s_dod_ab_off_frame=7;s_dod_draw_last_frame=8;
    s_dod_draw_frames={1};s_dod_sprites_before={1};s_dod_draw_summary="old";
}
bool HasPendingCapture(){return g_dodDumpPending||g_dodDumpFlags||!g_dodDumpDir.empty()||g_dodDumpFrames!=1;}

void DirtyPortState()
{
    GSHostTuning::portOptions.feedbackCopyGuard=true;
    GSHostTuning::portOptions.clampFootprint=true;
    GSHostTuning::portOptions.hdMinimumScale=7;
    GSHostTuning::portOptions.staticPageFormat="old";
    pcsx2gs::folder="old pack";pcsx2gs::title="old title";
    g_dodWidescreen=g_dodExclusiveFullscreen=g_dodDumpPng=true;
    g_dumpField=99;g_fields=40;g_dumpFrames=8;g_dodGsField=42;
}
bool PortStateClean()
{
    return !GSHostTuning::portOptions.feedbackCopyGuard && !GSHostTuning::portOptions.clampFootprint &&
        GSHostTuning::portOptions.hdMinimumScale==1 && GSHostTuning::portOptions.staticPageFormat.empty() &&
        pcsx2gs::folder.empty() && pcsx2gs::title.empty() && !g_dodWidescreen && !g_dodExclusiveFullscreen &&
        !g_dodDumpPng && g_dumpField==-1 && !g_fields && g_dumpFrames==1 && !g_dodGsField;
}
