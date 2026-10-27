#pragma once
#include "waveform_image.h"
#include "imgui_impl_sdlrenderer3.h"
#include <ostream>

inline void waveformRenderTests(std::ostream& log) {
    const auto require = [](bool value,const char* message) { if (!value) throw std::runtime_error(message); };
    struct Canvas {
        SDL_Surface* surface = nullptr;
        SDL_Renderer* renderer = nullptr;
        ImGuiContext* context = nullptr;
        bool backend = false;
        ~Canvas() {
            if (backend) ImGui_ImplSDLRenderer3_Shutdown();
            if (context) ImGui::DestroyContext(context);
            if (renderer) SDL_DestroyRenderer(renderer);
            if (surface) SDL_DestroySurface(surface);
        }
    };
    for (const auto& [size,dpi] : {std::pair{ImVec2{500,26},1.0f},std::pair{ImVec2{1920,720},1.0f},
        std::pair{ImVec2{3840,1600},1.0f},std::pair{ImVec2{1920,720},2.0f}}) {
        Canvas canvas;
        const int width = static_cast<int>(size.x), height = static_cast<int>(size.y);
        canvas.surface = SDL_CreateSurface(width,height,SDL_PIXELFORMAT_RGBA32);
        require(canvas.surface != nullptr,"Cannot create waveform pixel test surface.");
        canvas.renderer = SDL_CreateSoftwareRenderer(canvas.surface);
        require(canvas.renderer != nullptr,"Cannot create waveform software test renderer.");
        require(SDL_SetRenderScale(canvas.renderer,dpi,dpi),"Cannot set waveform test display scale.");
        canvas.context = ImGui::CreateContext();
        auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.LogFilename = nullptr;
        const ImVec2 logical{size.x/dpi,size.y/dpi};
        io.DisplaySize = logical; io.DisplayFramebufferScale = {dpi,dpi}; io.DeltaTime = 1.0f/60;
        canvas.backend = ImGui_ImplSDLRenderer3_Init(canvas.renderer);
        require(canvas.backend,"Cannot initialize waveform test backend.");
        auto waveform = std::make_shared<analysis::Result>();
        waveform->frames = 8192*analysis::FramesPerPeak;
        waveform->levels.resize(1); waveform->levels[0].resize(8192);
        for (size_t i = 0; i < waveform->levels[0].size(); ++i) {
            const float amplitude = .3f+.6f*static_cast<float>(i%137)/136;
            waveform->levels[0][i] = {-amplitude,amplitude,-amplitude,amplitude};
        }
        WaveformImage image(canvas.renderer);
        std::vector<float> reference;
        constexpr double First = .2137;
        for (const auto ratio : {First,.4173,.8031}) {
            ImGui_ImplSDLRenderer3_NewFrame(); ImGui::NewFrame();
            ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(logical);
            ImGui::Begin("waveform pixels",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoBackground|ImGuiWindowFlags_NoInputs);
            auto draw = ImGui::GetWindowDrawList(); draw->PushClipRect({0,0},logical,false);
            image.draw(waveform,{0,0},logical,ratio); draw->PopClipRect();
            ImGui::End(); ImGui::Render();
            SDL_SetRenderDrawColor(canvas.renderer,0,0,0,255); SDL_RenderClear(canvas.renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(),canvas.renderer); SDL_RenderPresent(canvas.renderer);
            std::vector<float> mask(static_cast<size_t>(width)*height);
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
                const auto pixel = static_cast<Uint8*>(canvas.surface->pixels)+static_cast<size_t>(y)*canvas.surface->pitch+x*4;
                const auto offset = static_cast<size_t>(y)*width+x;
                const auto tint = x < static_cast<int>(width*ratio) ? 224.0f : 126.0f;
                mask[offset] = pixel[1]/tint;
                if (!reference.empty() && std::abs(x-static_cast<int>(width*First)) > 6 && std::abs(x-static_cast<int>(width*ratio)) > 6) {
                    if (std::abs(mask[offset]-reference[offset]) >= .025f) log << "Pixel mismatch " << width << 'x' << height << " at " << x << ',' << y << ": " << reference[offset] << " vs " << mask[offset] << '\n';
                    require(std::abs(mask[offset]-reference[offset]) < .025f,"Waveform silhouette changed as playback marker moved.");
                }
            }
            // Both stereo lanes must extend across the right-hand side at large sizes.
            for (const auto lane : {.25,.75}) {
                size_t lit = 0;
                for (int x = width*3/4; x < width-4; ++x) lit += mask[static_cast<size_t>(height*lane)*width+x] > .1f ? 1 : 0;
                require(lit > static_cast<size_t>(width/16),"Waveform was diagonally clipped at the right edge.");
            }
            require(image.rebuilds == 1 && image.vertices <= 12,"Moving waveform marker rebuilt its raster or geometry.");
            if (reference.empty()) reference = std::move(mask);
        }
        log << "PASS waveform software pixels " << width << 'x' << height << " scale " << dpi << ": stable silhouette, full stereo width, fixed raster and <=12 vertices\n";
    }
}
