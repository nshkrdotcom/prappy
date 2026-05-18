#define SDL_MAIN_HANDLED

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <bx/math.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <random>
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

namespace {

struct Texture {
  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
};

struct Program {
  bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
};

constexpr float kPi = 3.14159265358979323846f;

float randomFloat(std::mt19937& rng, float minValue, float maxValue) {
  std::uniform_real_distribution<float> distribution(minValue, maxValue);
  return distribution(rng);
}

ImVec2 addVec2(const ImVec2& a, const ImVec2& b) {
  return ImVec2(a.x + b.x, a.y + b.y);
}

ImVec2 subVec2(const ImVec2& a, const ImVec2& b) {
  return ImVec2(a.x - b.x, a.y - b.y);
}

ImVec2 scaleVec2(const ImVec2& value, float scale) {
  return ImVec2(value.x * scale, value.y * scale);
}

float distanceSquared(const ImVec2& a, const ImVec2& b) {
  const ImVec2 delta = subVec2(a, b);
  return delta.x * delta.x + delta.y * delta.y;
}

struct VisualizationContext {
  ImDrawList* drawList = nullptr;
  ImVec2 origin{};
  ImVec2 size{};
  float deltaSeconds = 1.0f / 60.0f;
  float elapsedSeconds = 0.0f;
};

enum class VisualizationId {
  RandomLines2D,
  Starfield3D
};

const char* visualizationName(VisualizationId id) {
  switch (id) {
    case VisualizationId::RandomLines2D:
      return "Random Lines 2D";
    case VisualizationId::Starfield3D:
      return "Infinite Starfield";
  }

  return "Unknown";
}

struct RandomLinesVisualization {
  struct Endpoint {
    ImVec2 position{};
    ImVec2 velocity{};
  };

  struct Segment {
    Endpoint a{};
    Endpoint b{};
    float hue = 0.0f;
    float thickness = 1.0f;
  };

  std::mt19937 rng{0x5eed1234u};
  std::vector<Segment> segments;
  ImVec2 lastSize{};

  Endpoint makeEndpoint(const ImVec2& size) {
    const float angle = randomFloat(rng, 0.0f, kPi * 2.0f);
    const float speed = randomFloat(rng, 70.0f, 260.0f);

    Endpoint endpoint;
    endpoint.position = ImVec2(
      randomFloat(rng, 0.0f, std::max(size.x, 1.0f)),
      randomFloat(rng, 0.0f, std::max(size.y, 1.0f))
    );
    endpoint.velocity = ImVec2(std::cos(angle) * speed, std::sin(angle) * speed);
    return endpoint;
  }

  void reset(const ImVec2& size) {
    segments.clear();
    segments.reserve(72);
    lastSize = size;

    for (int i = 0; i < 72; ++i) {
      Segment segment;
      segment.a = makeEndpoint(size);
      segment.b = makeEndpoint(size);
      segment.hue = randomFloat(rng, 0.0f, 1.0f);
      segment.thickness = randomFloat(rng, 1.0f, 2.6f);
      segments.push_back(segment);
    }
  }

  void updateEndpoint(Endpoint& endpoint, const ImVec2& size, float deltaSeconds) {
    endpoint.position = addVec2(endpoint.position, scaleVec2(endpoint.velocity, deltaSeconds));

    if (endpoint.position.x < 0.0f) {
      endpoint.position.x = 0.0f;
      endpoint.velocity.x = std::abs(endpoint.velocity.x);
    } else if (endpoint.position.x > size.x) {
      endpoint.position.x = size.x;
      endpoint.velocity.x = -std::abs(endpoint.velocity.x);
    }

    if (endpoint.position.y < 0.0f) {
      endpoint.position.y = 0.0f;
      endpoint.velocity.y = std::abs(endpoint.velocity.y);
    } else if (endpoint.position.y > size.y) {
      endpoint.position.y = size.y;
      endpoint.velocity.y = -std::abs(endpoint.velocity.y);
    }
  }

  void drawGrid(const VisualizationContext& context) {
    const ImU32 gridColor = IM_COL32(255, 255, 255, 18);
    constexpr float spacing = 48.0f;

    for (float x = 0.0f; x <= context.size.x; x += spacing) {
      const ImVec2 a = addVec2(context.origin, ImVec2(x, 0.0f));
      const ImVec2 b = addVec2(context.origin, ImVec2(x, context.size.y));
      context.drawList->AddLine(a, b, gridColor, 1.0f);
    }

    for (float y = 0.0f; y <= context.size.y; y += spacing) {
      const ImVec2 a = addVec2(context.origin, ImVec2(0.0f, y));
      const ImVec2 b = addVec2(context.origin, ImVec2(context.size.x, y));
      context.drawList->AddLine(a, b, gridColor, 1.0f);
    }
  }

  void draw(VisualizationContext& context) {
    if (
      segments.empty() ||
      distanceSquared(context.size, lastSize) > 48.0f * 48.0f
    ) {
      reset(context.size);
    }

    const ImVec2 max = addVec2(context.origin, context.size);
    context.drawList->AddRectFilled(context.origin, max, IM_COL32(7, 9, 15, 255));
    drawGrid(context);

    for (Segment& segment : segments) {
      updateEndpoint(segment.a, context.size, context.deltaSeconds);
      updateEndpoint(segment.b, context.size, context.deltaSeconds);

      const float hue = std::fmod(segment.hue + context.elapsedSeconds * 0.035f, 1.0f);
      const ImU32 color = ImColor::HSV(hue, 0.78f, 0.95f, 0.78f);
      const ImVec2 a = addVec2(context.origin, segment.a.position);
      const ImVec2 b = addVec2(context.origin, segment.b.position);

      context.drawList->AddLine(a, b, color, segment.thickness);
      context.drawList->AddCircleFilled(a, segment.thickness + 1.2f, color);
      context.drawList->AddCircleFilled(b, segment.thickness + 1.2f, color);
    }
  }
};

struct StarfieldVisualization {
  struct Star {
    float x = 0.0f;
    float y = 0.0f;
    float z = 1.0f;
    float speed = 1.0f;
    float radius = 1.0f;
    float tint = 0.0f;
  };

  std::mt19937 rng{0xc0ffee42u};
  std::vector<Star> stars;
  ImVec2 lastSize{};

  void resetStar(Star& star, const ImVec2& size, bool spreadDepth) {
    const float aspect = std::max(size.x / std::max(size.y, 1.0f), 1.0f);

    star.x = randomFloat(rng, -aspect * 2.0f, aspect * 2.0f);
    star.y = randomFloat(rng, -2.0f, 2.0f);
    star.z = spreadDepth ? randomFloat(rng, 0.35f, 18.0f) : randomFloat(rng, 14.0f, 18.0f);
    star.speed = randomFloat(rng, 2.2f, 8.0f);
    star.radius = randomFloat(rng, 0.7f, 2.2f);
    star.tint = randomFloat(rng, 0.0f, 1.0f);
  }

  void reset(const ImVec2& size) {
    stars.clear();
    stars.resize(900);
    lastSize = size;

    for (Star& star : stars) {
      resetStar(star, size, true);
    }
  }

  bool project(const Star& star, float z, const VisualizationContext& context, ImVec2& out) const {
    if (z <= 0.05f) {
      return false;
    }

    const float focalLength = std::min(context.size.x, context.size.y) * 0.52f;
    out.x = context.origin.x + context.size.x * 0.5f + (star.x / z) * focalLength;
    out.y = context.origin.y + context.size.y * 0.5f + (star.y / z) * focalLength;
    return true;
  }

  void draw(VisualizationContext& context) {
    if (
      stars.empty() ||
      distanceSquared(context.size, lastSize) > 64.0f * 64.0f
    ) {
      reset(context.size);
    }

    const ImVec2 max = addVec2(context.origin, context.size);
    context.drawList->AddRectFilled(context.origin, max, IM_COL32(2, 4, 10, 255));

    const ImVec2 center = addVec2(context.origin, scaleVec2(context.size, 0.5f));
    context.drawList->AddCircle(center, std::min(context.size.x, context.size.y) * 0.08f, IM_COL32(80, 120, 255, 26), 48, 1.0f);
    context.drawList->AddCircle(center, std::min(context.size.x, context.size.y) * 0.18f, IM_COL32(80, 120, 255, 18), 64, 1.0f);

    for (Star& star : stars) {
      const float previousZ = star.z;
      star.z -= star.speed * context.deltaSeconds;

      if (star.z <= 0.18f) {
        resetStar(star, context.size, false);
        continue;
      }

      ImVec2 current{};
      ImVec2 previous{};
      if (!project(star, star.z, context, current) || !project(star, previousZ, context, previous)) {
        resetStar(star, context.size, false);
        continue;
      }

      const float margin = 80.0f;
      if (
        current.x < context.origin.x - margin ||
        current.x > max.x + margin ||
        current.y < context.origin.y - margin ||
        current.y > max.y + margin
      ) {
        resetStar(star, context.size, false);
        continue;
      }

      const float depthAlpha = std::clamp(1.0f - star.z / 18.0f, 0.08f, 1.0f);
      const float warm = star.tint > 0.82f ? 1.0f : 0.82f;
      const float cool = star.tint < 0.18f ? 1.0f : 0.86f;
      const ImU32 color = ImGui::ColorConvertFloat4ToU32(ImVec4(
        std::clamp(warm * (0.65f + depthAlpha * 0.45f), 0.0f, 1.0f),
        std::clamp(0.72f + depthAlpha * 0.28f, 0.0f, 1.0f),
        std::clamp(cool, 0.0f, 1.0f),
        std::clamp(depthAlpha, 0.0f, 1.0f)
      ));

      const float streak = std::clamp((18.0f - star.z) * 0.08f, 0.4f, 4.5f);
      const ImVec2 direction = subVec2(current, previous);
      const ImVec2 tail = subVec2(current, scaleVec2(direction, streak));
      const float radius = star.radius * (0.4f + depthAlpha * 1.4f);

      context.drawList->AddLine(tail, current, color, std::max(radius * 0.55f, 1.0f));
      context.drawList->AddCircleFilled(current, radius, color);
    }
  }
};

struct VisualizationHost {
  VisualizationId active = VisualizationId::RandomLines2D;
  bool showStatus = true;
  bool resetRequested = true;
  RandomLinesVisualization randomLines;
  StarfieldVisualization starfield;

  void setActive(VisualizationId next) {
    if (active != next) {
      active = next;
      resetRequested = true;
    }
  }

  void resetActive(const ImVec2& size) {
    switch (active) {
      case VisualizationId::RandomLines2D:
        randomLines.reset(size);
        break;
      case VisualizationId::Starfield3D:
        starfield.reset(size);
        break;
    }
    resetRequested = false;
  }

  void draw(VisualizationContext& context) {
    if (resetRequested) {
      resetActive(context.size);
    }

    switch (active) {
      case VisualizationId::RandomLines2D:
        randomLines.draw(context);
        break;
      case VisualizationId::Starfield3D:
        starfield.draw(context);
        break;
    }
  }
};

struct AppState {
  SDL_Window* window = nullptr;
  int width = 1280;
  int height = 720;
  bool running = true;
  bool smokeTest = false;
  int frameCount = 0;
  float deltaSeconds = 1.0f / 60.0f;
  float elapsedSeconds = 0.0f;
  bool bgfxReady = false;
  bool imguiReady = false;
  std::ofstream smokeLog;
  std::chrono::steady_clock::time_point lastFrameTime{};
  VisualizationHost visualizations;

  bgfx::VertexLayout imguiLayout;
  Program imguiProgram;
  Texture fontTexture;
  bgfx::UniformHandle s_tex = BGFX_INVALID_HANDLE;
};

void logSmoke(AppState& state, const char* message) {
  if (state.smokeLog.is_open()) {
    state.smokeLog << message << '\n';
    state.smokeLog.flush();
  }
}

std::vector<std::uint8_t> readFile(const char* path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error(std::string("Failed to open file: ") + path);
  }

  const auto size = file.tellg();
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));

  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(data.data()), size);

  return data;
}

bgfx::ShaderHandle loadShader(const char* path) {
  auto data = readFile(path);
  const bgfx::Memory* mem = bgfx::copy(data.data(), static_cast<std::uint32_t>(data.size()));
  return bgfx::createShader(mem);
}

Program loadProgram(const char* vsPath, const char* fsPath) {
  bgfx::ShaderHandle vs = loadShader(vsPath);
  bgfx::ShaderHandle fs = loadShader(fsPath);

  Program program;
  program.handle = bgfx::createProgram(vs, fs, true);
  return program;
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
  init.type = bgfx::RendererType::Count;
  init.resolution.width = static_cast<std::uint32_t>(state.width);
  init.resolution.height = static_cast<std::uint32_t>(state.height);
  init.resolution.reset = BGFX_RESET_VSYNC;
  init.platformData = pd;

  if (!bgfx::init(init)) {
    throw std::runtime_error("bgfx::init failed");
  }

  state.bgfxReady = true;

  bgfx::setDebug(BGFX_DEBUG_TEXT);
  bgfx::setViewClear(
    0,
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
  ImGui::StyleColorsDark();

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
  bgfx::setViewTransform(0, view, ortho);

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

        bgfx::submit(0, state.imguiProgram.handle);
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

void drawVisualizationStatus(AppState& state, const ImVec2& canvasOrigin) {
  if (!state.visualizations.showStatus) {
    return;
  }

  ImGui::SetCursorScreenPos(addVec2(canvasOrigin, ImVec2(16.0f, 16.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(8, 10, 18, 218));
  ImGui::BeginChild(
    "VisualizationStatus",
    ImVec2(310.0f, 104.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );

  ImGui::TextUnformatted(visualizationName(state.visualizations.active));
  ImGui::Separator();
  ImGui::Text("Frame %.2f ms", state.deltaSeconds * 1000.0f);
  ImGui::Text("Canvas %d x %d", state.width, state.height);

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
    ImGuiWindowFlags_MenuBar;

  ImGui::Begin("PrappyVisualizationHost", nullptr, windowFlags);

  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Reset Visualization")) {
        state.visualizations.resetRequested = true;
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

      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Status Overlay", nullptr, &state.visualizations.showStatus);
      ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::TextUnformatted(visualizationName(state.visualizations.active));
    ImGui::EndMenuBar();
  }

  const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
  ImVec2 canvasSize = ImGui::GetContentRegionAvail();
  canvasSize.x = std::max(canvasSize.x, 1.0f);
  canvasSize.y = std::max(canvasSize.y, 1.0f);

  ImGui::InvisibleButton("VisualizationCanvas", canvasSize);

  VisualizationContext context;
  context.drawList = ImGui::GetWindowDrawList();
  context.origin = canvasOrigin;
  context.size = canvasSize;
  context.deltaSeconds = state.deltaSeconds;
  context.elapsedSeconds = state.elapsedSeconds;

  state.visualizations.draw(context);
  drawVisualizationStatus(state, canvasOrigin);

  ImGui::End();
  ImGui::PopStyleVar(3);
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
  }

  return VisualizationId::RandomLines2D;
}

void cleanup(AppState& state) {
  if (state.imguiReady) {
    shutdownImGui(state);
  }

  if (state.bgfxReady) {
    bgfx::shutdown();
    state.bgfxReady = false;
  }

  if (state.window) {
    SDL_DestroyWindow(state.window);
    state.window = nullptr;
  }

  SDL_Quit();
}

} // namespace

int main(int argc, char** argv) {
  AppState state{};
  state.smokeTest = hasArg(argc, argv, "--smoke-test");
  state.visualizations.setActive(visualizationFromArgs(argc, argv));
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
        0,
        0,
        0,
        static_cast<std::uint16_t>(state.width),
        static_cast<std::uint16_t>(state.height)
      );

      bgfx::touch(0);

      logSmoke(state, "smoke: imgui frame");
      beginImGuiFrame(state);
      drawAppUi(state);
      ImGui::Render();
      logSmoke(state, "smoke: render imgui");
      renderImGui(state, ImGui::GetDrawData());

      logSmoke(state, "smoke: bgfx frame");
      bgfx::frame();

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
