#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
using u8=uint8_t;using u32=uint32_t;
namespace GSPng {enum Format{RGB_PNG,RGBA_PNG,RGB_A_PNG,R8I_PNG};bool Save(Format,const std::string&,const u8*,int,int,int,int,bool);}
bool g_dodDumpPng=true;
struct Saved{std::string path;u32 w,h;std::vector<u8> bytes;};std::vector<Saved> saved;int failAt=-1;
namespace GSTextureReplacements {bool SavePNGImage(std::string path,u32 w,u32 h,const u8* pixels,u32 pitch){saved.push_back({path,w,h,{pixels,pixels+pitch*h}});return static_cast<int>(saved.size())!=failAt;}}
#include "png_capture_current.inc"
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];
 const u8 image[24]={1,2,3,4,5,6,7,8,0xa5,0xa5,0xa5,0xa5,9,10,11,12,13,14,15,16,0xa5,0xa5,0xa5,0xa5};
 if(mode=="alpha"){
  assert(GSPng::Save(GSPng::RGB_A_PNG,"capture.png",image,2,2,12,0,false));
  assert(saved.size()==2&&saved[0].path=="capture.png"&&saved[1].path=="capture_alpha.png");
  assert((saved[0].bytes==std::vector<u8>{1,2,3,255,5,6,7,255,9,10,11,255,13,14,15,255}));
  assert((saved[1].bytes==std::vector<u8>{4,4,4,255,8,8,8,255,12,12,12,255,16,16,16,255}));
 }else if(mode=="formats"){
  assert(GSPng::Save(GSPng::RGBA_PNG,"dir/a.bmp",image,2,2,12,0,true));assert(saved.size()==1&&saved[0].path=="dir/a.png"&&saved[0].bytes[0]==3&&saved[0].bytes[3]==4&&saved[0].bytes[8]==11);
  saved.clear();assert(GSPng::Save(GSPng::R8I_PNG,"mask.png",image,2,2,12,0,false));assert(saved.size()==1&&saved[0].bytes[4]==2&&saved[0].bytes[8]==9);
 }else if(mode=="failure"){
  failAt=1;assert(!GSPng::Save(GSPng::RGB_A_PNG,"a",image,2,2,12,0,false));assert(saved.size()==2);
  saved.clear();failAt=2;assert(!GSPng::Save(GSPng::RGB_A_PNG,"a",image,2,2,12,0,false));
 }else if(mode=="bounds"){
  for(int pitch:{-1,0,1,7})assert(!GSPng::Save(GSPng::RGBA_PNG,"a",image,2,2,pitch,0,false));
  assert(!GSPng::Save(GSPng::RGBA_PNG,"a",nullptr,2,2,12,0,false));assert(!GSPng::Save(GSPng::RGBA_PNG,"a",image,0,2,12,0,false));assert(saved.empty());
 }else return 2;
}
