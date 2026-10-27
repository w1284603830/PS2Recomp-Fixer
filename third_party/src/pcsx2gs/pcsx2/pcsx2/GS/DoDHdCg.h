#pragma once
// GS-thread-only HD CG frames. Replace a native framebuffer upload BEFORE the
// game's subtitle/UI draws, and only when its pixel signature matches the frame.
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/Common/GSDevice.h"
#include <deque>
#include <vector>
#include <cstring>
#include <cstdio>
#include <algorithm>

namespace DoDHdCg {
struct Frame {
    uint32_t header[16]{};
    std::vector<uint32_t> preview;
    GSTexture* texture=nullptr;
    uint64_t hits=0;
};
inline std::deque<Frame> frames;
inline uint64_t totalHits=0, totalReceived=0;
inline bool Active() { return !frames.empty(); }
inline void Clear() {
    if(!frames.empty()) std::fprintf(stderr,"[cg-hd-gs] clear received=%llu matched_uploads=%llu\n",totalReceived,totalHits);
    for(auto& f:frames) if(f.texture && g_gs_device) g_gs_device->Recycle(f.texture);
    frames.clear(); totalHits=totalReceived=0;
}
inline void Receive(const void* data,uint32_t bytes) {
    if(!data || !bytes) { Clear(); return; }
    if(bytes<64 || !g_gs_device) return;
    Frame f; std::memcpy(f.header,data,64);
    const auto* h=f.header;
    if(h[0]!=0x31474348 || h[3]!=512 || (h[4]!=288 && h[4]!=336) || h[10]!=1920 || h[11]!=1080 ||
       h[6]!=0 || h[8]!=1920 || h[9]==0 || h[7]+h[9]>1080) return;
    const size_t lowBytes=512u*(h[4]/2)*4u;
    if(bytes!=64+lowBytes+1920u*1080u*4u) return;
    if(!frames.empty() && (frames.back().header[1]!=h[1] || h[2]<=frames.back().header[2])) Clear();
    f.preview.resize(lowBytes/4);
    std::memcpy(f.preview.data(),static_cast<const uint8_t*>(data)+64,lowBytes);
    f.texture=g_gs_device->CreateTexture(1920,1080,1,GSTexture::Format::Color,true);
    if(!f.texture) return;
    if(!f.texture->Update(GSVector4i(0,0,1920,1080),static_cast<const uint8_t*>(data)+64+lowBytes,1920*4)) {
        g_gs_device->Recycle(f.texture); return;
    }
    frames.push_back(std::move(f)); ++totalReceived;
    while(frames.size()>6) { g_gs_device->Recycle(frames.front().texture); frames.pop_front(); }
}
inline void Upload(const GSLocalMemory& mem,GIFRegTEX0 tex0,GSTexture* target,float scale,
                   const GSDevice::MultiStretchRect* dirty,uint32_t n,ShaderConvertSelector shader) {
    if(frames.empty() || (tex0.TBP0!=0 && tex0.TBP0!=0x800) || tex0.TBW!=8 ||
       (tex0.PSM!=PSMCT32 && tex0.PSM!=PSMCT24)) return;
    for(auto it=frames.rbegin();it!=frames.rend();++it) {
        auto& f=*it; const int rows=f.header[4]/2;
        for(int top=0;top<=64;++top) {
            bool match=true;
            for(int y: {0,rows/3,rows*2/3,rows-1}) {
                for(int x: {17,83,163,251,347,433,497}) {
                    if((mem.ReadPixel32(x,top+y,tex0.TBP0,tex0.TBW)&0x00ffffffu)!=
                       (f.preview[y*512+x]&0x00ffffffu)) { match=false; break; }
                }
                if(!match) break;
            }
            if(!match) continue;
            std::vector<GSDevice::MultiStretchRect> rects;
            const GSVector4 aperture(0.0f,top*scale,512.0f*scale,(top+rows)*scale);
            for(uint32_t i=0;i<n;++i) {
                const auto& d=dirty[i].dst_rect;
                const GSVector4 dr(std::max(d.x,aperture.x),std::max(d.y,aperture.y),std::min(d.z,aperture.z),std::min(d.w,aperture.w));
                if(dr.z<=dr.x || dr.w<=dr.y) continue;
                auto r=dirty[i]; r.src=f.texture; r.dst_rect=dr; r.filter=Filter::Biln;
                const float x0=dr.x/scale/512.0f, x1=dr.z/scale/512.0f;
                const float y0=(dr.y/scale-top)/rows, y1=(dr.w/scale-top)/rows;
                r.src_rect=GSVector4(x0,(f.header[7]+y0*f.header[9])/1080.0f,x1,(f.header[7]+y1*f.header[9])/1080.0f);
                rects.push_back(r);
            }
            if(!rects.empty()) {
                g_gs_device->DrawMultiStretchRects(rects.data(),static_cast<u32>(rects.size()),target,shader);
                ++f.hits; ++totalHits;
                if(f.hits==1 && (f.header[2]<3 || f.header[2]%300==0))
                    std::fprintf(stderr,"[cg-hd-gs] movie=%u frame=%u bp=%x top=%d rows=%d scale=%.1f before_subtitles=1\n",
                        f.header[1],f.header[2],tex0.TBP0,top,rows,scale);
            }
            return;
        }
    }
}
}
