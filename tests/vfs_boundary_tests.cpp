// Real VFS/ROM/path translation units; only synthetic temporary files are used.
#include "runtime/ps2_vfs.h"
#include "runtime/ps2_rom_device.h"
#include "runtime/ps2_memory.h"
#include <array>
#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
static void put(const std::filesystem::path& p,const std::string& data){std::ofstream f(p,std::ios::binary);f<<data;}
static std::string get(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){
 assert(argc==3);std::string mode=argv[1];std::filesystem::path root=argv[2];std::filesystem::create_directories(root);
 PS2Vfs vfs;PS2RomDevice rom;PS2VfsMounts mounts{root,root,root};std::array<char,8> data{};
 if(mode=="create-preserve"){
  put(root/"save","abcdef");auto fd=vfs.open("mc0:save",PS2_FIO_O_WRONLY|PS2_FIO_O_CREAT,mounts,rom);assert(fd>=3);
  assert(vfs.write(fd,"X",1)==1);assert(vfs.close(fd)==0);assert(get(root/"save")=="Xbcdef");
 }else if(mode=="create-readonly"){
  auto fd=vfs.open("host:new",PS2_FIO_O_RDONLY|PS2_FIO_O_CREAT,mounts,rom);assert(fd>=3);assert(vfs.read(fd,data.data(),1)==0);assert(vfs.write(fd,"X",1)==-1);assert(vfs.close(fd)==0);assert(get(root/"new").empty());
 }else if(mode=="missing"){
  for(auto flags:{PS2_FIO_O_WRONLY,PS2_FIO_O_WRONLY|PS2_FIO_O_APPEND,PS2_FIO_O_WRONLY|PS2_FIO_O_TRUNC,PS2_FIO_O_RDWR|PS2_FIO_O_APPEND}){
   assert(vfs.open("host:missing",flags,mounts,rom)==-1);assert(!std::filesystem::exists(root/"missing"));
  }
 }else if(mode=="permissions"){
  put(root/"save","abcdef");auto fd=vfs.open("host:save",PS2_FIO_O_WRONLY,mounts,rom);assert(fd>=3);
  assert(vfs.read(fd,data.data(),1)==-1);assert(vfs.write(fd,"X",1)==1);assert(vfs.close(fd)==0);
  fd=vfs.open("host:save",PS2_FIO_O_RDONLY,mounts,rom);assert(fd>=3);assert(vfs.write(fd,"Y",1)==-1);assert(vfs.read(fd,data.data(),1)==1&&data[0]=='X');assert(vfs.close(fd)==0);
 }else if(mode=="append"){
  put(root/"save","abc");auto fd=vfs.open("host:save",PS2_FIO_O_RDWR|PS2_FIO_O_APPEND,mounts,rom);assert(fd>=3);
  assert(vfs.seek(fd,0,SEEK_SET)==0);assert(vfs.write(fd,"X",1)==1);assert(vfs.seek(fd,0,SEEK_SET)==0);assert(vfs.read(fd,data.data(),4)==4);assert(std::string(data.data(),4)=="abcX");assert(vfs.close(fd)==0);
 }else if(mode=="truncate"){
  put(root/"save","abc");auto fd=vfs.open("host:save",PS2_FIO_O_WRONLY|PS2_FIO_O_TRUNC,mounts,rom);assert(fd>=3);assert(vfs.close(fd)==0);assert(get(root/"save").empty());
 }else if(mode=="exclusive"){
  put(root/"save","abc");assert(vfs.open("host:save",PS2_FIO_O_WRONLY|PS2_FIO_O_CREAT|PS2_FIO_O_EXCL|PS2_FIO_O_TRUNC,mounts,rom)==-1);assert(get(root/"save")=="abc");
  auto fd=vfs.open("host:new",PS2_FIO_O_RDWR|PS2_FIO_O_CREAT|PS2_FIO_O_EXCL,mounts,rom);assert(fd>=3);assert(vfs.close(fd)==0);
 }else if(mode=="exclusive-race"){
  constexpr unsigned count=12;std::array<int32_t,count> fds{};std::vector<std::thread> threads;std::atomic<bool> go=false;
  for(unsigned i=0;i<count;++i)threads.emplace_back([&,i]{while(!go.load()){}fds[i]=vfs.open("host:race",PS2_FIO_O_RDWR|PS2_FIO_O_CREAT|PS2_FIO_O_EXCL,mounts,rom);});
  go=true;for(auto& t:threads)t.join();unsigned success=0;for(auto fd:fds)if(fd>=3){++success;assert(vfs.close(fd)==0);}assert(success==1);
 }else if(mode=="rom-overflow"){
  auto fd=vfs.open("rom0:ROMVER",PS2_FIO_O_RDONLY,mounts,rom);assert(fd>=3);assert(vfs.seek(fd,1,SEEK_SET)==1);
  assert(vfs.seek(fd,INT64_MAX,SEEK_CUR)==-1);assert(vfs.seek(fd,INT64_MIN,SEEK_CUR)==-1);assert(vfs.seek(fd,0,SEEK_CUR)==1);
  assert(vfs.seek(fd,INT64_MAX,SEEK_END)==-1);assert(vfs.seek(fd,-14,SEEK_END)==0);assert(vfs.seek(fd,14,SEEK_SET)==14);assert(vfs.read(fd,data.data(),1)==0);assert(vfs.close(fd)==0);
 }else if(mode=="large-seek"){
  put(root/"sparse","");auto fd=vfs.open("host:sparse",PS2_FIO_O_RDWR,mounts,rom);assert(fd>=3);constexpr int64_t position=(int64_t(3)<<30)+17;
  assert(vfs.seek(fd,position,SEEK_SET)==position);assert(vfs.seek(fd,0,SEEK_CUR)==position); // no large allocation or write
  assert(vfs.seek(fd,-1,SEEK_SET)==-1);assert(vfs.seek(fd,0,999)==-1);assert(vfs.close(fd)==0);
 }else if(mode=="disc-readonly"){
  put(root/"asset","abc");for(auto flags:{PS2_FIO_O_RDWR,PS2_FIO_O_WRONLY|PS2_FIO_O_TRUNC,PS2_FIO_O_RDONLY|PS2_FIO_O_CREAT})assert(vfs.open("cdrom0:asset;1",flags,mounts,rom)==-1);
  assert(get(root/"asset")=="abc");auto fd=vfs.open("asset;1",PS2_FIO_O_RDONLY,mounts,rom);assert(fd>=3);assert(vfs.read(fd,data.data(),3)==3);assert(vfs.close(fd)==0);
  PS2VfsStat stat;assert(vfs.stat("cdrom:asset",mounts,rom,stat)&&stat.readOnly);
 }else if(mode=="versions"){
  using ps2x::iop::parsePs2Path;assert(parsePs2Path("host:save;1").path=="save;1");assert(parsePs2Path("mc0:save;1").path=="save;1");assert(parsePs2Path("cdrom0:asset;123").path=="asset");
  put(root/"save;1","correct");put(root/"save","wrong");auto fd=vfs.open("host:save;1",PS2_FIO_O_RDONLY,mounts,rom);assert(fd>=3);assert(vfs.read(fd,data.data(),7)==7&&std::string(data.data(),7)=="correct");assert(vfs.close(fd)==0);
 }else if(mode=="invalid"){
  assert(!ps2x::iop::parsePs2Path(std::string("host:a\0b",8)));assert(vfs.open("host:save",0,mounts,rom)==-1);
  assert(vfs.open("host:save",PS2_FIO_O_RDONLY|PS2_FIO_O_TRUNC,mounts,rom)==-1);std::filesystem::path path;
  assert(!vfs.resolveHostPath("host:../outside",mounts,path));assert(vfs.read(999,data.data(),1)==-1);
 }else if(mode=="mixed"){
  put(root/"save","abcdef");auto fd=vfs.open("host:save",PS2_FIO_O_RDWR,mounts,rom);assert(fd>=3);assert(vfs.read(fd,data.data(),1)==1&&data[0]=='a');assert(vfs.write(fd,"X",1)==1);assert(vfs.read(fd,data.data(),1)==1&&data[0]=='c');assert(vfs.close(fd)==0);assert(get(root/"save")=="aXcdef");
 }else if(mode=="unicode"){
  const auto name=std::filesystem::path(std::u8string(u8"存档.bin"));put(root/name,"abc");const auto u8name=name.generic_u8string();const std::string guest="host:"+std::string(u8name.begin(),u8name.end());
  auto fd=vfs.open(guest,PS2_FIO_O_RDONLY,mounts,rom);assert(fd>=3);assert(vfs.read(fd,data.data(),3)==3&&std::string(data.data(),3)=="abc");assert(vfs.close(fd)==0);
 }else return 2;
}
