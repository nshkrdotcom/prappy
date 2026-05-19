#include "../visualization_core.h"
#include "../oahu_topology.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace prappy {
namespace {

struct OahuTerrainVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float nx = 0.0f;
  float ny = 1.0f;
  float nz = 0.0f;
  std::uint32_t abgr = 0xffffffffu;
  float elevation01 = 0.0f;
  float land = 0.0f;
};

struct OahuOceanVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float u = 0.0f;
  float v = 0.0f;
  float fade = 1.0f;
};

bx::Vec3 addVec3(const bx::Vec3& a, const bx::Vec3& b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

bx::Vec3 subVec3(const bx::Vec3& a, const bx::Vec3& b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

bx::Vec3 crossVec3(const bx::Vec3& a, const bx::Vec3& b) {
  return {
    a.y * b.z - a.z * b.y,
    a.z * b.x - a.x * b.z,
    a.x * b.y - a.y * b.x
  };
}

bx::Vec3 normalizeVec3(const bx::Vec3& value) {
  const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
  if (length <= 1.0e-5f) {
    return {0.0f, 1.0f, 0.0f};
  }

  const float scale = 1.0f / length;
  return {value.x * scale, value.y * scale, value.z * scale};
}

bx::Vec3 terrainFaceNormal(const bx::Vec3& a, const bx::Vec3& b, const bx::Vec3& c) {
  bx::Vec3 normal = crossVec3(subVec3(b, a), subVec3(c, a));
  if (normal.y < 0.0f) {
    normal = {-normal.x, -normal.y, -normal.z};
  }
  return normalizeVec3(normal);
}

float clampRampBreakpoint(float value, float minimum, float maximum) {
  return std::clamp(value, minimum, maximum);
}

void setUniform(bgfx::UniformHandle handle, const ImVec4& value) {
  const float data[4] = {value.x, value.y, value.z, value.w};
  bgfx::setUniform(handle, data);
}

struct OahuFlyoverVisualization final : IVisualizationModule {
  ImVec2 lastSize{};

  bgfx::VertexLayout terrainLayout;
  ShaderProgram terrainProgram;
  bgfx::VertexBufferHandle terrainVertexBuffer = BGFX_INVALID_HANDLE;
  bgfx::IndexBufferHandle terrainIndexBuffer = BGFX_INVALID_HANDLE;
  std::uint32_t terrainVertexCount = 0;
  std::uint32_t terrainIndexCount = 0;

  bgfx::VertexLayout oceanLayout;
  ShaderProgram oceanProgram;
  bgfx::VertexBufferHandle oceanVertexBuffer = BGFX_INVALID_HANDLE;
  bgfx::IndexBufferHandle oceanIndexBuffer = BGFX_INVALID_HANDLE;
  std::uint32_t oceanVertexCount = 0;
  std::uint32_t oceanIndexCount = 0;

  bgfx::UniformHandle u_oahuLight = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuMaterial = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuFog = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuSky = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuRampBreaks = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuRamp0 = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuRamp1 = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuRamp2 = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuRamp3 = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuWaterNear = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuWaterFar = BGFX_INVALID_HANDLE;
  bgfx::UniformHandle u_oahuOcean = BGFX_INVALID_HANDLE;

  RenderPassDiagnostics terrainDiagnostics;
  RenderPassDiagnostics oceanDiagnostics;

  const VisualizationDescriptor& descriptor() const override {
    return visualizationDescriptor(VisualizationId::OahuFlyover);
  }

  const OahuTerrainSample& sampleAt(int col, int row) const {
    return kOahuTerrain[static_cast<std::size_t>(row * kOahuGridWidth + col)];
  }

  bx::Vec3 terrainPosition(float xValue, float yValue, float elevationMeters) const {
    constexpr float zSpan = 8.2f;
    constexpr float xSpan = zSpan * kOahuMapAspect;
    // World axes: X east, Y elevation, Z north.
    const float x = (xValue - 0.5f) * xSpan;
    const float z = (yValue - 0.5f) * zSpan;
    const float y = std::max(elevationMeters, 0.0f) / kOahuMaxElevationMeters * 1.85f;
    return {x, y, z};
  }

  bx::Vec3 terrainPosition(const OahuTerrainSample& sample) const {
    return terrainPosition(sample.x, sample.y, sample.elevationMeters);
  }

  std::uint32_t terrainColor(float elevationMeters) const {
    const float t = std::clamp(elevationMeters / kOahuMaxElevationMeters, 0.0f, 1.0f);
    if (t < 0.08f) {
      return rgbaFloatToAbgr(0.72f, 0.67f, 0.45f, 1.0f);
    }
    if (t < 0.38f) {
      const float k = t / 0.38f;
      return rgbaFloatToAbgr(0.16f + k * 0.17f, 0.48f + k * 0.25f, 0.21f + k * 0.08f, 1.0f);
    }
    if (t < 0.72f) {
      const float k = (t - 0.38f) / 0.34f;
      return rgbaFloatToAbgr(0.33f + k * 0.21f, 0.54f + k * 0.10f, 0.29f + k * 0.05f, 1.0f);
    }

    const float k = (t - 0.72f) / 0.28f;
    return rgbaFloatToAbgr(0.54f + k * 0.22f, 0.51f + k * 0.18f, 0.42f + k * 0.20f, 1.0f);
  }

  bool triangleTouchesLand(
    const OahuTerrainSample& a,
    const OahuTerrainSample& b,
    const OahuTerrainSample& c
  ) const {
    return static_cast<int>(a.land) + static_cast<int>(b.land) + static_cast<int>(c.land) >= 2;
  }

  void addTerrainTriangleNormal(
    std::vector<bx::Vec3>& normals,
    const std::vector<bx::Vec3>& positions,
    std::uint16_t ia,
    std::uint16_t ib,
    std::uint16_t ic
  ) const {
    const bx::Vec3 normal = terrainFaceNormal(positions[ia], positions[ib], positions[ic]);
    normals[ia] = addVec3(normals[ia], normal);
    normals[ib] = addVec3(normals[ib], normal);
    normals[ic] = addVec3(normals[ic], normal);
  }

  void appendTerrainTriangle(
    std::vector<std::uint16_t>& indices,
    std::vector<bx::Vec3>& normals,
    const std::vector<bx::Vec3>& positions,
    std::uint16_t ia,
    std::uint16_t ib,
    std::uint16_t ic
  ) const {
    indices.push_back(ia);
    indices.push_back(ib);
    indices.push_back(ic);
    addTerrainTriangleNormal(normals, positions, ia, ib, ic);
  }

  void ensureUniforms() {
    if (!bgfx::isValid(u_oahuLight)) {
      u_oahuLight = bgfx::createUniform("u_oahuLight", bgfx::UniformType::Vec4);
      u_oahuMaterial = bgfx::createUniform("u_oahuMaterial", bgfx::UniformType::Vec4);
      u_oahuFog = bgfx::createUniform("u_oahuFog", bgfx::UniformType::Vec4);
      u_oahuSky = bgfx::createUniform("u_oahuSky", bgfx::UniformType::Vec4);
      u_oahuRampBreaks = bgfx::createUniform("u_oahuRampBreaks", bgfx::UniformType::Vec4);
      u_oahuRamp0 = bgfx::createUniform("u_oahuRamp0", bgfx::UniformType::Vec4);
      u_oahuRamp1 = bgfx::createUniform("u_oahuRamp1", bgfx::UniformType::Vec4);
      u_oahuRamp2 = bgfx::createUniform("u_oahuRamp2", bgfx::UniformType::Vec4);
      u_oahuRamp3 = bgfx::createUniform("u_oahuRamp3", bgfx::UniformType::Vec4);
      u_oahuWaterNear = bgfx::createUniform("u_oahuWaterNear", bgfx::UniformType::Vec4);
      u_oahuWaterFar = bgfx::createUniform("u_oahuWaterFar", bgfx::UniformType::Vec4);
      u_oahuOcean = bgfx::createUniform("u_oahuOcean", bgfx::UniformType::Vec4);
    }
  }

  void destroyUniforms() {
    const bgfx::UniformHandle handles[] = {
      u_oahuLight,
      u_oahuMaterial,
      u_oahuFog,
      u_oahuSky,
      u_oahuRampBreaks,
      u_oahuRamp0,
      u_oahuRamp1,
      u_oahuRamp2,
      u_oahuRamp3,
      u_oahuWaterNear,
      u_oahuWaterFar,
      u_oahuOcean
    };

    for (bgfx::UniformHandle handle : handles) {
      if (bgfx::isValid(handle)) {
        bgfx::destroy(handle);
      }
    }

    u_oahuLight = BGFX_INVALID_HANDLE;
    u_oahuMaterial = BGFX_INVALID_HANDLE;
    u_oahuFog = BGFX_INVALID_HANDLE;
    u_oahuSky = BGFX_INVALID_HANDLE;
    u_oahuRampBreaks = BGFX_INVALID_HANDLE;
    u_oahuRamp0 = BGFX_INVALID_HANDLE;
    u_oahuRamp1 = BGFX_INVALID_HANDLE;
    u_oahuRamp2 = BGFX_INVALID_HANDLE;
    u_oahuRamp3 = BGFX_INVALID_HANDLE;
    u_oahuWaterNear = BGFX_INVALID_HANDLE;
    u_oahuWaterFar = BGFX_INVALID_HANDLE;
    u_oahuOcean = BGFX_INVALID_HANDLE;
  }

  void setSharedUniforms(const OahuRenderSettings& settings, bool topDown, float elapsedSeconds) {
    ensureUniforms();

    const OahuLightingSettings& lighting = settings.lighting;
    const float cosPitch = std::cos(lighting.sunPitch);
    const bx::Vec3 sun = normalizeVec3({
      std::sin(lighting.sunYaw) * cosPitch,
      std::sin(lighting.sunPitch),
      std::cos(lighting.sunYaw) * cosPitch
    });

    const float lightData[4] = {sun.x, sun.y, sun.z, lighting.ambient};
    const float materialData[4] = {
      lighting.diffuse,
      lighting.contrast,
      topDown ? 0.0f : settings.environment.hazeDensity,
      0.0f
    };
    const float fogData[4] = {
      settings.environment.hazeStart,
      settings.environment.hazeEnd,
      0.0f,
      0.0f
    };
    const float rampData[4] = {
      clampRampBreakpoint(settings.colorRamp.lowlandStart, 0.01f, 0.30f),
      clampRampBreakpoint(settings.colorRamp.ridgeStart, 0.12f, 0.70f),
      clampRampBreakpoint(settings.colorRamp.peakStart, 0.32f, 0.96f),
      1.0f
    };
    const float oceanData[4] = {
      elapsedSeconds,
      settings.environment.waveStrength,
      topDown ? 0.0f : 1.0f,
      settings.environment.hazeDensity
    };

    bgfx::setUniform(u_oahuLight, lightData);
    bgfx::setUniform(u_oahuMaterial, materialData);
    bgfx::setUniform(u_oahuFog, fogData);
    bgfx::setUniform(u_oahuRampBreaks, rampData);
    bgfx::setUniform(u_oahuOcean, oceanData);
    setUniform(u_oahuSky, settings.environment.horizon);
    setUniform(u_oahuRamp0, settings.colorRamp.beach);
    setUniform(u_oahuRamp1, settings.colorRamp.lowland);
    setUniform(u_oahuRamp2, settings.colorRamp.ridge);
    setUniform(u_oahuRamp3, settings.colorRamp.peak);
    setUniform(u_oahuWaterNear, settings.environment.waterNear);
    setUniform(u_oahuWaterFar, settings.environment.waterFar);
  }

  void ensureTerrainMesh() {
    if (
      bgfx::isValid(terrainVertexBuffer) &&
      bgfx::isValid(terrainIndexBuffer) &&
      terrainProgram.isValid()
    ) {
      return;
    }

    destroyTerrainResources();

    terrainLayout
      .begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .end();

    terrainProgram.loadGraphics(
      "oahu_terrain_vs / oahu_terrain_fs",
      "shaders/oahu_terrain_vs.bin",
      "shaders/oahu_terrain_fs.bin"
    );

    constexpr std::uint32_t vertexCount = kOahuGridWidth * kOahuGridHeight;
    static_assert(vertexCount <= 0xffffu, "Oahu terrain uses 16-bit indices");

    std::vector<OahuTerrainVertex> vertices(vertexCount);
    std::vector<bx::Vec3> positions(vertexCount, bx::Vec3{0.0f, 0.0f, 0.0f});
    std::vector<bx::Vec3> normals(vertexCount, bx::Vec3{0.0f, 0.0f, 0.0f});
    std::vector<std::uint16_t> indices;
    indices.reserve((kOahuGridWidth - 1) * (kOahuGridHeight - 1) * 6);

    for (int row = 0; row < kOahuGridHeight; ++row) {
      for (int col = 0; col < kOahuGridWidth; ++col) {
        const std::uint32_t index = static_cast<std::uint32_t>(row * kOahuGridWidth + col);
        const OahuTerrainSample& sample = sampleAt(col, row);
        const bx::Vec3 position = terrainPosition(sample);
        positions[index] = position;
        vertices[index].x = position.x;
        vertices[index].y = position.y;
        vertices[index].z = position.z;
        vertices[index].abgr = terrainColor(sample.elevationMeters);
        vertices[index].elevation01 = std::clamp(sample.elevationMeters / kOahuMaxElevationMeters, 0.0f, 1.0f);
        vertices[index].land = sample.land ? 1.0f : 0.0f;
      }
    }

    for (int row = 0; row < kOahuGridHeight - 1; ++row) {
      for (int col = 0; col < kOahuGridWidth - 1; ++col) {
        const OahuTerrainSample& a = sampleAt(col, row);
        const OahuTerrainSample& b = sampleAt(col + 1, row);
        const OahuTerrainSample& c = sampleAt(col, row + 1);
        const OahuTerrainSample& d = sampleAt(col + 1, row + 1);
        const std::uint16_t ia = static_cast<std::uint16_t>(row * kOahuGridWidth + col);
        const std::uint16_t ib = static_cast<std::uint16_t>(row * kOahuGridWidth + col + 1);
        const std::uint16_t ic = static_cast<std::uint16_t>((row + 1) * kOahuGridWidth + col);
        const std::uint16_t id = static_cast<std::uint16_t>((row + 1) * kOahuGridWidth + col + 1);

        if (triangleTouchesLand(a, b, c)) {
          appendTerrainTriangle(indices, normals, positions, ia, ib, ic);
        }
        if (triangleTouchesLand(b, d, c)) {
          appendTerrainTriangle(indices, normals, positions, ib, id, ic);
        }
      }
    }

    for (std::uint32_t index = 0; index < vertexCount; ++index) {
      const bx::Vec3 normal = normalizeVec3(normals[index]);
      vertices[index].nx = normal.x;
      vertices[index].ny = normal.y;
      vertices[index].nz = normal.z;
    }

    terrainVertexCount = static_cast<std::uint32_t>(vertices.size());
    terrainIndexCount = static_cast<std::uint32_t>(indices.size());

    const bgfx::Memory* vertexMemory = bgfx::copy(
      vertices.data(),
      static_cast<std::uint32_t>(vertices.size() * sizeof(OahuTerrainVertex))
    );
    terrainVertexBuffer = bgfx::createVertexBuffer(vertexMemory, terrainLayout);

    const bgfx::Memory* indexMemory = bgfx::copy(
      indices.data(),
      static_cast<std::uint32_t>(indices.size() * sizeof(std::uint16_t))
    );
    terrainIndexBuffer = bgfx::createIndexBuffer(indexMemory);
  }

  void ensureOceanMesh() {
    if (
      bgfx::isValid(oceanVertexBuffer) &&
      bgfx::isValid(oceanIndexBuffer) &&
      oceanProgram.isValid()
    ) {
      return;
    }

    destroyOceanResources();

    oceanLayout
      .begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::TexCoord0, 3, bgfx::AttribType::Float)
      .end();

    oceanProgram.loadGraphics(
      "oahu_ocean_vs / oahu_ocean_fs",
      "shaders/oahu_ocean_vs.bin",
      "shaders/oahu_ocean_fs.bin"
    );

    constexpr int columns = 41;
    constexpr int rows = 41;
    constexpr float minX = -38.0f;
    constexpr float maxX = 38.0f;
    constexpr float minZ = -28.0f;
    constexpr float maxZ = 48.0f;
    constexpr float y = -0.045f;

    std::vector<OahuOceanVertex> vertices;
    vertices.reserve(columns * rows);
    for (int row = 0; row < rows; ++row) {
      const float v = static_cast<float>(row) / static_cast<float>(rows - 1);
      const float z = minZ + (maxZ - minZ) * v;
      for (int col = 0; col < columns; ++col) {
        const float u = static_cast<float>(col) / static_cast<float>(columns - 1);
        const float x = minX + (maxX - minX) * u;
        const float edgeX = std::min(u, 1.0f - u);
        const float edgeZ = std::min(v, 1.0f - v);
        const float fade = std::clamp(std::min(edgeX, edgeZ) * 7.5f, 0.0f, 1.0f);
        vertices.push_back(OahuOceanVertex{x, y, z, u, v, fade});
      }
    }

    std::vector<std::uint16_t> indices;
    indices.reserve((columns - 1) * (rows - 1) * 6);
    for (int row = 0; row < rows - 1; ++row) {
      for (int col = 0; col < columns - 1; ++col) {
        const std::uint16_t a = static_cast<std::uint16_t>(row * columns + col);
        const std::uint16_t b = static_cast<std::uint16_t>(row * columns + col + 1);
        const std::uint16_t c = static_cast<std::uint16_t>((row + 1) * columns + col);
        const std::uint16_t d = static_cast<std::uint16_t>((row + 1) * columns + col + 1);
        indices.push_back(a);
        indices.push_back(b);
        indices.push_back(c);
        indices.push_back(b);
        indices.push_back(d);
        indices.push_back(c);
      }
    }

    oceanVertexCount = static_cast<std::uint32_t>(vertices.size());
    oceanIndexCount = static_cast<std::uint32_t>(indices.size());

    const bgfx::Memory* vertexMemory = bgfx::copy(
      vertices.data(),
      static_cast<std::uint32_t>(vertices.size() * sizeof(OahuOceanVertex))
    );
    oceanVertexBuffer = bgfx::createVertexBuffer(vertexMemory, oceanLayout);

    const bgfx::Memory* indexMemory = bgfx::copy(
      indices.data(),
      static_cast<std::uint32_t>(indices.size() * sizeof(std::uint16_t))
    );
    oceanIndexBuffer = bgfx::createIndexBuffer(indexMemory);
  }

  void submitOcean(bgfx::ViewId viewId, const OahuRenderSettings& settings, bool topDown, float elapsedSeconds) {
    ensureOceanMesh();
    if (
      !bgfx::isValid(oceanVertexBuffer) ||
      !bgfx::isValid(oceanIndexBuffer) ||
      !oceanProgram.isValid()
    ) {
      return;
    }

    setSharedUniforms(settings, topDown, elapsedSeconds);
    bgfx::setVertexBuffer(0, oceanVertexBuffer, 0, oceanVertexCount);
    bgfx::setIndexBuffer(oceanIndexBuffer, 0, oceanIndexCount);
    bgfx::setState(
      BGFX_STATE_WRITE_RGB |
      BGFX_STATE_WRITE_A |
      BGFX_STATE_WRITE_Z |
      BGFX_STATE_DEPTH_TEST_LESS
    );
    bgfx::submit(viewId, oceanProgram.get());

    oceanDiagnostics = RenderPassDiagnostics{
      "Oahu Ocean",
      oceanProgram.label(),
      "retained indexed ocean grid",
      "GPU ocean atmosphere pass",
      "ocean is retained and shader-colored; top-down diagnostics hide it by default",
      1,
      0,
      oceanVertexCount,
      oceanIndexCount,
      0,
      oceanVertexCount,
      false,
      false
    };
  }

  void submitTerrainMesh(bgfx::ViewId viewId, const OahuRenderSettings& settings, bool topDown, float elapsedSeconds) {
    ensureTerrainMesh();
    if (
      !bgfx::isValid(terrainVertexBuffer) ||
      !bgfx::isValid(terrainIndexBuffer) ||
      !terrainProgram.isValid() ||
      terrainIndexCount == 0
    ) {
      return;
    }

    setSharedUniforms(settings, topDown, elapsedSeconds);
    bgfx::setVertexBuffer(0, terrainVertexBuffer, 0, terrainVertexCount);
    bgfx::setIndexBuffer(terrainIndexBuffer, 0, terrainIndexCount);
    bgfx::setState(
      BGFX_STATE_WRITE_RGB |
      BGFX_STATE_WRITE_A |
      BGFX_STATE_WRITE_Z |
      BGFX_STATE_DEPTH_TEST_LESS
    );
    bgfx::submit(viewId, terrainProgram.get());

    terrainDiagnostics = RenderPassDiagnostics{
      "Oahu Terrain",
      terrainProgram.label(),
      "retained indexed terrain mesh",
      "GPU lit terrain pass",
      "terrain uses shader lighting, height ramp, and horizon haze",
      1,
      0,
      terrainVertexCount,
      terrainIndexCount,
      0,
      terrainVertexCount,
      false,
      false
    };
  }

  void pushGridLines(std::vector<ColorVertex>& lines) const {
    const std::uint32_t gridColor = rgbaToAbgr(217, 246, 255, 88);
    for (int row = 0; row < kOahuGridHeight; ++row) {
      for (int col = 0; col < kOahuGridWidth - 1; ++col) {
        const OahuTerrainSample& a = sampleAt(col, row);
        const OahuTerrainSample& b = sampleAt(col + 1, row);
        if (a.land && b.land) {
          const bx::Vec3 pa = terrainPosition(a);
          const bx::Vec3 pb = terrainPosition(b);
          pushLine(lines, pa.x, pa.y + 0.04f, pa.z, pb.x, pb.y + 0.04f, pb.z, gridColor);
        }
      }
    }

    for (int col = 0; col < kOahuGridWidth; ++col) {
      for (int row = 0; row < kOahuGridHeight - 1; ++row) {
        const OahuTerrainSample& a = sampleAt(col, row);
        const OahuTerrainSample& b = sampleAt(col, row + 1);
        if (a.land && b.land) {
          const bx::Vec3 pa = terrainPosition(a);
          const bx::Vec3 pb = terrainPosition(b);
          pushLine(lines, pa.x, pa.y + 0.04f, pa.z, pb.x, pb.y + 0.04f, pb.z, gridColor);
        }
      }
    }
  }

  void pushLandmarkLines(std::vector<ColorVertex>& lines) const {
    const std::uint32_t markerColor = rgbaToAbgr(255, 73, 216, 255);
    const std::uint32_t spikeColor = rgbaToAbgr(255, 255, 255, 220);
    constexpr float markerSize = 0.13f;
    for (const OahuLandmark& landmark : kOahuLandmarks) {
      const bx::Vec3 p = terrainPosition(landmark.x, landmark.y, 90.0f);
      pushLine(lines, p.x - markerSize, p.y + 0.09f, p.z, p.x + markerSize, p.y + 0.09f, p.z, markerColor);
      pushLine(lines, p.x, p.y + 0.09f, p.z - markerSize, p.x, p.y + 0.09f, p.z + markerSize, markerColor);
      pushLine(lines, p.x, p.y + 0.09f, p.z, p.x, p.y + 0.72f, p.z, spikeColor);
    }
  }

  void reset(const ImVec2& size) override {
    lastSize = size;
  }

  void destroyTerrainResources() {
    if (bgfx::isValid(terrainIndexBuffer)) {
      bgfx::destroy(terrainIndexBuffer);
      terrainIndexBuffer = BGFX_INVALID_HANDLE;
    }

    if (bgfx::isValid(terrainVertexBuffer)) {
      bgfx::destroy(terrainVertexBuffer);
      terrainVertexBuffer = BGFX_INVALID_HANDLE;
    }

    terrainProgram.destroy();
    terrainVertexCount = 0;
    terrainIndexCount = 0;
    terrainDiagnostics = {};
  }

  void destroyOceanResources() {
    if (bgfx::isValid(oceanIndexBuffer)) {
      bgfx::destroy(oceanIndexBuffer);
      oceanIndexBuffer = BGFX_INVALID_HANDLE;
    }

    if (bgfx::isValid(oceanVertexBuffer)) {
      bgfx::destroy(oceanVertexBuffer);
      oceanVertexBuffer = BGFX_INVALID_HANDLE;
    }

    oceanProgram.destroy();
    oceanVertexCount = 0;
    oceanIndexCount = 0;
    oceanDiagnostics = {};
  }

  void shutdown() override {
    destroyOceanResources();
    destroyTerrainResources();
    destroyUniforms();
  }

  void draw(VisualizationContext& context) override {
    lastSize = context.size;
    const OahuDiagnosticSettings defaults;
    const OahuDiagnosticSettings& diagnostics = context.oahuDiagnostics
      ? *context.oahuDiagnostics
      : defaults;
    const OahuRenderSettings defaultRenderSettings;
    const OahuRenderSettings& renderSettings = context.oahuRenderSettings
      ? *context.oahuRenderSettings
      : defaultRenderSettings;

    if (diagnostics.showBackground && !diagnostics.topDown) {
      submitOcean(context.viewId, renderSettings, diagnostics.topDown, context.elapsedSeconds);
    } else {
      oceanDiagnostics = {};
    }

    if (diagnostics.showFilledTerrain) {
      submitTerrainMesh(context.viewId, renderSettings, diagnostics.topDown, context.elapsedSeconds);
    } else {
      terrainDiagnostics = {};
    }

    std::vector<ColorVertex> lines;
    lines.reserve(
      kOahuCoastlinePointCount * 2 +
      kOahuGridWidth * kOahuGridHeight * 4 +
      kOahuLandmarkCount * 6
    );

    if (diagnostics.showCoastline) {
      const std::uint32_t coastColor = rgbaToAbgr(247, 228, 159, 255);
      for (int i = 0; i < kOahuCoastlinePointCount; ++i) {
        const OahuTopologyPoint& a = kOahuCoastline[static_cast<std::size_t>(i)];
        const OahuTopologyPoint& b = kOahuCoastline[static_cast<std::size_t>((i + 1) % kOahuCoastlinePointCount)];
        const bx::Vec3 pa = terrainPosition(a.x, a.y, 18.0f);
        const bx::Vec3 pb = terrainPosition(b.x, b.y, 18.0f);
        pushLine(lines, pa.x, pa.y + 0.015f, pa.z, pb.x, pb.y + 0.015f, pb.z, coastColor);
      }
    }

    if (diagnostics.showGrid) {
      pushGridLines(lines);
    }

    if (diagnostics.showRidges) {
      const std::uint32_t ridgeColor = rgbaToAbgr(255, 255, 255, 48);
      for (int row = 2; row < kOahuGridHeight - 2; row += 3) {
        for (int col = 1; col < kOahuGridWidth - 1; ++col) {
          const OahuTerrainSample& a = sampleAt(col - 1, row);
          const OahuTerrainSample& b = sampleAt(col, row);
          if (a.land && b.land && a.elevationMeters > 140.0f && b.elevationMeters > 140.0f) {
            const bx::Vec3 pa = terrainPosition(a);
            const bx::Vec3 pb = terrainPosition(b);
            pushLine(lines, pa.x, pa.y + 0.025f, pa.z, pb.x, pb.y + 0.025f, pb.z, ridgeColor);
          }
        }
      }
    }

    if (diagnostics.showLandmarks) {
      pushLandmarkLines(lines);
    }

    submitColorVertices(*context.renderer, context.viewId, lines, ColorPrimitive::Lines, true, false);
  }

  void drawInspector() override {
    int landSamples = 0;
    for (const OahuTerrainSample& sample : kOahuTerrain) {
      if (sample.land) {
        ++landSamples;
      }
    }

    ImGui::Text("Grid: %d", kOahuGridWidth * kOahuGridHeight);
    ImGui::Text("Land samples: %d", landSamples);
    ImGui::Text("Coast points: %d", kOahuCoastlinePointCount);
    ImGui::Text("Terrain vertices: %u", terrainVertexCount);
    ImGui::Text("Terrain indices: %u", terrainIndexCount);
    ImGui::Text("Ocean vertices: %u", oceanVertexCount);
    ImGui::Text("Ocean indices: %u", oceanIndexCount);
    ImGui::TextUnformatted("Terrain pass: retained lit indexed bgfx mesh");
    ImGui::TextUnformatted("Ocean pass: retained indexed bgfx atmosphere grid");
    ImGui::Text("Terrain shader: %s", terrainProgram.label());
    ImGui::Text("Ocean shader: %s", oceanProgram.label());
    ImGui::Text("Source points: %d", kOahuSourceCoastlinePointCount);
    ImGui::Text("Smoothing passes: %d", kOahuElevationSmoothingPasses);
    ImGui::Text("Landmarks: %d", kOahuLandmarkCount);
    ImGui::Text("Aspect: %.3f", kOahuMapAspect);
    ImGui::Text("Max elevation: %.0f m", kOahuMaxElevationMeters);

    if (ImGui::TreeNode("Landmark Controls")) {
      if (ImGui::BeginTable("OahuLandmarkTable", 4, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("X");
        ImGui::TableSetupColumn("Y");
        ImGui::TableSetupColumn("Land");
        ImGui::TableHeadersRow();
        for (const OahuLandmark& landmark : kOahuLandmarks) {
          ImGui::TableNextRow();
          ImGui::TableSetColumnIndex(0);
          ImGui::TextUnformatted(landmark.name);
          ImGui::TableSetColumnIndex(1);
          ImGui::Text("%.3f", landmark.x);
          ImGui::TableSetColumnIndex(2);
          ImGui::Text("%.3f", landmark.y);
          ImGui::TableSetColumnIndex(3);
          ImGui::TextUnformatted(landmark.land ? "yes" : "edge");
        }
        ImGui::EndTable();
      }
      ImGui::TreePop();
    }
  }

  RenderPassDiagnostics renderPassDiagnostics() const override {
    if (terrainDiagnostics.passName != nullptr && oceanDiagnostics.passName != nullptr) {
      return RenderPassDiagnostics{
        "Oahu Atmosphere + Terrain",
        "oahu_ocean + oahu_terrain",
        "retained ocean grid + retained terrain mesh",
        "GPU atmosphere, lighting, and terrain passes",
        "ocean draws first, terrain draws second, diagnostic lines remain transient",
        terrainDiagnostics.drawCalls + oceanDiagnostics.drawCalls,
        0,
        terrainDiagnostics.vertices + oceanDiagnostics.vertices,
        terrainDiagnostics.indices + oceanDiagnostics.indices,
        0,
        terrainDiagnostics.bufferCapacity + oceanDiagnostics.bufferCapacity,
        false,
        false
      };
    }

    if (terrainDiagnostics.passName != nullptr) {
      return terrainDiagnostics;
    }

    return oceanDiagnostics;
  }
};

} // namespace

std::unique_ptr<IVisualizationModule> createOahuFlyoverVisualization() {
  return std::make_unique<OahuFlyoverVisualization>();
}

} // namespace prappy
