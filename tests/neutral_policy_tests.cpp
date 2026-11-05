#include "ps2_input_readers.h"
#include "ps2_presentation_policy.h"
#include "ps2_pad_diagnostics.h"
#include "GS/HostTuning.h"
#include "kzvu.h"
static_assert(KzvuConfig{}.clampMode==1 && !KzvuConfig{}.iBitHack);
#include "GS/TextureExclusionList.h"
#include "ps2_gs_trace.h"
#include <array>
#include <cassert>
#include <cstring>
#include <limits>
#include <string>
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];
 if(mode=="presentation"){
  ps2host::FieldPresentation p;
  for(int i=0;i<5;++i){assert(p.atStart());assert(!p.atEnd());}
  p.configure(ps2host::PresentationPhase::End);
  assert(!p.atStart());p.setMovieActive(true);assert(p.atEnd());assert(!p.atEnd());
  assert(p.atStart());p.setMovieActive(false);assert(!p.atEnd());
  assert(!p.atStart());assert(p.atEnd());
  p.restartFieldState();assert(!p.atStart());assert(p.atEnd()); // run reset retains opt-in
  p.reset();assert(p.atStart());assert(!p.atEnd()); // load clears profile
 }else if(mode=="presentation-sequence"){
  ps2host::FieldPresentation p;p.configure(ps2host::PresentationPhase::End);
  for(unsigned pattern=0;pattern<256;++pattern){
   p.reset();p.configure(ps2host::PresentationPhase::End);
   for(unsigned f=0;f<8;++f){p.setMovieActive((pattern>>f)&1);bool a=p.atStart();p.setMovieActive((pattern>>(7-f))&1);bool b=p.atEnd();assert(unsigned(a)+unsigned(b)==1);assert(!p.atEnd());}
  }
 }else if(mode=="render-options"){
  using namespace GSHostTuning;ResetPortOptions();assert(FeedbackCopyMargin(8,true)==1);assert(!PreserveClampFootprint(true,true,true));
  ConfigurePortOption("feedback_copy_hpo_guard","1");assert(FeedbackCopyMargin(7,true)==4);assert(FeedbackCopyMargin(7,false)==1);assert(FeedbackCopyMargin(1,true)==1);assert(FeedbackCopyMargin(std::numeric_limits<float>::infinity(),true)==1);
  ConfigurePortOption("clamp_footprint","1");assert(PreserveClampFootprint(true,true,true));assert(!PreserveClampFootprint(false,true,true));assert(!PreserveClampFootprint(true,false,true));assert(!PreserveClampFootprint(true,true,false));
  for(auto v:{"0","-1","9","7x","","999999999999"}){ConfigurePortOption("hd_frame_min_scale",v);assert(portOptions.hdMinimumScale==1);}
  ConfigurePortOption("hd_frame_min_scale","7");assert(portOptions.hdMinimumScale==7);
  ConfigurePortOption("clamp_footprint","yes");assert(!portOptions.clampFootprint);
  ConfigurePortOption("static_page_format","page-rgba-512x448-x3-v1");ConfigurePortOption("static_page_dir","pack");
  ResetPortOptions();assert(!portOptions.feedbackCopyGuard&&!portOptions.hdDisableNativeScaling&&portOptions.staticPageFormat.empty()&&portOptions.staticPageDirectory.empty());
 }else if(mode=="exclusions"){
  GSHostTuning::TextureExclusionList list;list.load("\xef\xbb\xbf#pack\n ABC.dds\r\nabc.DDS\nabc.png\n../outside.dds\nC:\\file.dds\nno_extension\n");
  assert(list.size()==2&&list.rejected==3&&!list.limited);assert(list.excluded("AbC.dDs"));assert(!list.excluded("other.dds"));
  list.load("abc.dds");assert(!list.excluded("abc.png"));list.clear();assert(!list.excluded("abc.dds"));
  list.load(std::string("nul\0bad.dds",11));assert(list.size()==0&&list.rejected==1);
 }else if(mode=="exclusion-limits"){
  GSHostTuning::TextureExclusionList list;list.load(std::string(1024*1024+1,'a'));assert(list.limited&&list.size()==0);
  std::string data;for(int i=0;i<4096;++i)data+=std::to_string(i)+".dds\n";data+="0.DDS\n";list.load(data);assert(!list.limited&&list.size()==4096);
  list.load(data+"extra.dds\n");assert(list.limited&&list.size()==4096);
 }else if(mode=="address"){
  using ps2input::diagnosticWordAddress;for(auto v:{"", "0", "-1", "+1", "0x", "1junk", "0x100000000", "0xfffffffc", "33554429", "999999999999999999999"})assert(!diagnosticWordAddress(v));
  assert(diagnosticWordAddress("0x1fffffc")==0x1fffffc);assert(diagnosticWordAddress("16")==16);assert(!diagnosticWordAddress(nullptr));
 }else if(mode=="trace-config"){
  assert(dodpcsx2::cgtrace::state.first==12&&dodpcsx2::cgtrace::state.end==16);
 }else if(mode=="trace-off"){
  assert(!dodpcsx2::cgtrace::traceOn());
 }else if(mode=="trace"){
  struct Regs{std::array<uint64_t,15> regset1{};} regs;regs.regset1[9]=0x1040;const auto original=regs;
  dodpcsx2::cgtrace::state.first=1;dodpcsx2::cgtrace::state.end=4;dodpcsx2::cgtrace::submitted(32,2,true);uint8_t q[16]{};
  dodpcsx2::cgtrace::gif(3,q,16);dodpcsx2::cgtrace::vsync(1,regs,0);assert(!std::memcmp(&regs,&original,sizeof(regs)));
  dodpcsx2::cgtrace::reset();assert(!dodpcsx2::cgtrace::state.active&&!dodpcsx2::cgtrace::state.queued&&!dodpcsx2::cgtrace::state.frame);
 }else return 2;
}
