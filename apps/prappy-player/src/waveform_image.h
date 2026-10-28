#pragma once
#include "analysis.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// UI-owned raster cache: resample only when the track or horizontal resolution
// changes. Height and playback position only change rectangular copies and a marker.
class WaveformImage {
    SDL_Renderer* renderer;
    SDL_Texture* texture = nullptr;
    std::shared_ptr<const analysis::Result> source;
    int rasterWidth = 0;
    struct Copy { SDL_Renderer* renderer; SDL_Texture* texture; ImVec2 min,max; float split; };
    static void copy(const ImDrawList*,const ImDrawCmd* command) {
        const auto& data = *static_cast<const Copy*>(command->UserCallbackData);
        auto renderer = data.renderer;
        const auto frame = ImGui::GetDrawData();
        float sx = 1, sy = 1; SDL_GetRenderScale(renderer,&sx,&sy);
        sx = sx == 1 ? frame->FramebufferScale.x : 1; sy = sy == 1 ? frame->FramebufferScale.y : 1;
        const SDL_FRect target{(data.min.x-frame->DisplayPos.x)*sx,(data.min.y-frame->DisplayPos.y)*sy,
            (data.max.x-data.min.x)*sx,(data.max.y-data.min.y)*sy};
        const int left = static_cast<int>(std::max(0.0f,(command->ClipRect.x-frame->DisplayPos.x)*sx));
        const int top = static_cast<int>(std::max(0.0f,(command->ClipRect.y-frame->DisplayPos.y)*sy));
        const int right = static_cast<int>(std::min(frame->DisplaySize.x*sx,(command->ClipRect.z-frame->DisplayPos.x)*sx));
        const int bottom = static_cast<int>(std::min(frame->DisplaySize.y*sy,(command->ClipRect.w-frame->DisplayPos.y)*sy));
        const int split = static_cast<int>(std::floor(target.x+target.w*data.split));
        const bool clipped = SDL_RenderClipEnabled(renderer); SDL_Rect previous{}; SDL_GetRenderClipRect(renderer,&previous);
        Uint8 red = 255,green = 255,blue = 255; SDL_GetTextureColorMod(data.texture,&red,&green,&blue);
        const auto part = [&](int begin,int end,Uint8 r,Uint8 g,Uint8 b) {
            const SDL_Rect clip{begin,top,end-begin,bottom-top};
            if (clip.w <= 0 || clip.h <= 0) return;
            SDL_SetRenderClipRect(renderer,&clip); SDL_SetTextureColorMod(data.texture,r,g,b);
            SDL_RenderTexture(renderer,data.texture,nullptr,&target);
        };
        part(std::max(left,split),right,103,126,127);
        part(left,std::min(right,split),64,224,177);
        SDL_SetTextureColorMod(data.texture,red,green,blue);
        SDL_SetRenderClipRect(renderer,clipped ? &previous : nullptr);
    }
public:
    size_t rebuilds = 0;
    int vertices = 0;
    double markerRatio = 0;
    explicit WaveformImage(SDL_Renderer* value) : renderer(value) {}
    void resetTexture() { if (texture) SDL_DestroyTexture(texture); texture = nullptr; source.reset(); rasterWidth = 0; }
    ~WaveformImage() { if (texture) SDL_DestroyTexture(texture); }
    WaveformImage(const WaveformImage&) = delete;
    WaveformImage& operator=(const WaveformImage&) = delete;
    void prepare(const std::shared_ptr<const analysis::Result>& waveform, float width) {
        // draw() supplies the exact device-pixel span, including odd widths.
        const int pixelsWide = std::max(static_cast<int>(std::round(width)),1);
        const int count = (pixelsWide+1)/2;
        if (source == waveform && rasterWidth == pixelsWide) return;
        source = waveform; rasterWidth = pixelsWide;
        if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
        if (!source || source->levels.empty() || source->levels.front().empty()) return;
        size_t level = 0;
        while (level+1 < source->levels.size() && source->levels[level].size() > static_cast<size_t>(count*2)) ++level;
        const auto& peaks = source->levels[level];
        float amplitude = 0;
        for (const auto& peak : peaks) amplitude = std::max({amplitude,std::abs(peak.leftMin),std::abs(peak.leftMax),std::abs(peak.rightMin),std::abs(peak.rightMax)});
        const auto scale = amplitude > 1e-12f ? .9f/amplitude : 1;
        constexpr int Height = 128;
        std::vector<Uint8> pixels(static_cast<size_t>(pixelsWide)*Height*4,0);
        const double framesPerBin = static_cast<double>(analysis::FramesPerPeak*(1ULL << level));
        for (int x = 0; x < count; ++x) {
            const auto begin = static_cast<size_t>(static_cast<double>(x)/count*static_cast<double>(source->frames)/framesPerBin);
            const auto end = std::min(peaks.size(),static_cast<size_t>(std::ceil(static_cast<double>(x+1)/count*static_cast<double>(source->frames)/framesPerBin)));
            if (begin >= peaks.size()) continue;
            auto peak = peaks[begin];
            for (size_t i = begin+1; i < end; ++i) {
                peak.leftMin = std::min(peak.leftMin,peaks[i].leftMin); peak.leftMax = std::max(peak.leftMax,peaks[i].leftMax);
                peak.rightMin = std::min(peak.rightMin,peaks[i].rightMin); peak.rightMax = std::max(peak.rightMax,peaks[i].rightMax);
            }
            const auto lane = [&](float low,float high,float middle) {
                const auto top = std::clamp(static_cast<int>(Height*(middle-std::clamp(high*scale,-1.0f,1.0f)*.22f)),0,Height-1);
                const auto bottom = std::clamp(static_cast<int>(Height*(middle-std::clamp(low*scale,-1.0f,1.0f)*.22f)),top,Height-1);
                for (int y = top; y <= bottom; ++y) {
                    const auto offset = (static_cast<size_t>(y)*pixelsWide+static_cast<size_t>(x)*2)*4;
                    std::fill_n(pixels.data()+offset,4,Uint8{255});
                }
            };
            lane(peak.leftMin,peak.leftMax,.25f); lane(peak.rightMin,peak.rightMax,.75f);
        }
        texture = SDL_CreateTexture(renderer,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STATIC,pixelsWide,Height);
        if (!texture || !SDL_UpdateTexture(texture,nullptr,pixels.data(),pixelsWide*4) || !SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND) ||
            !SDL_SetTextureScaleMode(texture,SDL_SCALEMODE_NEAREST))
            throw std::runtime_error(SDL_GetError());
        ++rebuilds;
    }
    void draw(const std::shared_ptr<const analysis::Result>& waveform,ImVec2 min,ImVec2 max,double ratio) {
        // Layout widths can be fractional after font/UI scaling. Sampling an
        // integer raster into that span skips/repeats a column near its middle.
        // Snap both edges in device space and rasterize that same pixel span.
        float renderScaleX = 1, renderScaleY = 1;
        SDL_GetRenderScale(renderer,&renderScaleX,&renderScaleY);
        const float pixelScale = renderScaleX == 1 ? ImGui::GetIO().DisplayFramebufferScale.x : renderScaleX;
        const float origin = ImGui::GetMainViewport()->Pos.x;
        min.x = origin+std::round((min.x-origin)*pixelScale)/pixelScale;
        max.x = origin+std::round((max.x-origin)*pixelScale)/pixelScale;
        prepare(waveform,(max.x-min.x)*pixelScale);
        auto draw = ImGui::GetWindowDrawList();
        const auto before = draw->VtxBuffer.Size;
        const float split = static_cast<float>(std::clamp(ratio,0.0,1.0));
        markerRatio = split;
        const float x = min.x+(max.x-min.x)*split;
        if (texture) {
            // SDL's software textured triangles clip incorrectly at large sizes.
            // Native rectangular copies keep sampling fixed as the color split moves.
            Copy data{renderer,texture,min,max,split};
            draw->AddCallback(copy,&data,sizeof(data));
        } else draw->AddRectFilled({min.x,min.y+(max.y-min.y)*.5f},{max.x,min.y+(max.y-min.y)*.5f+1},IM_COL32(83,99,103,255));
        const auto marker = std::clamp(std::floor(x),min.x,std::max(min.x,max.x-2));
        // A pixel-aligned rectangle needs no anti-alias fringe or stroke tessellation.
        draw->PrimReserve(6,4);
        draw->PrimRect({marker,min.y},{marker+2,max.y},IM_COL32(245,189,99,255));
        vertices = draw->VtxBuffer.Size-before;
    }
};
