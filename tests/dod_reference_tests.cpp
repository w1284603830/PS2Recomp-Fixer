#include "render_profile.h"
#include "GS/ParticleEdges.h"
#include <cassert>
#include <iostream>
#include <string>
static dod_reference::Settings confirmed(){return {{"confirmed_game_revision","SLPM_662.75"},{"confirmed_elf_crc32","cbf43926"}};}
static auto build(dod_reference::Settings s={},std::string_view env="",std::optional<uint32_t> crc=0xcbf43926,std::string_view ui=""){
 return dod_reference::build(s,env,crc,ui);
}
int main(int argc,char**argv){
 assert(argc>=2);dod_reference::Settings argvCheck;assert(dod_reference::applyArguments(argvCheck,1,argv).empty());std::string mode=argv[1];
 if(mode=="default"){
  auto r=build();assert(r&&!r.particleSelected&&!r.staticPages&&!r.hdScaling);
  assert(r.options.find("blending=2;half_pixel_offset=5;native_scaling=2;bilinear_upscale=2")==0);
  ParticleEdges::BeginSession(r.options.c_str(),nullptr,nullptr);assert(!ParticleEdges::session.profile);
  assert(r.options.find("static_page_format")==std::string::npos&&r.options.find("hd_frame_min_scale")==std::string::npos);
 }else if(mode=="particle"){
  auto s=confirmed();s["reference_particles"]="1";auto r=build(s);assert(r&&r.particleSelected);
  ParticleEdges::BeginSession(r.options.c_str(),nullptr,nullptr);assert(ParticleEdges::session.profile);
  ParticleEdges::BeginSession(r.options.c_str(),"0","1");assert(ParticleEdges::SessionSkipReason()&&std::string(ParticleEdges::SessionSkipReason())=="env-off");
 }else if(mode=="unconfirmed"){
  for(const std::string revision:{"","SLPM_662.76","onimusha3"}){auto s=confirmed();s["reference_particles"]="1";s["confirmed_game_revision"]=revision;assert(!build(s));}
  auto s=confirmed();s["reference_particles"]="1";assert(!build(s,"",0x12345678));assert(!build(s,"",std::nullopt));s["confirmed_elf_crc32"]="0xcbf43926";assert(!build(s));
 }else if(mode=="raw"){
  auto s=confirmed();s["gs_options"]="game_id=onimusha-dawn-of-dreams;game_revision=SLPM_662.75;particle_edges_profile=dod-slpm66275-native-grid-v1";
  assert(build(s).particleSelected);s.erase("confirmed_game_revision");assert(!build(s));s=confirmed();s["gs_options"]="particle_edges_profile=off";assert(build(s)&&!build(s).particleSelected);
 }else if(mode=="conflicts"){
  auto s=confirmed();s["reference_particles"]="1";assert(!build(s,"particle_edges_profile=off"));s["reference_particles"]="0";
  s["gs_options"]="game_revision=SLPM_662.75";assert(!build(s," game_revision = SLPM_662.75 "));
  s["gs_options"]="game_id=onimusha3;game_revision=SLPM_662.75;particle_edges_profile=dod-slpm66275-native-grid-v1";assert(!build(s));
 }else if(mode=="hd-ui"){
  auto s=confirmed();s["reference_hd_cg_scaling"]="1";auto r=build(s,"",0xcbf43926,"pack/ui_hd");
  assert(r&&r.hdScaling&&r.staticPages&&!r.particleSelected);assert(r.options.find("hd_frame_min_scale=7;hd_frame_disable_native_scaling=1")!=r.options.npos);
  assert(r.options.find("static_page_format=page-rgba-512x448-x3-v1;static_page_dir=pack/ui_hd")!=r.options.npos);
  assert(!build(s,"static_page_format=other",0xcbf43926,"pack"));assert(!build(s,"",0xcbf43926,"a;particle_edges_profile=off"));
 }else if(mode=="precedence"){
  auto s=confirmed();s["reference_hd_cg_scaling"]="1";s["gs_options"]="hd_frame_min_scale=3;texture_dir=packA";
  auto r=build(s,"hd_frame_min_scale=2;texture_dir=packB");assert(r&&r.textureDirectory=="packB");
  assert(r.options.find("hd_frame_min_scale=7")<r.options.find("hd_frame_min_scale=3"));assert(r.options.find("hd_frame_min_scale=3")<r.options.find("hd_frame_min_scale=2"));
 }else if(mode=="invalid"){
  for(auto v:{"yes","-1","2"," 1"}){auto s=confirmed();s["reference_particles"]=v;assert(!build(s));}
  for(auto v:{"","123","0x12345678","1234567x","-1234567"})assert(!dod_reference::parseCrc(v));
 }else if(mode=="reuse"){
  auto s=confirmed();s["reference_particles"]="1";auto selected=build(s);assert(selected&&selected.particleSelected);
  auto next=build();assert(next&&!next.particleSelected);ParticleEdges::BeginSession(selected.options.c_str(),nullptr,nullptr);assert(ParticleEdges::session.profile);
  ParticleEdges::BeginSession(next.options.c_str(),nullptr,nullptr);assert(!ParticleEdges::session.profile);
 }else if(mode=="arguments"){
  auto s=confirmed();const auto original=s;const char* missing[]={"runner","--reference_particles","1","--confirmed_game_revision"};
  assert(!dod_reference::applyArguments(s,4,missing).empty()&&s==original);
  const char* bad[]={"runner","--","1"};assert(!dod_reference::applyArguments(s,3,bad).empty());
  const char* good[]={"runner","--reference_particles","0"};assert(dod_reference::applyArguments(s,3,good).empty()&&s["reference_particles"]=="0");
 }else if(mode=="environment"){
  assert(dod_reference::gsEnvironment()&&std::string(dod_reference::gsEnvironment())=="public");
  assert(dod_reference::uiEnvironment()&&std::string(dod_reference::uiEnvironment())=="public-ui");
 }else if(mode=="elf-crc"){
  assert(argc==4);auto crc=dod_reference::elfCrc(argv[2]);assert(crc);std::cout<<*crc;assert(*crc==std::stoul(argv[3]));
 }else if(mode=="elf-reject"){
  assert(argc==3);assert(!dod_reference::elfCrc(argv[2]));
 }else return 2;
}
