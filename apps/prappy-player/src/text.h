#pragma once
#include "paths.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <memory>
#include <vector>

namespace text {
void loadFonts();
struct Inspection {
    float width = 0;
    std::vector<uint32_t> clusters;
    size_t highlightedClusters = 0;
    bool missingGlyph = false;
};
// DirectWrite shapes whole strings. Only visible CPU-rasterized tiles are cached.
class Painter {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    explicit Painter(SDL_Renderer* renderer);
    ~Painter();
    Painter(const Painter&) = delete;
    Painter& operator=(const Painter&) = delete;
    void beginFrame();
    float fontSizeForCapHeight(float pixels) const;
    float width(const std::string& label, float size);
    void draw(const std::string& label, const std::vector<std::wstring>& terms, ImVec2 origin, float size, ImU32 color);
    Inspection inspect(const std::string& label, const std::vector<std::wstring>& terms, float size);
};
// UI thread only, installed for the lifetime of its Painter.
inline Painter* active = nullptr;
}
