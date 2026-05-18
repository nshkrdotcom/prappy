#include "../visualization_core.h"
#include "../oahu_topology.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
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

struct OahuFlyoverVisualization final : IVisualizationModule {
  ImVec2 lastSize{};
  bgfx::VertexLayout terrainLayout;
  ShaderProgram terrainProgram;
  bgfx::VertexBufferHandle terrainVertexBuffer = BGFX_INVALID_HANDLE;
  bgfx::IndexBufferHandle terrainIndexBuffer = BGFX_INVALID_HANDLE;
  std::uint32_t terrainVertexCount = 0;
  std::uint32_t terrainIndexCount = 0;
  RenderPassDiagnostics terrainDiagnostics;

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
    const bx::Vec3 normal = terrainFaceNormal(
      positions[ia],
      positions[ib],
      positions[ic]
    );
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

  void ensureTerrainMesh() {
    if (
      bgfx::isValid(terrainVertexBuffer) &&
      bgfx::isValid(terrainIndexBuffer) &&
      terrainProgram.isValid()
    ) {
      return;
    }

    shutdown();

    terrainLayout
      .begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Normal, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
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

  void submitTerrainMesh(bgfx::ViewId viewId) {
    ensureTerrainMesh();

    if (
      !bgfx::isValid(terrainVertexBuffer) ||
      !bgfx::isValid(terrainIndexBuffer) ||
      !terrainProgram.isValid() ||
      terrainIndexCount == 0
    ) {
      return;
    }

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
      "retained indexed vertex/index buffers",
      "GPU mesh draw",
      "terrain mesh is retained; coastline and diagnostic overlays stay transient",
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

  void pushOcean(std::vector<ColorVertex>& vertices) const {
    const std::uint32_t nearOcean = rgbaToAbgr(26, 128, 176, 255);
    const std::uint32_t farOcean = rgbaToAbgr(96, 181, 221, 255);
    const float y = -0.035f;
    vertices.push_back(ColorVertex{-36.0f, y, -24.0f, nearOcean});
    vertices.push_back(ColorVertex{36.0f, y, -24.0f, nearOcean});
    vertices.push_back(ColorVertex{36.0f, y, 42.0f, farOcean});
    vertices.push_back(ColorVertex{-36.0f, y, -24.0f, nearOcean});
    vertices.push_back(ColorVertex{36.0f, y, 42.0f, farOcean});
    vertices.push_back(ColorVertex{-36.0f, y, 42.0f, farOcean});
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

  void shutdown() override {
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

  void draw(VisualizationContext& context) override {
    lastSize = context.size;
    const OahuDiagnosticSettings defaults;
    const OahuDiagnosticSettings& diagnostics = context.oahuDiagnostics
      ? *context.oahuDiagnostics
      : defaults;

    if (diagnostics.showBackground && !diagnostics.topDown) {
      std::vector<ColorVertex> background;
      background.reserve(6);
      pushOcean(background);
      submitColorVertices(*context.renderer, context.viewId, background, ColorPrimitive::Triangles, false, false);
    }

    if (diagnostics.showFilledTerrain) {
      submitTerrainMesh(context.viewId);
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
    ImGui::TextUnformatted("Terrain pass: retained indexed bgfx mesh");
    ImGui::Text("Terrain shader: %s", terrainProgram.label());
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
    return terrainDiagnostics;
  }
};

} // namespace

std::unique_ptr<IVisualizationModule> createOahuFlyoverVisualization() {
  return std::make_unique<OahuFlyoverVisualization>();
}

} // namespace prappy
