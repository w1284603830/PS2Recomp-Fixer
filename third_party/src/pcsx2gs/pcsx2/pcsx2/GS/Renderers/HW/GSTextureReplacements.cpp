// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "common/AlignedMalloc.h"
#include "common/Buffer.h"
#include "common/Console.h"
#include "common/HashCombine.h"
#include "common/FileSystem.h"
#include "common/Path.h"
#include "common/StringUtil.h"
#include "common/ScopedGuard.h"
#include "common/Threading.h"
#include "common/TextureDecompress.h"
#include "common/ZipHelpers.h"

#include "Config.h"
#include "Host.h"
#include "IconsFontAwesome.h"
#include "GS/GSExtra.h"
#include "GS/GSLocalMemory.h"
#include "GS/Renderers/HW/GSTextureReplacements.h"
#include "GS/Renderers/HW/DoDTextureExclusions.h"
#include "VMManager.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <tuple>
#include <thread>
#include <vector>

// this is a #define instead of a variable to avoid warnings from non-literal format strings
#define TEXTURE_FILENAME_FORMAT_STRING "%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_REGION_FORMAT_STRING "%" PRIx64 "-r%ux%u-%08x"
#define TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-r%ux%u-%08x"
#define TEXTURE_FILENAME_OLD_REGION_FORMAT_STRING "%" PRIx64 "-r%" PRIx64 "-%08x"
#define TEXTURE_FILENAME_OLD_REGION_CLUT_FORMAT_STRING "%" PRIx64 "-%" PRIx64 "-r%" PRIx64 "-%08x"
#define TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME "replacements"
#define TEXTURE_DUMP_SUBDIRECTORY_NAME "dumps"

namespace
{
	struct TextureName // 32 bytes
	{
		u64 TEX0Hash;
		u64 CLUTHash;
		u32 region_width;
		u32 region_height;

		union
		{
			struct
			{
				u32 TEX0_PSM : 6;
				u32 TEX0_TW : 4;
				u32 TEX0_TH : 4;
				u32 unused0 : 1; // was TCC
				u32 TEXA_TA0 : 8;
				u32 TEXA_AEM : 1;
				u32 TEXA_TA1 : 8;
			};
			u32 bits;
		};
		u32 miplevel;

		__fi u32 Width() const { return (region_width ? region_width : (1u << TEX0_TW)); }
		__fi u32 Height() const { return (region_height ? region_height : (1u << TEX0_TH)); }
		__fi bool HasPalette() const { return (GSLocalMemory::m_psm[TEX0_PSM].pal > 0); }
		__fi bool HasRegion() const { return (region_width != 0 || region_height != 0); }

		__fi bool operator==(const TextureName& rhs) const { return BitEqual(*this, rhs); }
		__fi bool operator!=(const TextureName& rhs) const { return !BitEqual(*this, rhs); }
		__fi bool operator<(const TextureName& rhs) const { return (std::memcmp(this, &rhs, sizeof(*this)) < 0); }

		__fi void RemoveUnusedBits()
		{
			// Remove bits which were previously present, but no longer used.
			unused0 = 0;
		}
	};
	static_assert(sizeof(TextureName) == 32, "ReplacementTextureName is expected size");
} // namespace

namespace std
{
	template <>
	struct hash<TextureName>
	{
		std::size_t operator()(const TextureName& val) const
		{
			std::size_t h = 0;
			HashCombine(h, val.TEX0Hash, val.CLUTHash,
				static_cast<u64>(val.region_width) | (static_cast<u64>(val.region_height) << 32),
				static_cast<u64>(val.bits) | (static_cast<u64>(val.miplevel) << 32));
			return h;
		}
	};
} // namespace std

namespace GSTextureReplacements
{
	struct ReplacementFile
	{
		std::string path; ///< Path to archive or file
		s64 archive_idx;  ///< If archive, index into archive, otherwise -1
		bool pc_ui_direct = false; ///< Only files directly in replacements/pc_ui_hd.
		bool IsArchive() const { return archive_idx >= 0; }
	};
	static TextureName CreateTextureName(const GSTextureCache::HashCacheKey& hash, u32 miplevel);
	static GSTextureCache::HashCacheKey HashCacheKeyFromTextureName(const TextureName& tn);
	static std::optional<TextureName> ParseReplacementName(const char* filename);
	static std::string GetGameTextureDirectory();
	static std::string GetDumpFilename(const TextureName& name, u32 level);
	template <GSTexture::Format format>
	std::pair<u8, u8> GetBCAlphaMinMax(ReplacementTexture& rtex);
	static void SetReplacementTextureAlphaMinMax(ReplacementTexture& rtex);
	static std::optional<ReplacementTexture> LoadReplacementTexture(const ReplacementFile& file, bool only_base_image);
	static void QueueAsyncReplacementTextureLoad(const TextureName& name, const ReplacementFile& filename, bool mipmap, bool cache_only);
	static void PrecacheReplacementTextures();
	static void ClearReplacementTextures();

	static void StartWorkerThread();
	static void StopWorkerThread();
	static void QueueWorkerThreadItem(std::function<void()> fn, bool high_priority);
	static void WorkerThreadEntryPoint();
	static void SyncWorkerThread();
	static void CancelPendingLoadsAndDumps();

	static std::string s_current_serial;

	/// Textures that have been dumped, to save stat() calls.
	static std::unordered_set<TextureName> s_dumped_textures;
	static std::mutex s_dumped_textures_mutex;

	/// Lookup map of texture names to replacements, if they exist.
	static std::unordered_map<TextureName, ReplacementFile> s_replacement_texture_filenames;

	/// Lookup map of texture names without CLUT hash, to know when we need to disable paltex.
	static std::unordered_set<TextureName> s_replacement_textures_without_clut_hash;

	/// Lookup map of texture names to replacement data which has been cached.
	static std::unordered_map<TextureName, ReplacementTexture> s_replacement_texture_cache;
	static std::mutex s_replacement_texture_cache_mutex;

	/// List of textures that are pending asynchronous load. Second element is whether we're only precaching.
	static std::unordered_map<TextureName, bool> s_pending_async_load_textures;

	/// List of textures that we have asynchronously loaded and can now be injected back into the TC.
	/// Second element is whether the texture should be created with mipmaps.
	static std::vector<std::pair<TextureName, bool>> s_async_loaded_textures;

	/// Loader/dumper threads ([DoD] several: decoding a BC7 file for its alpha range is the slow part of a load).
	static std::vector<std::thread> s_worker_threads;
	static std::mutex s_worker_thread_mutex;
	static std::condition_variable s_worker_thread_cv;
	static std::deque<std::pair<std::function<void()>, bool>> s_worker_thread_queue;
	static bool s_worker_thread_running = false;
	static u32 s_worker_busy = 0; ///< [DoD] items the workers are running (under s_worker_thread_mutex)

	/// [DoD] HD pack loading (DodConfigure; dod.ini gs_options texture_threads / texture_cache_mb / texture_upload_mb /
	/// texture_keep_mb / texture_prefetch). The first time a room is drawn its replacements used to arrive together and
	/// be uploaded in one vsync, and any replacement unused for 30 frames was uploaded again when it came back: hitches
	/// the player sees (2026-10-26). Now a room's replacements are loaded and uploaded while it loads (prefetch list),
	/// uploads are spread over vsyncs, the hash cache keeps replacements longer, and the CPU copies have a cap.
	static u32 s_dod_threads = 3;
	static u64 s_dod_cache_cap = 1536ull << 20;
	static u64 s_dod_upload_budget = 16ull << 20;
	static u64 s_dod_keep_bytes = 2048ull << 20;
	static std::string s_dod_prefetch_file;

	/// Loaded bytes, and the last use of each loaded replacement for the cap (under s_replacement_texture_cache_mutex).
	static u64 s_dod_cache_bytes = 0;
	static u64 s_dod_use_tick = 0;
	static std::unordered_map<TextureName, u64> s_dod_cache_use;
	/// Prefetched replacements to upload into the hash cache once loaded (under s_replacement_texture_cache_mutex).
	static std::unordered_set<TextureName> s_dod_preupload;
	/// Bytes uploaded since the last vsync (GS thread).
	static u64 s_dod_uploaded_this_vsync = 0;
	/// Scene (0x7E1344) -> the replacements drawn there (GS thread).
	static std::unordered_map<u32, std::vector<TextureName>> s_dod_scene_textures;
	static std::atomic<u32> s_dod_requested_scene{0xFFFFFFFFu};
	static u32 s_dod_prefetched_scene = 0xFFFFFFFFu;

	static u64 DodTextureBytes(const ReplacementTexture& rtex);
	static void DodNoteCached(const TextureName& name, const ReplacementTexture& rtex);
	static void DodTrimCache();
	static void DodClearState();
	static void DodLoadPrefetchList(const std::string& texture_dir);
	static void DodHandleScenePrefetch();
}; // namespace GSTextureReplacements

void pcsx2gsDodLowerThreadPriority(); // [DoD] pcsx2-gs/compat/HostStubs.cpp: below normal, so loads never slow the game

TextureName GSTextureReplacements::CreateTextureName(const GSTextureCache::HashCacheKey& hash, u32 miplevel)
{
	TextureName name;
	name.bits = 0;
	name.TEX0_PSM = hash.TEX0.PSM;
	name.TEX0_TW = hash.TEX0.TW;
	name.TEX0_TH = hash.TEX0.TH;
	name.TEXA_TA0 = hash.TEXA.TA0;
	name.TEXA_AEM = hash.TEXA.AEM;
	name.TEXA_TA1 = hash.TEXA.TA1;
	name.TEX0Hash = hash.TEX0Hash;
	name.CLUTHash = name.HasPalette() ? hash.CLUTHash : 0;
	name.miplevel = miplevel;
	name.region_width = hash.region_width;
	name.region_height = hash.region_height;
	return name;
}

GSTextureCache::HashCacheKey GSTextureReplacements::HashCacheKeyFromTextureName(const TextureName& tn)
{
	const GSLocalMemory::psm_t& psm_s = GSLocalMemory::m_psm[tn.TEX0_PSM];
	GSTextureCache::HashCacheKey key = {};
	key.TEX0.PSM = tn.TEX0_PSM;
	key.TEX0.TW = tn.TEX0_TW;
	key.TEX0.TH = tn.TEX0_TH;
	if (psm_s.pal == 0 && psm_s.fmt > 0)
	{
		key.TEXA.TA0 = tn.TEXA_TA0;
		key.TEXA.AEM = tn.TEXA_AEM;
		key.TEXA.TA1 = tn.TEXA_TA1;
	}
	key.TEX0Hash = tn.TEX0Hash;
	key.CLUTHash = tn.HasPalette() ? tn.CLUTHash : 0;
	key.region_width = tn.region_width;
	key.region_height = tn.region_height;
	return key;
}

std::optional<TextureName> GSTextureReplacements::ParseReplacementName(const char* filename)
{
	TextureName ret;
	ret.miplevel = 0;

	GSTextureCache::SourceRegion full_region;

	char extension_dot;
	if (std::sscanf(filename, TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash,
			&ret.region_width, &ret.region_height, &ret.bits, &extension_dot) == 6 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		return ret;
	}

	if (std::sscanf(filename, TEXTURE_FILENAME_REGION_FORMAT_STRING "%c", &ret.TEX0Hash,
			&ret.region_width, &ret.region_height, &ret.bits, &extension_dot) == 5 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.CLUTHash = 0;
		return ret;
	}

	// Allow loading of dumped textures from older versions that included the full region bits.
	if (std::sscanf(filename, TEXTURE_FILENAME_OLD_REGION_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash,
			&full_region.bits, &ret.bits, &extension_dot) == 5 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.region_width = static_cast<u32>(full_region.GetWidth());
		ret.region_height = static_cast<u32>(full_region.GetHeight());
		return ret;
	}

	if (std::sscanf(filename, TEXTURE_FILENAME_OLD_REGION_FORMAT_STRING "%c", &ret.TEX0Hash, &full_region.bits,
			&ret.bits, &extension_dot) == 4 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.CLUTHash = 0;
		ret.region_width = static_cast<u32>(full_region.GetWidth());
		ret.region_height = static_cast<u32>(full_region.GetHeight());
		return ret;
	}

	ret.region_width = 0;
	ret.region_height = 0;

	if (std::sscanf(filename, TEXTURE_FILENAME_CLUT_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.CLUTHash, &ret.bits,
			&extension_dot) == 4 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		return ret;
	}

	if (std::sscanf(filename, TEXTURE_FILENAME_FORMAT_STRING "%c", &ret.TEX0Hash, &ret.bits, &extension_dot) == 3 &&
		extension_dot == '.')
	{
		ret.RemoveUnusedBits();
		ret.CLUTHash = 0;
		return ret;
	}

	return std::nullopt;
}

std::string GSTextureReplacements::GetGameTextureDirectory()
{
	return Path::Combine(EmuFolders::Textures, s_current_serial);
}

std::string GSTextureReplacements::GetDumpFilename(const TextureName& name, u32 level)
{
	std::string ret;
	if (s_current_serial.empty())
		return ret;

	const std::string game_dir(GetGameTextureDirectory());
	const std::string game_subdir(Path::Combine(game_dir, TEXTURE_DUMP_SUBDIRECTORY_NAME));

	if (!FileSystem::DirectoryExists(game_subdir.c_str()))
	{
		// create both dumps and replacements
		if (!FileSystem::CreateDirectoryPath(game_dir.c_str(), false) ||
			!FileSystem::EnsureDirectoryExists(game_subdir.c_str(), false) ||
			!FileSystem::EnsureDirectoryExists(Path::Combine(game_dir, TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME).c_str(), false))
		{
			// if it fails to create, we're not going to be able to use it anyway
			return ret;
		}
	}

	std::string filename;
	if (name.HasRegion())
	{
		if (name.HasPalette())
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING "-mip%u.png",
					name.TEX0Hash, name.CLUTHash, name.region_width, name.region_height, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_CLUT_FORMAT_STRING ".png",
					name.TEX0Hash, name.CLUTHash, name.region_width, name.region_height, name.bits);
		}
		else
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_FORMAT_STRING "-mip%u.png",
					name.TEX0Hash, name.region_width, name.region_height, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_REGION_FORMAT_STRING ".png",
					name.TEX0Hash, name.region_width, name.region_height, name.bits);
		}
	}
	else
	{
		if (name.HasPalette())
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_CLUT_FORMAT_STRING "-mip%u.png",
				                                  name.TEX0Hash, name.CLUTHash, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_CLUT_FORMAT_STRING ".png",
				                                  name.TEX0Hash, name.CLUTHash, name.bits);
		}
		else
		{
			filename = (level > 0)
				? StringUtil::StdStringFromFormat(TEXTURE_FILENAME_FORMAT_STRING "-mip%u.png",
				                                  name.TEX0Hash, name.bits, level)
				: StringUtil::StdStringFromFormat(TEXTURE_FILENAME_FORMAT_STRING ".png",
				                                  name.TEX0Hash, name.bits);
		}
	}

	ret = Path::Combine(game_subdir, filename);

	return ret;
}

void GSTextureReplacements::Initialize()
{
	s_current_serial = VMManager::GetDiscSerial();

	if (GSConfig.DumpReplaceableTextures || GSConfig.LoadTextureReplacements)
		StartWorkerThread();

	ReloadReplacementMap();
}

void GSTextureReplacements::GameChanged()
{
	std::string new_serial = VMManager::GetDiscSerial();
	if (s_current_serial == new_serial)
		return;

	s_current_serial = std::move(new_serial);
	ReloadReplacementMap();
	ClearDumpedTextureList();
}

/// If the given file exists in the given directory, but with a different case than the original file, write its path to `*output` and return true.
static bool GetWrongCasePath(std::string* output, const char* dir, std::string_view file, FileSystem::FindResultsArray* reuseme)
{
	if (FileSystem::FindFiles(dir, "*", FILESYSTEM_FIND_FOLDERS | FILESYSTEM_FIND_HIDDEN_FILES, reuseme))
	{
		for (const FILESYSTEM_FIND_DATA& fd : *reuseme)
		{
			std::string_view name = Path::GetFileName(fd.FileName);
			if (name.size() != file.size())
				continue;
			if (0 == strncmp(name.data(), file.data(), name.size()))
				continue;
			if (0 == StringUtil::Strncasecmp(name.data(), file.data(), name.size()))
			{
				*output = fd.FileName;
				return true;
			}
		}
	}
	return false;
}

static void AddReplacementTexture(TextureName key, const char* filename, std::string path, s64 archive_idx = -1, bool pc_ui_direct = false)
{
	using namespace GSTextureReplacements;

	DbgCon.WriteLn("Found %ux%u replacement '%s'", key.Width(), key.Height(), filename);
	auto existing = s_replacement_texture_filenames.find(key);
	if (existing == s_replacement_texture_filenames.end())
		s_replacement_texture_filenames.emplace(key, ReplacementFile { std::move(path), archive_idx, pc_ui_direct });
	else if (pc_ui_direct && !existing->second.pc_ui_direct)
		existing->second = ReplacementFile { std::move(path), archive_idx, true };

	// zero out the CLUT hash, because we need this for checking if there's any replacements with this hash when using paltex
	key.CLUTHash = 0;
	s_replacement_textures_without_clut_hash.insert(key);
}

void GSTextureReplacements::ReloadReplacementMap()
{
	SyncWorkerThread();

	// clear out the caches
	{
		s_replacement_texture_filenames.clear();
		s_replacement_textures_without_clut_hash.clear();

		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		s_replacement_texture_cache.clear();
		s_pending_async_load_textures.clear();
		s_async_loaded_textures.clear();
		DodClearState();
	}

	// can't replace bios textures.
	if (s_current_serial.empty() || !GSConfig.LoadTextureReplacements)
		return;

	const std::string texture_dir = GetGameTextureDirectory();
	const std::string replacement_dir(Path::Combine(texture_dir, TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME));
	const std::string pc_ui_dir = Path::ToNativePath(Path::Combine(replacement_dir, "pc_ui_hd"));
	const std::string replacement_zip_name = s_current_serial + ".zip";
	const std::string replacement_zip = Path::Combine(EmuFolders::Textures, replacement_zip_name);

	FileSystem::FindResultsArray files;

	// For some reason texture pack authors think it's a good idea to rename the replacements directory to something with the wrong case...
	std::string wrong_case_path;
	const std::string* right_case_path = nullptr;
	if (GetWrongCasePath(&wrong_case_path, EmuFolders::Textures.c_str(), s_current_serial, &files))
		right_case_path = &texture_dir;
	else if (GetWrongCasePath(&wrong_case_path, texture_dir.c_str(), TEXTURE_REPLACEMENT_SUBDIRECTORY_NAME, &files))
		right_case_path = &replacement_dir;
	else if (GetWrongCasePath(&wrong_case_path, EmuFolders::Textures.c_str(), replacement_zip_name, &files))
		right_case_path = &replacement_zip;
	if (right_case_path)
	{
		Host::AddKeyedOSDMessage("TextureReplacementDirCaseMismatch",
			fmt::format(TRANSLATE_FS("TextureReplacement", "Texture replacement directory {} will not work on case sensitive filesystems.\n"
			                                               "Rename it to {} to remove this warning."),
			            wrong_case_path, *right_case_path),
			Host::OSD_WARNING_DURATION);
	}

	files.clear();
	u32 dod_excluded = 0;
	ScopedGuard log_excluded([&dod_excluded]() {
		if (dod_excluded)
			std::fprintf(stderr, "[hd-texture] %u replacements excluded (DoDTextureExclusions.h)\n", dod_excluded);
	});
	FileSystem::FindFiles(replacement_dir.c_str(), "*", FILESYSTEM_FIND_FILES | FILESYSTEM_FIND_HIDDEN_FILES | FILESYSTEM_FIND_RECURSIVE, &files);
	if (FileSystem::FileExists(replacement_zip.c_str()))
	{
		FILESYSTEM_FIND_DATA extra = {};
		extra.FileName = replacement_zip;
		files.push_back(std::move(extra));
	}
	if (files.empty())
		return;

	for (FILESYSTEM_FIND_DATA& fd : files)
	{
		// file format we can handle?
		const char* filename = Path::GetFileName(fd.FileName).data(); // GetFileName takes a substring from the end, so the null terminator is preserved
		std::string_view ext = Path::GetExtension(filename);
		if (StringUtil::compareNoCase(ext, "zip"))
		{
			zip_error_t ze = {};
			auto zf = zip_open_managed(fd.FileName.c_str(), ZIP_RDONLY, &ze);
			zip_int64_t count = zip_get_num_entries(zf.get(), 0);
			if (count < 0)
				continue;
			for (zip_int64_t i = 0; i < count; i++)
			{
				const char* entry = zip_get_name(zf.get(), i, 0);
				std::string_view entry_sv;
				if (!entry || !GetLoader((entry_sv = entry)))
					continue;
				const char* entry_filename = Path::GetFileName(entry_sv).data();
				if (DoDTextureExclusions::excluded(entry_filename))
					continue;
				if (std::optional<TextureName> name = ParseReplacementName(entry_filename))
					AddReplacementTexture(*name, entry_filename, fd.FileName, i);
			}
			continue;
		}
		if (!GetLoader(filename))
			continue;
		// [DoD] Replacements that change the texture's meaning (DoDTextureExclusions.h): the game's own texture is used.
		if (DoDTextureExclusions::excluded(filename))
		{
			++dod_excluded;
			continue;
		}

		// parse the name if it's valid
		if (std::optional<TextureName> name = ParseReplacementName(filename))
		{
			const std::string parent = Path::ToNativePath(Path::GetDirectory(fd.FileName));
			const bool pc_ui_direct = StringUtil::compareNoCase(parent, pc_ui_dir);
			AddReplacementTexture(*name, filename, std::move(fd.FileName), -1, pc_ui_direct);
		}
	}

	if (!s_replacement_texture_filenames.empty())
	{
		if (GSConfig.PrecacheTextureReplacements)
			PrecacheReplacementTextures();
		else
			DodLoadPrefetchList(texture_dir);

		// log a warning when paltex is on and preloading is off, since we'll be disabling paltex
		if (GSConfig.GPUPaletteConversion && GSConfig.TexturePreloading != TexturePreloadingLevel::Full)
		{
			Console.Warning("Replacement textures were found, and GPU palette conversion is enabled without full preloading.");
			Console.Warning("Palette textures will be disabled. Please enable full preloading or disable GPU palette conversion.");
		}
	}
}

void GSTextureReplacements::UpdateConfig(Pcsx2Config::GSOptions& old_config)
{
	// get rid of worker thread if it's no longer needed
	if (s_worker_thread_running && !GSConfig.DumpReplaceableTextures && !GSConfig.LoadTextureReplacements)
		StopWorkerThread();
	if (!s_worker_thread_running && (GSConfig.DumpReplaceableTextures || GSConfig.LoadTextureReplacements))
		StartWorkerThread();

	if ((!GSConfig.DumpReplaceableTextures && old_config.DumpReplaceableTextures) ||
		(!GSConfig.LoadTextureReplacements && old_config.LoadTextureReplacements))
	{
		CancelPendingLoadsAndDumps();
	}

	if (GSConfig.LoadTextureReplacements && !old_config.LoadTextureReplacements)
		ReloadReplacementMap();
	else if (!GSConfig.LoadTextureReplacements && old_config.LoadTextureReplacements)
		ClearReplacementTextures();

	if (!GSConfig.DumpReplaceableTextures && old_config.DumpReplaceableTextures)
		ClearDumpedTextureList();

	if (GSConfig.LoadTextureReplacements && GSConfig.PrecacheTextureReplacements && !old_config.PrecacheTextureReplacements)
		PrecacheReplacementTextures();
}

void GSTextureReplacements::Shutdown()
{
	StopWorkerThread();

	std::string().swap(s_current_serial);
	ClearReplacementTextures();
	ClearDumpedTextureList();
}

u32 GSTextureReplacements::CalcMipmapLevelsForReplacement(u32 width, u32 height)
{
	return static_cast<u32>(std::log2(std::max(width, height))) + 1u;
}

bool GSTextureReplacements::HasAnyReplacementTextures()
{
	return !s_replacement_texture_filenames.empty();
}

bool GSTextureReplacements::HasReplacementTexture(const GSTextureCache::HashCacheKey& hash)
{
	return s_replacement_texture_filenames.find(CreateTextureName(hash, 0)) != s_replacement_texture_filenames.end();
}

bool GSTextureReplacements::HasReplacementTextureWithOtherPalette(const GSTextureCache::HashCacheKey& hash)
{
	const TextureName name(CreateTextureName(hash.WithRemovedCLUTHash(), 0));
	return s_replacement_textures_without_clut_hash.find(name) != s_replacement_textures_without_clut_hash.end();
}

GSTexture* GSTextureReplacements::LookupReplacementTexture(const GSTextureCache::HashCacheKey& hash, bool mipmap,
	bool* pending, std::pair<u8, u8>* alpha_minmax)
{
	const TextureName name(CreateTextureName(hash, 0));
	*pending = false;

	// replacement for this name exists?
	auto fnit = s_replacement_texture_filenames.find(name);
	if (fnit == s_replacement_texture_filenames.end())
		return nullptr;

	const bool ui_direct = fnit->second.pc_ui_direct;
	// Called only on a successful direct GPU upload, once per full key while tracing is enabled.
	auto trace_ui = [&](const ReplacementTexture& rtex, GSTexture* tex, const char* source) {
		static const bool trace = (std::getenv("DOD_TEXTURE_TRACE") != nullptr);
		static std::unordered_set<TextureName> reported;
		if (ui_direct && tex && trace && reported.insert(name).second)
			std::fprintf(stderr, "[hd-texture] ui-direct %" PRIx64 "-%" PRIx64 "-%08x region=%ux%u source=%ux%u hd=%ux%u %s\n",
				name.TEX0Hash, name.CLUTHash, name.bits, name.region_width, name.region_height,
				name.Width(), name.Height(), rtex.width, rtex.height, source);
	};

	// try the full cache first, to avoid reloading from disk
	{
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		auto it = s_replacement_texture_cache.find(name);
		if (it != s_replacement_texture_cache.end())
		{
			s_dod_cache_use[name] = ++s_dod_use_tick;
			// [DoD] Within this vsync's upload budget: upload now. Past it, the upload waits for the next vsyncs like
			// an async load (the game's own texture is drawn meanwhile), so many textures coming back at once are
			// spread over a few frames instead of making one long frame.
			if (ui_direct || s_dod_upload_budget == 0 || s_dod_uploaded_this_vsync < s_dod_upload_budget ||
				!GSConfig.LoadTextureReplacementsAsync)
			{
				// replacement is cached, can immediately upload to host GPU
				if (!ui_direct)
					s_dod_uploaded_this_vsync += DodTextureBytes(it->second);
				*alpha_minmax = it->second.alpha_minmax;
				GSTexture* tex = CreateReplacementTexture(it->second, mipmap);
				trace_ui(it->second, tex, "cache");
				return tex;
			}
			auto pit = s_pending_async_load_textures.find(name);
			if (pit == s_pending_async_load_textures.end())
			{
				s_pending_async_load_textures.emplace(name, false);
				s_async_loaded_textures.emplace_back(name, mipmap);
			}
			else
			{
				pit->second = false; // loaded ahead (cache only) and not uploaded yet: now it is wanted
			}
			*pending = true;
			return nullptr;
		}
	}

	// load asynchronously?
	if (GSConfig.LoadTextureReplacementsAsync && !ui_direct)
	{
		// replacement will be injected into the TC later on
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		QueueAsyncReplacementTextureLoad(name, fnit->second, mipmap, false);

		*pending = true;
		return nullptr;
	}
	else
	{
		// synchronous load
		std::optional<ReplacementTexture> replacement(LoadReplacementTexture(fnit->second, !mipmap));
		if (!replacement.has_value())
			return nullptr;

		// insert into cache
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		const ReplacementTexture& rtex = s_replacement_texture_cache.emplace(name, std::move(replacement.value())).first->second;
		DodNoteCached(name, rtex);

		// and upload to gpu
		*alpha_minmax = rtex.alpha_minmax;
		GSTexture* tex = CreateReplacementTexture(rtex, mipmap);
		trace_ui(rtex, tex, "disk");
		return tex;
	}
}

template <GSTexture::Format format>
std::pair<u8, u8> GSTextureReplacements::GetBCAlphaMinMax(ReplacementTexture& rtex)
{
	constexpr u32 BC_BLOCK_SIZE = 4;
	constexpr u32 BC_BLOCK_BYTES = (format == GSTexture::Format::BC1) ? 8 : 16;

	const u32 blocks_wide = (rtex.width + (BC_BLOCK_SIZE - 1)) / BC_BLOCK_SIZE;
	const u32 blocks_high = (rtex.height + (BC_BLOCK_SIZE - 1)) / BC_BLOCK_SIZE;

	GSVector4i minc = GSVector4i::xffffffff();
	GSVector4i maxc = GSVector4i::zero();

	for (u32 y = 0; y < blocks_high; y++)
	{
		const u8* block_in = rtex.data.data() + y * rtex.pitch;
		alignas(16) u8 block_pixels_out[BC_BLOCK_SIZE * BC_BLOCK_SIZE * sizeof(u32)];

		for (u32 x = 0; x < blocks_wide; x++, block_in += BC_BLOCK_BYTES)
		{
			switch (format)
			{
				case GSTexture::Format::BC1:
					DecompressBlockBC1(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;
				case GSTexture::Format::BC2:
					DecompressBlockBC2(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;
				case GSTexture::Format::BC3:
					DecompressBlockBC3(0, 0, sizeof(u32) * BC_BLOCK_SIZE, block_in, block_pixels_out);
					break;

				case GSTexture::Format::BC7:
					bc7decomp::unpack_bc7(block_in, reinterpret_cast<bc7decomp::color_rgba*>(block_pixels_out));
					break;
			}

			const u8* out_ptr = block_pixels_out;
			for (u32 i = 0; i < ((BC_BLOCK_SIZE * BC_BLOCK_SIZE * sizeof(u32)) / sizeof(GSVector4i)); i++)
			{
				const GSVector4i v = GSVector4i::load<true>(out_ptr);
				out_ptr += sizeof(GSVector4i);
				minc = minc.min_u32(v);
				maxc = maxc.max_u32(v);
			}
		}
	}

	return std::make_pair<u8, u8>(static_cast<u8>(minc.minv_u32() >> 24), static_cast<u8>(maxc.maxv_u32() >> 24));
}

void GSTextureReplacements::SetReplacementTextureAlphaMinMax(ReplacementTexture& rtex)
{
	switch (rtex.format)
	{
		case GSTexture::Format::BC1:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC1>(rtex);
			break;

		case GSTexture::Format::BC2:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC2>(rtex);
			break;

		case GSTexture::Format::BC3:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC3>(rtex);
			break;

		case GSTexture::Format::BC7:
			rtex.alpha_minmax = GetBCAlphaMinMax<GSTexture::Format::BC7>(rtex);
			break;

		default:
			pxAssert(rtex.format == GSTexture::Format::Color);
			rtex.alpha_minmax = GSGetRGBA8AlphaMinMax(rtex.data.data(), rtex.width, rtex.height, rtex.pitch);
			break;
	}
}

std::optional<GSTextureReplacements::ReplacementTexture> GSTextureReplacements::LoadReplacementTexture(const ReplacementFile& file, bool only_base_image)
{
	ReplacementTexture rtex;

	if (file.IsArchive())
	{
		zip_error_t ze = {};
		auto zf = zip_open_managed(file.path.c_str(), ZIP_RDONLY, &ze);
		if (!zf)
		{
			Console.Warning("Failed to open replacement texture zip file %s: %s", file.path.c_str(), zip_error_strerror(&ze));
			return std::nullopt;
		}
		zip_stat_t stat;
		if (0 != zip_stat_index(zf.get(), file.archive_idx, 0, &stat))
		{
			Console.Warning("Replacement texture " PRId64 " missing from zip %s: %s", file.archive_idx, file.path.c_str(), zip_strerror(zf.get()));
			return std::nullopt;
		}
		ReplacementTextureLoader loader = GetLoader(stat.name);
		if (!loader)
			return std::nullopt;
		Common::Buffer buf(stat.size);
		auto zff = zip_fopen_index_managed(zf.get(), file.archive_idx, 0);
		if (!zff || static_cast<u64>(zip_fread(zff.get(), buf.get(), stat.size)) != stat.size)
		{
			if (!zff && zip_get_error(zf.get())->zip_err == ZIP_ER_COMPNOTSUPP)
				Host::AddKeyedOSDMessage("ZipCompressionNotSupported",
					fmt::format(TRANSLATE_FS("TextureReplacement", "Zip file {} contains replacement textures with an unsupported compression type.\n"
					                                               "Unzip the file to use its replacement textures."), file.path),
					Host::OSD_WARNING_DURATION);
			Console.Warning("Failed to load replacement texture %s from zip file %s: %s", stat.name, file.path.c_str(), zip_strerror(zf.get()));
			return std::nullopt;
		}
		MemoryFile memfile(buf.get(), stat.size);
		if (!loader(memfile, stat.name, &rtex, only_base_image))
		{
			Console.Warning("Failed to load replacement texture %s", stat.name);
			return std::nullopt;
		}
	}
	else
	{
		ReplacementTextureLoader loader = GetLoader(file.path);
		if (!loader)
			return std::nullopt;
		FileSystem::ManagedCFilePtr fp = FileSystem::OpenManagedCFile(file.path.c_str(), "rb");
		CFile cfile(fp.get());
		if (!loader(cfile, file.path.c_str(), &rtex, only_base_image))
		{
			Console.Warning("Failed to load replacement texture %s", file.path.c_str());
			return std::nullopt;
		}
	}

	SetReplacementTextureAlphaMinMax(rtex);

	return rtex;
}

void GSTextureReplacements::QueueAsyncReplacementTextureLoad(const TextureName& name, const ReplacementFile& filename, bool mipmap, bool cache_only)
{
	// check the pending list, so we don't queue it up multiple times
	auto it = s_pending_async_load_textures.find(name);
	if (it != s_pending_async_load_textures.end())
	{
		// remove from queue if it's cache-only, so we bump it to the front of the work items
		if (!cache_only && it->second)
		{
			s_pending_async_load_textures.erase(it);
		}
		else
		{
			it->second &= cache_only;
			return;
		}
	}

	s_pending_async_load_textures.emplace(name, cache_only);
	QueueWorkerThreadItem([name, filename, mipmap]() {
		// actually load the file, this is what will take the time
		std::optional<ReplacementTexture> replacement(LoadReplacementTexture(filename, !mipmap));

		// check the pending set, there's a race here if we disable replacements while loading otherwise
		// also check the full replacement list, if async loading is off, it might already be in there
		std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
		auto it = s_pending_async_load_textures.find(name);
		if (it == s_pending_async_load_textures.end() ||
			s_replacement_texture_cache.find(name) != s_replacement_texture_cache.end())
		{
			if (it != s_pending_async_load_textures.end())
				s_pending_async_load_textures.erase(it);

			return;
		}

		// insert into the cache and queue for later injection
		if (replacement.has_value())
		{
			const ReplacementTexture& rtex = s_replacement_texture_cache.emplace(name, std::move(replacement.value())).first->second;
			s_async_loaded_textures.emplace_back(name, mipmap);
			DodNoteCached(name, rtex);
		}
		else
		{
			// loading failed, so clear it from the pending list
			s_pending_async_load_textures.erase(name);
		}
	}, !cache_only);
}

void GSTextureReplacements::PrecacheReplacementTextures()
{
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);

	// predict whether the requests will come with mipmaps
	// TODO: This will be wrong for hw mipmap games like Jak.
	const bool mipmap = GSConfig.HWMipmap || GSConfig.TriFilter == TriFiltering::Forced;

	// pretty simple, just go through the filenames and if any aren't cached, cache them
	for (const auto& it : s_replacement_texture_filenames)
	{
		if (it.second.pc_ui_direct)
			continue; // Always select UI synchronously on first use; do not queue a racing preload.
		if (s_replacement_texture_cache.find(it.first) != s_replacement_texture_cache.end())
			continue;

		// precaching always goes async.. for now
		QueueAsyncReplacementTextureLoad(it.first, it.second, mipmap, true);
	}
}

void GSTextureReplacements::ClearReplacementTextures()
{
	s_replacement_texture_filenames.clear();
	s_replacement_textures_without_clut_hash.clear();

	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	s_replacement_texture_cache.clear();
	s_pending_async_load_textures.clear();
	s_async_loaded_textures.clear();
	DodClearState();
}

GSTexture* GSTextureReplacements::CreateReplacementTexture(const ReplacementTexture& rtex, bool mipmap)
{
	// can't use generated mipmaps with compressed formats, because they can't be rendered to
	// in the future I guess we could decompress the dds and generate them... but there's no reason that modders can't generate mips in dds
	if (mipmap && GSTexture::IsCompressedFormat(rtex.format) && rtex.mips.empty())
	{
		static bool log_once = false;
		if (!log_once)
		{
			Console.Warning("Disabling autogenerated mipmaps on one or more compressed replacement textures.");
			Host::AddIconOSDMessage("DisablingReplacementAutoGeneratedMipmap", ICON_FA_CIRCLE_EXCLAMATION,
				TRANSLATE_SV("GS", "Disabling autogenerated mipmaps on one or more compressed replacement textures. "
								   "Please generate mipmaps when compressing your textures."),
				Host::OSD_WARNING_DURATION);
			log_once = true;
		}

		mipmap = false;
	}

	// Urban Reign PC port: a replacement without its own mips (PNG) gets a generated chain, which the HW renderer samples
	// with the GPU's LOD (GSRendererHW::EmulateTextureSampler): otherwise far surfaces show the full size HD texture
	// (moiré on fine patterns). PS2X_PCSX2_HD_MIPMAPS=0 turns it off.
	static const bool s_generate_mips = []() {
		const char* value = std::getenv("PS2X_PCSX2_HD_MIPMAPS");
		return !(value && value[0] == '0');
	}();
	const int levels = (s_generate_mips && rtex.mips.empty() && !GSTexture::IsCompressedFormat(rtex.format)) ?
	                       static_cast<int>(CalcMipmapLevelsForReplacement(rtex.width, rtex.height)) :
	                       static_cast<int>(rtex.mips.size()) + 1;
	GSTexture* tex = g_gs_device->CreateTexture(rtex.width, rtex.height, levels, rtex.format);
	if (!tex)
		return nullptr;

	// upload base level
	tex->Update(GSVector4i(0, 0, rtex.width, rtex.height), rtex.data.data(), rtex.pitch);

	// and the mips if they're present in the replacement texture
	if (!rtex.mips.empty())
	{
		for (u32 i = 0; i < static_cast<u32>(rtex.mips.size()); i++)
		{
			const ReplacementTexture::MipData& mip = rtex.mips[i];
			tex->Update(GSVector4i(0, 0, static_cast<int>(mip.width), static_cast<int>(mip.height)), mip.data.data(), mip.pitch, i + 1);
		}
	}

	return tex;
}

void GSTextureReplacements::ProcessAsyncLoadedTextures()
{
	// [DoD] A new scene's prefetch first; then this vsync's uploads, up to s_dod_upload_budget bytes (the rest stay
	// queued for the next vsyncs instead of all landing in one frame).
	DodHandleScenePrefetch();
	s_dod_uploaded_this_vsync = 0;

	// this holds the lock while doing the upload, but it should be reasonably quick
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	size_t done = 0;
	for (; done < s_async_loaded_textures.size(); done++)
	{
		if (s_dod_upload_budget != 0 && s_dod_uploaded_this_vsync >= s_dod_upload_budget)
			break;

		const auto [name, mipmap] = s_async_loaded_textures[done];
		const bool prefetched = s_dod_preupload.erase(name) != 0;

		// no longer pending!
		const auto pit = s_pending_async_load_textures.find(name);
		if (pit != s_pending_async_load_textures.end())
		{
			const bool cache_only = pit->second;
			s_pending_async_load_textures.erase(pit);

			// if we were precaching, don't inject into the TC if we didn't actually get requested
			// ([DoD] a room prefetch is uploaded anyway, ahead of its first draw)
			if (cache_only && !prefetched)
				continue;
		}

		// we should be in the cache now, lock and loaded
		auto it = s_replacement_texture_cache.find(name);
		if (it == s_replacement_texture_cache.end())
			continue;

		const GSTextureCache::HashCacheKey key = HashCacheKeyFromTextureName(name);
		if (prefetched && g_texture_cache->DodHasHashCacheReplacement(key))
			continue; // still in VRAM from an earlier visit

		// upload and inject into TC
		GSTexture* tex = CreateReplacementTexture(it->second, mipmap);
		s_dod_uploaded_this_vsync += DodTextureBytes(it->second);
		s_dod_cache_use[name] = ++s_dod_use_tick;
		if (tex)
		{
			if (prefetched)
				g_texture_cache->DodInjectPrefetchedTexture(key, tex, it->second.alpha_minmax);
			else
				g_texture_cache->InjectHashCacheTexture(key, tex, it->second.alpha_minmax);
			// Opt-in acceptance evidence: successful GPU injection, not merely a file found on disk.
			static const bool trace_replacements = (std::getenv("DOD_TEXTURE_TRACE") != nullptr);
			if (trace_replacements)
				std::fprintf(stderr, "[hd-texture] injected %" PRIx64 "-%" PRIx64 "-%08x region=%ux%u source=%ux%u hd=%ux%u%s\n",
					name.TEX0Hash, name.CLUTHash, name.bits, name.region_width, name.region_height,
					name.Width(), name.Height(), it->second.width, it->second.height, prefetched ? " (prefetched)" : "");
		}
	}
	s_async_loaded_textures.erase(s_async_loaded_textures.begin(), s_async_loaded_textures.begin() + done);
}

u64 GSTextureReplacements::DodTextureBytes(const ReplacementTexture& rtex)
{
	u64 bytes = rtex.data.size();
	for (const ReplacementTexture::MipData& mip : rtex.mips)
		bytes += mip.data.size();
	return bytes;
}

void GSTextureReplacements::DodNoteCached(const TextureName& name, const ReplacementTexture& rtex)
{
	s_dod_cache_bytes += DodTextureBytes(rtex);
	s_dod_cache_use[name] = ++s_dod_use_tick;
	DodTrimCache();
}

void GSTextureReplacements::DodTrimCache()
{
	// The cap (texture_cache_mb, 0 = none) is for the CPU copies only: what the GPU holds is not touched. A dropped
	// replacement is loaded from disk again if it is needed later. Never with precaching (it wants them all), never one
	// waiting to be uploaded, and never the newest (the caller may hold a reference to it).
	if (s_dod_cache_cap == 0 || GSConfig.PrecacheTextureReplacements)
		return;
	while (s_dod_cache_bytes > s_dod_cache_cap)
	{
		auto oldest = s_replacement_texture_cache.end();
		u64 oldest_use = ~0ull;
		for (auto it = s_replacement_texture_cache.begin(); it != s_replacement_texture_cache.end(); ++it)
		{
			if (s_pending_async_load_textures.find(it->first) != s_pending_async_load_textures.end())
				continue;
			const auto use = s_dod_cache_use.find(it->first);
			const u64 last = (use != s_dod_cache_use.end()) ? use->second : 0;
			if (last < oldest_use && last != s_dod_use_tick)
			{
				oldest_use = last;
				oldest = it;
			}
		}
		if (oldest == s_replacement_texture_cache.end())
			break;
		s_dod_cache_bytes -= std::min(s_dod_cache_bytes, DodTextureBytes(oldest->second));
		s_dod_cache_use.erase(oldest->first);
		s_replacement_texture_cache.erase(oldest);
	}
}

void GSTextureReplacements::DodClearState()
{
	s_dod_cache_bytes = 0;
	s_dod_cache_use.clear();
	s_dod_preupload.clear();
	s_dod_scene_textures.clear();
	s_dod_prefetched_scene = 0xFFFFFFFFu;
}

void GSTextureReplacements::DodConfigure(u32 threads, u32 cache_mb, u32 upload_mb, u32 keep_mb, std::string prefetch_file)
{
	s_dod_threads = std::clamp<u32>(threads, 1u, 8u);
	s_dod_cache_cap = static_cast<u64>(cache_mb) << 20;
	s_dod_upload_budget = static_cast<u64>(upload_mb) << 20;
	s_dod_keep_bytes = static_cast<u64>(keep_mb) << 20;
	s_dod_prefetch_file = std::move(prefetch_file);
}

void GSTextureReplacements::DodRequestScenePrefetch(u32 scene)
{
	s_dod_requested_scene.store(scene, std::memory_order_relaxed);
}

u64 GSTextureReplacements::DodReplacementKeepBytes()
{
	return s_dod_keep_bytes;
}

void GSTextureReplacements::DodLoadPrefetchList(const std::string& texture_dir)
{
	// <texture dir>/dod_prefetch.txt (tools/scripts/diag/build_hd_prefetch.py): one line per scene, "0x181 name name ...",
	// the names of replacement files (without the extension) seen drawn there in earlier runs.
	if (s_dod_prefetch_file == "0")
		return;
	const std::string path = s_dod_prefetch_file.empty() ? Path::Combine(texture_dir, "dod_prefetch.txt") : s_dod_prefetch_file;
	const std::optional<std::string> text = FileSystem::ReadFileToString(path.c_str());
	if (!text.has_value())
	{
		std::fprintf(stderr, "[hd-texture] no room prefetch list (%s)\n", path.c_str());
		return;
	}
	size_t rooms = 0, names = 0, missing = 0;
	std::string_view rest(*text);
	while (!rest.empty())
	{
		const size_t eol = rest.find('\n');
		const std::string_view line = rest.substr(0, eol);
		rest = (eol == std::string_view::npos) ? std::string_view() : rest.substr(eol + 1);
		if (line.empty() || line[0] == '#')
			continue;
		std::vector<TextureName>* list = nullptr;
		size_t pos = 0;
		for (;;)
		{
			while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t' || line[pos] == '\r'))
				pos++;
			size_t end = pos;
			while (end < line.size() && line[end] != ' ' && line[end] != '\t' && line[end] != '\r')
				end++;
			if (end == pos)
				break;
			const std::string token(line.substr(pos, end - pos));
			pos = end;
			if (!list)
			{
				list = &s_dod_scene_textures[static_cast<u32>(std::strtoul(token.c_str(), nullptr, 16))];
				rooms++;
				continue;
			}
			const std::optional<TextureName> name = ParseReplacementName((token + ".dds").c_str());
			if (name.has_value() && s_replacement_texture_filenames.find(*name) != s_replacement_texture_filenames.end())
			{
				list->push_back(*name);
				names++;
			}
			else
			{
				missing++;
			}
		}
	}
	std::fprintf(stderr, "[hd-texture] room prefetch: %zu rooms, %zu textures (%zu not in the pack or excluded), %u loader threads, "
						 "cache cap %llu MB, upload %llu MB/vsync, VRAM kept %llu MB\n",
		rooms, names, missing, s_dod_threads, static_cast<unsigned long long>(s_dod_cache_cap >> 20),
		static_cast<unsigned long long>(s_dod_upload_budget >> 20), static_cast<unsigned long long>(s_dod_keep_bytes >> 20));
}

void GSTextureReplacements::DodHandleScenePrefetch()
{
	// GS thread, every vsync: when the game's scene changed, queue that room's replacements as low-priority loads
	// (what the game draws meanwhile goes first) and mark them for upload as soon as they are loaded, which is
	// during the room's loading screen, so its first frames need no upload. Those still in VRAM are skipped.
	const u32 scene = s_dod_requested_scene.load(std::memory_order_relaxed);
	if (scene == s_dod_prefetched_scene)
		return;
	s_dod_prefetched_scene = scene;
	const auto list = s_dod_scene_textures.find(scene);
	if (list == s_dod_scene_textures.end() || !g_texture_cache)
		return;

	const bool mipmap = GSConfig.HWMipmap || GSConfig.TriFilter == TriFiltering::Forced;
	u32 queued = 0, loaded = 0, resident = 0;
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	for (const TextureName& name : list->second)
	{
		const auto file = s_replacement_texture_filenames.find(name);
		if (file == s_replacement_texture_filenames.end() || file->second.pc_ui_direct)
			continue;
		if (g_texture_cache->DodHasHashCacheReplacement(HashCacheKeyFromTextureName(name)))
		{
			resident++;
			continue;
		}
		s_dod_preupload.insert(name);
		if (s_replacement_texture_cache.find(name) != s_replacement_texture_cache.end())
		{
			// loaded before (CPU copy) but no longer in VRAM: just the upload
			s_dod_cache_use[name] = ++s_dod_use_tick;
			if (s_pending_async_load_textures.emplace(name, true).second)
				s_async_loaded_textures.emplace_back(name, mipmap);
			loaded++;
		}
		else
		{
			QueueAsyncReplacementTextureLoad(name, file->second, mipmap, true);
			queued++;
		}
	}
	static const bool trace_replacements = (std::getenv("DOD_TEXTURE_TRACE") != nullptr);
	if (trace_replacements)
		std::fprintf(stderr, "[hd-texture] scene 0x%03X prefetch: %u to load, %u to upload, %u still in VRAM\n", scene, queued,
			loaded, resident);
}

void GSTextureReplacements::DumpTexture(const GSTextureCache::HashCacheKey& hash, const GIFRegTEX0& TEX0,
	const GIFRegTEXA& TEXA, GSTextureCache::SourceRegion region, GSLocalMemory& mem, u32 level)
{
	// check if it's been dumped or replaced already
	const TextureName name(CreateTextureName(hash, level));
	{
		std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
		if (s_dumped_textures.find(name) != s_dumped_textures.end() || s_replacement_texture_filenames.find(name) != s_replacement_texture_filenames.end())
			return;

		s_dumped_textures.insert(name);
	}

	// already exists on disk?
	std::string filename(GetDumpFilename(name, level));
	if (filename.empty() || FileSystem::FileExists(filename.c_str()))
		return;

	const std::string_view title(Path::GetFileTitle(filename));
	DevCon.WriteLn("Dumping %ux%u texture '%.*s'.", name.Width(), name.Height(), static_cast<int>(title.size()), title.data());

	// compute width/height
	const GSLocalMemory::psm_t& psm = GSLocalMemory::m_psm[TEX0.PSM];
	const GSVector2i& bs = psm.bs;
	const int tw = region.HasX() ? region.GetWidth() : (1 << TEX0.TW);
	const int th = region.HasY() ? region.GetHeight() : (1 << TEX0.TH);
	const GSVector4i rect(region.GetRect(tw, th));
	const GSVector4i block_rect(rect.ralign<Align_Outside>(bs));
	const int read_width = block_rect.width();
	const int read_height = block_rect.height();
	const u32 pitch = static_cast<u32>(read_width) * sizeof(u32);

	// use per-texture buffer so we can compress the texture asynchronously and not block the GS thread
	// must be 32 byte aligned for ReadTexture().
	u8* buffer = static_cast<u8*>(_aligned_malloc(pitch * static_cast<u32>(read_height), 32));
	psm.rtx(mem, mem.GetOffset(TEX0.TBP0, TEX0.TBW, TEX0.PSM), block_rect, buffer, pitch, TEXA);

	// okay, now we can actually dump it
	const u32 buffer_offset = ((rect.top - block_rect.top) * pitch) + ((rect.left - block_rect.left) * sizeof(u32));
	QueueWorkerThreadItem([filename = std::move(filename), tw, th, pitch, buffer, buffer_offset]() {
		if (!SavePNGImage(filename.c_str(), tw, th, buffer + buffer_offset, pitch))
			Console.Error(fmt::format("Failed to dump texture to '{}'.", filename));
		_aligned_free(buffer);
	}, false);
}

void GSTextureReplacements::ClearDumpedTextureList()
{
	std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
	s_dumped_textures.clear();
}

u32 GSTextureReplacements::GetDumpedTextureCount()
{
	std::unique_lock<std::mutex> lock(s_dumped_textures_mutex);
	return static_cast<u32>(s_dumped_textures.size());
}

u32 GSTextureReplacements::GetLoadedTextureCount()
{
	std::unique_lock<std::mutex> lock(s_replacement_texture_cache_mutex);
	return static_cast<u32>(s_replacement_texture_cache.size());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Worker Thread
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void GSTextureReplacements::StartWorkerThread()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);

	if (!s_worker_threads.empty())
		return;

	s_worker_thread_running = true;
	for (u32 i = 0; i < s_dod_threads; i++)
		s_worker_threads.emplace_back(WorkerThreadEntryPoint);
}

void GSTextureReplacements::StopWorkerThread()
{
	{
		std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
		if (s_worker_threads.empty())
			return;

		s_worker_thread_running = false;
		s_worker_thread_cv.notify_all();
	}

	for (std::thread& thread : s_worker_threads)
		thread.join();
	s_worker_threads.clear();

	// clear out workery-things too
	CancelPendingLoadsAndDumps();
}

void GSTextureReplacements::QueueWorkerThreadItem(std::function<void()> fn, bool high_priority)
{
	pxAssert(!s_worker_threads.empty());

	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	if (!high_priority)
	{
		// Low priority => throw on end.
		s_worker_thread_queue.emplace_back(std::move(fn), false);
	}
	else
	{
		auto iter = s_worker_thread_queue.rbegin();
		for (; iter != s_worker_thread_queue.rend(); ++iter)
		{
			// Found our first high priority item?
			if (iter->second)
			{
				// Insert after here!
				break;
			}
		}

		if (iter != s_worker_thread_queue.rend())
		{
			// Insert after the last high priority item. Remember base() points to the next element.
			s_worker_thread_queue.insert(iter.base(), std::make_pair(std::move(fn), true));
		}
		else
		{
			// All low-priority => insert at beginning.
			s_worker_thread_queue.emplace_front(std::move(fn), true);
		}
	}

	s_worker_thread_cv.notify_one();
}

void GSTextureReplacements::WorkerThreadEntryPoint()
{
	Threading::SetNameOfCurrentThread("HD Texture Loader");
	pcsx2gsDodLowerThreadPriority();

	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	while (s_worker_thread_running)
	{
		if (s_worker_thread_queue.empty())
		{
			s_worker_thread_cv.wait(lock);
			continue;
		}

		std::function<void()> fn = std::move(s_worker_thread_queue.front().first);
		s_worker_thread_queue.pop_front();
		s_worker_busy++;
		lock.unlock();
		fn();
		lock.lock();
		s_worker_busy--;
	}
}

void GSTextureReplacements::SyncWorkerThread()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	if (s_worker_threads.empty())
		return;

	// not the most efficient by far, but it only gets called on config changes, so whatever
	// ([DoD] with several workers, also wait for the items they are running)
	for (;;)
	{
		if (s_worker_thread_queue.empty() && s_worker_busy == 0)
			break;

		lock.unlock();
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		lock.lock();
	}
}

void GSTextureReplacements::CancelPendingLoadsAndDumps()
{
	std::unique_lock<std::mutex> lock(s_worker_thread_mutex);
	while (!s_worker_thread_queue.empty())
		s_worker_thread_queue.pop_back();
	s_async_loaded_textures.clear();
	s_pending_async_load_textures.clear();
}

bool GSTextureReplacements::CFile::Read(void* data, size_t amt)
{
	return std::fread(data, amt, 1, file);
}

bool GSTextureReplacements::CFile::Seek(size_t offset)
{
	return std::fseek(file, offset, SEEK_SET) == 0;
}

s64 GSTextureReplacements::CFile::Size()
{
	return FileSystem::FSize64(file);
}

bool GSTextureReplacements::MemoryFile::Read(void* data, size_t amt)
{
	if (amt > len - pos)
		return false;
	memcpy(data, static_cast<const u8*>(buffer) + pos, amt);
	pos += amt;
	return true;
}

bool GSTextureReplacements::MemoryFile::Seek(size_t offset)
{
	pos = std::min(offset, len);
	return true;
}

s64 GSTextureReplacements::MemoryFile::Size()
{
	return len;
}
