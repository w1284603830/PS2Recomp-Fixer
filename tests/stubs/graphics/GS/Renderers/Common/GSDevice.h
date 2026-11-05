#pragma once
#include <memory>
#include <vector>
#include <cstdint>
struct GSVector4{float x=0,y=0,z=0,w=0;GSVector4()=default;GSVector4(float a,float b,float c,float d):x(a),y(b),z(c),w(d){}};
struct GSVector4i{int x,y,z,w;GSVector4i(int a,int b,int c,int d):x(a),y(b),z(c),w(d){}};
enum class Filter{Biln};enum class ShaderConvertSelector{Copy};
struct GSTexture{enum class Format{Color};bool Update(GSVector4i,const void*,uint32_t){return true;}};
struct GSDevice{
 struct MultiStretchRect{GSTexture*src=nullptr;GSVector4 dst_rect,src_rect;Filter filter{};struct{uint32_t key=15;}wmask;};
 unsigned draws=0,recycled=0;std::vector<MultiStretchRect> last;
 GSTexture* CreateTexture(int,int,int,GSTexture::Format,bool){return new GSTexture;}
 void Recycle(GSTexture*p){++recycled;delete p;}
 void DrawMultiStretchRects(const MultiStretchRect*r,uint32_t n,GSTexture*,ShaderConvertSelector){++draws;last.assign(r,r+n);}
};
inline std::unique_ptr<GSDevice> g_gs_device=std::make_unique<GSDevice>();
