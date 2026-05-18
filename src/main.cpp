#define SDL_MAIN_HANDLED

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <bx/math.h>

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
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

struct AppState {
  SDL_Window* window = nullptr;
  int width = 1280;
  int height = 720;
  bool running = true;
  bool smokeTest = false;
  int frameCount = 0;
  bool bgfxReady = false;
  bool imguiReady = false;
  std::ofstream smokeLog;

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
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(state.width), static_cast<float>(state.height));
  io.DeltaTime = 1.0f / 60.0f;

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

void drawAppUi() {
  ImGui::Begin("Prappy Native");

  ImGui::TextUnformatted("C++20 + SDL3 + bgfx + Dear ImGui");
  ImGui::Separator();

  static float value = 0.5f;
  ImGui::SliderFloat("value", &value, 0.0f, 1.0f);

  if (ImGui::Button("Do Thing")) {
    std::printf("Do Thing clicked\n");
  }

  ImGui::TextUnformatted("Frame UI is immediate-mode.");
  ImGui::End();

  ImGui::ShowDemoWindow();
}

bool hasArg(int argc, char** argv, const char* expected) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == expected) {
      return true;
    }
  }

  return false;
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
      drawAppUi();
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
