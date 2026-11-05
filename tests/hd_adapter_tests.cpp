#include "GS/DoDHdCg.h"
#include "GS/DoDHdStatic.h"
#include <array>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
static std::vector<uint32_t> packet(){
 std::vector<uint32_t> p(16+512*144+1920*1080,0xff112233);p[0]=0x31474348;p[1]=1;p[2]=1;p[3]=512;p[4]=288;p[6]=0;p[7]=0;p[8]=1920;p[9]=1080;p[10]=1920;p[11]=1080;
 // Distinct identity at each native pixel, not a blank field.
 for(unsigned y=0;y<144;++y)for(unsigned x=0;x<512;++x)p[16+y*512+x]=0xff000000u|(y*512+x);
 return p;
}
int main(int argc,char**argv){
 assert(argc>=2);const std::string mode=argv[1];
 if(mode=="cg-match"||mode=="cg-collision"){
  auto p=packet();DoDHdCg::Receive(p.data(),p.size()*4);assert(DoDHdCg::Active());
  GSLocalMemory mem;for(unsigned y=0;y<144;++y)for(unsigned x=0;x<512;++x)mem.pixels[y*1024+x]=p[16+y*512+x];
  GSTexture target;GSDevice::MultiStretchRect rect;rect.dst_rect={0,0,1024,288};
  if(mode=="cg-collision")mem.pixels[1*1024+1]^=1; // old sparse samples never checked this pixel
  DoDHdCg::Upload(mem,{0x321,12,PSMCT32},&target,2,&rect,1,ShaderConvertSelector::Copy);
  assert(g_gs_device->draws==(mode=="cg-match"?1u:0u));
  if(mode=="cg-match")assert(g_gs_device->last[0].wmask.key==15&&g_gs_device->last[0].src!=rect.src);
  DoDHdCg::Clear();assert(!DoDHdCg::Active());
 }else if(mode=="cg-uniform"){
  auto p=packet();std::fill(p.begin()+16,p.begin()+16+512*144,0xff000000);DoDHdCg::Receive(p.data(),p.size()*4);assert(!DoDHdCg::Active());
 }else if(mode=="cg-ambiguous"){
  auto p=packet();DoDHdCg::Receive(p.data(),p.size()*4);p[2]=2;p.back()^=1;DoDHdCg::Receive(p.data(),p.size()*4);assert(DoDHdCg::frames.size()==2);
  GSLocalMemory mem;for(unsigned y=0;y<144;++y)for(unsigned x=0;x<512;++x)mem.pixels[y*1024+x]=p[16+y*512+x];
  GSTexture target;GSDevice::MultiStretchRect rect;rect.dst_rect={0,0,1024,288};DoDHdCg::Upload(mem,{0,8,0},&target,2,&rect,1,ShaderConvertSelector::Copy);assert(!g_gs_device->draws);DoDHdCg::Clear();
 }else if(mode=="static-nonfinite"){
  DoDHdStatic::enabled=true;DoDHdStatic::Page page;page.preview=std::make_shared<const std::vector<u32>>(512*448,123);DoDHdStatic::pages.push_back(page);GSLocalMemory mem;GSDevice::MultiStretchRect rect;rect.dst_rect={0,0,512,448};auto* original=rect.src;
  for(float scale:{0.0f,-1.0f,std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()})DoDHdStatic::Prepare(mem,{0,8,0},scale,&rect,1,true);
  for(float x:{std::numeric_limits<float>::quiet_NaN(),-std::numeric_limits<float>::infinity(),1e30f}){rect.dst_rect.x=x;DoDHdStatic::Prepare(mem,{0,8,0},1,&rect,1,true);}
  assert(rect.src==original);DoDHdStatic::Clear();
 }else if(mode=="cg-bounds"){
  auto p=packet();p[7]=UINT32_MAX;p[9]=2;DoDHdCg::Receive(p.data(),p.size()*4);assert(!DoDHdCg::Active());
  p=packet();DoDHdCg::Receive(p.data(),63);assert(!DoDHdCg::Active());DoDHdCg::Receive(p.data(),p.size()*4);assert(DoDHdCg::Active());
  GSLocalMemory mem;GSTexture target;GSDevice::MultiStretchRect rect;rect.dst_rect={0,0,512,144};
  for(float scale:{0.0f,-1.0f,std::numeric_limits<float>::infinity()})DoDHdCg::Upload(mem,{0,8,0},&target,scale,&rect,1,ShaderConvertSelector::Copy);
  DoDHdCg::Upload(mem,{0,7,0},&target,1,&rect,1,ShaderConvertSelector::Copy);assert(!g_gs_device->draws);DoDHdCg::Receive(nullptr,0);assert(!DoDHdCg::Active());
 }else if(mode=="static-selection"){
  assert(argc==3);auto root=std::filesystem::path(argv[2]);std::filesystem::create_directories(root);
  std::ofstream(root/"pages.tsv")<<"page\tpreview.png\thd.png\t0\tfull\n";std::ofstream(root/"preview.png")<<"fixture";
  GSTextureReplacements::ReplacementTexture preview;preview.width=512;preview.height=448;preview.pitch=2048;preview.data.resize(512*448*4);
  GSTextureReplacements::fixtures[(root/"preview.png").string()]=preview;
  GSHostTuning::portOptions.staticPageDirectory=root.string();DoDHdStatic::Configure();assert(DoDHdStatic::pages.empty());
  GSHostTuning::portOptions.staticPageFormat="unknown";DoDHdStatic::Configure();assert(DoDHdStatic::pages.empty());
  GSHostTuning::portOptions.staticPageFormat="page-rgba-512x448-x3-v1";DoDHdStatic::Configure();assert(DoDHdStatic::pages.size()==1);
  GSHostTuning::ResetPortOptions();DoDHdStatic::Configure();assert(!DoDHdStatic::enabled&&DoDHdStatic::pages.empty());
 }else return 2;
}
