#pragma once
#include <filesystem>
#include <string>
namespace Path{inline std::string Combine(const std::string&a,const std::string&b){return(std::filesystem::path(a)/b).string();}}
