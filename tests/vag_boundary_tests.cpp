#include "runtime/ps2_audio.h"
#include "runtime/ps2_memory.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
namespace ps2_vag{bool decode(const uint8_t*,uint32_t,std::vector<int16_t>&,uint32_t&);}
static void be32(uint8_t*p,uint32_t v){for(int i=0;i<4;++i)p[i]=uint8_t(v>>(24-i*8));}
static std::vector<uint8_t> vag(uint32_t payload=16){std::vector<uint8_t>d(48+payload);std::memcpy(d.data(),"VAGp",4);be32(d.data()+12,payload);be32(d.data()+16,22050);return d;}
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];std::vector<int16_t> pcm{123};uint32_t rate=123;
 if(mode=="header-bounds"){
  auto d=vag();for(uint32_t n:{17u,32u,0xfffffff0u,0xffffffffu}){be32(d.data()+12,n);assert(!ps2_vag::decode(d.data(),d.size(),pcm,rate));assert(pcm==std::vector<int16_t>{123}&&rate==123);}
 }else if(mode=="complete"){
  auto d=vag();d[48]=0;d[50]=0x8f;assert(ps2_vag::decode(d.data(),d.size(),pcm,rate));assert(rate==22050&&pcm.size()==28&&pcm[0]==-4096&&pcm[1]==-32768);
  std::reverse(d.begin(),d.begin()+4);assert(ps2_vag::decode(d.data(),d.size(),pcm,rate)&&pcm[0]==-4096); // retained legacy magic tolerance, BE fields
 }else if(mode=="partial"){
  auto d=vag();for(unsigned n=0;n<48;++n)assert(!ps2_vag::decode(d.data(),n,pcm,rate));assert(!ps2_vag::decode(nullptr,64,pcm,rate));
  be32(d.data()+12,15);assert(!ps2_vag::decode(d.data(),d.size(),pcm,rate));
 }else if(mode=="rate-empty"){
  auto d=vag(0);be32(d.data()+16,0);assert(ps2_vag::decode(d.data(),d.size(),pcm,rate)&&pcm.empty()&&rate==44100);
 }else if(mode=="transfer-overflow"){
  std::vector<uint8_t> ram(PS2_RAM_SIZE);auto d=vag();std::memcpy(ram.data()+PS2_RAM_SIZE-48,d.data(),48);
  PS2AudioBackend audio;audio.onVagTransfer(ram.data(),PS2_RAM_SIZE-48,0xffffffffu);audio.onVagTransfer(ram.data(),PS2_RAM_SIZE-48,64); // no decoder access beyond RAM
 }else return 2;
}
