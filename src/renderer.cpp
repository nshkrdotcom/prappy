#include "renderer.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace prappy {

float randomFloat(std::mt19937& rng, float minValue, float maxValue) {
  std::uniform_real_distribution<float> distribution(minValue, maxValue);
  return distribution(rng);
}

std::uint32_t rgbaToAbgr(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
  return
    (static_cast<std::uint32_t>(a) << 24u) |
    (static_cast<std::uint32_t>(b) << 16u) |
    (static_cast<std::uint32_t>(g) << 8u) |
    static_cast<std::uint32_t>(r);
}

std::uint32_t rgbaFloatToAbgr(float r, float g, float b, float a) {
  const auto toByte = [](float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f);
  };

  return rgbaToAbgr(toByte(r), toByte(g), toByte(b), toByte(a));
}

std::uint32_t hsvToAbgr(float hue, float saturation, float value, float alpha) {
  const ImVec4 color = ImColor::HSV(hue, saturation, value, alpha).Value;
  return rgbaFloatToAbgr(color.x, color.y, color.z, color.w);
}

bool ShaderProgram::isValid() const {
  return bgfx::isValid(handle);
}

bgfx::ProgramHandle ShaderProgram::get() const {
  return handle;
}

const char* ShaderProgram::label() const {
  return programLabel.empty() ? "unloaded" : programLabel.c_str();
}

void ShaderProgram::loadGraphics(const char* label, const char* vsPath, const char* fsPath) {
  if (isValid()) {
    return;
  }

  programLabel = label ? label : "graphics";
  handle = loadProgram(vsPath, fsPath).handle;
}

void ShaderProgram::loadCompute(const char* label, const char* csPath) {
  if (isValid()) {
    return;
  }

  programLabel = label ? label : "compute";
  bgfx::ShaderHandle cs = loadShader(csPath);
  handle = bgfx::createProgram(cs, true);
}

void ShaderProgram::destroy() {
  if (bgfx::isValid(handle)) {
    bgfx::destroy(handle);
    handle = BGFX_INVALID_HANDLE;
  }

  programLabel.clear();
}

bool DynamicVertexBuffer::isValid() const {
  return bgfx::isValid(handle);
}

bgfx::DynamicVertexBufferHandle DynamicVertexBuffer::get() const {
  return handle;
}

std::uint32_t DynamicVertexBuffer::capacity() const {
  return vertexCapacity;
}

std::uint32_t DynamicVertexBuffer::lastUploadBytes() const {
  return uploadBytes;
}

std::uint16_t DynamicVertexBuffer::flags() const {
  return creationFlags;
}

std::uint32_t DynamicVertexBuffer::stride() const {
  return vertexStride;
}

void DynamicVertexBuffer::ensure(
  std::uint32_t requiredCapacity,
  std::uint32_t requiredStride,
  const bgfx::VertexLayout& layout,
  std::uint16_t bufferFlags
) {
  if (
    isValid() &&
    vertexCapacity >= requiredCapacity &&
    vertexStride == requiredStride &&
    creationFlags == bufferFlags
  ) {
    return;
  }

  destroy();
  vertexCapacity = requiredCapacity;
  vertexStride = requiredStride;
  creationFlags = bufferFlags;
  uploadBytes = 0;
  handle = bgfx::createDynamicVertexBuffer(vertexCapacity, layout, creationFlags);
}

void DynamicVertexBuffer::createWithData(
  const void* data,
  std::uint32_t vertexCount,
  std::uint32_t requiredStride,
  const bgfx::VertexLayout& layout,
  std::uint16_t bufferFlags
) {
  destroy();

  vertexCapacity = vertexCount;
  vertexStride = requiredStride;
  creationFlags = bufferFlags;
  uploadBytes = vertexCount * requiredStride;
  const bgfx::Memory* memory = bgfx::copy(data, uploadBytes);
  handle = bgfx::createDynamicVertexBuffer(memory, layout, creationFlags);
}

void DynamicVertexBuffer::update(
  const void* data,
  std::uint32_t vertexCount,
  std::uint32_t requiredStride,
  const bgfx::VertexLayout& layout,
  std::uint16_t bufferFlags
) {
  uploadBytes = vertexCount * requiredStride;
  if (vertexCount == 0) {
    return;
  }

  ensure(vertexCount, requiredStride, layout, bufferFlags);
  if (!isValid()) {
    return;
  }

  const bgfx::Memory* memory = bgfx::copy(data, uploadBytes);
  bgfx::update(handle, 0, memory);
}

void DynamicVertexBuffer::destroy() {
  if (bgfx::isValid(handle)) {
    bgfx::destroy(handle);
    handle = BGFX_INVALID_HANDLE;
  }

  vertexCapacity = 0;
  vertexStride = 0;
  uploadBytes = 0;
  creationFlags = 0;
}

void pushLine(
  std::vector<ColorVertex>& vertices,
  float ax,
  float ay,
  float az,
  float bx,
  float by,
  float bz,
  std::uint32_t color
) {
  vertices.push_back(ColorVertex{ax, ay, az, color});
  vertices.push_back(ColorVertex{bx, by, bz, color});
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

void submitColorVertices(
  const VisualizationRenderer& renderer,
  bgfx::ViewId viewId,
  const std::vector<ColorVertex>& vertices,
  ColorPrimitive primitive,
  bool depthTest,
  bool depthWrite
) {
  if (vertices.empty() || !bgfx::isValid(renderer.colorProgram.handle)) {
    return;
  }

  std::uint32_t vertexCount = static_cast<std::uint32_t>(vertices.size());
  const std::uint32_t available = bgfx::getAvailTransientVertexBuffer(
    vertexCount,
    renderer.colorLayout
  );

  vertexCount = std::min(vertexCount, available);
  if (primitive == ColorPrimitive::Lines) {
    vertexCount -= vertexCount % 2u;
  } else {
    vertexCount -= vertexCount % 3u;
  }

  if (vertexCount == 0) {
    return;
  }

  bgfx::TransientVertexBuffer vertexBuffer;
  bgfx::allocTransientVertexBuffer(&vertexBuffer, vertexCount, renderer.colorLayout);
  std::memcpy(vertexBuffer.data, vertices.data(), vertexCount * sizeof(ColorVertex));

  std::uint64_t state =
    BGFX_STATE_WRITE_RGB |
    BGFX_STATE_WRITE_A |
    BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);

  if (primitive == ColorPrimitive::Lines) {
    state |= BGFX_STATE_PT_LINES;
  }

  if (depthTest) {
    state |= BGFX_STATE_DEPTH_TEST_LESS;
  }

  if (depthWrite) {
    state |= BGFX_STATE_WRITE_Z;
  }

  bgfx::setVertexBuffer(0, &vertexBuffer, 0, vertexCount);
  bgfx::setState(state);
  bgfx::submit(viewId, renderer.colorProgram.handle);
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

} // namespace prappy
