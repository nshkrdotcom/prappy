#pragma once

#include <bgfx/bgfx.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace prappy {

struct Texture {
  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
};

struct Program {
  bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
};

class ShaderProgram {
public:
  ShaderProgram() = default;
  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;

  bool isValid() const;
  bgfx::ProgramHandle get() const;
  const char* label() const;

  void loadGraphics(const char* label, const char* vsPath, const char* fsPath);
  void loadCompute(const char* label, const char* csPath);
  void destroy();

private:
  bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
  std::string programLabel;
};

class DynamicVertexBuffer {
public:
  DynamicVertexBuffer() = default;
  DynamicVertexBuffer(const DynamicVertexBuffer&) = delete;
  DynamicVertexBuffer& operator=(const DynamicVertexBuffer&) = delete;

  bool isValid() const;
  bgfx::DynamicVertexBufferHandle get() const;
  std::uint32_t capacity() const;
  std::uint32_t lastUploadBytes() const;
  std::uint16_t flags() const;
  std::uint32_t stride() const;

  void ensure(
    std::uint32_t vertexCapacity,
    std::uint32_t vertexStride,
    const bgfx::VertexLayout& layout,
    std::uint16_t bufferFlags
  );

  void createWithData(
    const void* data,
    std::uint32_t vertexCount,
    std::uint32_t vertexStride,
    const bgfx::VertexLayout& layout,
    std::uint16_t bufferFlags
  );

  void update(
    const void* data,
    std::uint32_t vertexCount,
    std::uint32_t vertexStride,
    const bgfx::VertexLayout& layout,
    std::uint16_t bufferFlags
  );

  void destroy();

private:
  bgfx::DynamicVertexBufferHandle handle = BGFX_INVALID_HANDLE;
  std::uint32_t vertexCapacity = 0;
  std::uint32_t vertexStride = 0;
  std::uint32_t uploadBytes = 0;
  std::uint16_t creationFlags = 0;
};

struct RenderPassDiagnostics {
  const char* passName = nullptr;
  const char* shaderName = nullptr;
  const char* resourceKind = nullptr;
  const char* backendName = nullptr;
  const char* note = nullptr;
  std::uint32_t drawCalls = 0;
  std::uint32_t dispatches = 0;
  std::uint32_t vertices = 0;
  std::uint32_t indices = 0;
  std::uint32_t uploadedBytes = 0;
  std::uint32_t bufferCapacity = 0;
  bool computeSupported = false;
  bool computeActive = false;
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
