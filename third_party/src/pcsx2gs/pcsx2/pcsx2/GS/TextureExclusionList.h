#pragma once
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

// Pack-owned full basenames (including extension), never a compiled game's hash list.
namespace GSHostTuning
{
class TextureExclusionList
{
    std::unordered_set<std::string> names;
    static std::string canonical(std::string_view value)
    {
        std::string out(value);
        for (char& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return out;
    }
public:
    size_t rejected = 0;
    bool limited = false;
    void clear() { names.clear(); rejected = 0; limited = false; }
    void load(std::string_view text)
    {
        clear();
        if (text.size() > 1024u * 1024u) { limited = true; return; }
        if (text.substr(0, 3) == "\xef\xbb\xbf") text.remove_prefix(3);
        while (!text.empty())
        {
            const size_t end = text.find('\n');
            auto line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            const size_t first = line.find_first_not_of(" \t\r");
            if (first == std::string_view::npos) continue;
            line.remove_prefix(first);
            line = line.substr(0, line.find_last_not_of(" \t\r") + 1);
            if (line.front() == '#') continue;
            if (line.size() > 240 || line.find("..") != std::string_view::npos ||
                line.find_first_of("/\\:\t\r") != std::string_view::npos || line.find('\0') != std::string_view::npos || line.find('.') == std::string_view::npos)
            { ++rejected; continue; }
            auto name = canonical(line);
            if (names.find(name) != names.end()) continue;
            if (names.size() >= 4096) { limited = true; break; }
            names.insert(std::move(name));
        }
    }
    bool excluded(std::string_view filename) const { return names.find(canonical(filename)) != names.end(); }
    size_t size() const { return names.size(); }
};
}
