#include "common.h"
namespace edm {
std::wstring wide(std::string_view s, unsigned cp) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(cp, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(n, L' ');
    MultiByteToWideChar(cp, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}
std::string utf8(std::wstring_view s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, ' ');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::string pathString(const fs::path& p) {
    return utf8(p.wstring());
}
std::string lower(std::string s) {
    auto w = wide(s);
    CharLowerBuffW(w.data(), DWORD(w.size()));
    return utf8(w);
}
std::string normalized(std::string s) {
    s = lower(s);
    s.erase(
        std::remove_if(s.begin(), s.end(),
                       [](unsigned char c) { return !(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9'); }),
        s.end());
    return s;
}
std::vector<uint8_t> readFile(const fs::path& p, size_t limit) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    require(bool(f), "Cannot open " + pathString(p));
    auto n = f.tellg();
    require(n >= 0 && uint64_t(n) <= limit, "File exceeds size limit: " + pathString(p));
    std::vector<uint8_t> b(size_t(n), uint8_t{});
    f.seekg(0);
    f.read(reinterpret_cast<char*>(b.data()), n);
    require(bool(f) || b.empty(), "Cannot read " + pathString(p));
    return b;
}
void writeFile(const fs::path& p, std::span<const uint8_t> b) {
    if (!p.parent_path().empty())
        fs::create_directories(p.parent_path());
    auto tmp = p;
    tmp += L".tmp-" + std::to_wstring(GetCurrentProcessId());
    {
        std::ofstream f(tmp, std::ios::binary);
        require(bool(f), "Cannot write " + pathString(tmp));
        f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
        require(bool(f), "Write failed " + pathString(tmp));
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(tmp);
        throw std::runtime_error("Cannot replace " + pathString(p));
    }
}
void writeJson(const fs::path& p, const Json& j) {
    auto text = j.dump(2, ' ', false, Json::error_handler_t::replace);
    writeFile(p, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}
std::string decodeText(std::span<const uint8_t> b) {
    if (b.size() >= 3 && b[0] == 239 && b[1] == 187 && b[2] == 191)
        b = b.subspan(3);
    auto s = std::string_view(reinterpret_cast<const char*>(b.data()), b.size());
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0) > 0 ||
        s.empty())
        return std::string(s);
    return utf8(wide(s, 1251));
}
bool within(const fs::path& child, const fs::path& root) {
    auto a = lower(pathString(fs::weakly_canonical(child))),
         b = lower(pathString(fs::weakly_canonical(root)));
    if (a == b)
        return true;
    if (!b.ends_with('\\'))
        b += '\\';
    return a.starts_with(b);
}
std::vector<fs::path> uniquePaths(const std::vector<fs::path>& paths) {
    std::vector<fs::path> out;
    std::set<std::string> seen;
    for (auto& p : paths) {
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            auto q = fs::weakly_canonical(p, ec);
            if (!ec && seen.insert(lower(pathString(q))).second)
                out.push_back(q);
        }
    }
    return out;
}
MappedFile::MappedFile(const fs::path& p) {
    file = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "Cannot open " + pathString(p));
    LARGE_INTEGER n{};
    if (!GetFileSizeEx(file, &n) || n.QuadPart <= 0) {
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        throw std::runtime_error("Empty or invalid file");
    }
    size = size_t(n.QuadPart);
    mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping)
        data = static_cast<const uint8_t*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!data) {
        if (mapping)
            CloseHandle(mapping);
        CloseHandle(file);
        file = INVALID_HANDLE_VALUE;
        throw std::runtime_error("File mapping failed");
    }
}
MappedFile::~MappedFile() {
    if (data)
        UnmapViewOfFile(data);
    if (mapping)
        CloseHandle(mapping);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
}
V4 slerp(V4 a, V4 b, double t) {
    a = qnormalize(a);
    b = qnormalize(b);
    double d = a.dot(b);
    if (d < 0) {
        b = -b;
        d = -d;
    }
    d = std::min(1., d);
    if (d > .9995)
        return qnormalize(a + (b - a) * t);
    double angle = acos(d);
    return (sin((1 - t) * angle) * a + sin(t * angle) * b) / sin(angle);
}
} // namespace edm
