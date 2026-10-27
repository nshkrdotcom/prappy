#pragma once
#include <SDL3/SDL.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <chrono>
#include <stdexcept>
#include <string>

// Owns presentation and its latency policy. SDL/ImGui resources stay on the
// window thread; asynchronous analysis and audio never touch the renderer.
class Presentation {
    SDL_Renderer* renderer = nullptr;
    double lastInput = -1;
public:
    using Clock = std::chrono::steady_clock;
    struct Timing { double build = 0, draw = 0, present = 0, total = 0, input = -1; } timing;
    const char* name() const { return SDL_GetRendererName(renderer); }
    void configureLatency() {
        if (std::string(name()) != "direct3d11") return;
        const auto properties = SDL_GetRendererProperties(renderer);
        auto device = static_cast<ID3D11Device*>(SDL_GetPointerProperty(properties,SDL_PROP_RENDERER_D3D11_DEVICE_POINTER,nullptr));
        Microsoft::WRL::ComPtr<IDXGIDevice1> dxgi;
        if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgi))) || FAILED(dxgi->SetMaximumFrameLatency(1)))
            throw std::runtime_error("Cannot configure low-latency Direct3D presentation.");
        if (!SDL_SetRenderVSync(renderer,0)) throw std::runtime_error(SDL_GetError());
    }
    ~Presentation() { close(); }
    void close() { if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; } }
    Presentation() = default;
    Presentation(const Presentation&) = delete;
    Presentation& operator=(const Presentation&) = delete;
    SDL_Renderer* open(SDL_Window* window,bool benchmarkSoftware = false) {
        renderer = SDL_CreateRenderer(window,benchmarkSoftware ? SDL_SOFTWARE_RENDERER : "direct3d11");
        if (!renderer) throw std::runtime_error(SDL_GetError());
        if (!benchmarkSoftware) {
            // The event scheduler paces animation; don't block input behind a
            // refresh wait in Present or add a second independent frame limiter.
            configureLatency();
        }
        return renderer;
    }
    static double ms(Clock::time_point first,Clock::time_point last) { return std::chrono::duration<double,std::milli>(last-first).count(); }
    template<class Draw> void paint(Clock::time_point frameStart,Clock::time_point built,Uint64 input,Draw draw) {
        SDL_SetRenderDrawColor(renderer,46,49,51,255);
        if (!SDL_RenderClear(renderer)) throw std::runtime_error(SDL_GetError());
        draw();
        const auto drawn = Clock::now();
        if (!SDL_RenderPresent(renderer) && !SDL_HasEvent(SDL_EVENT_RENDER_DEVICE_RESET)) throw std::runtime_error(SDL_GetError());
        const auto end = Clock::now();
        if (input) lastInput = static_cast<double>(SDL_GetTicksNS()-input)/1e6;
        timing = {ms(frameStart,built),ms(built,drawn),ms(drawn,end),ms(frameStart,end),
            lastInput};
    }
};
