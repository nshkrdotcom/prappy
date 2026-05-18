#pragma once

#include <bgfx/bgfx.h>
#include <imgui.h>

#include <cstdint>
#include <random>
#include <vector>

namespace prappy {

struct Texture {
  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
};

struct Program {
  bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
};

constexpr bgfx::ViewId kFrameClearView = 0;
constexpr bgfx::ViewId kVisualizationView = 1;
constexpr bgfx::ViewId kUiView = 2;
constexpr float kPi = 3.14159265358979323846f;

struct ColorVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  std::uint32_t abgr = 0xffffffffu;
};

struct VisualizationRenderer {
  bgfx::VertexLayout colorLayout;
  Program colorProgram;
};

enum class ColorPrimitive {
  Lines,
  Triangles
};

float randomFloat(std::mt19937& rng, float minValue, float maxValue);
std::uint32_t rgbaToAbgr(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);
std::uint32_t rgbaFloatToAbgr(float r, float g, float b, float a);
std::uint32_t hsvToAbgr(float hue, float saturation, float value, float alpha);

void pushLine(
  std::vector<ColorVertex>& vertices,
  float ax,
  float ay,
  float az,
  float bx,
  float by,
  float bz,
  std::uint32_t color
);

ImVec2 addVec2(const ImVec2& a, const ImVec2& b);
ImVec2 subVec2(const ImVec2& a, const ImVec2& b);
ImVec2 scaleVec2(const ImVec2& value, float scale);
float distanceSquared(const ImVec2& a, const ImVec2& b);

void submitColorVertices(
  const VisualizationRenderer& renderer,
  bgfx::ViewId viewId,
  const std::vector<ColorVertex>& vertices,
  ColorPrimitive primitive,
  bool depthTest = false,
  bool depthWrite = false
);

std::vector<std::uint8_t> readFile(const char* path);
bgfx::ShaderHandle loadShader(const char* path);
Program loadProgram(const char* vsPath, const char* fsPath);

} // namespace prappy
