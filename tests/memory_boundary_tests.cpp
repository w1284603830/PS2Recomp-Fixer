#include "runtime/ps2_memory.h"
#include "runtime/ps2_address.h"
#include "ps2x/iop/iop_types.h"
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
struct R5900Context{std::array<uint32_t,32> regs{};int32_t result=-99;};
uint32_t getRegU32(R5900Context*c,unsigned n){return c->regs[n];}
void setReturnS32(R5900Context*c,int32_t v){c->result=v;}
void ps2TraceGuestRangeWrite(uint8_t*,uint32_t,uint32_t,const char*,void*){}
struct PS2Runtime{
 std::vector<uint8_t> iop=std::vector<uint8_t>(65536);unsigned writes=0;
 bool isIopMemoryRange(uint32_t a,uint32_t n){return a<=iop.size()&&n<=iop.size()-a;}
 bool writeIopMemory(uint32_t a,const void*p,size_t n){if(!isIopMemoryRange(a,n))return false;std::memcpy(iop.data()+a,p,n);++writes;return true;}
 bool readIopMemory(uint32_t a,void*p,size_t n){if(!isIopMemoryRange(a,n))return false;std::memcpy(p,iop.data()+a,n);return true;}
};
struct PS2IopTransport{static void notifyTransfer(PS2Runtime*,uint8_t*,const ps2x::iop::SifTransfer&) {}};
namespace ps2_syscalls{unsigned interrupts=0;void dispatchDmacHandlersForCause(uint8_t*,PS2Runtime*,unsigned){++interrupts;}}
uint32_t allocateSifDmaTransferId(){return 7;}
struct Ps2SifDmaTransfer{uint32_t src,dest;int32_t size,attr;};
#define PS2_IF_AGRESSIVE_LOGS(...) do{}while(false)
#include "memory_hle_current.inc"
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];std::vector<uint8_t> ram(PS2_RAM_SIZE,0x7e),scratch(PS2_SCRATCHPAD_SIZE,0x55);ps2SetScratchpadHostPtr(scratch.data());auto* p=ram.data();
 if(mode=="segments"){
  for(uint32_t a:{0x40000000u,0x60000000u,0x70004000u,0xc0000000u,0xffffffffu}){assert(!getMemPtr(p,a)&&!getConstMemPtr(p,a));}
  for(uint32_t a:{0u,0x20000000u,0x80000000u,0xa0000000u}){assert(getMemPtr(p,a+123)==p+123);assert(getMemPtr(p,a+PS2_RAM_SIZE-4,4)==p+PS2_RAM_SIZE-4);assert(!getMemPtr(p,a+PS2_RAM_SIZE-4,5));}
  assert(getMemPtr(p,0x70000000)==scratch.data()&&getMemPtr(p,0xf0000000)==scratch.data());assert(!getMemPtr(nullptr,0,1));
 }else if(mode=="spans"){
  assert(getMemPtr(p,PS2_RAM_SIZE-1,1)==p+PS2_RAM_SIZE-1);assert(!getMemPtr(p,PS2_RAM_SIZE-1,2));
  assert(!getConstMemPtr(p,0x70003fff,2));assert(!getConstMemPtr(p,0xf0003fff,SIZE_MAX));
  ps2SetScratchpadHostPtr(nullptr);assert(!getMemPtr(p,0x70000000,4));
 }else if(mode=="vu"){
  float v[4]={1,2,3,4},out[4]={};float matrix[16]={};
  assert(writeVuVec4f(p,PS2_RAM_SIZE-16,v));assert(readVuVec4f(p,PS2_RAM_SIZE-16,out));assert(!std::memcmp(v,out,sizeof(v)));
  assert(!writeVuVec4f(p,PS2_RAM_SIZE-15,v));assert(!readVuMatrix4f(p,0x70003ff0,matrix));assert(!writeVuMatrix4f(p,0x70003ff0,matrix));
  std::array<uint8_t,32> original{};for(unsigned i=0;i<32;++i)p[0x100+i]=i;std::memcpy(original.data(),p+0x100,32);
  R5900Context c;c.regs[4]=0x104;c.regs[5]=0x100;sceVu0CopyVector(p,&c,nullptr);assert(!std::memcmp(p+0x104,original.data(),16));
  c.regs[4]=PS2_RAM_SIZE-4;sceVu0CopyMatrix(p,&c,nullptr); // invalid span: no out-of-bounds write
 }else if(mode=="gs"){
  GsImageMem image{};assert(writeGsImage(p,PS2_RAM_SIZE-sizeof(image),image));assert(!writeGsImage(p,PS2_RAM_SIZE-1,image));assert(!readGsImage(p,0x70003fff,image));
  GsDispEnvMem env{};assert(writeGsDispEnv(p,0x70004000-sizeof(env),123,456));assert(readGsDispEnv(p,0x70004000-sizeof(env),env)&&env.display==123&&env.dispfb==456);
  assert(!writeGsDispEnv(p,0x70003fff,1,2));GsRegPairMem pairs[2]{};
  assert(!readGsRegPairs(p,PS2_RAM_SIZE-8,pairs,2));assert(!readGsRegPairs(p,0x100,pairs,SIZE_MAX));
 }else if(mode=="mc-string"){
  const std::string value="abc";writeMcCString(p,PS2_RAM_SIZE-4,value);assert(!std::memcmp(p+PS2_RAM_SIZE-4,"abc\0",4));
  const auto before=scratch;writeMcCString(p,0x70003fff,value);assert(scratch==before);writeMcCString(p,0xffffffff,value);
 }else if(mode=="sif-mirror"){
  const uint32_t address=PS2_RAM_SIZE-8;Ps2SifDmaTransfer d{0x100,64,4,0};const auto* src=reinterpret_cast<const uint8_t*>(&d);
  for(unsigned i=0;i<sizeof(d);++i)*getMemPtr(p,address+i)=src[i];std::memcpy(p+0x100,"DATA",4);
  PS2Runtime rt;R5900Context c;c.regs[4]=address;c.regs[5]=1;sceSifSetDma(p,&c,&rt);
  assert(c.result==7&&rt.writes==1&&ps2_syscalls::interrupts==1&&!std::memcmp(rt.iop.data()+64,"DATA",4));
 }else if(mode=="sif-scratch-invalid"){
  PS2Runtime rt;R5900Context c;c.regs[4]=0x70003ff8;c.regs[5]=1;sceSifSetDma(p,&c,&rt);assert(c.result==0&&!rt.writes&&!ps2_syscalls::interrupts);
 }else if(mode=="sif-metadata"){
  PS2Runtime rt;std::memcpy(rt.iop.data()+64,"DATA",4);R5900Context c;c.regs[4]=0;c.regs[5]=64;c.regs[6]=0x100;c.regs[7]=4;
  sceSifGetOtherData(p,&c,&rt);assert(c.result==0);for(unsigned i=16;i<28;++i)assert(p[i]==0x7e);
  c.regs[4]=PS2_RAM_SIZE-20;sceSifGetOtherData(p,&c,&rt);assert(c.result==0);uint32_t destination=0;assert(readEeRange(p,c.regs[4]+0x14,&destination,4));assert(destination==0x100);
  c.regs[4]=0x70003fff;c.result=-99;sceSifGetOtherData(p,&c,&rt);assert(c.result==-1);
 }else return 2;
 ps2SetScratchpadHostPtr(nullptr);
}
