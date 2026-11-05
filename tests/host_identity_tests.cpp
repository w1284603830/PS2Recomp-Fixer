#include <cassert>
#include <filesystem>
#include <string>
namespace EmuFolders{std::string Textures;}
std::string g_textureSerial,g_gameTitle;
namespace pcsx2gs{void SetTextureFolder(const std::string&);void SetGameTitle(const std::string&);}
namespace VMManager{std::string GetDiscSerial();std::string GetTitle(bool);}
#include "host_identity_current.inc"
int main(){
 assert(VMManager::GetDiscSerial().empty()&&VMManager::GetTitle(false)=="PS2Recomp port");
 pcsx2gs::SetTextureFolder("/packs/another-title/replacements/");assert(VMManager::GetDiscSerial()=="another-title"&&EmuFolders::Textures=="/packs");
 pcsx2gs::SetGameTitle("Explicit title");assert(VMManager::GetTitle(false)=="Explicit title");
 pcsx2gs::SetTextureFolder("/");assert(VMManager::GetDiscSerial().empty()&&EmuFolders::Textures.empty());
 pcsx2gs::SetTextureFolder("");pcsx2gs::SetGameTitle("");assert(VMManager::GetDiscSerial().empty()&&VMManager::GetTitle(true)=="PS2Recomp port");
}
