#include "../visualization_core.h"
#include "../oahu_topology.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace prappy {
namespace {

struct OahuFlyoverVisualization final : IVisualizationModule {
  ImVec2 lastSize{};

  const VisualizationDescriptor& descriptor() const override {
    return visualizationDescriptor(VisualizationId::OahuFlyover);
  }

  const OahuTerrainSample& sampleAt(int col, int row) const {
    return kOahuTerrain[static_cast<std::size_t>(row * kOahuGridWidth + col)];
  }

  bx::Vec3 terrainPosition(float xValue, float yValue, float elevationMeters) const {
    constexpr float zSpan = 8.2f;
    constexpr float xSpan = zSpan * kOahuMapAspect;
    const float x = (xValue - 0.5f) * xSpan;
    const float z = (0.5f - yValue) * zSpan;
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

  void pushTerrainTriangle(
    std::vector<ColorVertex>& vertices,
    const OahuTerrainSample& a,
    const OahuTerrainSample& b,
    const OahuTerrainSample& c
  ) const {
    const bx::Vec3 pa = terrainPosition(a);
    const bx::Vec3 pb = terrainPosition(b);
    const bx::Vec3 pc = terrainPosition(c);

    vertices.push_back(ColorVertex{pa.x, pa.y, pa.z, terrainColor(a.elevationMeters)});
    vertices.push_back(ColorVertex{pb.x, pb.y, pb.z, terrainColor(b.elevationMeters)});
    vertices.push_back(ColorVertex{pc.x, pc.y, pc.z, terrainColor(c.elevationMeters)});
  }

  void pushOcean(std::vector<ColorVertex>& vertices) const {
    const std::uint32_t nearOcean = rgbaToAbgr(26, 128, 176, 255);
    const std::uint32_t farOcean = rgbaToAbgr(96, 181, 221, 255);
    const float y = -0.035f;
    vertices.push_back(ColorVertex{-36.0f, y, -42.0f, farOcean});
    vertices.push_back(ColorVertex{36.0f, y, -42.0f, farOcean});
    vertices.push_back(ColorVertex{36.0f, y, 24.0f, nearOcean});
    vertices.push_back(ColorVertex{-36.0f, y, -42.0f, farOcean});
    vertices.push_back(ColorVertex{36.0f, y, 24.0f, nearOcean});
    vertices.push_back(ColorVertex{-36.0f, y, 24.0f, nearOcean});
  }

  void pushHorizon(std::vector<ColorVertex>& vertices) const {
    const std::uint32_t skyTop = rgbaToAbgr(88, 176, 239, 255);
    const std::uint32_t horizon = rgbaToAbgr(204, 235, 246, 255);
    const float z = -42.0f;
    vertices.push_back(ColorVertex{-42.0f, 0.0f, z, horizon});
    vertices.push_back(ColorVertex{42.0f, 0.0f, z, horizon});
    vertices.push_back(ColorVertex{42.0f, 20.0f, z, skyTop});
    vertices.push_back(ColorVertex{-42.0f, 0.0f, z, horizon});
    vertices.push_back(ColorVertex{42.0f, 20.0f, z, skyTop});
    vertices.push_back(ColorVertex{-42.0f, 20.0f, z, skyTop});
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

  void draw(VisualizationContext& context) override {
    lastSize = context.size;
    const OahuDiagnosticSettings defaults;
    const OahuDiagnosticSettings& diagnostics = context.oahuDiagnostics
      ? *context.oahuDiagnostics
      : defaults;

    if (diagnostics.showBackground && !diagnostics.topDown) {
      std::vector<ColorVertex> background;
      background.reserve(12);
      pushHorizon(background);
      pushOcean(background);
      submitColorVertices(*context.renderer, context.viewId, background, ColorPrimitive::Triangles, false, false);
    }

    if (diagnostics.showFilledTerrain) {
      std::vector<ColorVertex> terrain;
      terrain.reserve((kOahuGridWidth - 1) * (kOahuGridHeight - 1) * 6);
      for (int row = 0; row < kOahuGridHeight - 1; ++row) {
        for (int col = 0; col < kOahuGridWidth - 1; ++col) {
          const OahuTerrainSample& a = sampleAt(col, row);
          const OahuTerrainSample& b = sampleAt(col + 1, row);
          const OahuTerrainSample& c = sampleAt(col, row + 1);
          const OahuTerrainSample& d = sampleAt(col + 1, row + 1);

          if (triangleTouchesLand(a, b, c)) {
            pushTerrainTriangle(terrain, a, b, c);
          }
          if (triangleTouchesLand(b, d, c)) {
            pushTerrainTriangle(terrain, b, d, c);
          }
        }
      }
      submitColorVertices(*context.renderer, context.viewId, terrain, ColorPrimitive::Triangles, true, true);
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
    ImGui::Text("Source points: %d", kOahuSourceCoastlinePointCount);
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
};

} // namespace

std::unique_ptr<IVisualizationModule> createOahuFlyoverVisualization() {
  return std::make_unique<OahuFlyoverVisualization>();
}

} // namespace prappy
