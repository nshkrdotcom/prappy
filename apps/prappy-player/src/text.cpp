#include "text.h"
#include "imgui_freetype.h"
#include <dwrite_3.h>
#include <wrl/client.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <list>

namespace text {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT result) { if (FAILED(result)) throw std::runtime_error("DirectWrite text operation failed (" + std::to_string(static_cast<uint32_t>(result)) + ")."); }
std::wstring wide(const std::string& value) {
    if (value.empty()) return {};
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!length) throw std::runtime_error("Invalid UTF-8 text.");
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), length); return result;
}
struct Ink final : IUnknown {
    std::atomic<ULONG> references{1}; COLORREF color;
    explicit Ink(COLORREF value) : color(value) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override { if (!out) return E_POINTER; *out = nullptr; if (id == __uuidof(IUnknown)) { *out = this; AddRef(); return S_OK; } return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references; if (!remaining) delete this; return remaining; }
};
struct Renderer final : IDWriteTextRenderer {
    std::atomic<ULONG> references{1};
    ComPtr<IDWriteBitmapRenderTarget1> target;
    ComPtr<IDWriteFactory2> factory;
    ComPtr<IDWriteRenderingParams> parameters;
    COLORREF color;
    bool missingGlyph = false;
    Renderer(IDWriteBitmapRenderTarget1* surface, IDWriteRenderingParams* params, COLORREF ink, IDWriteFactory2* source = nullptr) : target(surface), factory(source), parameters(params), color(ink) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) { *out = this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { const auto remaining = --references; if (!remaining) delete this; return remaining; }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override { *disabled = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* matrix) override { *matrix = {1, 0, 0, 1, 0, 0}; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* value) override { *value = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
        const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown* effect) override {
        for (UINT32 i = 0; i < run->glyphCount; ++i) missingGlyph = missingGlyph || run->glyphIndices[i] == 0;
        if (!target) return S_OK;
        const auto ink = effect ? static_cast<Ink*>(effect)->color : color;
        ComPtr<IDWriteColorGlyphRunEnumerator> layers;
        const auto translated = factory->TranslateColorGlyphRun(x, y, run, nullptr, mode, nullptr, 0, &layers);
        if (translated == DWRITE_E_NOCOLOR) return target->DrawGlyphRun(x, y, mode, run, parameters.Get(), ink);
        if (FAILED(translated)) return translated;
        BOOL more = FALSE;
        while (SUCCEEDED(layers->MoveNext(&more)) && more) {
            const DWRITE_COLOR_GLYPH_RUN* layer = nullptr;
            auto result = layers->GetCurrentRun(&layer); if (FAILED(result)) return result;
            const auto c = layer->runColor;
            const auto layerInk = layer->paletteIndex == 0xffff ? ink : RGB(static_cast<BYTE>(c.r * 255), static_cast<BYTE>(c.g * 255), static_cast<BYTE>(c.b * 255));
            result = target->DrawGlyphRun(layer->baselineOriginX, layer->baselineOriginY, mode, &layer->glyphRun, parameters.Get(), layerInk);
            if (FAILED(result)) return result;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object, BOOL sideways, BOOL rtl, IUnknown* effect) override {
        return object->Draw(context, this, x, y, sideways, rtl, effect);
    }
};
constexpr int TileWidth = 1024, Padding = 2;
constexpr size_t MaxBytes = 64 * 1024 * 1024, MaxEntries = 1024;
COLORREF rgb(ImU32 color) { return RGB((color >> IM_COL32_R_SHIFT) & 255, (color >> IM_COL32_G_SHIFT) & 255, (color >> IM_COL32_B_SHIFT) & 255); }
}

void loadFonts() {
    static_assert(sizeof(ImWchar) == 4);
    std::wstring windows(32768, L'\0');
    const auto length = GetWindowsDirectoryW(windows.data(), static_cast<UINT>(windows.size()));
    if (!length || length >= windows.size()) throw std::runtime_error("Cannot locate Windows font directory.");
    windows.resize(length);
    const auto directory = fs::path(windows) / "Fonts";
    auto atlas = ImGui::GetIO().Fonts;
    const auto add = [&](const fs::path& file, bool merge, unsigned int flags) {
        if (!fs::is_regular_file(file)) return false;
        ImFontConfig config; config.MergeMode = merge; config.FontLoaderFlags = flags;
        if (!atlas->AddFontFromFileTTF(utf8(file).c_str(), 18, &config)) throw std::runtime_error("Cannot load font " + utf8(file.filename()));
        return true;
    };
    if (!add(directory / "segoeui.ttf", false, 0)) throw std::runtime_error("Segoe UI is required.");
    for (const auto name : {L"msyh.ttc", L"YuGothR.ttc", L"malgun.ttf", L"simsunb.ttf", L"SimsunExtG.ttf"}) add(directory / name, true, 0);
    if (!add(executableDir() / "fonts/NotoSansCJKsc-Regular.otf", true, 0)) throw std::runtime_error("Missing bundled Noto CJK fallback font.");
    add(directory / "seguiemj.ttf", true, ImGuiFreeTypeLoaderFlags_LoadColor);
    add(directory / "seguisym.ttf", true, 0);
}

struct Painter::Impl {
    SDL_Renderer* renderer;
    ComPtr<IDWriteFactory5> factory;
    ComPtr<IDWriteFontCollection1> fonts;
    ComPtr<IDWriteFontFallback> fallback;
    ComPtr<IDWriteRenderingParams> parameters;
    ComPtr<IDWriteGdiInterop> interop;
    std::wstring locale;
    float capitalRatio = 1;
    struct Tile { int index = 0; SDL_Texture* texture = nullptr; size_t bytes = 0; ~Tile() { if (texture) SDL_DestroyTexture(texture); } };
    struct Layout {
        std::string label; float size;
        float maximumWidth = 0;
        ComPtr<IDWriteTextLayout> layout;
        float width = 0;
        std::vector<DWRITE_CLUSTER_METRICS> clusters;
    };
    struct Entry {
        std::shared_ptr<Layout> shape;
        std::vector<std::wstring> terms; ImU32 color;
        std::vector<DWRITE_TEXT_RANGE> highlights;
        std::list<std::unique_ptr<Tile>> tiles;
    };
    std::list<std::shared_ptr<Layout>> layouts;
    std::list<Entry> cache;
    // Draw lists retain texture pointers until the frame has been presented.
    std::list<std::unique_ptr<Tile>> retired;
    size_t bytes = 0;
    uint64_t shaped = 0, rasterized = 0;
    explicit Impl(SDL_Renderer* output) : renderer(output) {
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(factory.GetAddressOf())));
        check(factory->CreateRenderingParams(&parameters)); check(factory->GetGdiInterop(&interop));
        wchar_t name[LOCALE_NAME_MAX_LENGTH]{}; GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH); locale = name;
        ComPtr<IDWriteFontSet> system, combined; ComPtr<IDWriteFontSetBuilder1> builder; ComPtr<IDWriteFontFile> bundled;
        check(factory->GetSystemFontSet(&system)); check(factory->CreateFontSetBuilder(&builder)); check(builder->AddFontSet(system.Get()));
        check(factory->CreateFontFileReference((executableDir() / "fonts/NotoSansCJKsc-Regular.otf").c_str(), nullptr, &bundled));
        check(builder->AddFontFile(bundled.Get())); check(builder->CreateFontSet(&combined)); check(factory->CreateFontCollectionFromFontSet(combined.Get(), &fonts));
        UINT32 familyIndex = 0; BOOL exists = FALSE;
        check(fonts->FindFamilyName(L"Segoe UI", &familyIndex, &exists));
        if (!exists) throw std::runtime_error("Segoe UI is required for text layout.");
        ComPtr<IDWriteFontFamily> family; ComPtr<IDWriteFont> font;
        check(fonts->GetFontFamily(familyIndex, &family));
        check(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font));
        DWRITE_FONT_METRICS metrics{}; font->GetMetrics(&metrics);
        capitalRatio = static_cast<float>(metrics.capHeight) / metrics.designUnitsPerEm;
        ComPtr<IDWriteFontFallback> native; ComPtr<IDWriteFontFallbackBuilder> fallbacks;
        check(factory->GetSystemFontFallback(&native)); check(factory->CreateFontFallbackBuilder(&fallbacks));
        // Prefer the user's installed locale-specific CJK family, then the bundled font.
        const wchar_t* preferred = locale.starts_with(L"ja") ? L"Yu Gothic" : locale.starts_with(L"ko") ? L"Malgun Gothic" : L"Microsoft YaHei";
        const wchar_t* names[] = {preferred, L"Noto Sans CJK SC"};
        const DWRITE_UNICODE_RANGE ranges[] = {{0x2e80, 0x9fff}, {0xac00, 0xd7af}, {0xf900, 0xfaff}, {0xff00, 0xffef}, {0x20000, 0x323af}};
        check(fallbacks->AddMapping(ranges, static_cast<UINT32>(std::size(ranges)), names, 2, fonts.Get()));
        check(fallbacks->AddMappings(native.Get())); check(fallbacks->CreateFontFallback(&fallback));
    }
    std::shared_ptr<Layout> layout(const std::string& label,float size,float maximumWidth = 0) {
        const auto found = std::find_if(layouts.begin(),layouts.end(),[&](const auto& item) { return item->label == label && item->size == size && item->maximumWidth == maximumWidth; });
        if (found != layouts.end()) { layouts.splice(layouts.begin(),layouts,found); return layouts.front(); }
        auto stored = std::make_shared<Layout>(); auto& item = *stored;
        item.label = label; item.size = size; item.maximumWidth = maximumWidth; ++shaped;
        const auto value = wide(label);
        ComPtr<IDWriteTextFormat> format;
        check(factory->CreateTextFormat(L"Segoe UI", fonts.Get(), DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, locale.c_str(), &format));
        check(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)); check(format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, size, size * .8f));
        check(factory->CreateTextLayout(value.data(), static_cast<UINT32>(value.size()), format.Get(), maximumWidth > 0 ? maximumWidth : 1000000, size * 2, &item.layout));
        if (maximumWidth > 0) {
            ComPtr<IDWriteInlineObject> ellipsis; check(factory->CreateEllipsisTrimmingSign(format.Get(),&ellipsis));
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
            check(item.layout->SetTrimming(&trimming,ellipsis.Get()));
        }
        ComPtr<IDWriteTextLayout2> layout2; check(item.layout.As(&layout2)); check(layout2->SetFontFallback(fallback.Get()));
        DWRITE_TEXT_METRICS metrics{}; check(item.layout->GetMetrics(&metrics)); item.width = metrics.widthIncludingTrailingWhitespace;
        if (maximumWidth > 0) item.width = std::min(item.width,maximumWidth);
        UINT32 count = 0; item.layout->GetClusterMetrics(nullptr, 0, &count); item.clusters.resize(count);
        if (count) check(item.layout->GetClusterMetrics(item.clusters.data(), count, &count));
        layouts.push_front(stored);
        while (layouts.size() > MaxEntries) layouts.pop_back();
        return stored;
    }
    Entry& entry(const std::string& label, const std::vector<std::wstring>& terms, float size, ImU32 color, float maximumWidth = 0) {
        auto shape = layout(label,size,maximumWidth);
        const auto found = std::find_if(cache.begin(), cache.end(), [&](const auto& item) { return item.shape == shape && item.terms == terms && item.color == color; });
        if (found != cache.end()) { cache.splice(cache.begin(), cache, found); return cache.front(); }
        cache.emplace_front(); auto& item = cache.front(); item.shape = std::move(shape); item.terms = terms; item.color = color;
        const auto normalized = searchText(label); std::vector<bool> marked(normalized.size(), false);
        for (const auto& term : terms) if (!term.empty())
            for (size_t offset = 0; (offset = normalized.find(term, offset)) != std::wstring::npos; offset += term.size())
                for (size_t i = offset; i < offset + term.size(); ++i) marked[i] = true;
        UINT32 start = 0;
        for (const auto& cluster : item.shape->clusters) {
            const auto end = std::min<size_t>(start + cluster.length, marked.size());
            bool hit = false; for (size_t i = start; i < end; ++i) hit = hit || marked[i];
            if (hit) item.highlights.push_back({start,cluster.length});
            start += cluster.length;
        }
        trim(); return item;
    }
    void trim() {
        while (cache.size() > MaxEntries || bytes > MaxBytes) {
            if (cache.size() <= 1) break;
            for (const auto& tile : cache.back().tiles) bytes -= tile->bytes;
            retired.splice(retired.end(), cache.back().tiles);
            cache.pop_back();
        }
    }
    std::vector<uint32_t> pixels(Entry& item, int index, bool white) {
        const int height = static_cast<int>(std::ceil(item.shape->size * 1.5f)) + Padding * 2;
        ComPtr<IDWriteBitmapRenderTarget> base; check(interop->CreateBitmapRenderTarget(nullptr, TileWidth, height, &base));
        ComPtr<IDWriteBitmapRenderTarget1> surface; check(base.As(&surface));
        check(surface->SetPixelsPerDip(1)); check(surface->SetTextAntialiasMode(DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE));
        const auto dc = surface->GetMemoryDC(); const auto brush = CreateSolidBrush(white ? RGB(255, 255, 255) : RGB(0, 0, 0));
        if (!brush) throw std::runtime_error("Cannot create text background brush.");
        RECT bounds{0, 0, TileWidth, height}; FillRect(dc, &bounds, brush); DeleteObject(brush);
        const float x = static_cast<float>(Padding - index * TileWidth);
        ComPtr<Renderer> painter; painter.Attach(new Renderer(surface.Get(), parameters.Get(), rgb(item.color), factory.Get()));
        // Layout is shared across appearances. Effects are applied only while
        // rasterizing on the UI thread and cleared before another appearance.
        const auto layout = item.shape->layout.Get();
        check(layout->SetDrawingEffect(nullptr,{0,UINT32_MAX}));
        ComPtr<Ink> highlight; highlight.Attach(new Ink(RGB(245,189,99)));
        for (const auto& range : item.highlights) check(layout->SetDrawingEffect(highlight.Get(),range));
        check(layout->Draw(nullptr, painter.Get(), x, Padding));
        check(layout->SetDrawingEffect(nullptr,{0,UINT32_MAX}));
        BITMAP bitmap{}; if (!GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof(bitmap), &bitmap) || !bitmap.bmBits) throw std::runtime_error("Cannot read DirectWrite bitmap.");
        std::vector<uint32_t> result(static_cast<size_t>(TileWidth) * height);
        for (int row = 0; row < height; ++row) {
            const auto source = static_cast<const unsigned char*>(bitmap.bmBits) + static_cast<size_t>(row) * bitmap.bmWidthBytes;
            std::memcpy(result.data() + static_cast<size_t>(row) * TileWidth, source, TileWidth * 4);
        }
        return result;
    }
    SDL_Texture* tile(Entry& item, int index) {
        const auto found = std::find_if(item.tiles.begin(), item.tiles.end(), [&](const auto& tile) { return tile->index == index; });
        if (found != item.tiles.end()) { item.tiles.splice(item.tiles.begin(), item.tiles, found); return item.tiles.front()->texture; }
        auto black = pixels(item, index, false); const auto white = pixels(item, index, true);
        ++rasterized;
        for (size_t i = 0; i < black.size(); ++i) {
            int alpha = 0;
            for (int shift : {0, 8, 16}) alpha = std::max(alpha, 255 - static_cast<int>((white[i] >> shift) & 255) + static_cast<int>((black[i] >> shift) & 255));
            alpha = std::clamp(alpha, 0, 255); uint32_t value = static_cast<uint32_t>(alpha) << 24;
            if (alpha) for (int shift : {0, 8, 16}) value |= static_cast<uint32_t>(std::min(255, static_cast<int>((black[i] >> shift) & 255) * 255 / alpha)) << shift;
            black[i] = value;
        }
        const int height = static_cast<int>(black.size() / TileWidth);
        auto stored = std::make_unique<Tile>(); stored->index = index; stored->bytes = black.size() * 4;
        stored->texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, TileWidth, height);
        if (!stored->texture || !SDL_UpdateTexture(stored->texture, nullptr, black.data(), TileWidth * 4) || !SDL_SetTextureBlendMode(stored->texture, SDL_BLENDMODE_BLEND))
            throw std::runtime_error(SDL_GetError());
        bytes += stored->bytes; item.tiles.push_front(std::move(stored));
        while (item.tiles.size() > 8) { bytes -= item.tiles.back()->bytes; retired.splice(retired.end(), item.tiles, std::prev(item.tiles.end())); }
        trim(); return item.tiles.front()->texture;
    }
};

Painter::Painter(SDL_Renderer* renderer) : impl(std::make_unique<Impl>(renderer)) {}
Painter::~Painter() { if (active == this) active = nullptr; }
void Painter::beginFrame() { impl->retired.clear(); }
void Painter::resetTextures() {
    for (auto& entry : impl->cache) entry.tiles.clear();
    impl->retired.clear(); impl->bytes = 0;
}
CacheStats Painter::cacheStats() const {
    CacheStats stats{impl->layouts.size(),impl->cache.size(),0,impl->bytes,impl->shaped,impl->rasterized};
    for (const auto& entry : impl->cache) stats.tiles += entry.tiles.size();
    return stats;
}
float Painter::fontSizeForCapHeight(float pixels) const { return pixels / impl->capitalRatio; }
float Painter::width(const std::string& label, float size) { return impl->layout(label,size)->width; }
void Painter::draw(const std::string& label, const std::vector<std::wstring>& terms, ImVec2 origin, float size, ImU32 color, float maximumWidth) {
    auto& entry = impl->entry(label, terms, size, color, maximumWidth); auto draw = ImGui::GetWindowDrawList();
    if (maximumWidth > 0) draw->PushClipRect({origin.x,draw->GetClipRectMin().y},{origin.x+maximumWidth,draw->GetClipRectMax().y},true);
    const auto first = std::max(0, static_cast<int>(std::floor((draw->GetClipRectMin().x - origin.x + Padding) / TileWidth)));
    const auto last = std::min(static_cast<int>(std::ceil((entry.shape->width + Padding * 2) / TileWidth)), static_cast<int>(std::ceil((draw->GetClipRectMax().x - origin.x + Padding) / TileWidth)));
    const auto height = static_cast<float>(static_cast<int>(std::ceil(size * 1.5f)) + Padding * 2);
    for (int index = first; index < last; ++index) {
        auto texture = impl->tile(entry, index); const ImVec2 min{origin.x + static_cast<float>(index * TileWidth - Padding), origin.y - Padding};
        draw->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)), min, {min.x + TileWidth, min.y + height});
    }
    if (maximumWidth > 0) draw->PopClipRect();
}
Inspection Painter::inspect(const std::string& label, const std::vector<std::wstring>& terms, float size, float maximumWidth) {
    const auto& entry = impl->entry(label, terms, size, IM_COL32_WHITE,maximumWidth); Inspection result; result.width = entry.shape->width;
    UINT32 count = 0; entry.shape->layout->GetLineMetrics(nullptr,0,&count);
    std::vector<DWRITE_LINE_METRICS> lines(count);
    if (count) { check(entry.shape->layout->GetLineMetrics(lines.data(),count,&count)); for (const auto& line : lines) result.trimmed = result.trimmed || line.isTrimmed; }
    for (const auto& cluster : entry.shape->clusters) result.clusters.push_back(cluster.length);
    result.highlightedClusters = entry.highlights.size();
    ComPtr<Renderer> audit; audit.Attach(new Renderer(nullptr, impl->parameters.Get(), RGB(255, 255, 255)));
    check(entry.shape->layout->Draw(nullptr, audit.Get(), 0, 0)); result.missingGlyph = audit->missingGlyph; return result;
}
}
