#pragma once

#include "platform/screenshot.h"
#include "renderer.h"
#include "visualization_core.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <filesystem>
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
  bool presentationMode = false;
  bool showStackPanel = true;
  bool showInspectorPanel = true;
  bool showStatusStrip = true;
  bool screenshotRequested = false;
  int frameCount = 0;
  int screenshotFrame = 1;
  int exitFrame = 3;
  float deltaSeconds = 1.0f / 60.0f;
  float fixedDeltaSeconds = 0.0f;
  float elapsedSeconds = 0.0f;
  float screenshotStatusSeconds = 0.0f;
  std::string screenshotStatus;
  std::filesystem::path screenshotOutputPath;
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
  OahuDiagnosticSettings oahuDiagnostics;
  OahuRenderSettings oahuRenderSettings;

  bgfx::VertexLayout imguiLayout;
  Program imguiProgram;
  Texture fontTexture;
  bgfx::UniformHandle s_tex = BGFX_INVALID_HANDLE;
};

int runApp(int argc, char** argv);

} // namespace prappy
