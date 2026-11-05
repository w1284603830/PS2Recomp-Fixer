#pragma once
#include <cstdint>
struct Sound{unsigned id=0;};struct Wave{unsigned frameCount=0;};
inline bool IsSoundPlaying(Sound){return false;}inline void UnloadSound(Sound){}inline void StopSound(Sound){}
inline Wave LoadWaveFromMemory(const char*,const unsigned char*,int){return {};}
inline Sound LoadSoundFromWave(Wave){return {};}inline void UnloadWave(Wave){}
inline void SetSoundPitch(Sound,float){}inline void SetSoundVolume(Sound,float){}inline void PlaySound(Sound){}
