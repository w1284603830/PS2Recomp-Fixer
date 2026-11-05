// Compile the actual runner preflight/INI/relative-path bodies with a recording
// initialization boundary. This does not build generated game code or a window.
#include "render_profile.h"
#include <cassert>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
static unsigned initialized=0;
#include "dod_runner_helpers_current.inc"
static int preflight(const fs::path& exeDir,const fs::path& elf,const dod_reference::Settings& ini)
{
#include "dod_runner_preflight_current.inc"
    ++initialized; // the real main reaches runtime/window initialization only after this block
    return 0;
}
int main(int argc,char**argv){
 assert(argc==3);std::string mode=argv[1];const fs::path root=argv[2];fs::create_directories(root);const auto elf=root/"renamed.elf";
 std::array<unsigned char,52> bytes{};const unsigned char ident[]={0x7f,'E','L','F',1,1,1};std::copy(std::begin(ident),std::end(ident),bytes.begin());bytes[16]=2;bytes[18]=8;
 {std::ofstream f(elf,std::ios::binary);f.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
 const auto crc=dod_reference::elfCrc(elf);assert(crc);char checksum[9];std::snprintf(checksum,sizeof(checksum),"%08x",*crc);
 dod_reference::Settings settings{{"confirmed_game_revision","SLPM_662.75"},{"confirmed_elf_crc32",checksum}};
 if(mode=="refusal"){
  assert(preflight(root,elf,{{"reference_particles","1"}})==2&&!initialized);
  settings["ui_hd_dir"]="missing";assert(preflight(root,elf,settings)==2&&!initialized);
 }else if(mode=="ini-success"){
  fs::create_directories(root/"ui");std::ofstream(root/"ui/pages.tsv")<<"# synthetic empty manifest\n";
  std::ofstream(root/"dod.ini",std::ios::binary)<<"\xef\xbb\xbf# synthetic config\nconfirmed_game_revision=SLPM_662.75\nconfirmed_elf_crc32="<<checksum<<"\nreference_particles=1\nui_hd_dir=ui\ngs_options=blending=3;texture_filtering=2\n";
  settings=readIni(root/"dod.ini");assert(settings["gs_options"]=="blending=3;texture_filtering=2");assert(preflight(root,elf,settings)==0&&initialized==1);
 }else if(mode=="environment-refusal"){
  assert(preflight(root,elf,{})==2&&!initialized); // explicit env profile cannot bypass revision/CRC confirmation
 }else return 2;
}
