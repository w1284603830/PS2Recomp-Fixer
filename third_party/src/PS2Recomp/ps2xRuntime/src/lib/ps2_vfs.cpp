#include "runtime/ps2_vfs.h"

#include "runtime/ps2_memory.h"
#include "runtime/ps2_rom_device.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>
#include <fcntl.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace
{
    class HostOpenFile final : public IPS2OpenFile
    {
    public:
        HostOpenFile(int descriptor, uint32_t flags) : m_descriptor(descriptor),
            m_readable((flags & PS2_FIO_O_RDONLY) != 0), m_writable((flags & PS2_FIO_O_WRONLY) != 0) {}
        ~HostOpenFile() override
        {
#ifdef _WIN32
            _close(m_descriptor);
#else
            ::close(m_descriptor);
#endif
        }
        int64_t read(void *destination, size_t size) override
        {
            if (!m_readable || (!destination && size)) return -1;
            if (!size) return 0;
#ifdef _WIN32
            return _read(m_descriptor, destination, static_cast<unsigned>(std::min(size, size_t(INT_MAX))));
#else
            return ::read(m_descriptor, destination, std::min(size, size_t(INT64_MAX)));
#endif
        }
        int64_t write(const void *source, size_t size) override
        {
            if (!m_writable || (!source && size)) return -1;
            if (!size) return 0;
#ifdef _WIN32
            return _write(m_descriptor, source, static_cast<unsigned>(std::min(size, size_t(INT_MAX))));
#else
            return ::write(m_descriptor, source, std::min(size, size_t(INT64_MAX)));
#endif
        }
        int64_t seek(int64_t offset, int whence) override
        {
            if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) return -1;
#ifdef _WIN32
            return _lseeki64(m_descriptor, offset, whence);
#else
            if (offset < std::numeric_limits<off_t>::min() || offset > std::numeric_limits<off_t>::max()) return -1;
            return ::lseek(m_descriptor, static_cast<off_t>(offset), whence);
#endif
        }
    private:
        int m_descriptor;
        bool m_readable, m_writable;
    };

    std::unique_ptr<IPS2OpenFile> openHostFile(const std::filesystem::path& path, uint32_t flags)
    {
        // Translate creation/truncation/append independently. The OS enforces
        // access rights and atomically implements O_CREAT|O_EXCL.
        const uint32_t access = flags & PS2_FIO_O_RDWR;
#ifdef _WIN32
        int mode = access == PS2_FIO_O_RDONLY ? _O_RDONLY : access == PS2_FIO_O_WRONLY ? _O_WRONLY : _O_RDWR;
        mode |= _O_BINARY;
        if (flags & PS2_FIO_O_CREAT) mode |= _O_CREAT;
        if (flags & PS2_FIO_O_TRUNC) mode |= _O_TRUNC;
        if (flags & PS2_FIO_O_APPEND) mode |= _O_APPEND;
        if ((flags & (PS2_FIO_O_CREAT | PS2_FIO_O_EXCL)) == (PS2_FIO_O_CREAT | PS2_FIO_O_EXCL)) mode |= _O_EXCL;
        const int descriptor = _wopen(path.c_str(), mode, _S_IREAD | _S_IWRITE);
#else
        int mode = access == PS2_FIO_O_RDONLY ? O_RDONLY : access == PS2_FIO_O_WRONLY ? O_WRONLY : O_RDWR;
        if (flags & PS2_FIO_O_CREAT) mode |= O_CREAT;
        if (flags & PS2_FIO_O_TRUNC) mode |= O_TRUNC;
        if (flags & PS2_FIO_O_APPEND) mode |= O_APPEND;
        if ((flags & (PS2_FIO_O_CREAT | PS2_FIO_O_EXCL)) == (PS2_FIO_O_CREAT | PS2_FIO_O_EXCL)) mode |= O_EXCL;
        const int descriptor = ::open(path.c_str(), mode, 0666);
#endif
        if (descriptor < 0) return {};
        return std::make_unique<HostOpenFile>(descriptor, flags);
    }

    class MemoryOpenFile final : public IPS2OpenFile
    {
    public:
        explicit MemoryOpenFile(std::vector<uint8_t> bytes) : m_bytes(std::move(bytes)) {}

        int64_t read(void *destination, size_t size) override
        {
            if (!destination && size != 0u)
                return -1;
            const size_t available = m_position < m_bytes.size() ? m_bytes.size() - m_position : 0u;
            const size_t count = std::min(size, available);
            if (count != 0u)
                std::memcpy(destination, m_bytes.data() + m_position, count);
            m_position += count;
            return static_cast<int64_t>(count);
        }

        int64_t write(const void *, size_t) override
        {
            return -1;
        }

        int64_t seek(int64_t offset, int whence) override
        {
            int64_t base = 0;
            if (whence == SEEK_CUR)
                base = static_cast<int64_t>(m_position);
            else if (whence == SEEK_END)
                base = static_cast<int64_t>(m_bytes.size());
            else if (whence != SEEK_SET)
                return -1;

            // Compare before adding: INT64_MAX + a nonzero base must not overflow.
            if (offset < -base || (offset >= 0 && static_cast<uint64_t>(offset) > m_bytes.size() - static_cast<size_t>(base)))
                return -1;
            const int64_t position = base + offset;
            m_position = static_cast<size_t>(position);
            return position;
        }

    private:
        std::vector<uint8_t> m_bytes;
        size_t m_position = 0u;
    };

    bool safeRelativePath(std::string_view suffix, std::filesystem::path &relative)
    {
        try { relative = std::filesystem::path(std::u8string(suffix.begin(), suffix.end())).lexically_normal(); }
        catch (const std::filesystem::filesystem_error&) { return false; }
        if (relative.is_absolute() || relative.has_root_name())
            return false;
        for (const auto &part : relative)
        {
            if (part == "..")
                return false;
        }
        return true;
    }

    std::time_t toTimeT(std::filesystem::file_time_type value)
    {
        const auto systemValue = std::chrono::time_point_cast<std::chrono::system_clock::duration>(value - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(systemValue);
    }
}

PS2Vfs::~PS2Vfs() = default;

int32_t PS2Vfs::open(std::string_view path, uint32_t flags, const PS2VfsMounts &mounts, const PS2RomDevice &rom)
{
    const ps2x::iop::ParsedPs2Path parsed = ps2x::iop::parsePs2Path(path);
    if (!parsed)
        return -1;

    const uint32_t access = flags & PS2_FIO_O_RDWR;
    if (access == 0 || (access == PS2_FIO_O_RDONLY && (flags & (PS2_FIO_O_TRUNC | PS2_FIO_O_APPEND)))) return -1;
    if (parsed.device == ps2x::iop::Ps2PathDevice::Cdrom &&
        (access != PS2_FIO_O_RDONLY || (flags & PS2_FIO_O_CREAT))) return -1;
    std::unique_ptr<IPS2OpenFile> file;
    if (parsed.device == ps2x::iop::Ps2PathDevice::Rom0)
    {
        const uint32_t access = flags & PS2_FIO_O_RDWR;
        if (access != PS2_FIO_O_RDONLY || (flags & (PS2_FIO_O_CREAT | PS2_FIO_O_TRUNC)) != 0u)
            return -1;
        std::vector<uint8_t> bytes;
        if (!rom.readFile(parsed.path, bytes))
            return -1;
        file = std::make_unique<MemoryOpenFile>(std::move(bytes));
    }
    else
    {
        std::filesystem::path hostPath;
        if (!resolveHostPath(path, mounts, hostPath))
            return -1;

        file = openHostFile(hostPath, flags);
        if (!file) return -1;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_nextDescriptor < 3)
        m_nextDescriptor = 3;
    const int32_t first = m_nextDescriptor;
    do
    {
        const int32_t descriptor = m_nextDescriptor;
        m_nextDescriptor = descriptor == INT32_MAX ? 3 : descriptor + 1;
        if (!m_descriptors.contains(descriptor))
        {
            m_descriptors.emplace(descriptor, OpenDescriptor{std::move(file), parsed.deviceName, std::string(path)});
            return descriptor;
        }
    } while (m_nextDescriptor != first);
    return -1;
}

int32_t PS2Vfs::close(int32_t descriptor)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_descriptors.erase(descriptor) == 1u ? 0 : -1;
}

int64_t PS2Vfs::read(int32_t descriptor, void *destination, size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_descriptors.find(descriptor);
    return found == m_descriptors.end() ? -1 : found->second.file->read(destination, size);
}

int64_t PS2Vfs::write(int32_t descriptor, const void *source, size_t size)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_descriptors.find(descriptor);
    return found == m_descriptors.end() ? -1 : found->second.file->write(source, size);
}

int64_t PS2Vfs::seek(int32_t descriptor, int64_t offset, int whence)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto found = m_descriptors.find(descriptor);
    return found == m_descriptors.end() ? -1 : found->second.file->seek(offset, whence);
}

bool PS2Vfs::stat(std::string_view path, const PS2VfsMounts &mounts, const PS2RomDevice &rom, PS2VfsStat &result) const
{
    result = {};
    const ps2x::iop::ParsedPs2Path parsed = ps2x::iop::parsePs2Path(path);
    if (!parsed)
        return false;
    if (parsed.device == ps2x::iop::Ps2PathDevice::Rom0)
    {
        if (!rom.fileSize(parsed.path, result.size))
            return false;
        result.readOnly = true;
        return true;
    }

    std::filesystem::path hostPath;
    if (!resolveHostPath(path, mounts, hostPath))
        return false;
    std::error_code error;
    const auto status = std::filesystem::status(hostPath, error);
    if (error || !std::filesystem::exists(status))
        return false;
    result.directory = std::filesystem::is_directory(status);
    if (!result.directory)
    {
        result.size = std::filesystem::file_size(hostPath, error);
        if (error)
            return false;
    }
    const auto modified = std::filesystem::last_write_time(hostPath, error);
    if (!error)
        result.created = result.accessed = result.modified = toTimeT(modified);
    result.readOnly = parsed.device == ps2x::iop::Ps2PathDevice::Cdrom || (status.permissions() & std::filesystem::perms::owner_write) == std::filesystem::perms::none;
    return true;
}

bool PS2Vfs::resolveHostPath(std::string_view path, const PS2VfsMounts &mounts, std::filesystem::path &result) const
{
    result.clear();
    const ps2x::iop::ParsedPs2Path parsed = ps2x::iop::parsePs2Path(path);
    if (!parsed || parsed.device == ps2x::iop::Ps2PathDevice::Rom0)
        return false;
    if (parsed.device == ps2x::iop::Ps2PathDevice::NativeHost)
    {
        try { result = std::filesystem::path(std::u8string(parsed.path.begin(), parsed.path.end())).lexically_normal(); }
        catch (const std::filesystem::filesystem_error&) { return false; }
        return !result.empty();
    }

    std::filesystem::path base;
    switch (parsed.device)
    {
    case ps2x::iop::Ps2PathDevice::Host:
        base = mounts.hostRoot;
        break;
    case ps2x::iop::Ps2PathDevice::Cdrom:
        base = mounts.cdRoot;
        break;
    case ps2x::iop::Ps2PathDevice::MemoryCard0:
        base = mounts.memoryCard0Root;
        break;
    default:
        return false;
    }
    std::filesystem::path relative;
    if (base.empty() || !safeRelativePath(parsed.path, relative))
        return false;
    result = (base / relative).lexically_normal();
    return true;
}

std::vector<PS2VfsDescriptorInfo> PS2Vfs::descriptors() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<PS2VfsDescriptorInfo> result;
    result.reserve(m_descriptors.size());
    for (const auto &[descriptor, entry] : m_descriptors)
        result.push_back({descriptor, entry.device, entry.path});
    return result;
}
