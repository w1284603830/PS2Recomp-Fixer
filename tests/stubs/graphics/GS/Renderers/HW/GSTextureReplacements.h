#pragma once
#include "GS/Renderers/Common/GSDevice.h"
#include <cstdio>
#include <map>
#include <string>
#include <vector>
namespace GSTextureReplacements{
struct ReplacementTexture{GSTexture::Format format=GSTexture::Format::Color;uint32_t pitch=0,width=0,height=0;std::vector<uint8_t>data;};
struct CFile{explicit CFile(FILE*){}};
inline std::map<std::string,ReplacementTexture> fixtures;
inline auto GetLoader(const std::string&)->bool(*)(CFile&,const char*,ReplacementTexture*,bool){return [](CFile&,const char*p,ReplacementTexture*out,bool){auto it=fixtures.find(p);if(it==fixtures.end())return false;*out=it->second;return true;};}
inline bool SavePNGImage(const std::string&,uint32_t,uint32_t,const uint8_t*,uint32_t){return true;}
}
