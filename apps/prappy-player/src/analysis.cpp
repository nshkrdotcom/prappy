#include "analysis.h"
#include "analysis_dsp.h"
#include "audio.h"
#include "library.h"
#include <bit>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <numeric>

namespace analysis {
namespace {
constexpr uint64_t Version = 1;
constexpr size_t MaxPeaks = static_cast<size_t>((MaxFrames + FramesPerPeak - 1) / FramesPerPeak);
// The parser validates the serialized reduction pyramid independently of
// the portable DSP that produced it.
Peak combine(const Peak& a, const Peak& b) {
    return {std::min(a.leftMin, b.leftMin), std::max(a.leftMax, b.leftMax),
        std::min(a.rightMin, b.rightMin), std::max(a.rightMax, b.rightMax)};
}
bool samePeak(const Peak& a, const Peak& b) {
    return a.leftMin == b.leftMin && a.leftMax == b.leftMax &&
           a.rightMin == b.rightMin && a.rightMax == b.rightMax;
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) { if (!h || h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create analysis pipe/event."); }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void cancelled(std::stop_token stop) { if (stop.stop_requested()) throw std::runtime_error("Analysis cancelled."); }
struct DecoderMetadata {
    std::vector<std::pair<std::string, std::string>> tags;
    std::string codec;
};
void pollDecoder(Audio& decoder, bool& eof, const Metadata& metadata, DecoderMetadata& observed) {
    for (;;) {
        const auto event = decoder.wait_event(decoder.handle, 0);
        if (event->event_id == MPV_EVENT_NONE) return;
        // Synchronous properties can wait for mpv's audio thread, which can
        // itself be blocked writing to the pipe this thread must drain.
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && metadata) {
            const auto* property = static_cast<const mpv_event_property*>(event->data);
            if (event->reply_userdata == 4 && property->format == MPV_FORMAT_NODE)
                observed.tags = Audio::tagsFromNode(*static_cast<const mpv_node*>(property->data));
            if (event->reply_userdata == 5 && property->format == MPV_FORMAT_STRING)
                observed.codec = *static_cast<const char* const*>(property->data);
            if (!observed.codec.empty()) metadata(observed.tags, observed.codec);
        }
        if (event->event_id == MPV_EVENT_END_FILE) {
            const auto end = static_cast<mpv_event_end_file*>(event->data);
            if (decoder.retryEmptyFlac(*end)) continue;
            if (end->reason == MPV_END_FILE_REASON_ERROR) throw std::runtime_error(std::string("Analysis decode: ") + decoder.error_string(end->error));
            if (end->reason == MPV_END_FILE_REASON_EOF) eof = true;
        }
    }
}
}

Identity identify(const fs::path& path) {
    const auto extended = extendedPath(path);
    return {path, fs::file_size(extended), fs::last_write_time(extended).time_since_epoch().count()};
}

Result measure(const fs::path& path, std::stop_token stop, const Progress& progress, const Metadata& metadata) {
    cancelled(stop);
    static std::atomic<uint64_t> sequence{0};
    const auto pipeName = L"\\\\.\\pipe\\PrappyAnalysis-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    // Decoder dies after the pipe is closed, including exception/cancellation paths.
    Audio decoder;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Handle pipe(CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr));
    OVERLAPPED io{}; io.hEvent = event.value;
    bool ioPending = false;
    // Windows borrows this storage until completion, including cancellation.
    // Declare it before PendingIo so cancellation and draining happen first
    // when any wait/decoder operation throws.
    std::vector<unsigned char> buffer(65536 + 8);
    struct PendingIo {
        HANDLE pipe;
        OVERLAPPED& io;
        bool& pending;
        ~PendingIo() {
            if (pending) {
                CancelIoEx(pipe, nullptr);
                DWORD ignored = 0; GetOverlappedResult(pipe, &io, &ignored, TRUE);
            }
        }
    } pendingIo{pipe.value, io, ioPending};
    bool eof = false;
    DecoderMetadata observed;
    const auto output = utf8(pipeName);
    decoder.volume = 100;
    decoder.open(false, {{"ao", "pcm"}, {"ao-pcm-file", output.c_str()}, {"ao-pcm-waveheader", "no"},
        {"audio-format", "float"}, {"audio-samplerate", "48000"}, {"audio-channels", "stereo"},
        {"gapless-audio", "no"}, {"replaygain", "no"}, {"keep-open", "no"}}, false);
    if (metadata) {
        decoder.check(decoder.observe_property(decoder.handle, 4, "metadata", MPV_FORMAT_NODE));
        decoder.check(decoder.observe_property(decoder.handle, 5, "current-tracks/audio/codec", MPV_FORMAT_STRING));
    }
    bool connected = ConnectNamedPipe(pipe.value, &io) != FALSE;
    if (!connected) {
        const auto error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) connected = true;
        else if (error == ERROR_IO_PENDING) ioPending = true;
        else throw std::runtime_error("Cannot connect analysis pipe.");
    }
    if (!decoder.play(path)) throw std::runtime_error(decoder.error);
    auto lastData = std::chrono::steady_clock::now();
    const auto await = [&] {
        for (;;) {
            cancelled(stop);
            const auto ready = WaitForSingleObject(event.value, 20);
            pollDecoder(decoder, eof, metadata, observed);
            if (ready == WAIT_OBJECT_0) return;
            if (ready == WAIT_FAILED) throw std::runtime_error("Analysis pipe wait failed.");
            if (std::chrono::steady_clock::now() - lastData > std::chrono::seconds(30))
                throw std::runtime_error("Analysis decoder stalled.");
        }
    };
    if (!connected) {
        await(); DWORD ignored = 0;
        if (!GetOverlappedResult(pipe.value, &io, &ignored, FALSE)) throw std::runtime_error("Analysis connection failed.");
        ioPending = false;
    }
    Accumulator accumulator;
    size_t carry = 0;
    for (;;) {
        cancelled(stop); ResetEvent(event.value); io = {}; io.hEvent = event.value;
        DWORD received = 0;
        const bool immediate = ReadFile(pipe.value, buffer.data() + carry, 65536, &received, &io) != FALSE;
        if (!immediate) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) break;
            if (error != ERROR_IO_PENDING) throw std::runtime_error("Analysis pipe read failed.");
            ioPending = true;
            await();
            const bool completed = GetOverlappedResult(pipe.value, &io, &received, FALSE) != FALSE;
            ioPending = false;
            if (!completed) {
                if (GetLastError() == ERROR_BROKEN_PIPE) break;
                throw std::runtime_error("Analysis pipe completion failed.");
            }
        }
        if (!received) break;
        lastData = std::chrono::steady_clock::now();
        const size_t available = carry + received, frames = available / 8;
        for (size_t i = 0; i < frames; ++i) {
            float l = 0, r = 0;
            std::memcpy(&l, buffer.data() + i * 8, 4); std::memcpy(&r, buffer.data() + i * 8 + 4, 4);
            accumulator.add(l, r);
        }
        carry = available % 8;
        std::memmove(buffer.data(), buffer.data() + frames * 8, carry);
        if (progress) progress(accumulator.frames());
        pollDecoder(decoder, eof, metadata, observed);
    }
    // Pipe closure alone is insufficient: only a clean decoder EOF commits data.
    while (!eof) {
        cancelled(stop); pollDecoder(decoder, eof, metadata, observed);
        if (!eof) {
            if (std::chrono::steady_clock::now() - lastData > std::chrono::seconds(30)) throw std::runtime_error("Analysis EOF timeout.");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (carry) throw std::runtime_error("Incomplete decoded frame.");
    return accumulator.finish();
}

std::string serialize(const Identity& identity, const Result& result) {
    stateio::BinaryWriter payload;
    payload.text(utf8(identity.path)); payload.number(identity.bytes); payload.number(std::bit_cast<uint64_t>(identity.stamp));
    payload.number(result.frames); payload.number(result.loudness.has_value());
    payload.number(std::bit_cast<uint64_t>(result.loudness.value_or(0)));
    payload.number(result.levels.size());
    for (const auto& level : result.levels) {
        payload.number(level.size());
        for (const auto& peak : level) for (float value : {peak.leftMin, peak.leftMax, peak.rightMin, peak.rightMax})
            payload.number(std::bit_cast<uint32_t>(value));
    }
    stateio::BinaryWriter header; header.bytes = "PRAPANA\n";
    header.number(Version); header.number(payload.bytes.size()); header.number(stateio::checksum(payload.bytes));
    header.bytes += payload.bytes;
    if (header.bytes.size() > stateio::MaxFile) throw std::runtime_error("Analysis cache too large.");
    return header.bytes;
}
Result parse(const std::string& bytes, const Identity& expected) {
    if (bytes.size() < 32 || bytes.size() > stateio::MaxFile || bytes.substr(0, 8) != "PRAPANA\n") throw std::runtime_error("Invalid analysis cache header.");
    stateio::BinaryReader header{std::string_view(bytes).substr(8, 24)};
    if (header.number() != Version || header.number() != bytes.size() - 32 || header.number() != stateio::checksum(std::string_view(bytes).substr(32)))
        throw std::runtime_error("Incompatible/damaged analysis cache.");
    stateio::BinaryReader input{std::string_view(bytes).substr(32)};
    Identity identity; identity.path = stateio::pathFromText(input.text()); identity.bytes = input.number(); identity.stamp = std::bit_cast<int64_t>(input.number());
    if (identity != expected) throw std::runtime_error("Stale analysis cache.");
    Result result; result.frames = input.number();
    if (!result.frames || result.frames > MaxFrames) throw std::runtime_error("Invalid analysis duration.");
    const bool hasLoudness = input.count(1) != 0;
    const double loudness = std::bit_cast<double>(input.number());
    if (!std::isfinite(loudness) || loudness < -70 || loudness > 50) throw std::runtime_error("Invalid analysis loudness.");
    if (hasLoudness) result.loudness = loudness;
    const auto levels = input.count(32);
    size_t count = static_cast<size_t>((result.frames + FramesPerPeak - 1) / FramesPerPeak);
    if (!levels || count > MaxPeaks) throw std::runtime_error("Invalid analysis pyramid.");
    for (size_t i = 0; i < levels; ++i) {
        if (input.count(MaxPeaks) != count || (i && result.levels.back().size() == 1)) throw std::runtime_error("Invalid analysis peak count.");
        std::vector<Peak> level; level.reserve(count);
        for (size_t j = 0; j < count; ++j) {
            const auto sample = [&] {
                const auto bits = input.number();
                if (bits > UINT32_MAX) throw std::runtime_error("Invalid analysis sample bits.");
                const auto value = std::bit_cast<float>(static_cast<uint32_t>(bits));
                if (!std::isfinite(value) || std::abs(value) > 64) throw std::runtime_error("Invalid analysis peak.");
                return value;
            };
            Peak peak; peak.leftMin = sample(); peak.leftMax = sample(); peak.rightMin = sample(); peak.rightMax = sample();
            if (peak.leftMin > peak.leftMax || peak.rightMin > peak.rightMax) throw std::runtime_error("Inverted analysis extrema.");
            if (i) {
                const auto& previous = result.levels.back(); const auto k = j * 2;
                if (!samePeak(peak, k + 1 < previous.size() ? combine(previous[k], previous[k + 1]) : previous[k])) throw std::runtime_error("Invalid analysis reduction.");
            }
            level.push_back(peak);
        }
        result.levels.push_back(std::move(level)); count = (count + 1) / 2;
    }
    if (result.levels.back().size() != 1) throw std::runtime_error("Incomplete analysis pyramid.");
    input.finish(); return result;
}
}
