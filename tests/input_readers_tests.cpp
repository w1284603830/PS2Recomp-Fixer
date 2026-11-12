#include "ps2_input_readers.h"
#include "runtime/ps2_pad.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>
static bool pad=false,menu=false,trainer=false;static int heldKey=0;
static std::deque<int> keys;
bool IsWindowFocused(){return true;} bool IsGamepadAvailable(int){return pad;}
bool IsGamepadButtonDown(int,int){return false;}float GetGamepadAxisMovement(int,int){return 0;}
bool IsKeyDown(int key){return key==heldKey;}int GetKeyPressed(){if(keys.empty())return 0;int k=keys.front();keys.pop_front();return k;}
double GetTime(){return 100;}
bool dodPollGamepad(uint8_t*,bool){return false;}bool dodGamepadBackendReady(){return false;}
bool ps2CheatsMenuOpen(){return menu;}void ps2CheatsSetHostPad(uint16_t,bool){}
bool ps2TrainerMenuOpen(){return trainer;}void ps2TrainerSetHostPad(uint16_t,bool){}
void dodPollHostPadInput();bool dodReadHostPadInputForPoll(uint8_t*,size_t);
static uint16_t buttons(const std::array<uint8_t,32>& b){return b[2]|uint16_t(b[3])<<8;}
int main(int argc,char**argv){
 assert(argc==2);std::string mode=argv[1];ps2input::resetSession();
 auto a=ps2input::registerReader(),b=ps2input::registerReader();assert(a&&b&&a!=b);
 std::array<uint8_t,32> packet{};
 if(mode=="readers"){
  keys.push_back(88);dodPollHostPadInput(); // short down/up, no held key
  assert(dodReadHostPadInputForPoll(packet.data(),packet.size())&&!(buttons(packet)&0x4000));
  dodPollHostPadInput(); // SIO2 has already acknowledged, reader A and B have not
  assert(ps2input::readLatest(a,packet.data(),packet.size())&&!(buttons(packet)&0x4000));
  assert(ps2input::readLatest(a,packet.data(),packet.size())&&buttons(packet)==0xffff);
  assert(ps2input::readLatest(b,packet.data(),packet.size())&&!(buttons(packet)&0x4000));
  heldKey=88;dodPollHostPadInput();
  for(int i=0;i<3;++i)assert(ps2input::readLatest(a,packet.data(),packet.size())&&!(buttons(packet)&0x4000));
  heldKey=0;dodPollHostPadInput();assert(ps2input::readLatest(a,packet.data(),packet.size())&&buttons(packet)==0xffff);
 }else if(mode=="capture"){
  keys.push_back(88);dodPollHostPadInput();menu=true; // menu opens before next host publish
  assert(ps2input::readLatest(a,packet.data(),packet.size())&&buttons(packet)==0xffff);
  for(int i=4;i<8;++i)assert(packet[i]==128);
  assert(dodReadHostPadInputForPoll(packet.data(),32)&&buttons(packet)==0xffff);
  menu=false;assert(dodReadHostPadInputForPoll(packet.data(),32)&&buttons(packet)==0xffff);
  assert(ps2input::readLatest(b,packet.data(),packet.size())&&buttons(packet)==0xffff);
  keys.push_back(88);dodPollHostPadInput();pad=true;dodPollHostPadInput();
  assert(ps2input::readLatest(a,packet.data(),packet.size())&&buttons(packet)==0xffff);
  pad=false;dodPollHostPadInput();assert(ps2input::readLatest(b,packet.data(),packet.size())&&buttons(packet)==0xffff);
 }else if(mode=="trainer-capture"){ // the optional F3 panel captures input exactly like the F2 menu
  keys.push_back(88);dodPollHostPadInput();trainer=true;
  assert(ps2input::readLatest(a,packet.data(),packet.size())&&buttons(packet)==0xffff);
  for(int i=4;i<8;++i)assert(packet[i]==128);
  assert(dodReadHostPadInputForPoll(packet.data(),32)&&buttons(packet)==0xffff);
  heldKey=88;dodPollHostPadInput();assert(dodReadHostPadInputForPoll(packet.data(),32)&&buttons(packet)==0xffff);
  trainer=false;heldKey=0;dodPollHostPadInput();
  assert(ps2input::readLatest(b,packet.data(),packet.size())&&buttons(packet)==0xffff);
 }else if(mode=="reset"){
  keys.push_back(88);dodPollHostPadInput();packet.fill(0xa5);auto untouched=packet;
  assert(!ps2input::readLatest(a,nullptr,32));assert(!ps2input::readLatest(a,packet.data(),31));assert(packet==untouched);
  assert(ps2input::readLatest(a,packet.data(),32)&&!(buttons(packet)&0x4000)); // invalid reads did not consume
  ps2input::resetSession();packet=untouched;assert(!ps2input::readLatest(a,packet.data(),32)&&packet==untouched);
  auto c=ps2input::registerReader();assert(c&&c!=a&&c!=b);
  assert(!ps2input::unregisterReader(a));assert(ps2input::readLatest(c,packet.data(),32)&&buttons(packet)==0xffff);
  assert(ps2input::unregisterReader(c));assert(!ps2input::unregisterReader(c));
 }else if(mode=="capacity"){
  for(int i=0;i<6;++i)assert(ps2input::registerReader());assert(!ps2input::registerReader());
  assert(ps2input::unregisterReader(a));assert(ps2input::registerReader());
 }else if(mode=="threads"){
  std::atomic<bool> go{false};std::thread reader([&]{while(!go.load()){} for(int i=0;i<5000;++i){std::array<uint8_t,32> p{};assert(ps2input::readLatest(a,p.data(),32));}});
  go=true;for(int i=0;i<5000;++i)dodPollHostPadInput();reader.join();
 }else if(mode=="boot-neutral"){
  dodPollHostPadInput();assert(ps2input::readLatest(a,packet.data(),32)&&buttons(packet)==0xffff);
 }else if(mode=="boot-explicit"){
  dodPollHostPadInput();assert(ps2input::readLatest(a,packet.data(),32)&&!(buttons(packet)&0x0008));
 }else return 2;
}
