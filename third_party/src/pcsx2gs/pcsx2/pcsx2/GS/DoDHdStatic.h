#pragma once
// PC-only static page replacements. GS-thread only; RGB proof before every modified dirty upload.
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace DoDHdStatic {
struct Page {
    std::string id, hd_path;
    std::shared_ptr<const std::vector<u32>> preview;
    int top=0, field=-1; // -1 full; 0 even; 1 odd
    u64 hits=0;
    int alpha_compatible=0; // 0 unchecked, 1 exact, -1 incompatible
};
struct CachedTexture { GSTexture* texture=nullptr; std::vector<u8> alpha; u64 used=0; bool failed=false; };
inline std::vector<Page> pages;
inline std::unordered_map<std::string,std::shared_ptr<const std::vector<u32>>> previews;
inline std::unordered_map<std::string,CachedTexture> textures;
inline std::unordered_map<u32,size_t> last_page;
inline u64 texture_tick=0;
inline std::unordered_set<std::string> batch_pinned;
inline std::string capture_dir;
inline std::unordered_set<u64> captured;
inline u32 capture_count=0;
inline bool enabled=false;

inline void Clear() {
    for (auto& item:textures) if(item.second.texture && g_gs_device) g_gs_device->Recycle(item.second.texture);
    textures.clear(); previews.clear(); pages.clear(); last_page.clear(); batch_pinned.clear(); texture_tick=0;
    captured.clear(); capture_count=0; capture_dir.clear(); enabled=false;
}
inline bool SafeName(const std::string& s) {
    return !s.empty() && s.size()<240 && s.find("..") == std::string::npos &&
        s.find_first_of("\\/:\r\n") == std::string::npos;
}
inline bool LoadPng(const std::string& path, GSTextureReplacements::ReplacementTexture& out) {
    auto fp=FileSystem::OpenManagedCFile(path.c_str(),"rb");
    auto loader=GSTextureReplacements::GetLoader(path);
    if(!fp || !loader) return false;
    GSTextureReplacements::CFile file(fp.get());
    return loader(file,path.c_str(),&out,true) && out.format==GSTexture::Format::Color &&
        out.pitch==out.width*4 && out.data.size()==static_cast<size_t>(out.pitch)*out.height;
}
inline void Configure() {
    Clear();
    const char* cap=std::getenv("DOD_UI_CAPTURE_DIR");
    if(cap && *cap && FileSystem::EnsureDirectoryExists(cap,true)) capture_dir=cap;
    const char* env=std::getenv("DOD_UI_HD_DIR");
    const std::string program=FileSystem::GetProgramPath();
    const std::string root=(env && *env) ? std::string(env) : Path::Combine(Path::GetDirectory(program),"ui_hd");
    auto fp=FileSystem::OpenManagedCFile(Path::Combine(root,"pages.tsv").c_str(),"rb");
    if(fp) {
        char line[2048];
        while(pages.size()<2048 && std::fgets(line,sizeof(line),fp.get())) {
            if(!std::strchr(line,'\n') && !std::feof(fp.get())) { int ch; while((ch=std::fgetc(fp.get()))!='\n' && ch!=EOF) {} continue; }
            if(line[0]=='#' || line[0]=='\n' || line[0]=='\r') continue;
            std::string cols[5]; int col=0;
            for(const char* q=line;*q && *q!='\r' && *q!='\n';++q) {
                if(*q=='\t') { if(++col>=5) break; } else cols[col].push_back(*q);
            }
            if(col!=4 || !SafeName(cols[0]) || !SafeName(cols[1]) || !SafeName(cols[2])) continue;
            char* end=nullptr; long top=std::strtol(cols[3].c_str(),&end,10);
            if(cols[3].empty() || !end || *end || top<0 || top>64) continue;
            int field=cols[4]=="full" ? -1 : cols[4]=="even" ? 0 : cols[4]=="odd" ? 1 : -2;
            if(field==-2) continue;
            const std::string preview_path=Path::Combine(root,cols[1]);
            auto found=previews.find(preview_path);
            if(found==previews.end()) {
                GSTextureReplacements::ReplacementTexture image{};
                if(!LoadPng(preview_path,image) || image.width!=512 || image.height!=448) continue;
                auto pixels=std::make_shared<std::vector<u32>>(512u*448u);
                std::memcpy(pixels->data(),image.data.data(),image.data.size());
                found=previews.emplace(preview_path,std::move(pixels)).first;
            }
            Page p; p.id=cols[0];p.hd_path=Path::Combine(root,cols[2]);p.top=static_cast<int>(top);p.field=field;p.preview=found->second;
            // Exact duplicate mappings represent the same replacement, not an ambiguous collision.
            const auto duplicate=std::find_if(pages.begin(),pages.end(),[&](const Page& q) {
                return q.preview==p.preview && q.hd_path==p.hd_path && q.top==p.top && q.field==p.field;
            });
            if(duplicate==pages.end()) pages.push_back(std::move(p));
        }
    }
    enabled=!pages.empty() || !capture_dir.empty();
    if(enabled) std::fprintf(stderr,"[ui-hd] pages=%zu capture=%s\n",pages.size(),capture_dir.empty()?"off":capture_dir.c_str());
}
// All HD pixels must carry exactly the source page alpha, at native GS scale.
// This is checked once per preview/HD mapping, not during each draw.
inline bool CheckAlpha(Page& p,const std::vector<u8>& alpha) {
    if(p.alpha_compatible) return p.alpha_compatible>0;
    if(alpha.size()!=1536u*1344u) return false;
    for(int y=0;y<1344;++y) for(int x=0;x<1536;++x)
        if(alpha[static_cast<size_t>(y)*1536+x] != static_cast<u8>((*p.preview)[(y/3)*512+x/3]>>24)) {
            p.alpha_compatible=-1;std::fprintf(stderr,"[ui-hd] %s rejected: HD alpha differs from native preview\n",p.id.c_str());return false;
        }
    p.alpha_compatible=1;return true;
}
inline GSTexture* Texture(Page& p) {
    if(p.alpha_compatible<0) return nullptr;
    auto& entry=textures[p.hd_path];
    if(entry.texture) {
        if(!CheckAlpha(p,entry.alpha)) return nullptr;
        entry.used=++texture_tick;batch_pinned.insert(p.hd_path);return entry.texture;
    }
    if(entry.failed) return nullptr;
    // Never recycle a texture already referenced by an unsubmitted rectangle.
    size_t alive=0;for(const auto& item:textures) if(item.second.texture)++alive;
    if(alive>=4) {
        auto oldest=textures.end();
        for(auto it=textures.begin();it!=textures.end();++it)
            if(it->second.texture && batch_pinned.find(it->first)==batch_pinned.end() &&
               (oldest==textures.end() || it->second.used<oldest->second.used)) oldest=it;
        if(oldest==textures.end()) return nullptr;
        g_gs_device->Recycle(oldest->second.texture);oldest->second.texture=nullptr;oldest->second.alpha.clear();
    }
    GSTextureReplacements::ReplacementTexture im{};
    if(!g_gs_device || !LoadPng(p.hd_path,im) || im.width!=1536 || im.height!=1344) {entry.failed=true;return nullptr;}
    entry.alpha.resize(1536u*1344u);
    for(size_t i=0;i<entry.alpha.size();++i) entry.alpha[i]=im.data[i*4+3];
    // A file may have changed since eviction; re-check every mapping of this HD path.
    for(auto& other:pages) if(other.hd_path==p.hd_path) other.alpha_compatible=0;
    if(!CheckAlpha(p,entry.alpha)) {entry.alpha.clear();return nullptr;}
    entry.texture=g_gs_device->CreateTexture(im.width,im.height,1,GSTexture::Format::Color,true);
    if(entry.texture && !entry.texture->Update(GSVector4i(0,0,im.width,im.height),im.data.data(),im.pitch)) {
        g_gs_device->Recycle(entry.texture);entry.texture=nullptr;
    }
    if(!entry.texture) {entry.alpha.clear();entry.failed=true;return nullptr;}
    entry.used=++texture_tick;batch_pinned.insert(p.hd_path);return entry.texture;
}
inline void BeginBatch() { batch_pinned.clear(); }
inline void EndBatch() { batch_pinned.clear(); }
inline void Capture(const GSLocalMemory& mem,GIFRegTEX0 tex0,const GSDevice::MultiStretchRect* dirty,u32 n,float scale) {
    if(capture_dir.empty() || capture_count>=128 || !n) return;
    // Diagnostic only: deduplicate on sparse target pixels + rectangle, no work when opt-in is absent.
    u64 hash=1469598103934665603ull;
    auto add=[&](u64 v){ hash=(hash^v)*1099511628211ull; };
    add(tex0.TBP0);add(tex0.PSM);add(n);
    for(u32 i=0;i<n;++i) {
        const auto& r=dirty[i].dst_rect;
        add(static_cast<u32>(r.x/scale));add(static_cast<u32>(r.y/scale));add(static_cast<u32>(r.z/scale));add(static_cast<u32>(r.w/scale));add(dirty[i].wmask.key);
    }
    for(int y: {0,32,96,160,224,320,400,447,511}) for(int x: {11,83,163,251,347,433,497})
        add(mem.ReadPixel32(x,y,tex0.TBP0,tex0.TBW));
    if(!captured.insert(hash).second) return;
    std::vector<u32> pixels(512u*512u);
    for(int y=0;y<512;++y) for(int x=0;x<512;++x) pixels[y*512+x]=mem.ReadPixel32(x,y,tex0.TBP0,tex0.TBW);
    char stem[80];std::snprintf(stem,sizeof(stem),"capture_%03u_bp%04x",capture_count++,tex0.TBP0);
    const std::string png=Path::Combine(capture_dir,std::string(stem)+".png");
    GSTextureReplacements::SavePNGImage(png,512,448,reinterpret_cast<const u8*>(pixels.data()),512*4);
    GSTextureReplacements::SavePNGImage(Path::Combine(capture_dir,std::string(stem)+"_y64.png"),512,448,
        reinterpret_cast<const u8*>(pixels.data()+64u*512u),512*4);
    auto fp=FileSystem::OpenManagedCFile(Path::Combine(capture_dir,std::string(stem)+".txt").c_str(),"wb");
    if(fp) {
        std::fprintf(fp.get(),"TBP0=%x TBW=%u PSM=%u scale=%.4f png_y=0 second_png_y=64 height=448 dirty_count=%u\n",tex0.TBP0,tex0.TBW,tex0.PSM,scale,n);
        for(u32 i=0;i<n;++i) {const auto& r=dirty[i].dst_rect;
            std::fprintf(fp.get(),"dirty=%g,%g,%g,%g native=%g,%g,%g,%g wmask=%u\n",r.x,r.y,r.z,r.w,r.x/scale,r.y/scale,r.z/scale,r.w/scale,static_cast<unsigned>(dirty[i].wmask.key));}
    }
}
// Mutate source and UV only; caller submits the entire batch once, with original dst, wmask, filter and shader.
inline void Prepare(const GSLocalMemory& mem,GIFRegTEX0 tex0,float scale,
                    GSDevice::MultiStretchRect* dirty,u32 n,bool replace_enabled) {
    if(!enabled || !dirty || !n || scale<=0 || tex0.TBW!=8 ||
       (tex0.PSM!=PSMCT32 && tex0.PSM!=PSMCT24)) return;
    Capture(mem,tex0,dirty,n,scale);
    if(!replace_enabled || pages.empty()) return;
    for(u32 i=0;i<n;++i) {
        const auto& d=dirty[i].dst_rect;
        const float fx0=d.x/scale,fy0=d.y/scale,fx1=d.z/scale,fy1=d.w/scale;
        const int x0=static_cast<int>(std::round(fx0)),y0=static_cast<int>(std::round(fy0));
        const int x1=static_cast<int>(std::round(fx1)),y1=static_cast<int>(std::round(fy1));
        // Exact integer native boundaries; never replace only part of a dirty rectangle.
        if(std::abs(fx0-x0)>0.001f || std::abs(fy0-y0)>0.001f || std::abs(fx1-x1)>0.001f || std::abs(fy1-y1)>0.001f ||
           x0<0 || x1>512 || y0<0 || y1>512 || x1-x0<128 || y1-y0<8) continue;
        // Interior samples include letterbox art. Corner-heavy samples missed the title,
        // whose border and lower half are black. Every pixel is still proven below.
        std::array<int,64> xs{},ys{};std::array<u32,64> native{};std::array<u32,64> distinct_colors{};int distinct=0;
        for(int sy=0;sy<8;++sy) for(int sx=0;sx<8;++sx) {
            const int j=sy*8+sx;xs[j]=x0+(x1-x0)*(2*sx+1)/16;ys[j]=y0+(y1-y0)*(2*sy+1)/16;
            native[j]=mem.ReadPixel32(xs[j],ys[j],tex0.TBP0,tex0.TBW);
            const u32 rgb=native[j]&0x00ffffffu;
            if(std::find(distinct_colors.begin(),distinct_colors.begin()+distinct,rgb)==distinct_colors.begin()+distinct)
                distinct_colors[distinct++]=rgb;
        }
        if(distinct<2) continue; // blank regions carry no identity
        Page* selected=nullptr;bool ambiguous=false;
        const auto last=last_page.find(tex0.TBP0);
        const size_t preferred=last!=last_page.end() && last->second<pages.size() ? last->second : pages.size();
        for(size_t order=0;order<pages.size();++order) {
            const size_t pi=preferred==pages.size() ? order : (order==0 ? preferred : (order<=preferred ? order-1 : order));
            Page& p=pages[pi];const int rows=p.field<0 ? 448 : 224;
            if(y0<p.top || y1>p.top+rows) continue;
            auto expected=[&](int x,int y)->u32 {int yy=y-p.top;if(p.field>=0)yy=yy*2+p.field;return (*p.preview)[yy*512+x];};
            bool match=true;
            // Both formats compare raw RGBA: Target::Update reads PSMCT24 using a PSMCT32 offset.
            for(int j=0;j<64;++j) if(native[j]!=expected(xs[j],ys[j])) {match=false;break;}
            if(!match) continue;
            for(int y=y0;y<y1 && match;++y) for(int x=x0;x<x1;++x)
                if(mem.ReadPixel32(x,y,tex0.TBP0,tex0.TBW)!=expected(x,y)) {match=false;break;}
            if(!match) continue;
            if(selected) {ambiguous=true;break;}
            selected=&p;
        }
        if(ambiguous || !selected) continue;
        Page& p=*selected;
        // An odd field ending on native line 224 would exceed the source image aperture.
        if(p.field==1 && (y1-p.top)*2+1>448) continue;
        GSTexture* hd=Texture(p);if(!hd) continue;
        auto& rect=dirty[i];rect.src=hd;
        const float a=static_cast<float>(p.field<0?1:2), parity=static_cast<float>(std::max(0,p.field));
        rect.src_rect=GSVector4(x0/512.0f,((y0-p.top)*a+parity)/448.0f,x1/512.0f,((y1-p.top)*a+parity)/448.0f);
        last_page[tex0.TBP0]=static_cast<size_t>(selected-pages.data());
        if(++p.hits==1)std::fprintf(stderr,"[ui-hd] direct %s bp=%x rect=%d,%d,%d,%d field=%d\n",p.id.c_str(),tex0.TBP0,x0,y0,x1,y1,p.field);
    }
}
}
