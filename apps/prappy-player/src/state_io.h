#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <thread>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace stateio {
namespace fs = std::filesystem;
constexpr size_t MaxFile = 128 * 1024 * 1024;
constexpr size_t MaxRecords = 1000000;
constexpr size_t MaxString = 131072;
struct FutureVersion : std::runtime_error { using std::runtime_error::runtime_error; };
inline std::string pathText(const fs::path& path) {
    const auto value = path.u8string();
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}
inline fs::path pathFromText(const std::string& value) {
    if (value.empty() || value.size() > MaxString || value.find_first_of(std::string("\0\r\n", 3)) != std::string::npos ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0))
        throw std::runtime_error("Invalid saved path.");
    const fs::path path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size()));
    if (!path.is_absolute()) throw std::runtime_error("Saved path must be absolute.");
    return path;
}
inline std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot read " + pathText(path.filename()));
    const auto end = input.tellg();
    if (end < 0 || static_cast<uint64_t>(end) > MaxFile) throw std::runtime_error("State file is too large.");
    std::string bytes(static_cast<size_t>(end), '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("State file read failed.");
    return bytes;
}
inline void write(const fs::path& path, const std::string& bytes, bool backup = true) {
    if (bytes.size() > MaxFile) throw std::runtime_error("State file exceeds the size limit.");
    static std::atomic<uint64_t> sequence{0};
    const fs::path temporary = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create temporary state file (Windows " + std::to_string(GetLastError()) + ").");
    bool ok = true;
    for (size_t offset = 0; ok && offset < bytes.size();) {
        DWORD written = 0;
        const auto count = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1024 * 1024));
        ok = WriteFile(file, bytes.data() + offset, count, &written, nullptr) && written != 0;
        offset += written;
    }
    if (ok) ok = FlushFileBuffers(file) != FALSE;
    if (!CloseHandle(file)) ok = false;
    if (ok) {
        const fs::path previous = path.wstring() + L".bak";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            ok = ReplaceFileW(path.c_str(), temporary.c_str(), backup ? previous.c_str() : nullptr, 0, nullptr, nullptr) != FALSE;
        else ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    }
    if (!ok) {
        const auto code = GetLastError();
        DeleteFileW(temporary.c_str());
        throw std::runtime_error("Cannot save " + pathText(path.filename()) + " (Windows " + std::to_string(code) + ").");
    }
}
inline uint64_t checksum(std::string_view bytes) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char byte : bytes) { hash ^= byte; hash *= 1099511628211ULL; }
    return hash;
}
struct BinaryWriter {
    std::string bytes;
    void number(uint64_t value) {
        if (bytes.size() > MaxFile - 8) throw std::runtime_error("Catalog exceeds size limit.");
        for (int i = 0; i < 8; ++i) { bytes.push_back(static_cast<char>(value & 255)); value >>= 8; }
    }
    void text(const std::string& value) {
        if (value.size() > MaxString) throw std::runtime_error("Catalog string is too long.");
        number(value.size());
        if (value.size() > MaxFile - bytes.size()) throw std::runtime_error("Catalog exceeds size limit.");
        bytes += value;
    }
};
struct BinaryReader {
    std::string_view bytes;
    size_t offset = 0;
    uint64_t number() {
        if (bytes.size() - offset < 8) throw std::runtime_error("Truncated catalog.");
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset++])) << (i * 8);
        return value;
    }
    size_t count(size_t maximum = MaxRecords) {
        const auto value = number();
        if (value > maximum) throw std::runtime_error("Invalid catalog length.");
        return static_cast<size_t>(value);
    }
    std::string text() {
        const auto length = count(MaxString);
        if (length > bytes.size() - offset) throw std::runtime_error("Truncated catalog string.");
        auto value = std::string(bytes.substr(offset, length)); offset += length; return value;
    }
    void finish() const { if (offset != bytes.size()) throw std::runtime_error("Unexpected catalog data."); }
};
// One immutable snapshot at a time; callers coalesce dirty revisions while busy.
struct Writer {
    std::future<std::string> job;
    std::jthread worker;
    std::function<void()> notify;
    std::string error;
    bool backup = true;
    uint64_t revision = 0;
    ~Writer() { if (job.valid()) job.wait(); }
    bool busy() const { return job.valid(); }
    bool collect(bool wait = false) {
        if (!job.valid() || (!wait && job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return false;
        error = job.get();
        if (error.empty()) backup = true;
        return true;
    }
    template<class Generate> void startGenerated(const fs::path& path, Generate generate, uint64_t version) {
        revision = version;
        std::promise<std::string> promise; job = promise.get_future();
        worker = std::jthread([path, generate = std::move(generate), keep = backup, notify = notify, promise = std::move(promise)] () mutable {
            std::string error;
            try { write(path, generate(), keep); }
            catch (const std::exception& e) { error = e.what(); }
            promise.set_value(std::move(error));
            if (notify) notify();
        });
    }
};
}
