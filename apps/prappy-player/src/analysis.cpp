#include "analysis.h"
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
struct Biquad {
    double b0, b1, b2, a1, a2;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double process(double x) {
        const auto y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = x; y2 = y1; y1 = y; return y;
    }
};
struct Weighting {
    // ITU-R BS.1770-5 Annex 1, 48 kHz K-weighting coefficients.
    Biquad shelf{1.53512485958697, -2.69169618940638, 1.19839281085285, -1.69065929318241, .73248077421585};
    Biquad highpass{1, -2, 1, -1.99004745483398, .99007225036621};
    double process(double x) { return highpass.process(shelf.process(x)); }
};
Peak combine(const Peak& a, const Peak& b) {
    return {std::min(a.leftMin, b.leftMin), std::max(a.leftMax, b.leftMax),
        std::min(a.rightMin, b.rightMin), std::max(a.rightMax, b.rightMax)};
}
bool samePeak(const Peak& a, const Peak& b) {
    return a.leftMin == b.leftMin && a.leftMax == b.leftMax && a.rightMin == b.rightMin && a.rightMax == b.rightMax;
}
struct Accumulator {
    Result result;
    Weighting left, right;
    Peak peak;
    std::array<double, 4> energies{};
    double energy = 0;
    uint64_t hops = 0;
    std::vector<double> blocks;
    Accumulator() { result.levels.emplace_back(); }
    void add(float l, float r) {
        if (result.frames >= MaxFrames) throw std::runtime_error("Analysis exceeds the eight-hour limit.");
        if (!std::isfinite(l) || !std::isfinite(r) || std::abs(l) > 64 || std::abs(r) > 64)
            throw std::runtime_error("Invalid decoded sample.");
        if (result.frames % FramesPerPeak == 0) peak = {l, l, r, r};
        else peak = combine(peak, {l, l, r, r});
        const auto kl = left.process(l), kr = right.process(r);
        energy += kl * kl + kr * kr;
        ++result.frames;
        if (result.frames % FramesPerPeak == 0) result.levels[0].push_back(peak);
        // 400 ms blocks at 100 ms intervals, with complete blocks only.
        if (result.frames % 4800 == 0) {
            energies[hops % 4] = energy / 4800; energy = 0; ++hops;
            if (hops >= 4) blocks.push_back(std::accumulate(energies.begin(), energies.end(), 0.0) / 4);
        }
    }
    Result finish() {
        if (!result.frames) throw std::runtime_error("No audio frames decoded.");
        if (result.frames % FramesPerPeak) result.levels[0].push_back(peak);
        while (result.levels.back().size() > 1) {
            const auto& previous = result.levels.back(); std::vector<Peak> next;
            next.reserve((previous.size() + 1) / 2);
            for (size_t i = 0; i < previous.size(); i += 2)
                next.push_back(i + 1 < previous.size() ? combine(previous[i], previous[i + 1]) : previous[i]);
            result.levels.push_back(std::move(next));
        }
        // Absolute -70 LUFS gate, then -10 LU relative to its gated mean.
        const double absolute = std::pow(10.0, (-70 + .691) / 10);
        double sum = 0; size_t count = 0;
        for (auto block : blocks) if (block > absolute) { sum += block; ++count; }
        if (count) {
            const double threshold = std::max(absolute, sum / static_cast<double>(count) / 10);
            sum = 0; count = 0;
            for (auto block : blocks) if (block > threshold) { sum += block; ++count; }
            if (count) result.loudness = -.691 + 10 * std::log10(sum / static_cast<double>(count));
        }
        return std::move(result);
    }
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h) : value(h) { if (!h || h == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create analysis pipe/event."); }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
void cancelled(std::stop_token stop) { if (stop.stop_requested()) throw std::runtime_error("Analysis cancelled."); }
void pollDecoder(Audio& decoder, bool& eof) {
    for (;;) {
        const auto event = decoder.wait_event(decoder.handle, 0);
        if (event->event_id == MPV_EVENT_NONE) return;
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

Result measureSamples(const std::vector<float>& samples) {
    if (samples.size() % 2) throw std::runtime_error("Incomplete stereo frame.");
    Accumulator accumulator;
    for (size_t i = 0; i < samples.size(); i += 2) accumulator.add(samples[i], samples[i + 1]);
    return accumulator.finish();
}

Result measure(const fs::path& path, std::stop_token stop, const Progress& progress) {
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
    const auto output = utf8(pipeName);
    decoder.volume = 100;
    decoder.open(false, {{"ao", "pcm"}, {"ao-pcm-file", output.c_str()}, {"ao-pcm-waveheader", "no"},
        {"audio-format", "float"}, {"audio-samplerate", "48000"}, {"audio-channels", "stereo"},
        {"gapless-audio", "no"}, {"replaygain", "no"}, {"keep-open", "no"}}, false);
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
            pollDecoder(decoder, eof);
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
        if (progress) progress(accumulator.result.frames);
        pollDecoder(decoder, eof);
    }
    // Pipe closure alone is insufficient: only a clean decoder EOF commits data.
    while (!eof) {
        cancelled(stop); pollDecoder(decoder, eof);
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
fs::path cachePath(const fs::path& directory, const Identity& identity) {
    std::ostringstream name; name << std::hex << stateio::checksum(utf8(identity.path)) << ".bin";
    return directory / name.str();
}
Worker::Worker(fs::path cacheDirectory, std::function<void()> completion) : directory(std::move(cacheDirectory)), notify(std::move(completion)), thread([this](std::stop_token stop) { run(stop); }) {}
Worker::~Worker() { thread.request_stop(); wake.notify_all(); }
void Worker::reconcile(const Library& library, bool force) {
    std::lock_guard lock(mutex); pending.clear(); completed.clear();
    forcePending = force;
    selectedReady = false;
    bool keepActive = false;
    for (const auto& track : library.tracks) if (!track.offline) {
        Identity identity{track.path, track.bytes, track.stamp};
        if (!force && active == identity) keepActive = true;
        else pending.push_back(std::move(identity));
    }
    if (!keepActive) activeStop.request_stop();
    wake.notify_all();
}
void Worker::prioritize(const Track& track) {
    if (track.offline) return;
    std::lock_guard lock(mutex);
    const Identity identity{track.path, track.bytes, track.stamp};
    const bool ready = selected == identity && selectedReady;
    selected = identity;
    if (!ready) selectedReady = false;
    const auto found = std::find(pending.begin(), pending.end(), identity);
    if (found != pending.end()) { pending.erase(found); pending.push_front(identity); wake.notify_all(); }
    else if (!ready && active != identity) { pending.push_front(identity); wake.notify_all(); }
}
std::vector<Summary> Worker::collect() {
    std::lock_guard lock(mutex); auto result = std::move(completed); completed.clear(); return result;
}
void Worker::run(std::stop_token stop) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!stop.stop_requested()) {
        Identity identity;
        std::stop_source taskStop;
        bool force = false;
        {
            std::unique_lock lock(mutex);
            if (!wake.wait(lock, stop, [&] { return !pending.empty(); })) break;
            identity = std::move(pending.front()); pending.pop_front(); active = identity;
            activeStop = taskStop;
            force = forcePending;
        }
        std::stop_callback stopTask(stop, [&] { taskStop.request_stop(); });
        const auto taskToken = taskStop.get_token();
        Summary summary; summary.identity = identity;
        std::shared_ptr<const Result> waveform;
        try {
            cancelled(taskToken);
            if (identify(identity.path) != identity) throw std::runtime_error("Source changed before analysis.");
            Result result;
            const auto file = cachePath(directory, identity);
            try {
                if (force) throw std::runtime_error("Forced analysis refresh.");
                result = parse(stateio::read(file), identity); summary.cached = true;
            }
            catch (const std::exception&) { result = measure(identity.path, taskToken); }
            cancelled(taskToken);
            if (identify(identity.path) != identity) throw std::runtime_error("Source changed during analysis.");
            summary.duration = result.duration(); summary.loudness = result.loudness;
            const auto& peak = result.levels.back()[0];
            summary.samplePeak = std::max({std::abs(peak.leftMin), std::abs(peak.leftMax), std::abs(peak.rightMin), std::abs(peak.rightMax)});
            if (!summary.cached) { fs::create_directories(directory); stateio::write(file, serialize(identity, result), false); }
            waveform = std::make_shared<Result>(std::move(result));
        } catch (const std::exception& e) { summary.error = e.what(); }
        if (stop.stop_requested()) break;
        {
            std::lock_guard lock(mutex);
            if (!taskToken.stop_requested()) {
                if (selected == identity) { summary.waveform = std::move(waveform); selectedReady = true; }
                completed.push_back(std::move(summary));
            }
            active.reset();
        }
        if (notify) notify();
    }
}
}
