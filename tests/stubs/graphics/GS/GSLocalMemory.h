#pragma once
#include <cstdint>
#include <vector>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
constexpr int PSMCT32=0,PSMCT24=1;
struct GIFRegTEX0{u32 TBP0=0,TBW=8,PSM=0;};
struct GSLocalMemory{std::vector<u32> pixels=std::vector<u32>(1024*512);u32 ReadPixel32(int x,int y,u32,u32)const{return pixels.at(y*1024+x);}};
