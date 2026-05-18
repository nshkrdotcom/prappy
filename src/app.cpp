#define SDL_MAIN_HANDLED

#include "app.h"

#include "oahu_topology.h"

#include <SDL3/SDL_main.h>
#include <bgfx/platform.h>
#include <bx/math.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace prappy {

void logSmoke(AppState& state, const char* message) {
  if (state.smokeLog.is_open()) {
    state.smokeLog << message << '\n';
    state.smokeLog.flush();
  }
}

void setScreenshotStatus(AppState& state, const std::string& message, float seconds = 5.0f) {
  state.screenshotStatus = message;
  state.screenshotStatusSeconds = seconds;
}


void requestScreenshot(AppState& state) {
  state.screenshotRequested = true;
  setScreenshotStatus(state, "Capture queued", 2.0f);
}

bool queueVisualizationCanvasCapture(AppState& state, std::filesystem::path& outputPath, std::string& error) {
  const int width = static_cast<int>(std::round(state.visualizationCanvasSize.x));
  const int height = static_cast<int>(std::round(state.visualizationCanvasSize.y));
  if (width <= 0 || height <= 0) {
    error = "capture failed: visualization canvas is empty";
    return false;
  }

  outputPath = nextScreenshotPath();
  state.bgfxCallback.queueScreenshot(outputPath, state.visualizationCanvasOrigin, state.visualizationCanvasSize);

  const bgfx::FrameBufferHandle backbuffer = BGFX_INVALID_HANDLE;
  const std::string bgfxPath = outputPath.string();
  bgfx::requestScreenShot(backbuffer, bgfxPath.c_str());
  return true;
}

void processScreenshotRequest(AppState& state) {
  if (!state.screenshotRequested) {
    return;
  }

  state.screenshotRequested = false;
  std::filesystem::path outputPath;
  std::string error;
  if (queueVisualizationCanvasCapture(state, outputPath, error)) {
    setScreenshotStatus(state, std::string("Capture queued: ") + outputPath.string(), 3.0f);
  } else {
    setScreenshotStatus(state, error, 7.0f);
  }
}

void pollScreenshotStatus(AppState& state) {
  std::string message;
  if (state.bgfxCallback.consumeScreenshotStatus(message)) {
    setScreenshotStatus(state, message, 7.0f);
  }
}


void applyPrappyStyle();

void initVisualizationRenderer(AppState& state) {
  state.visualizationRenderer.colorLayout
    .begin()
    .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
    .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
    .end();

  state.visualizationRenderer.colorProgram = loadProgram(
    "shaders/color_vs.bin",
    "shaders/color_fs.bin"
  );
}

void shutdownVisualizationRenderer(AppState& state) {
  if (bgfx::isValid(state.visualizationRenderer.colorProgram.handle)) {
    bgfx::destroy(state.visualizationRenderer.colorProgram.handle);
    state.visualizationRenderer.colorProgram.handle = BGFX_INVALID_HANDLE;
  }
}

void fatalMessage(const char* title, const std::string& message) {
#ifdef _WIN32
  MessageBoxA(nullptr, message.c_str(), title, MB_OK | MB_ICONERROR);
#else
  std::fprintf(stderr, "%s: %s\n", title, message.c_str());
#endif
}

void initBgfx(AppState& state) {
  bgfx::PlatformData pd{};

#ifdef _WIN32
  SDL_PropertiesID props = SDL_GetWindowProperties(state.window);
  pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif

  bgfx::Init init;
  init.type = state.requestedRenderer;
  init.resolution.width = static_cast<std::uint32_t>(state.width);
  init.resolution.height = static_cast<std::uint32_t>(state.height);
  init.resolution.reset = BGFX_RESET_VSYNC;
  init.platformData = pd;
  init.callback = &state.bgfxCallback;

  if (!bgfx::init(init)) {
    throw std::runtime_error("bgfx::init failed");
  }

  state.bgfxReady = true;

  bgfx::setDebug(BGFX_DEBUG_TEXT);
  bgfx::setViewMode(kVisualizationView, bgfx::ViewMode::Sequential);
  bgfx::setViewMode(kUiView, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(
    kVisualizationView,
    BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
    0x101018ff,
    1.0f,
    0
  );
}

void initImGui(AppState& state) {
  logSmoke(state, "smoke: initImGui/check version");
  IMGUI_CHECKVERSION();
  logSmoke(state, "smoke: initImGui/create context");
  ImGui::CreateContext();
  state.imguiReady = true;

  logSmoke(state, "smoke: initImGui/io");
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.DisplaySize = ImVec2(static_cast<float>(state.width), static_cast<float>(state.height));

  logSmoke(state, "smoke: initImGui/style");
  applyPrappyStyle();

  logSmoke(state, "smoke: initImGui/layout");
  state.imguiLayout
    .begin()
    .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
    .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
    .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
    .end();

  logSmoke(state, "smoke: initImGui/uniforms");
  state.s_tex = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

  logSmoke(state, "smoke: initImGui/program");
  state.imguiProgram = loadProgram("shaders/imgui_vs.bin", "shaders/imgui_fs.bin");

  unsigned char* pixels = nullptr;
  int texWidth = 0;
  int texHeight = 0;

  logSmoke(state, "smoke: initImGui/font pixels");
  io.Fonts->GetTexDataAsRGBA32(&pixels, &texWidth, &texHeight);

  logSmoke(state, "smoke: initImGui/font texture memory");
  const bgfx::Memory* mem = bgfx::copy(
    pixels,
    static_cast<std::uint32_t>(texWidth * texHeight * 4)
  );

  logSmoke(state, "smoke: initImGui/font texture");
  state.fontTexture.handle = bgfx::createTexture2D(
    static_cast<std::uint16_t>(texWidth),
    static_cast<std::uint16_t>(texHeight),
    false,
    1,
    bgfx::TextureFormat::RGBA8,
    0,
    mem
  );

  logSmoke(state, "smoke: initImGui/set font texture id");
  io.Fonts->SetTexID(static_cast<ImTextureID>(
    static_cast<std::uint64_t>(state.fontTexture.handle.idx) + 1u
  ));
  logSmoke(state, "smoke: initImGui/done");
}

void shutdownImGui(AppState& state) {
  if (bgfx::isValid(state.fontTexture.handle)) {
    bgfx::destroy(state.fontTexture.handle);
  }

  if (bgfx::isValid(state.imguiProgram.handle)) {
    bgfx::destroy(state.imguiProgram.handle);
  }

  if (bgfx::isValid(state.s_tex)) {
    bgfx::destroy(state.s_tex);
  }

  if (state.imguiReady) {
    ImGui::DestroyContext();
    state.imguiReady = false;
  }
}

void beginImGuiFrame(AppState& state) {
  const auto now = std::chrono::steady_clock::now();
  if (state.lastFrameTime.time_since_epoch().count() != 0) {
    const std::chrono::duration<float> elapsed = now - state.lastFrameTime;
    state.deltaSeconds = std::clamp(elapsed.count(), 1.0f / 240.0f, 1.0f / 15.0f);
  }
  state.lastFrameTime = now;
  state.elapsedSeconds += state.deltaSeconds;
  if (state.screenshotStatusSeconds > 0.0f) {
    state.screenshotStatusSeconds = std::max(0.0f, state.screenshotStatusSeconds - state.deltaSeconds);
    if (state.screenshotStatusSeconds == 0.0f) {
      state.screenshotStatus.clear();
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(state.width), static_cast<float>(state.height));
  io.DeltaTime = state.deltaSeconds;

  ImGui::NewFrame();
}

ImTextureID commandTextureId(const ImDrawCmd* command) {
#if IMGUI_VERSION_NUM >= 19191
  return command->GetTexID();
#else
  return command->TextureId;
#endif
}

bgfx::TextureHandle textureHandleFromImGuiId(ImTextureID textureId) {
  bgfx::TextureHandle texture;
  texture.idx = static_cast<std::uint16_t>(
    static_cast<std::uint64_t>(textureId) - 1u
  );
  return texture;
}

void renderImGui(AppState& state, ImDrawData* drawData) {
  if (drawData == nullptr || drawData->TotalVtxCount == 0) {
    return;
  }

  const float left = drawData->DisplayPos.x;
  const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
  const float top = drawData->DisplayPos.y;
  const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;

  float ortho[16];
  bx::mtxOrtho(
    ortho,
    left,
    right,
    bottom,
    top,
    0.0f,
    100.0f,
    0.0f,
    bgfx::getCaps()->homogeneousDepth
  );
  float view[16];
  bx::mtxIdentity(view);
  bgfx::setViewTransform(kUiView, view, ortho);
  bgfx::setViewRect(
    kUiView,
    0,
    0,
    static_cast<std::uint16_t>(state.width),
    static_cast<std::uint16_t>(state.height)
  );

  const ImVec2 clipOffset = drawData->DisplayPos;

  for (int n = 0; n < drawData->CmdListsCount; ++n) {
    const ImDrawList* cmdList = drawData->CmdLists[n];

    const std::uint32_t vtxSize =
      static_cast<std::uint32_t>(cmdList->VtxBuffer.Size * sizeof(ImDrawVert));

    const std::uint32_t idxSize =
      static_cast<std::uint32_t>(cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));

    const bgfx::Memory* vtxMem = bgfx::copy(cmdList->VtxBuffer.Data, vtxSize);
    const bgfx::Memory* idxMem = bgfx::copy(cmdList->IdxBuffer.Data, idxSize);

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vtxMem, state.imguiLayout);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(
      idxMem,
      sizeof(ImDrawIdx) == 4 ? BGFX_BUFFER_INDEX32 : 0
    );

    for (int cmdIndex = 0; cmdIndex < cmdList->CmdBuffer.Size; ++cmdIndex) {
      const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmdIndex];

      if (pcmd->UserCallback != nullptr) {
        pcmd->UserCallback(cmdList, pcmd);
      } else {
        const float clipMinX = bx::max(pcmd->ClipRect.x - clipOffset.x, 0.0f);
        const float clipMinY = bx::max(pcmd->ClipRect.y - clipOffset.y, 0.0f);
        const float clipMaxX = bx::max(pcmd->ClipRect.z - clipOffset.x, 0.0f);
        const float clipMaxY = bx::max(pcmd->ClipRect.w - clipOffset.y, 0.0f);

        bgfx::setScissor(
          static_cast<std::uint16_t>(clipMinX),
          static_cast<std::uint16_t>(clipMinY),
          static_cast<std::uint16_t>(clipMaxX - clipMinX),
          static_cast<std::uint16_t>(clipMaxY - clipMinY)
        );

        const std::uint32_t numVertices =
          static_cast<std::uint32_t>(cmdList->VtxBuffer.Size) - pcmd->VtxOffset;

        bgfx::setVertexBuffer(0, vbh, pcmd->VtxOffset, numVertices);
        bgfx::setIndexBuffer(ibh, pcmd->IdxOffset, pcmd->ElemCount);

        bgfx::TextureHandle texture = textureHandleFromImGuiId(commandTextureId(pcmd));

        bgfx::setTexture(0, state.s_tex, texture);
        bgfx::setState(
          BGFX_STATE_WRITE_RGB |
          BGFX_STATE_WRITE_A |
          BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)
        );

        bgfx::submit(kUiView, state.imguiProgram.handle);
      }
    }

    bgfx::destroy(vbh);
    bgfx::destroy(ibh);
  }
}

void handleEvent(AppState& state, const SDL_Event& event) {
  ImGuiIO& io = ImGui::GetIO();

  switch (event.type) {
    case SDL_EVENT_QUIT:
      state.running = false;
      break;

    case SDL_EVENT_WINDOW_RESIZED:
      state.width = event.window.data1;
      state.height = event.window.data2;
      bgfx::reset(
        static_cast<std::uint32_t>(state.width),
        static_cast<std::uint32_t>(state.height),
        BGFX_RESET_VSYNC
      );
      state.visualizations.resetRequested = true;
      break;

    case SDL_EVENT_MOUSE_MOTION:
      io.AddMousePosEvent(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      int button = -1;
      if (event.button.button == SDL_BUTTON_LEFT) {
        button = 0;
      }
      if (event.button.button == SDL_BUTTON_RIGHT) {
        button = 1;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE) {
        button = 2;
      }
      if (button >= 0) {
        io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      }
      break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
      io.AddMouseWheelEvent(static_cast<float>(event.wheel.x), static_cast<float>(event.wheel.y));
      break;

    default:
      break;
  }
}

const char* vendorName(std::uint16_t vendorId) {
  switch (vendorId) {
    case BGFX_PCI_ID_NVIDIA:
      return "NVIDIA";
    case BGFX_PCI_ID_AMD:
      return "AMD";
    case BGFX_PCI_ID_INTEL:
      return "Intel";
    case BGFX_PCI_ID_MICROSOFT:
      return "Microsoft";
    case BGFX_PCI_ID_SOFTWARE_RASTERIZER:
      return "Software";
    default:
      return "Unknown";
  }
}

double timerMilliseconds(int64_t begin, int64_t end, int64_t frequency) {
  if (frequency <= 0 || end <= begin) {
    return 0.0;
  }

  return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
}

void applyPrappyStyle() {
  ImGui::StyleColorsDark();

  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.ChildRounding = 5.0f;
  style.FrameRounding = 4.0f;
  style.PopupRounding = 5.0f;
  style.GrabRounding = 4.0f;
  style.TabRounding = 4.0f;
  style.ScrollbarRounding = 8.0f;
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(10.0f, 10.0f);
  style.FramePadding = ImVec2(9.0f, 5.0f);
  style.ItemSpacing = ImVec2(8.0f, 7.0f);
  style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = ImVec4(0.91f, 0.94f, 0.97f, 1.0f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.58f, 0.64f, 1.0f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.043f, 0.058f, 0.96f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.064f, 0.083f, 0.94f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.052f, 0.068f, 0.98f);
  colors[ImGuiCol_Border] = ImVec4(0.19f, 0.23f, 0.28f, 0.82f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.10f, 0.13f, 0.92f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.16f, 0.20f, 0.96f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.22f, 0.28f, 1.0f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.04f, 0.05f, 0.06f, 1.0f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.07f, 0.09f, 1.0f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.035f, 0.043f, 0.058f, 0.98f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.035f, 0.043f, 0.058f, 0.78f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.29f, 0.34f, 0.9f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.38f, 0.44f, 0.95f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.31f, 0.76f, 0.86f, 1.0f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.31f, 0.76f, 0.86f, 0.88f);
  colors[ImGuiCol_Button] = ImVec4(0.10f, 0.13f, 0.17f, 0.96f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.22f, 0.28f, 1.0f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.23f, 0.36f, 0.42f, 1.0f);
  colors[ImGuiCol_Header] = ImVec4(0.11f, 0.16f, 0.20f, 0.92f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.23f, 0.29f, 1.0f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.36f, 0.42f, 1.0f);
  colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.24f, 0.29f, 0.75f);
  colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.10f, 0.13f, 0.96f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.19f, 0.29f, 0.35f, 1.0f);
  colors[ImGuiCol_TabSelected] = ImVec4(0.14f, 0.23f, 0.28f, 1.0f);
  colors[ImGuiCol_TableHeaderBg] = ImVec4(0.08f, 0.10f, 0.13f, 1.0f);
  colors[ImGuiCol_TableBorderStrong] = ImVec4(0.22f, 0.27f, 0.32f, 1.0f);
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.18f, 0.22f, 1.0f);
}

const char* buildConfiguration() {
#ifdef NDEBUG
  return "Release";
#else
  return "Debug";
#endif
}

const char* compilerLabel() {
#ifdef _MSC_VER
  static char label[32];
  std::snprintf(label, sizeof(label), "MSVC %d", _MSC_VER);
  return label;
#else
  return "C++20";
#endif
}

const char* requestedRendererLabel(bgfx::RendererType::Enum renderer) {
  return renderer == bgfx::RendererType::Count ? "Auto" : bgfx::getRendererName(renderer);
}

const char* capabilityLabel(bool available) {
  return available ? "available" : "unavailable";
}

std::string supportedRenderersLabel() {
  bgfx::RendererType::Enum renderers[16]{};
  const std::uint8_t count = bgfx::getSupportedRenderers(16, renderers);
  std::ostringstream label;
  for (std::uint8_t i = 0; i < count; ++i) {
    if (i > 0) {
      label << ", ";
    }
    label << bgfx::getRendererName(renderers[i]);
  }
  return label.str();
}

bool drawModeButton(const char* label, bool active, const ImVec2& size) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.38f, 0.43f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.23f, 0.47f, 0.53f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.32f, 0.38f, 1.0f));
  }

  const bool pressed = ImGui::Button(label, size);

  if (active) {
    ImGui::PopStyleColor(3);
  }

  return pressed;
}

void drawKeyValue(const char* key, const char* value) {
  ImGui::TextDisabled("%s", key);
  ImGui::SameLine(136.0f);
  ImGui::TextUnformatted(value);
}

void drawKeyValueNumber(const char* key, int value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d", value);
  drawKeyValue(key, buffer);
}

void drawKeyValueFloat(const char* key, double value, const char* suffix = "") {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.2f%s", value, suffix);
  drawKeyValue(key, buffer);
}

void drawMetricTile(const char* label, const char* value, const ImVec4& accent) {
  ImGui::BeginGroup();
  ImGui::PushStyleColor(ImGuiCol_Text, accent);
  ImGui::TextUnformatted(value);
  ImGui::PopStyleColor();
  ImGui::TextDisabled("%s", label);
  ImGui::EndGroup();
}

void drawLibraryModule(
  const char* name,
  const char* role,
  const char* primary,
  const char* secondary,
  const ImVec4& accent
) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.065f, 0.078f, 0.098f, 0.96f));
  ImGui::BeginChild(name, ImVec2(0.0f, 86.0f), ImGuiChildFlags_Borders);
  ImGui::PushStyleColor(ImGuiCol_Text, accent);
  ImGui::TextUnformatted(name);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", role);
  ImGui::Separator();
  ImGui::TextUnformatted(primary);
  ImGui::TextDisabled("%s", secondary);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void drawVisualizationSelector(AppState& state) {
  const float available = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
  const float gap = 6.0f;
  const float width = std::clamp((available - gap * 3.0f) / 4.0f, 88.0f, 140.0f);
  const ImVec2 size(width, 30.0f);

  if (drawModeButton(
    visualizationShortName(VisualizationId::RandomLines2D),
    state.visualizations.active == VisualizationId::RandomLines2D,
    size
  )) {
    state.visualizations.setActive(VisualizationId::RandomLines2D);
  }
  ImGui::SameLine(0.0f, gap);
  if (drawModeButton(
    visualizationShortName(VisualizationId::Starfield3D),
    state.visualizations.active == VisualizationId::Starfield3D,
    size
  )) {
    state.visualizations.setActive(VisualizationId::Starfield3D);
  }
  ImGui::SameLine(0.0f, gap);
  if (drawModeButton(
    visualizationShortName(VisualizationId::OahuFlyover),
    state.visualizations.active == VisualizationId::OahuFlyover,
    size
  )) {
    state.visualizations.setActive(VisualizationId::OahuFlyover);
  }
  ImGui::SameLine(0.0f, gap);
  if (drawModeButton(
    visualizationShortName(VisualizationId::ParticleField),
    state.visualizations.active == VisualizationId::ParticleField,
    size
  )) {
    state.visualizations.setActive(VisualizationId::ParticleField);
  }
}

void applyOahuDiagnosticPreset(OahuDiagnosticSettings& diagnostics, const std::string& preset);

void drawOahuQuickControls(AppState& state) {
  if (state.visualizations.active != VisualizationId::OahuFlyover) {
    return;
  }

  OahuDiagnosticSettings& diagnostics = state.oahuDiagnostics;
  ImGui::TextDisabled("Oahu view");
  ImGui::SameLine();
  ImGui::Checkbox("Top down", &diagnostics.topDown);
  ImGui::SameLine();
  if (ImGui::Button("Coast", ImVec2(62.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "coastline");
  }
  ImGui::SameLine();
  if (ImGui::Button("Mesh", ImVec2(62.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "mesh");
  }
  ImGui::SameLine();
  if (ImGui::Button("All", ImVec2(54.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "all");
  }
  ImGui::SameLine();
  if (ImGui::Button("Flyover", ImVec2(72.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "flyover");
  }
  ImGui::SameLine();
  if (ImGui::Button("Panel", ImVec2(62.0f, 26.0f))) {
    state.focusMode = false;
    state.showInspectorPanel = true;
  }
}

void drawOahuCanvasControls(AppState& state, const ImVec2& canvasOrigin, const ImVec2& canvasSize) {
  if (
    state.visualizations.active != VisualizationId::OahuFlyover ||
    !state.focusMode ||
    !state.visualizations.showStatus
  ) {
    return;
  }

  const float panelWidth = std::min(520.0f, std::max(1.0f, canvasSize.x - 32.0f));
  ImGui::SetCursorScreenPos(addVec2(canvasOrigin, ImVec2(canvasSize.x - panelWidth - 16.0f, 16.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(8, 10, 18, 218));
  ImGui::BeginChild(
    "OahuCanvasControls",
    ImVec2(panelWidth, 56.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );

  OahuDiagnosticSettings& diagnostics = state.oahuDiagnostics;
  ImGui::TextUnformatted("Oahu");
  ImGui::SameLine();
  ImGui::Checkbox("Top Down", &diagnostics.topDown);
  ImGui::SameLine();
  if (ImGui::Button("Coast", ImVec2(58.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "coastline");
  }
  ImGui::SameLine();
  if (ImGui::Button("Mesh", ImVec2(56.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "mesh");
  }
  ImGui::SameLine();
  if (ImGui::Button("All", ImVec2(46.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "all");
  }
  ImGui::SameLine();
  if (ImGui::Button("Flyover", ImVec2(70.0f, 26.0f))) {
    applyOahuDiagnosticPreset(diagnostics, "flyover");
  }
  ImGui::SameLine();
  if (ImGui::Button("Panel", ImVec2(58.0f, 26.0f))) {
    state.focusMode = false;
    state.showInspectorPanel = true;
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void drawCommandBar(AppState& state) {
  const bool showOahuControls = state.visualizations.active == VisualizationId::OahuFlyover;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
  ImGui::BeginChild(
    "CommandBar",
    ImVec2(0.0f, showOahuControls ? 102.0f : 66.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );

  if (ImGui::BeginTable("CommandBarLayout", 3, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Brand", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Modes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 410.0f);
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("PRAPPY");
    ImGui::TextDisabled("SDL3 / bgfx / Dear ImGui");

    ImGui::TableSetColumnIndex(1);
    drawVisualizationSelector(state);
    ImGui::TextDisabled("%s", visualizationSpaceLabel(state.visualizations.active));

    ImGui::TableSetColumnIndex(2);
    if (ImGui::Button(state.focusMode ? "Workspace" : "Focus", ImVec2(92.0f, 30.0f))) {
      state.focusMode = !state.focusMode;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(82.0f, 30.0f))) {
      state.visualizations.resetRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture", ImVec2(92.0f, 30.0f))) {
      requestScreenshot(state);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Diagnostics", &state.visualizations.showStatus);
    ImGui::TextDisabled("%s / %s", buildConfiguration(), compilerLabel());

    ImGui::EndTable();
  }

  drawOahuQuickControls(state);

  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void drawStackPanel(AppState& state) {
  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();

  char sdlPrimary[64];
  std::snprintf(sdlPrimary, sizeof(sdlPrimary), "%d x %d window", state.width, state.height);

  char bgfxPrimary[96];
  std::snprintf(
    bgfxPrimary,
    sizeof(bgfxPrimary),
    "%s / %s",
    caps ? bgfx::getRendererName(caps->rendererType) : "renderer",
    caps ? vendorName(caps->vendorId) : "GPU"
  );

  char imguiPrimary[64];
  std::snprintf(imguiPrimary, sizeof(imguiPrimary), "%.1f ms UI tick", state.deltaSeconds * 1000.0f);

  char buildPrimary[64];
  std::snprintf(buildPrimary, sizeof(buildPrimary), "%s C++20", buildConfiguration());

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
  ImGui::BeginChild("StackPanel", ImVec2(284.0f, 0.0f), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("Native Stack");
  ImGui::TextDisabled("runtime surface");
  ImGui::Separator();

  drawLibraryModule(
    "SDL3",
    "window/input",
    sdlPrimary,
    "event pump + native handle",
    ImVec4(0.37f, 0.78f, 0.92f, 1.0f)
  );
  drawLibraryModule(
    "bgfx",
    "renderer",
    bgfxPrimary,
    stats ? "custom transient vertex passes" : "initializing",
    ImVec4(0.58f, 0.84f, 0.46f, 1.0f)
  );
  drawLibraryModule(
    "Dear ImGui",
    "tool UI",
    imguiPrimary,
    "menus, tabs, tables, overlays",
    ImVec4(0.94f, 0.70f, 0.36f, 1.0f)
  );
  drawLibraryModule(
    "CMake/Ninja",
    compilerLabel(),
    buildPrimary,
    "scripted Windows build",
    ImVec4(0.84f, 0.62f, 0.92f, 1.0f)
  );

  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void drawRendererTab(AppState& state) {
  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();
  const double renderCpuMs = stats
    ? timerMilliseconds(stats->cpuTimeBegin, stats->cpuTimeEnd, stats->cpuTimerFreq)
    : 0.0;
  const double gpuMs = stats
    ? timerMilliseconds(stats->gpuTimeBegin, stats->gpuTimeEnd, stats->gpuTimerFreq)
    : 0.0;

  char frameValue[32];
  char cpuValue[32];
  char gpuValue[32];
  std::snprintf(frameValue, sizeof(frameValue), "%.2f ms", state.deltaSeconds * 1000.0f);
  std::snprintf(cpuValue, sizeof(cpuValue), "%.2f ms", renderCpuMs);
  std::snprintf(gpuValue, sizeof(gpuValue), "%.2f ms", gpuMs);

  if (ImGui::BeginTable("RendererMetrics", 3, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    drawMetricTile("frame", frameValue, ImVec4(0.38f, 0.82f, 0.92f, 1.0f));
    ImGui::TableNextColumn();
    drawMetricTile("CPU", cpuValue, ImVec4(0.80f, 0.88f, 0.52f, 1.0f));
    ImGui::TableNextColumn();
    drawMetricTile("GPU", gpuValue, ImVec4(0.94f, 0.68f, 0.42f, 1.0f));
    ImGui::EndTable();
  }

  ImGui::Separator();
  if (caps) {
    const std::string renderers = supportedRenderersLabel();
    drawKeyValue("Requested", requestedRendererLabel(state.requestedRenderer));
    drawKeyValue("Renderer", bgfx::getRendererName(caps->rendererType));
    drawKeyValue("Supported", renderers.c_str());
    drawKeyValue("Vendor", vendorName(caps->vendorId));
    drawKeyValueNumber("Device", caps->deviceId);
    drawKeyValueNumber("GPUs", static_cast<int>(caps->numGPUs));
    drawKeyValue("Compute", capabilityLabel((caps->supported & BGFX_CAPS_COMPUTE) != 0));
    drawKeyValue("Instancing", capabilityLabel((caps->supported & BGFX_CAPS_INSTANCING) != 0));
    drawKeyValue("Readback", capabilityLabel((caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) != 0));
    drawKeyValue("Indirect", capabilityLabel((caps->supported & BGFX_CAPS_DRAW_INDIRECT) != 0));
  }
  if (stats) {
    drawKeyValueNumber("Draw calls", static_cast<int>(stats->numDraw));
    drawKeyValueNumber("Transient VB", static_cast<int>(stats->transientVbUsed));
  }
}

void drawCameraControls(AppState& state) {
  const VisualizationDescriptor& descriptor = visualizationDescriptor(state.visualizations.active);
  if (!descriptor.usesCamera) {
    ImGui::TextDisabled("Camera: not used by this module");
    return;
  }

  CameraRig& camera = state.visualizations.camera;
  ImGui::SeparatorText("Camera");

  if (descriptor.hasAutoCamera) {
    bool autoCamera = !camera.manual;
    if (ImGui::Checkbox("Auto route", &autoCamera)) {
      camera.manual = !autoCamera;
    }
    ImGui::SliderFloat("Route speed", &camera.routeSpeed, 0.15f, 3.0f, "%.2f");
  } else {
    ImGui::Checkbox("Manual look", &camera.manual);
  }

  ImGui::SliderFloat("FOV", &camera.fovDegrees, 38.0f, 96.0f, "%.0f");

  if (camera.manual) {
    ImGui::SliderFloat("Yaw", &camera.yaw, -kPi, kPi, "%.2f");
    ImGui::SliderFloat("Pitch", &camera.pitch, -1.15f, 1.25f, "%.2f");

    if (
      state.visualizations.active == VisualizationId::OahuFlyover ||
      state.visualizations.active == VisualizationId::ParticleField
    ) {
      ImGui::SliderFloat("Distance", &camera.distance, 2.2f, 34.0f, "%.1f");
      ImGui::SliderFloat("Target X", &camera.target.x, -8.0f, 8.0f, "%.2f");
      ImGui::SliderFloat("Target Y", &camera.target.y, -1.0f, 4.0f, "%.2f");
      ImGui::SliderFloat("Target Z", &camera.target.z, -8.0f, 8.0f, "%.2f");
    }
  }

  if (ImGui::Button("Reset Camera", ImVec2(-1.0f, 30.0f))) {
    camera.resetFor(state.visualizations.active);
  }
}

void drawOahuDiagnosticControls(AppState& state) {
  OahuDiagnosticSettings& diagnostics = state.oahuDiagnostics;

  ImGui::Separator();
  ImGui::TextUnformatted("Oahu Isolation");
  ImGui::Checkbox("Top-down north-up", &diagnostics.topDown);
  ImGui::Checkbox("Background", &diagnostics.showBackground);
  ImGui::Checkbox("Filled terrain", &diagnostics.showFilledTerrain);
  ImGui::Checkbox("Coastline", &diagnostics.showCoastline);
  ImGui::Checkbox("Terrain grid", &diagnostics.showGrid);
  ImGui::Checkbox("Ridge lines", &diagnostics.showRidges);
  ImGui::Checkbox("Landmarks", &diagnostics.showLandmarks);
}

void drawVisualizationTab(AppState& state) {
  drawVisualizationSelector(state);
  ImGui::Separator();
  drawKeyValue("Active", visualizationName(state.visualizations.active));
  drawKeyValue("Space", visualizationSpaceLabel(state.visualizations.active));
  drawKeyValue("Primitive", visualizationDescriptor(state.visualizations.active).primitiveLabel);
  state.visualizations.activeModule().drawInspector();
  if (state.visualizations.active == VisualizationId::OahuFlyover) {
    drawOahuDiagnosticControls(state);
  }
  drawCameraControls(state);

  ImGui::Separator();
  ImGui::Checkbox("Renderer overlay", &state.visualizations.showStatus);
  ImGui::Checkbox("Focus mode", &state.focusMode);
  if (ImGui::Button("Reset Active Visualization", ImVec2(-1.0f, 30.0f))) {
    state.visualizations.resetRequested = true;
  }
  if (ImGui::Button("Capture Screenshot", ImVec2(-1.0f, 30.0f))) {
    requestScreenshot(state);
  }
  if (!state.screenshotStatus.empty()) {
    ImGui::TextWrapped("%s", state.screenshotStatus.c_str());
  }
}

void drawDataTab() {
  drawKeyValue("Oahu source", "Hawaii GIS Coastline");
  drawKeyValue("Coastline", "USGS DLG-derived polygon");
  drawKeyValue("Elevation", "USGS EPQS meters");
  drawKeyValueNumber("Grid width", kOahuGridWidth);
  drawKeyValueNumber("Grid height", kOahuGridHeight);
  drawKeyValueNumber("Coast samples", kOahuCoastlinePointCount);
  drawKeyValueNumber("Source samples", kOahuSourceCoastlinePointCount);
  drawKeyValueFloat("Map aspect", kOahuMapAspect);
  ImGui::Separator();
  drawKeyValue("Topology file", "src/oahu_topology.h");
  drawKeyValue("Refresh tool", "tools/fetch_oahu_topology.py");
  drawKeyValue("Validate tool", "tools/validate_oahu_topology.py");
  drawKeyValue("Debug artifacts", "build/oahu_debug");
}

void drawInspectorPanel(AppState& state) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
  ImGui::BeginChild("InspectorPanel", ImVec2(340.0f, 0.0f), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("Inspector");
  ImGui::TextDisabled("%s", visualizationName(state.visualizations.active));
  ImGui::Separator();

  if (ImGui::BeginTabBar("InspectorTabs")) {
    if (ImGui::BeginTabItem("Renderer")) {
      drawRendererTab(state);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Visual")) {
      drawVisualizationTab(state);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Data")) {
      drawDataTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void drawStatusStrip(AppState& state) {
  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 5.0f));
  ImGui::BeginChild(
    "StatusStrip",
    ImVec2(0.0f, 32.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );
  ImGui::Text(
    "%s | %s | %.0f x %.0f",
    caps ? bgfx::getRendererName(caps->rendererType) : "bgfx",
    visualizationName(state.visualizations.active),
    state.visualizationCanvasSize.x,
    state.visualizationCanvasSize.y
  );
  ImGui::SameLine();
  ImGui::TextDisabled(
    "| frame %.2f ms | draws %u | %s",
    state.deltaSeconds * 1000.0f,
    stats ? stats->numDraw : 0u,
    state.focusMode ? "focus" : "workspace"
  );
  if (!state.screenshotStatus.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", state.screenshotStatus.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void handleVisualizationCanvasInput(AppState& state) {
  const VisualizationDescriptor& descriptor = visualizationDescriptor(state.visualizations.active);
  if (!descriptor.usesCamera) {
    return;
  }

  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  if (!hovered && !active) {
    return;
  }

  ImGuiIO& io = ImGui::GetIO();
  CameraRig& camera = state.visualizations.camera;

  if (hovered && std::abs(io.MouseWheel) > 0.001f) {
    camera.zoom(state.visualizations.active, io.MouseWheel);
  }

  if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
    camera.orbit(io.MouseDelta);
  }

  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
    camera.pan(io.MouseDelta);
  }
}

void drawVisualizationStatus(AppState& state, const ImVec2& canvasOrigin) {
  if (!state.visualizations.showStatus) {
    return;
  }

  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();
  const double renderCpuMs = stats
    ? timerMilliseconds(stats->cpuTimeBegin, stats->cpuTimeEnd, stats->cpuTimerFreq)
    : 0.0;
  const double gpuMs = stats
    ? timerMilliseconds(stats->gpuTimeBegin, stats->gpuTimeEnd, stats->gpuTimerFreq)
    : 0.0;
  const float desiredHeight = state.visualizations.active == VisualizationId::OahuFlyover
    ? 364.0f
    : 348.0f;
  const float maxHeight = std::max(state.visualizationCanvasSize.y - 32.0f, 160.0f);
  const float panelHeight = std::min(desiredHeight, maxHeight);
  const bool needsScrollbar = panelHeight + 0.5f < desiredHeight;
  const ImGuiWindowFlags panelFlags = needsScrollbar
    ? ImGuiWindowFlags_None
    : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

  ImGui::SetCursorScreenPos(addVec2(canvasOrigin, ImVec2(16.0f, 16.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(8, 10, 18, 218));
  ImGui::BeginChild(
    "VisualizationStatus",
    ImVec2(390.0f, panelHeight),
    ImGuiChildFlags_Borders,
    panelFlags
  );

  ImGui::TextUnformatted(visualizationName(state.visualizations.active));
  ImGui::Separator();
  if (caps) {
    ImGui::Text("Renderer: %s", bgfx::getRendererName(caps->rendererType));
    ImGui::Text(
      "GPU: %s 0x%04x:0x%04x",
      vendorName(caps->vendorId),
      caps->vendorId,
      caps->deviceId
    );
    ImGui::Text("Enumerated GPUs: %u", static_cast<unsigned>(caps->numGPUs));
    ImGui::Text("Compute shaders: %s", (caps->supported & BGFX_CAPS_COMPUTE) ? "yes" : "no");
    ImGui::Text("Instancing: %s", (caps->supported & BGFX_CAPS_INSTANCING) ? "yes" : "no");
    ImGui::Text("Texture readback: %s", (caps->supported & BGFX_CAPS_TEXTURE_READ_BACK) ? "yes" : "no");
  }
  ImGui::Text("App frame: %.2f ms", state.deltaSeconds * 1000.0f);
  ImGui::Text("Render CPU: %.2f ms", renderCpuMs);
  ImGui::Text("GPU frame: %.2f ms", gpuMs);
  if (stats) {
    ImGui::Text("Draw calls: %u", stats->numDraw);
    ImGui::Text(
      "Transient VB: %d / %u",
      stats->transientVbUsed,
      caps ? caps->limits.maxTransientVbSize : 0u
    );
  }
  if (state.visualizations.active == VisualizationId::OahuFlyover) {
    ImGui::Text(
      "Oahu grid: %d x %d, max %.0f m",
      kOahuGridWidth,
      kOahuGridHeight,
      kOahuMaxElevationMeters
    );
  }
  ImGui::Text("Canvas: %.0f x %.0f", state.visualizationCanvasSize.x, state.visualizationCanvasSize.y);

  if (ImGui::Button("Reset")) {
    state.visualizations.resetRequested = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Hide")) {
    state.visualizations.showStatus = false;
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void drawAppUi(AppState& state) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

  const ImGuiWindowFlags windowFlags =
    ImGuiWindowFlags_NoTitleBar |
    ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoNavFocus |
    ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_MenuBar;

  ImGui::Begin("PrappyVisualizationHost", nullptr, windowFlags);

  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Reset Visualization")) {
        state.visualizations.resetRequested = true;
      }
      if (ImGui::MenuItem("Reset Camera")) {
        state.visualizations.camera.resetFor(state.visualizations.active);
      }
      if (ImGui::MenuItem("Capture Visualization")) {
        requestScreenshot(state);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Quit")) {
        state.running = false;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Visualization")) {
      if (ImGui::MenuItem(
        "Random Lines 2D",
        nullptr,
        state.visualizations.active == VisualizationId::RandomLines2D
      )) {
        state.visualizations.setActive(VisualizationId::RandomLines2D);
      }

      if (ImGui::MenuItem(
        "Infinite Starfield",
        nullptr,
        state.visualizations.active == VisualizationId::Starfield3D
      )) {
        state.visualizations.setActive(VisualizationId::Starfield3D);
      }

      if (ImGui::MenuItem(
        "Oahu Flyover",
        nullptr,
        state.visualizations.active == VisualizationId::OahuFlyover
      )) {
        state.visualizations.setActive(VisualizationId::OahuFlyover);
      }

      if (ImGui::MenuItem(
        "GPU Particle Field",
        nullptr,
        state.visualizations.active == VisualizationId::ParticleField
      )) {
        state.visualizations.setActive(VisualizationId::ParticleField);
      }

      ImGui::EndMenu();
    }

    if (state.visualizations.active == VisualizationId::OahuFlyover && ImGui::BeginMenu("Oahu")) {
      ImGui::MenuItem("Top Down", nullptr, &state.oahuDiagnostics.topDown);
      ImGui::Separator();
      if (ImGui::MenuItem("Coastline Only")) {
        applyOahuDiagnosticPreset(state.oahuDiagnostics, "coastline");
      }
      if (ImGui::MenuItem("Mesh Debug")) {
        applyOahuDiagnosticPreset(state.oahuDiagnostics, "mesh");
      }
      if (ImGui::MenuItem("All Layers")) {
        applyOahuDiagnosticPreset(state.oahuDiagnostics, "all");
      }
      if (ImGui::MenuItem("Flyover")) {
        applyOahuDiagnosticPreset(state.oahuDiagnostics, "flyover");
      }
      ImGui::Separator();
      ImGui::MenuItem("Background", nullptr, &state.oahuDiagnostics.showBackground);
      ImGui::MenuItem("Filled Terrain", nullptr, &state.oahuDiagnostics.showFilledTerrain);
      ImGui::MenuItem("Coastline", nullptr, &state.oahuDiagnostics.showCoastline);
      ImGui::MenuItem("Terrain Grid", nullptr, &state.oahuDiagnostics.showGrid);
      ImGui::MenuItem("Ridge Lines", nullptr, &state.oahuDiagnostics.showRidges);
      ImGui::MenuItem("Landmarks", nullptr, &state.oahuDiagnostics.showLandmarks);
      if (ImGui::MenuItem("Show Inspector Panel")) {
        state.focusMode = false;
        state.showInspectorPanel = true;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Renderer Diagnostics", nullptr, &state.visualizations.showStatus);
      ImGui::MenuItem("Focus Mode", nullptr, &state.focusMode);
      ImGui::Separator();
      ImGui::MenuItem("Stack Panel", nullptr, &state.showStackPanel);
      ImGui::MenuItem("Inspector Panel", nullptr, &state.showInspectorPanel);
      ImGui::MenuItem("Status Strip", nullptr, &state.showStatusStrip);
      ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::TextUnformatted(visualizationName(state.visualizations.active));
    ImGui::EndMenuBar();
  }

  if (!state.focusMode) {
    drawCommandBar(state);
  }

  const float statusStripHeight = (!state.focusMode && state.showStatusStrip) ? 36.0f : 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
  ImGui::BeginChild(
    "Workspace",
    ImVec2(0.0f, -statusStripHeight),
    false,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground
  );

  const bool showStack = !state.focusMode && state.showStackPanel;
  const bool showInspector = !state.focusMode && state.showInspectorPanel;
  const int workspaceColumns = 1 + (showStack ? 1 : 0) + (showInspector ? 1 : 0);

  if (ImGui::BeginTable(
    "WorkspaceLayout",
    workspaceColumns,
    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
  )) {
    if (showStack) {
      ImGui::TableSetupColumn("Stack", ImGuiTableColumnFlags_WidthFixed, 284.0f);
    }
    ImGui::TableSetupColumn("Visualization", ImGuiTableColumnFlags_WidthStretch);
    if (showInspector) {
      ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 340.0f);
    }

    ImGui::TableNextRow();
    int column = 0;

    if (showStack) {
      ImGui::TableSetColumnIndex(column++);
      drawStackPanel(state);
    }

    ImGui::TableSetColumnIndex(column++);
    ImGui::BeginChild(
      "VisualizationRegion",
      ImVec2(0.0f, 0.0f),
      false,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
    );

    const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 1.0f);
    canvasSize.y = std::max(canvasSize.y, 1.0f);

    ImGui::InvisibleButton("VisualizationCanvas", canvasSize);
    handleVisualizationCanvasInput(state);

    state.visualizationCanvasOrigin = canvasOrigin;
    state.visualizationCanvasSize = canvasSize;
    drawOahuCanvasControls(state, canvasOrigin, canvasSize);
    drawVisualizationStatus(state, canvasOrigin);

    ImGui::EndChild();

    if (showInspector) {
      ImGui::TableSetColumnIndex(column++);
      drawInspectorPanel(state);
    }

    ImGui::EndTable();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();

  if (!state.focusMode && state.showStatusStrip) {
    drawStatusStrip(state);
  }

  ImGui::End();
  ImGui::PopStyleVar(3);
}

std::uint32_t visualizationClearColor(VisualizationId id) {
  switch (id) {
    case VisualizationId::RandomLines2D:
      return 0x07090fff;
    case VisualizationId::Starfield3D:
      return 0x02040aff;
    case VisualizationId::OahuFlyover:
      return 0x78c7efff;
    case VisualizationId::ParticleField:
      return 0x050714ff;
  }

  return 0x101018ff;
}

void renderVisualization(AppState& state) {
  const int x = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasOrigin.x)),
    0,
    std::max(state.width - 1, 0)
  );
  const int y = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasOrigin.y)),
    0,
    std::max(state.height - 1, 0)
  );
  const int width = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasSize.x)),
    1,
    std::max(state.width - x, 1)
  );
  const int height = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasSize.y)),
    1,
    std::max(state.height - y, 1)
  );

  bgfx::setViewRect(
    kVisualizationView,
    static_cast<std::uint16_t>(x),
    static_cast<std::uint16_t>(y),
    static_cast<std::uint16_t>(width),
    static_cast<std::uint16_t>(height)
  );
  bgfx::setViewClear(
    kVisualizationView,
    BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
    visualizationClearColor(state.visualizations.active),
    1.0f,
    0
  );

  const bgfx::Caps* caps = bgfx::getCaps();
  const CameraRig& camera = state.visualizations.camera;
  float view[16];
  float projection[16];

  if (state.visualizations.active == VisualizationId::OahuFlyover) {
    if (state.oahuDiagnostics.topDown) {
      const float terrainHeight = 8.2f;
      const float terrainWidth = terrainHeight * kOahuMapAspect;
      const float viewAspect = static_cast<float>(width) / static_cast<float>(height);
      float halfWidth = terrainWidth * 0.58f;
      float halfHeight = terrainHeight * 0.58f;
      if (viewAspect > kOahuMapAspect) {
        halfWidth = halfHeight * viewAspect;
      } else {
        halfHeight = halfWidth / viewAspect;
      }

      bx::mtxLookAt(
        view,
        bx::Vec3{0.0f, 14.0f, 0.0f},
        bx::Vec3{0.0f, 0.0f, 0.0f},
        bx::Vec3{0.0f, 0.0f, -1.0f}
      );
      bx::mtxOrtho(
        projection,
        -halfWidth,
        halfWidth,
        -halfHeight,
        halfHeight,
        0.05f,
        40.0f,
        0.0f,
        caps->homogeneousDepth
      );
    } else {
      bx::Vec3 eye = {0.0f, 0.0f, 0.0f};
      bx::Vec3 at = {0.0f, 0.0f, 0.0f};
      if (camera.manual) {
        eye = camera.orbitEye();
        at = camera.target;
      } else {
        const float cycle = std::fmod(state.elapsedSeconds * 0.025f * camera.routeSpeed, 1.0f);
        const float route = cycle * 2.0f - 1.0f;
        const float sway = std::sin(state.elapsedSeconds * 0.35f) * 0.55f;
        eye = {sway, 2.65f, 7.4f - route * 8.5f};
        at = {sway * 0.25f, 0.42f, 4.1f - route * 8.5f};
      }
      bx::mtxLookAt(view, eye, at);
      bx::mtxProj(
        projection,
        camera.fovDegrees,
        static_cast<float>(width) / static_cast<float>(height),
        0.05f,
        80.0f,
        caps->homogeneousDepth
      );
    }
  } else if (state.visualizations.active == VisualizationId::Starfield3D) {
    const bx::Vec3 eye = {0.0f, 0.0f, 0.0f};
    const bx::Vec3 direction = camera.manual ? camera.lookDirection() : bx::Vec3{0.0f, 0.0f, -1.0f};
    const bx::Vec3 at = {direction.x, direction.y, direction.z};
    bx::mtxLookAt(view, eye, at);
    bx::mtxProj(
      projection,
      camera.fovDegrees,
      static_cast<float>(width) / static_cast<float>(height),
      0.05f,
      60.0f,
      caps->homogeneousDepth
    );
  } else if (state.visualizations.active == VisualizationId::ParticleField) {
    const bx::Vec3 eye = camera.orbitEye();
    const bx::Vec3 at = camera.target;
    bx::mtxLookAt(view, eye, at);
    bx::mtxProj(
      projection,
      camera.fovDegrees,
      static_cast<float>(width) / static_cast<float>(height),
      0.05f,
      90.0f,
      caps->homogeneousDepth
    );
  } else {
    bx::mtxIdentity(view);
    bx::mtxOrtho(
      projection,
      0.0f,
      static_cast<float>(width),
      static_cast<float>(height),
      0.0f,
      0.0f,
      100.0f,
      0.0f,
      caps->homogeneousDepth
    );
  }

  bgfx::setViewTransform(kVisualizationView, view, projection);
  bgfx::touch(kVisualizationView);

  VisualizationContext context;
  context.renderer = &state.visualizationRenderer;
  context.viewId = kVisualizationView;
  context.size = ImVec2(static_cast<float>(width), static_cast<float>(height));
  context.deltaSeconds = state.deltaSeconds;
  context.elapsedSeconds = state.elapsedSeconds;
  context.oahuDiagnostics = &state.oahuDiagnostics;

  state.visualizations.draw(context);
}

bool hasArg(int argc, char** argv, const char* expected) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == expected) {
      return true;
    }
  }

  return false;
}

VisualizationId visualizationFromArgs(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--visualization=starfield" || arg == "--starfield") {
      return VisualizationId::Starfield3D;
    }
    if (arg == "--visualization=random-lines" || arg == "--random-lines") {
      return VisualizationId::RandomLines2D;
    }
    if (arg == "--visualization=oahu" || arg == "--oahu") {
      return VisualizationId::OahuFlyover;
    }
    if (arg == "--visualization=particle-field" || arg == "--particles") {
      return VisualizationId::ParticleField;
    }
  }

  return VisualizationId::RandomLines2D;
}

bgfx::RendererType::Enum rendererFromArgs(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--renderer=d3d11") {
      return bgfx::RendererType::Direct3D11;
    }
    if (arg == "--renderer=d3d12") {
      return bgfx::RendererType::Direct3D12;
    }
    if (arg == "--renderer=vulkan") {
      return bgfx::RendererType::Vulkan;
    }
    if (arg == "--renderer=auto") {
      return bgfx::RendererType::Count;
    }
  }

  return bgfx::RendererType::Count;
}

std::string argumentValue(int argc, char** argv, const char* prefix) {
  const std::string prefixString(prefix);
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind(prefixString, 0) == 0) {
      return arg.substr(prefixString.size());
    }
  }

  return {};
}

void applyOahuDiagnosticPreset(OahuDiagnosticSettings& diagnostics, const std::string& preset) {
  if (preset == "all") {
    diagnostics.topDown = true;
    diagnostics.showBackground = false;
    diagnostics.showFilledTerrain = true;
    diagnostics.showCoastline = true;
    diagnostics.showGrid = true;
    diagnostics.showRidges = true;
    diagnostics.showLandmarks = true;
  } else if (preset == "coastline") {
    diagnostics.topDown = true;
    diagnostics.showBackground = false;
    diagnostics.showFilledTerrain = false;
    diagnostics.showCoastline = true;
    diagnostics.showGrid = false;
    diagnostics.showRidges = false;
    diagnostics.showLandmarks = true;
  } else if (preset == "mesh") {
    diagnostics.topDown = true;
    diagnostics.showBackground = false;
    diagnostics.showFilledTerrain = true;
    diagnostics.showCoastline = true;
    diagnostics.showGrid = true;
    diagnostics.showRidges = false;
    diagnostics.showLandmarks = false;
  } else if (preset == "landmarks") {
    diagnostics.topDown = true;
    diagnostics.showBackground = false;
    diagnostics.showFilledTerrain = false;
    diagnostics.showCoastline = true;
    diagnostics.showGrid = false;
    diagnostics.showRidges = false;
    diagnostics.showLandmarks = true;
  } else if (preset == "flyover") {
    diagnostics.topDown = false;
    diagnostics.showBackground = true;
    diagnostics.showFilledTerrain = true;
    diagnostics.showCoastline = true;
    diagnostics.showGrid = false;
    diagnostics.showRidges = true;
    diagnostics.showLandmarks = false;
  }
}

void applyRuntimeArgs(AppState& state, int argc, char** argv) {
  if (hasArg(argc, argv, "--oahu-topdown")) {
    state.oahuDiagnostics.topDown = true;
  }

  const std::string oahuPreset = argumentValue(argc, argv, "--oahu-diagnostic=");
  if (!oahuPreset.empty()) {
    applyOahuDiagnosticPreset(state.oahuDiagnostics, oahuPreset);
  }

  if (hasArg(argc, argv, "--focus")) {
    state.focusMode = true;
  }

  if (hasArg(argc, argv, "--no-overlay")) {
    state.visualizations.showStatus = false;
    state.showStatusStrip = false;
  }
}

void cleanup(AppState& state) {
  if (state.imguiReady) {
    shutdownImGui(state);
  }

  if (state.bgfxReady) {
    shutdownVisualizationRenderer(state);
    bgfx::shutdown();
    state.bgfxReady = false;
  }

  if (state.window) {
    SDL_DestroyWindow(state.window);
    state.window = nullptr;
  }

  SDL_Quit();
}

int runApp(int argc, char** argv) {
  AppState state{};
  state.smokeTest = hasArg(argc, argv, "--smoke-test");
  state.screenshotSmoke = hasArg(argc, argv, "--screenshot-smoke");
  state.requestedRenderer = rendererFromArgs(argc, argv);
  state.visualizations.setActive(visualizationFromArgs(argc, argv));
  applyRuntimeArgs(state, argc, argv);
  if (state.smokeTest) {
    state.smokeLog.open("prappy_smoke.log", std::ios::out | std::ios::trunc);
    logSmoke(state, "smoke: start");
  }

  try {
    logSmoke(state, "smoke: SDL_SetMainReady");
    SDL_SetMainReady();

    logSmoke(state, "smoke: SDL_Init");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
      throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    logSmoke(state, "smoke: SDL_CreateWindow");
    state.window = SDL_CreateWindow(
      "prappy-native",
      state.width,
      state.height,
      SDL_WINDOW_RESIZABLE
    );

    if (!state.window) {
      throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    logSmoke(state, "smoke: initBgfx");
    initBgfx(state);
    logSmoke(state, "smoke: initVisualizationRenderer");
    initVisualizationRenderer(state);
    logSmoke(state, "smoke: initImGui");
    initImGui(state);
    logSmoke(state, "smoke: entering main loop");

    while (state.running) {
      logSmoke(state, "smoke: poll events");
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        handleEvent(state, event);
      }

      logSmoke(state, "smoke: frame setup");
      bgfx::setViewRect(
        kUiView,
        0,
        0,
        static_cast<std::uint16_t>(state.width),
        static_cast<std::uint16_t>(state.height)
      );

      bgfx::touch(kUiView);

      logSmoke(state, "smoke: imgui frame");
      beginImGuiFrame(state);
      drawAppUi(state);
      logSmoke(state, "smoke: render visualization");
      renderVisualization(state);
      ImGui::Render();
      logSmoke(state, "smoke: render imgui");
      renderImGui(state, ImGui::GetDrawData());

      if (state.screenshotSmoke && state.frameCount == 1) {
        requestScreenshot(state);
      }

      processScreenshotRequest(state);
      logSmoke(state, "smoke: bgfx frame");
      bgfx::frame();
      pollScreenshotStatus(state);

      ++state.frameCount;
      if (state.smokeTest && state.frameCount >= 3) {
        state.running = false;
      }
    }

    logSmoke(state, "smoke: cleanup");
    cleanup(state);
    logSmoke(state, "smoke: done");
    return 0;
  } catch (const std::exception& ex) {
    logSmoke(state, ex.what());
    fatalMessage("prappy-native fatal error", ex.what());
    cleanup(state);
    return 1;
  }
}

} // namespace prappy
