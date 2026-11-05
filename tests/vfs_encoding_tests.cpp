// Production VFS/path/ROM, explicit example decoder; synthetic files only.
#include "runtime/ps2_vfs.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_rom_device.h"
#include "path_encoding_example.h"
#include <cassert>
#include <fstream>
#include <initializer_list>
#include <iterator>

static std::string bytes(std::initializer_list<unsigned> values)
{
    std::string out;
    for (const auto value : values) out.push_back(static_cast<char>(value));
    return out;
}
struct Profile { const char* label; port_paths::Encoding encoding; std::string raw, utf8, rawTrail, utf8Trail; };
#include "encoding_fixtures.inc"
static std::filesystem::path u8path(const std::string& v) { return std::filesystem::path(std::u8string(v.begin(), v.end())); }
static void put(const std::filesystem::path& p, const std::string& v)
{
    std::ofstream out(p, std::ios::binary); out.write(v.data(), v.size()); assert(out.good());
}
static std::string get(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary); assert(in.good()); return {std::istreambuf_iterator<char>(in), {}};
}
static std::string decode(const std::string& v, port_paths::Encoding e)
{
    std::string out = "untouched"; assert(port_paths::decode(v, e, out)); return out;
}
static size_t entries(const std::filesystem::path& p)
{
    return static_cast<size_t>(std::distance(std::filesystem::recursive_directory_iterator(p), std::filesystem::recursive_directory_iterator{}));
}
int main(int argc, char** argv)
{
    assert(argc == 4);
    const std::string mode = argv[1], label = argv[2];
    const Profile* profile = nullptr;
    for (const auto& candidate : profiles) if (label == candidate.label) profile = &candidate;
    assert(profile); const auto& p = *profile;
    const auto root = std::filesystem::path(argv[3]) / std::filesystem::path(u8"隔离_目录");
    std::filesystem::create_directories(root);
    PS2Vfs vfs; PS2RomDevice rom; PS2VfsMounts mounts{root, root, root};
    const auto file = root / u8path(p.utf8), sentinel = root / "sentinel.bin";
    const std::string saved = bytes({0,255,1,2,3,128}); put(sentinel, "DO NOT TOUCH");
    if (mode == "read")
    {
        put(file, saved); const auto guest = decode("host:" + p.raw, p.encoding);
        assert(guest == "host:" + p.utf8);
        const int fd = vfs.open(guest, PS2_FIO_O_RDONLY, mounts, rom); assert(fd >= 3);
        std::string data(saved.size(), '\0'); assert(vfs.read(fd, data.data(), data.size()) == static_cast<int64_t>(data.size()));
        assert(data == saved && vfs.close(fd) == 0);
        PS2VfsStat status; assert(vfs.stat(guest, mounts, rom, status) && status.size == saved.size()); assert(get(file) == saved);
        const auto cd = decode("cdrom0:" + p.raw + ";1", p.encoding);
        const int disc = vfs.open(cd, PS2_FIO_O_RDONLY, mounts, rom); assert(disc >= 3 && vfs.close(disc) == 0);
        assert(vfs.stat(cd, mounts, rom, status) && status.readOnly);
    }
    else if (mode == "create")
    {
        const auto guest = decode("mc0:" + p.raw, p.encoding);
        const int fd = vfs.open(guest, PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT | PS2_FIO_O_EXCL, mounts, rom); assert(fd >= 3);
        assert(vfs.write(fd, saved.data(), saved.size()) == static_cast<int64_t>(saved.size()) && vfs.close(fd) == 0);
        assert(get(file) == saved && entries(root) == 2);
        assert(vfs.open(guest, PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT | PS2_FIO_O_EXCL, mounts, rom) == -1); assert(get(file) == saved);
    }
    else if (mode == "save")
    {
        put(file, saved); const auto guest = decode("mc0:" + p.raw, p.encoding);
        int fd = vfs.open(guest, PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT, mounts, rom); assert(fd >= 3);
        assert(vfs.write(fd, "X", 1) == 1 && vfs.close(fd) == 0); assert(get(file) == "X" + saved.substr(1));
        fd = vfs.open(guest, PS2_FIO_O_WRONLY | PS2_FIO_O_TRUNC, mounts, rom); assert(fd >= 3);
        assert(vfs.write(fd, "new", 3) == 3 && vfs.close(fd) == 0); assert(get(file) == "new" && entries(root) == 2);
    }
    else if (mode == "trail")
    {
        const auto sub = root / "slot"; std::filesystem::create_directory(sub);
        const auto expected = sub / u8path(p.utf8Trail); put(expected, saved);
        const auto guest = decode("mc0:\\slot\\" + p.rawTrail, p.encoding);
        std::filesystem::path resolved; assert(vfs.resolveHostPath(guest, mounts, resolved) && resolved == expected);
        assert(resolved.filename() == u8path(p.utf8Trail));
        const int fd = vfs.open(guest, PS2_FIO_O_RDONLY, mounts, rom); assert(fd >= 3 && vfs.close(fd) == 0); assert(entries(root) == 3);
    }
    else if (mode == "raw-reject")
    {
        assert(p.encoding != port_paths::Encoding::Utf8); put(file, saved);
        for (const auto& raw : {p.raw, p.rawTrail})
        {
            assert(!ps2::paths::validUtf8(raw)); const auto before = entries(root);
            for (const auto* prefix : {"host:", "mc0:", "cdrom0:"})
            {
                const std::string guest = prefix + raw;
                for (const auto flags : {PS2_FIO_O_RDONLY, PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT, PS2_FIO_O_WRONLY | PS2_FIO_O_TRUNC})
                    assert(vfs.open(guest, flags, mounts, rom) == -1);
                std::filesystem::path resolved = "stale"; assert(!vfs.resolveHostPath(guest, mounts, resolved) && resolved.empty());
                PS2VfsStat status; status.size = 99; assert(!vfs.stat(guest, mounts, rom, status) && status.size == 0);
            }
            assert(entries(root) == before && get(file) == saved && vfs.descriptors().empty());
        }
    }
    else if (mode == "decode-invalid")
    {
        std::string out = "untouched";
        assert(!port_paths::decode(bytes({0x81}), p.encoding, out) && out == "untouched");
        assert(!port_paths::decode(std::string("host:a\0b", 8), p.encoding, out) && out == "untouched");
        assert(!port_paths::decode("host:save", static_cast<port_paths::Encoding>(0), out) && out == "untouched");
    }
    else if (mode == "utf8-invalid")
    {
        const std::string invalid[] = {bytes({0}),bytes({0x80}),bytes({0xc0,0xaf}),bytes({0xc1,0xbf}),bytes({0xc2}),
            bytes({0xe0,0x80,0xaf}),bytes({0xe2,0x82}),bytes({0xed,0xa0,0x80}),bytes({0xef,0xbf}),
            bytes({0xf0,0x80,0x80,0xaf}),bytes({0xf4,0x90,0x80,0x80}),bytes({0xf5,0x80,0x80,0x80}),bytes({0xff})};
        for (const auto& value : invalid)
        {
            assert(!ps2::paths::validUtf8(value)); const std::string guest = "mc0:" + value;
            assert(vfs.open(guest, PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT, mounts, rom) == -1);
            std::filesystem::path resolved; assert(!vfs.resolveHostPath(guest, mounts, resolved));
            PS2VfsStat status; assert(!vfs.stat(guest, mounts, rom, status));
        }
        for (const auto& value : {bytes({0xc2,0x80}),bytes({0xe0,0xa0,0x80}),bytes({0xed,0x9f,0xbf}),bytes({0xf0,0x90,0x80,0x80}),bytes({0xf4,0x8f,0xbf,0xbf})})
            assert(ps2::paths::validUtf8(value));
        assert(entries(root) == 1);
    }
    else if (mode == "ambiguous")
    {
        // Legal UTF-8 and legal legacy bytes with different meanings: no detection.
        const auto raw = label == "gbk" ? bytes({0xc2,0xa0}) : bytes({0xc2,0xa1});
        const auto expected = label == "gbk" ? bytes({0xe8,0x81,0xbd}) : bytes({0xe7,0x99,0x92});
        assert(decode(raw, p.encoding) == expected && decode(raw, port_paths::Encoding::Utf8) == raw);
        put(root / u8path(raw + ".bin"), "UTF8"); put(root / u8path(expected + ".bin"), "LEGACY");
        const int fd = vfs.open("host:" + raw + ".bin", PS2_FIO_O_WRONLY | PS2_FIO_O_TRUNC, mounts, rom);
        assert(fd >= 3 && vfs.close(fd) == 0);
        assert(get(root / u8path(raw + ".bin")).empty() && get(root / u8path(expected + ".bin")) == "LEGACY");
    }
    else if (mode == "switches")
    {
        for (int repeat = 0; repeat < 3; ++repeat)
            for (const auto& next : profiles) assert(decode(next.raw, next.encoding) == next.utf8);
        for (const auto& next : profiles) assert(decode("mc0:/ASCII.bin", next.encoding) == "mc0:/ASCII.bin");
        auto value = p.raw; assert(port_paths::decode(value, p.encoding, value) && value == p.utf8);
    }
    else if (mode == "native")
    {
#ifdef _WIN32
        put(file, saved); const auto chars = file.generic_u8string(); const std::string path(chars.begin(), chars.end());
        std::filesystem::path resolved; assert(vfs.resolveHostPath(path, mounts, resolved) && resolved == file);
        const int fd = vfs.open(path, PS2_FIO_O_RDONLY, mounts, rom); assert(fd >= 3 && vfs.close(fd) == 0);
        const auto invalid = path + bytes({0x81,0x5c});
        assert(!vfs.resolveHostPath(invalid, mounts, resolved) && resolved.empty());
        assert(vfs.open(invalid, PS2_FIO_O_WRONLY | PS2_FIO_O_CREAT, mounts, rom) == -1);
#else
        return 77;
#endif
    }
    else return 2;
    assert(get(sentinel) == "DO NOT TOUCH");
}
