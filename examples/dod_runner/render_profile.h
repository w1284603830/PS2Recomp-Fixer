#pragma once
// Optional Dawn of Dreams reference adaptation. Nothing in shared Host/GS
// includes this file. Revision is an operator assertion, pinned to an ELF CRC;
// neither its filename nor an HPO value authenticates a game revision.
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace dod_reference
{
inline constexpr std::string_view revision = "SLPM_662.75";
inline constexpr std::string_view particleProfile = "dod-slpm66275-native-grid-v1";
inline constexpr std::string_view game = "onimusha-dawn-of-dreams";
using Settings = std::map<std::string,std::string>;
inline std::string applyArguments(Settings& settings,int argc,const char* const* argv)
{
    Settings candidate=settings;
    for(int i=1;i<argc;i+=2)
    {
        const std::string_view key=argv[i];
        if(key.size()<=2||!key.starts_with("--"))return "unknown argument: "+std::string(key);
        if(i+1>=argc)return "missing value for "+std::string(key);
        candidate[std::string(key.substr(2))]=argv[i+1];
    }
    settings=std::move(candidate);return {};
}
inline std::string value(const Settings& settings,const char* key)
{
    const auto it=settings.find(key);return it==settings.end()?std::string{}:it->second;
}
inline std::string_view trim(std::string_view s)
{
    const auto a=s.find_first_not_of(" \t\r\n");
    return a==s.npos?std::string_view{}:s.substr(a,s.find_last_not_of(" \t\r\n")-a+1);
}
inline const char* gsEnvironment()
{
    // Preflight runs before PS2Runtime installs the legacy aliases.
    const char* s=std::getenv("PS2_GS_OPTIONS");return s?s:std::getenv("DOD_GS_OPTIONS");
}
inline const char* uiEnvironment()
{
    const char* s=std::getenv("PS2_UI_HD_DIR");return s?s:std::getenv("DOD_UI_HD_DIR");
}
inline std::optional<uint32_t> parseCrc(std::string_view s)
{
    if(s.size()!=8) return {};
    uint32_t n=0;const auto p=std::from_chars(s.data(),s.data()+s.size(),n,16);
    if(p.ec!=std::errc{}||p.ptr!=s.data()+s.size())return {};
    return n;
}
inline std::optional<uint32_t> elfCrc(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary);std::array<unsigned char,8192> bytes{};
    if(!file.read(reinterpret_cast<char*>(bytes.data()),52))return {};
    // ELF32, little endian, executable MIPS. This is format validation, not a game identity database.
    const unsigned char ident[]={0x7f,'E','L','F',1,1,1};
    for(size_t i=0;i<sizeof(ident);++i)if(bytes[i]!=ident[i])return {};
    if(bytes[16]!=2||bytes[17]!=0||bytes[18]!=8||bytes[19]!=0)return {};
    uint32_t crc=0xffffffffu;
    const auto add=[&](const unsigned char* p,size_t n){
        for(size_t i=0;i<n;++i){crc^=p[i];for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0u);}
    };
    add(bytes.data(),52);
    while(file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())||file.gcount())
        add(bytes.data(),static_cast<size_t>(file.gcount()));
    if(file.bad()||!file.eof())return {};
    return ~crc;
}
struct Result
{
    std::string options,error,textureDirectory;
    bool particleSelected=false,staticPages=false,hdScaling=false;
    explicit operator bool()const{return error.empty();}
};
inline Result build(const Settings& settings,std::string_view environmentOptions,
                    std::optional<uint32_t> actualElfCrc,std::string_view resolvedUiDirectory)
{
    Result out;
    // Existing reference GameIndex choices remain confined to this runner.
    out.options="blending=2;half_pixel_offset=5;native_scaling=2;bilinear_upscale=2;game_title=Onimusha Dawn of Dreams";
    const std::string particle=value(settings,"reference_particles"),hd=value(settings,"reference_hd_cg_scaling");
    const auto enabled=[&out](const std::string& s,const char* key){
        if(s.empty()||s=="0")return false;
        if(s=="1")return true;
        out.error=std::string(key)+" accepts only 0 or 1";return false;
    };
    const bool preset=enabled(particle,"reference_particles");
    out.hdScaling=enabled(hd,"reference_hd_cg_scaling");
    if(!out)return out;
    out.staticPages=!resolvedUiDirectory.empty();
    if(resolvedUiDirectory.find_first_of(";\r\n")!=std::string_view::npos||resolvedUiDirectory.find('\0')!=std::string_view::npos){
        out.error="ui_hd_dir cannot be represented in GS options";return out;
    }
    std::map<std::string,std::string> identity;
    bool rawStatic=false;
    const auto inspect=[&](std::string_view rest){
        while(!rest.empty()){
            const auto end=rest.find(';');const auto item=trim(rest.substr(0,end));
            rest=end==rest.npos?std::string_view{}:rest.substr(end+1);
            const auto eq=item.find('=');if(eq==item.npos)continue;
            const auto key=trim(item.substr(0,eq)),v=trim(item.substr(eq+1));
            if(key=="game_id"||key=="game_revision"||key=="particle_edges_profile"){
                if(!identity.emplace(std::string(key),std::string(v)).second)out.error="duplicate particle identity/profile option";
            }
            if(key=="static_page_format"||key=="static_page_dir")rawStatic=true;
            if(key=="texture_dir")out.textureDirectory=std::string(v);
        }
    };
    const auto iniOptions=value(settings,"gs_options");inspect(iniOptions);inspect(environmentOptions);
    if(!out)return out;
    const auto selected=identity.find("particle_edges_profile");
    const bool rawParticle=selected!=identity.end()&&selected->second!="off";
    if(preset&&!identity.empty()){out.error="use reference_particles or raw particle options, not both";return out;}
    if(rawParticle&&(selected->second!=particleProfile||identity["game_id"]!=game||identity["game_revision"]!=revision)){
        out.error="this runner supplies only the SLPM_662.75 DoD particle reference";return out;
    }
    // All selected reference enhancements need a revision confirmation and a
    // checksum supplied from the user's independently verified ELF. No trusted
    // release CRC is invented here and no enhancement is selected automatically.
    if(preset||rawParticle||out.hdScaling||out.staticPages){
        const auto expected=parseCrc(value(settings,"confirmed_elf_crc32"));
        if(value(settings,"confirmed_game_revision")!=revision||!expected||!actualElfCrc||*expected!=*actualElfCrc){
            out.error="reference enhancement requires confirmed_game_revision=SLPM_662.75 and matching confirmed_elf_crc32";return out;
        }
    }
    if(out.staticPages&&rawStatic){out.error="use ui_hd_dir or raw static page options, not both";return out;}
    if(preset){
        out.options+=";game_id="+std::string(game)+";game_revision="+std::string(revision)+";particle_edges_profile="+std::string(particleProfile);
    }
    out.particleSelected=preset||rawParticle;
    if(out.hdScaling)out.options+=";hd_frame_min_scale=7;hd_frame_disable_native_scaling=1";
    if(out.staticPages)out.options+=";static_page_format=page-rgba-512x448-x3-v1;static_page_dir="+std::string(resolvedUiDirectory);
    if(!iniOptions.empty())out.options+=';'+iniOptions;
    if(!environmentOptions.empty())out.options+=';'+std::string(environmentOptions);
    return out;
}
}
