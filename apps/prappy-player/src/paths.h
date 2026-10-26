#pragma once
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <stdexcept>
namespace fs = std::filesystem;
inline std::string utf8(const fs::path& p) {
    auto s = p.u8string(); return {reinterpret_cast<const char*>(s.data()), s.size()};
}
inline std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \r\n\t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \r\n\t") - a + 1);
}
inline std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}
inline std::wstring searchText(const std::string& text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring value(count, L'\0');
    if (count) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), value.data(), count);
    CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    std::replace(value.begin(), value.end(), L'\\', L'/');
    return value;
}
inline std::wstring key(const fs::path& p) {
    auto s = p.lexically_normal().wstring();
    CharLowerBuffW(s.data(), static_cast<DWORD>(s.size())); return s;
}
inline fs::path extendedPath(const fs::path& path) {
    auto value = path.wstring();
    if (value.starts_with(L"\\\\?\\")) return path;
    if (value.starts_with(L"\\\\")) return L"\\\\?\\UNC\\" + value.substr(2);
    return L"\\\\?\\" + value;
}
inline fs::path displayPath(const fs::path& path) {
    auto value = path.wstring();
    if (value.starts_with(L"\\\\?\\UNC\\")) return L"\\\\" + value.substr(8);
    if (value.starts_with(L"\\\\?\\")) return value.substr(4);
    return path;
}
inline std::string readablePath(const fs::path& path, const std::vector<fs::path>& roots) {
    if (path.empty()) return {};
    fs::path relative;
    bool afterScraper = false;
    for (const auto& part : path) {
        if (afterScraper) relative /= part;
        else if (key(part) == L"scraper") afterScraper = true;
    }
    if (afterScraper) return utf8(relative);
    for (const auto& root : roots) {
        relative = path.lexically_relative(root);
        if (!relative.empty() && !relative.is_absolute() && *relative.begin() != L"..")
            return utf8(root.filename() / relative);
    }
    return utf8(path);
}
inline fs::path executableDir() {
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (!n || n >= buf.size()) throw std::runtime_error("Cannot locate executable directory.");
    buf.resize(n); return fs::path(buf).parent_path();
}
