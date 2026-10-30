#include "analysis_registry.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <limits>
#ifdef _WIN32
#include "state_io.h"
#else
#include <atomic>
#endif
namespace analysis_registry {
namespace {
constexpr size_t MaxBytes = 128 * 1024 * 1024;
constexpr uint64_t MaxEntries = 1000000;
constexpr uint64_t MaxString = 131072;
constexpr std::string_view Magic = "PRAPREG2";
struct Writer {
    std::string bytes;
    void number(uint64_t value) {
        if (bytes.size() > MaxBytes - 8) throw std::runtime_error("Registry exceeds limit");
        for (int i = 0; i != 8; ++i) { bytes.push_back(static_cast<char>(value)); value >>= 8; }
    }
    void text(std::string_view value) {
        if (value.size() > MaxString || bytes.size() > MaxBytes - value.size() - 8)
            throw std::runtime_error("Registry string exceeds limit");
        number(value.size()); bytes.append(value);
    }
};
struct Reader {
    std::string_view bytes;
    size_t position = 0;
    uint64_t number() {
        if (position > bytes.size() || bytes.size() - position < 8) throw std::runtime_error("Truncated registry");
        uint64_t value = 0;
        for (unsigned i = 0; i != 8; ++i) value |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[position++])) << (8 * i);
        return value;
    }
    std::string text() {
        const auto size = number();
        if (size > MaxString || position > bytes.size() || size > bytes.size() - position)
            throw std::runtime_error("Registry text exceeds limit");
        auto result = std::string(bytes.substr(position, static_cast<size_t>(size)));
        position += static_cast<size_t>(size); return result;
    }
    void end() const { if (position != bytes.size()) throw std::runtime_error("Trailing registry bytes"); }
};
void finite(double value, double low, double high) {
    if (!std::isfinite(value) || value < low || value > high) throw std::runtime_error("Invalid registry floating-point value");
}
void putOptional(Writer& out, std::optional<double> value) {
    out.number(value.has_value()); out.number(std::bit_cast<uint64_t>(value.value_or(0.0)));
}
std::optional<double> getOptional(Reader& in, double min, double max) {
    const auto has = in.number(); if (has > 1) throw std::runtime_error("Invalid presence flag");
    const double value = std::bit_cast<double>(in.number()); finite(value, min, max);
    return has ? std::optional<double>(value) : std::nullopt;
}
void save(Writer& out, const Entry& entry) {
    out.text(entry.path); out.number(entry.bytes); out.number(std::bit_cast<uint64_t>(entry.stamp));
    out.number(entry.algorithm); out.number(static_cast<uint64_t>(entry.state)); out.number(entry.frames);
    out.number(std::bit_cast<uint64_t>(entry.duration));
    putOptional(out, entry.loudness); putOptional(out, entry.samplePeak);
    out.text(entry.waveform);
    out.number(entry.tagsProbed); out.number(static_cast<uint64_t>(entry.tags.state));
    out.text(entry.tags.reason); out.number(entry.tags.selected.has_value());
    if (entry.tags.selected) {
        const auto& t = *entry.tags.selected;
        out.number(static_cast<uint64_t>(t.origin));
        out.number(std::bit_cast<uint64_t>(t.adjustmentDb));
        out.number(std::bit_cast<uint64_t>(t.referenceLufs));
        putOptional(out, t.samplePeak); out.text(t.reference);
    }
    out.number(entry.retryAfter); out.text(entry.error); out.number(entry.elapsedMs);
}
Entry load(Reader& in) {
    Entry entry;
    entry.path = in.text(); entry.bytes = in.number(); entry.stamp = std::bit_cast<int64_t>(in.number());
    entry.algorithm = in.number();
    const auto state = in.number(); if (state > static_cast<uint64_t>(State::Cancelled)) throw std::runtime_error("Invalid registry status");
    entry.state = static_cast<State>(state); entry.frames = in.number();
    entry.duration = std::bit_cast<double>(in.number()); finite(entry.duration, 0, 28800);
    entry.loudness = getOptional(in, -70, 50); entry.samplePeak = getOptional(in, 0, 64);
    entry.waveform = in.text();
    if (entry.waveform.find_first_of("/\\:\r\n") != std::string::npos ||
        entry.waveform.find("..") != std::string::npos) throw std::runtime_error("Invalid waveform artifact name");
    const auto probed = in.number(), tagState = in.number();
    if (probed > 1 || tagState > static_cast<uint64_t>(gain::TagState::Unsupported)) throw std::runtime_error("Invalid tag state");
    entry.tagsProbed = probed != 0; entry.tags.state = static_cast<gain::TagState>(tagState);
    entry.tags.reason = in.text();
    const auto tagPresent = in.number(); if (tagPresent > 1) throw std::runtime_error("Invalid tag presence");
    if (tagPresent) {
        gain::Tag tag;
        const auto origin = in.number();
        if (origin != static_cast<uint64_t>(gain::Origin::R128) && origin != static_cast<uint64_t>(gain::Origin::ReplayGain))
            throw std::runtime_error("Invalid persisted tag origin");
        tag.origin = static_cast<gain::Origin>(origin);
        tag.adjustmentDb = std::bit_cast<double>(in.number()); finite(tag.adjustmentDb, -96, 96);
        tag.referenceLufs = std::bit_cast<double>(in.number()); finite(tag.referenceLufs, -70, 50);
        tag.samplePeak = getOptional(in, 0, 64); tag.reference = in.text();
        entry.tags.selected = std::move(tag);
    }
    entry.retryAfter = in.number(); entry.error = in.text(); entry.elapsedMs = in.number();
    if (entry.algorithm != Algorithm) throw Unsupported("Future analysis algorithm; original preserved");
    if (entry.path.empty() || entry.path.size() > MaxString ||
        entry.path.find('\0') != std::string::npos ||
        entry.frames > 48000ULL * 60 * 60 * 8 || entry.elapsedMs > 24ULL * 60 * 60 * 1000)
        throw std::runtime_error("Incompatible/invalid registry entry");
    return entry;
}
}
uint64_t checksum(std::string_view data) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char ch : data) { hash ^= ch; hash *= 1099511628211ULL; }
    return hash;
}
std::string encode(const Entries& entries) {
    if (entries.size() > MaxEntries) throw std::runtime_error("Too many analysis records");
    Writer payload; payload.number(entries.size());
    for (const auto& [path, entry] : entries) {
        if (path != entry.path) throw std::runtime_error("Analysis record key mismatch");
        save(payload, entry);
    }
    Writer header; header.bytes = Magic; header.number(2); header.number(payload.bytes.size());
    header.number(checksum(payload.bytes)); header.bytes += payload.bytes;
    if (header.bytes.size() > MaxBytes) throw std::runtime_error("Analysis registry too large");
    return header.bytes;
}
Entries decode(std::string_view data) {
    if (data.size() < 32 || data.size() > MaxBytes || data.substr(0, Magic.size()) != Magic)
        throw Unsupported("Unsupported analysis registry header; original preserved");
    Reader header{data.substr(8, 24)};
    if (header.number() != 2) throw Unsupported("Newer analysis registry schema; original preserved");
    const auto length = header.number(), sum = header.number();
    if (length != data.size() - 32 || sum != checksum(data.substr(32))) throw std::runtime_error("Registry checksum/length mismatch");
    Reader input{data.substr(32)};
    const auto count = input.number(); if (count > MaxEntries) throw std::runtime_error("Too many registry records");
    Entries result;
    for (uint64_t i = 0; i < count; ++i) {
        auto entry = load(input);
        const auto key = entry.path;
        if (!result.emplace(key, std::move(entry)).second) throw std::runtime_error("Duplicate registry identity");
    }
    input.end(); return result;
}
std::string pathUtf8(const fs::path& path) {
    const auto bytes = path.u8string();
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
fs::path pathFromUtf8(const std::string& text) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
std::string blobName(const Entry& entry) {
    const auto hash = checksum(entry.path + ":" + std::to_string(entry.bytes) + ":" + std::to_string(entry.stamp));
    char output[32]{};
    static constexpr char hex[] = "0123456789abcdef";
    for (int i = 15; i >= 0; --i) output[15-i] = hex[(hash >> (i*4)) & 15];
    return std::string("wave-") + std::string(output, 16) + ".bin";
}
std::string readFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("Cannot open registry/blob");
    auto size = in.tellg();
    if (size < 0 || static_cast<uint64_t>(size) > MaxBytes) throw std::runtime_error("Registry/blob exceeds limit");
    std::string data(static_cast<size_t>(size), '\0'); in.seekg(0);
    if (!in.read(data.data(), static_cast<std::streamsize>(data.size()))) throw std::runtime_error("Incomplete registry/blob read");
    return data;
}
void atomicWrite(const fs::path& path, const std::string& bytes) {
    if (bytes.size() > MaxBytes) throw std::runtime_error("Registry/blob exceeds limit");
    fs::create_directories(path.parent_path());
#ifdef _WIN32
    stateio::write(path, bytes, true);
#else
    static std::atomic<uint64_t> sequence{0};
    const auto temp = fs::path(path.string() + ".tmp-" + std::to_string(++sequence));
    try {
        { std::ofstream out(temp, std::ios::binary | std::ios::trunc);
          if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !out.flush())
              throw std::runtime_error("Cannot write analysis temp file"); }
        fs::rename(temp, path);
    } catch (...) { std::error_code error; fs::remove(temp, error); throw; }
#endif
}
}
