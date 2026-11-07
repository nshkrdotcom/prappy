#include "analysis.h"
#include "analysis_dsp.h"
#include "../tests/mpv_oracle.h"
#include <bit>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <numeric>

namespace analysis {
namespace {
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
void pollDecoder(MpvOracle& decoder, bool& eof, const Metadata& metadata, DecoderMetadata& observed) {
    for (;;) {
        const auto event = decoder.wait_event(decoder.handle, 0);
        if (event->event_id == MPV_EVENT_NONE) return;
        // Synchronous properties can wait for mpv's audio thread, which can
        // itself be blocked writing to the pipe this thread must drain.
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE && metadata) {
            const auto* property = static_cast<const mpv_event_property*>(event->data);
            if (event->reply_userdata == 4 && property->format == MPV_FORMAT_NODE)
                observed.tags = MpvOracle::tagsFromNode(*static_cast<const mpv_node*>(property->data));
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

Result measure(const fs::path& path, std::stop_token stop, const Progress& progress, const Metadata& metadata) {
    cancelled(stop);
    static std::atomic<uint64_t> sequence{0};
    const auto pipeName = L"\\\\.\\pipe\\PrappyAnalysis-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
    // Decoder dies after the pipe is closed, including exception/cancellation paths.
    MpvOracle decoder;
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
        {"gapless-audio", "no"}, {"replaygain", "no"}, {"keep-open", "no"}});
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

}
