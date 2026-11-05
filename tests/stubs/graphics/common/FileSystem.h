#pragma once
#include <cstdio>
#include <filesystem>
#include <memory>
namespace FileSystem{inline auto OpenManagedCFile(const char*p,const char*m){return std::unique_ptr<FILE,int(*)(FILE*)>(std::fopen(p,m),std::fclose);}inline bool EnsureDirectoryExists(const char*p,bool){std::error_code e;std::filesystem::create_directories(p,e);return !e;}}
