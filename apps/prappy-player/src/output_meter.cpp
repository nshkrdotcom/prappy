#include "output_meter.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
struct OutputMeter::Impl {
    std::atomic<bool> enabled{false}, connected{false};
    std::atomic<float> peak{0}, latest{0};
    std::mutex mutex;
    std::condition_variable_any wake;
    std::jthread thread;
    Impl() : thread([this](std::stop_token stop) { run(stop); }) {}
    ~Impl() { thread.request_stop(); wake.notify_all(); thread.join(); }
    void run(std::stop_token stop) {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return;
        struct ComScope { ~ComScope() { CoUninitialize(); } } com;
        ComPtr<IMMDeviceEnumerator> devices;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&devices)))) return;
        std::vector<ComPtr<IAudioMeterInformation>> meters;
        auto refresh = std::chrono::steady_clock::time_point::min();
        while (!stop.stop_requested()) {
            if (!enabled.load()) {
                peak.store(0); latest.store(0); connected.store(false); meters.clear();
                refresh = std::chrono::steady_clock::time_point::min();
                std::unique_lock lock(mutex);
                wake.wait(lock,stop,[this] { return enabled.load(); });
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= refresh) {
                refresh = now + std::chrono::milliseconds(500);
                meters.clear();
                ComPtr<IMMDeviceCollection> endpoints;
                // Include active nondefault endpoints: Windows can route an app
                // separately, or move it when a headset is connected.
                if (SUCCEEDED(devices->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&endpoints))) {
                    UINT count = 0; endpoints->GetCount(&count);
                    for (UINT i = 0; i < count && !stop.stop_requested(); ++i) {
                        ComPtr<IMMDevice> endpoint;
                        ComPtr<IAudioSessionManager2> manager;
                        ComPtr<IAudioSessionEnumerator> sessions;
                        if (FAILED(endpoints->Item(i,&endpoint)) ||
                            FAILED(endpoint->Activate(__uuidof(IAudioSessionManager2),CLSCTX_INPROC_SERVER,nullptr,&manager)) ||
                            FAILED(manager->GetSessionEnumerator(&sessions))) continue;
                        int size = 0; sessions->GetCount(&size);
                        for (int j = 0; j < size; ++j) {
                            ComPtr<IAudioSessionControl> session;
                            ComPtr<IAudioSessionControl2> identity;
                            ComPtr<IAudioMeterInformation> meter;
                            DWORD pid = 0;
                            if (SUCCEEDED(sessions->GetSession(j,&session)) && SUCCEEDED(session.As(&identity)) &&
                                identity->GetProcessId(&pid) == S_OK && pid == GetCurrentProcessId() &&
                                SUCCEEDED(session.As(&meter))) meters.push_back(std::move(meter));
                        }
                    }
                }
            }
            float value = 0; bool valid = false;
            for (const auto& meter : meters) {
                float sample = 0;
                if (SUCCEEDED(meter->GetPeakValue(&sample))) { value = std::max(value,sample); valid = true; }
                else refresh = std::chrono::steady_clock::time_point::min();
            }
            connected.store(valid);
            latest.store(value);
            // Preserve short transients between UI frames, never a decoder
            // lookahead value. The UI consumes and clears this accumulator.
            auto previous = peak.load();
            while (previous < value && !peak.compare_exchange_weak(previous,value)) {}
            std::unique_lock lock(mutex);
            wake.wait_for(lock,stop,std::chrono::milliseconds(10),[this] { return !enabled.load(); });
        }
    }
};
OutputMeter::OutputMeter() : impl(std::make_unique<Impl>()) {}
OutputMeter::~OutputMeter() = default;
void OutputMeter::enable(bool value) {
    // Take the same mutex as the wait predicate to avoid a lost idle wakeup.
    { std::lock_guard lock(impl->mutex); impl->enabled.store(value); }
    impl->wake.notify_all();
}
double OutputMeter::read() { return std::max(impl->peak.exchange(0),impl->latest.load()); }
bool OutputMeter::available() const { return impl->connected.load(); }
