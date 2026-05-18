#pragma once

#include "platform/screenshot.h"
#include "renderer.h"
#include "visualization_core.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <fstream>
#include <string>

namespace prappy {

struct AppState {
  SDL_Window* window = nullptr;
  int width = 1280;
  int height = 720;
  bool running = true;
  bool smokeTest = false;
  bool screenshotSmoke = false;
  bool focusMode = false;
  bool showStackPanel = true;
  bool showInspectorPanel = true;
  bool showStatusStrip = true;
  bool screenshotRequested = false;
  int frameCount = 0;
  float deltaSeconds = 1.0f / 60.0f;
  float elapsedSeconds = 0.0f;
  float screenshotStatusSeconds = 0.0f;
  std::string screenshotStatus;
  bool bgfxReady = false;
  bool imguiReady = false;
  std::ofstream smokeLog;
  std::chrono::steady_clock::time_point lastFrameTime{};
  ImVec2 visualizationCanvasOrigin{};
  ImVec2 visualizationCanvasSize{1280.0f, 720.0f};
  bgfx::RendererType::Enum requestedRenderer = bgfx::RendererType::Count;
  PrappyBgfxCallback bgfxCallback;
  VisualizationRenderer visualizationRenderer;
  VisualizationHost visualizations;

  bgfx::VertexLayout imguiLayout;
  Program imguiProgram;
  Texture fontTexture;
  bgfx::UniformHandle s_tex = BGFX_INVALID_HANDLE;
};

int runApp(int argc, char** argv);

} // namespace prappy
