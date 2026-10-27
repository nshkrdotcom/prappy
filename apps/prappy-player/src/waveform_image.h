#pragma once
#include "analysis.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>

// UI-owned raster cache: resample only when the track or horizontal resolution
// changes. Height and playback position only change two image quads and a marker.
class WaveformImage {
    SDL_Renderer* renderer;
    SDL_Texture* texture = nullptr;
    std::shared_ptr<const analysis::Result> source;
    int columns = 0;
public:
    size_t rebuilds = 0;
    int vertices = 0;
    double markerRatio = 0;
    explicit WaveformImage(SDL_Renderer* value) : renderer(value) {}
    ~WaveformImage() { if (texture) SDL_DestroyTexture(texture); }
    WaveformImage(const WaveformImage&) = delete;
    WaveformImage& operator=(const WaveformImage&) = delete;
    void prepare(const std::shared_ptr<const analysis::Result>& waveform, float width) {
        const int count = std::clamp(static_cast<int>(std::ceil(width/2)),1,2048);
        if (source == waveform && columns == count) return;
        source = waveform; columns = count;
        if (texture) { SDL_DestroyTexture(texture); texture = nullptr; }
        if (!source || source->levels.empty() || source->levels.front().empty()) return;
        size_t level = 0;
        while (level+1 < source->levels.size() && source->levels[level].size() > static_cast<size_t>(count*2)) ++level;
        const auto& peaks = source->levels[level];
        float amplitude = 0;
        for (const auto& peak : peaks) amplitude = std::max({amplitude,std::abs(peak.leftMin),std::abs(peak.leftMax),std::abs(peak.rightMin),std::abs(peak.rightMax)});
        const auto scale = amplitude > 1e-12f ? .9f/amplitude : 1;
        constexpr int Height = 128;
        const int pixelsWide = count*2;
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
        if (!texture || !SDL_UpdateTexture(texture,nullptr,pixels.data(),pixelsWide*4) || !SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND))
            throw std::runtime_error(SDL_GetError());
        ++rebuilds;
    }
    void draw(const std::shared_ptr<const analysis::Result>& waveform,ImVec2 min,ImVec2 max,double ratio) {
        prepare(waveform,max.x-min.x);
        auto draw = ImGui::GetWindowDrawList();
        const auto before = draw->VtxBuffer.Size;
        const float split = static_cast<float>(std::clamp(ratio,0.0,1.0));
        markerRatio = split;
        const float x = min.x+(max.x-min.x)*split;
        if (texture) {
            const auto id = static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture));
            if (split > 0) draw->AddImage(id,min,{x,max.y},{0,0},{split,1},IM_COL32(64,224,177,255));
            if (split < 1) draw->AddImage(id,{x,min.y},max,{split,0},{1,1},IM_COL32(103,126,127,255));
        } else draw->AddRectFilled({min.x,min.y+(max.y-min.y)*.5f},{max.x,min.y+(max.y-min.y)*.5f+1},IM_COL32(83,99,103,255));
        const auto marker = std::clamp(std::floor(x),min.x,std::max(min.x,max.x-2));
        // A pixel-aligned rectangle needs no anti-alias fringe or stroke tessellation.
        draw->PrimReserve(6,4);
        draw->PrimRect({marker,min.y},{marker+2,max.y},IM_COL32(245,189,99,255));
        vertices = draw->VtxBuffer.Size-before;
    }
};
