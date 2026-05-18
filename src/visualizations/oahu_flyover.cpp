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

  bx::Vec3 terrainPosition(const OahuTerrainSample& sample) const {
    const float x = (sample.x - 0.5f) * 12.0f;
    const float z = (0.5f - sample.y) * 8.2f;
    const float y = std::max(sample.elevationMeters, 0.0f) / kOahuMaxElevationMeters * 1.85f;
    return {x, y, z};
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

  void reset(const ImVec2& size) override {
    lastSize = size;
  }

  void draw(VisualizationContext& context) override {
    lastSize = context.size;

    std::vector<ColorVertex> background;
    background.reserve(12);
    pushHorizon(background);
    pushOcean(background);
    submitColorVertices(*context.renderer, context.viewId, background, ColorPrimitive::Triangles, false, false);

    std::vector<ColorVertex> terrain;
    terrain.reserve((kOahuGridWidth - 1) * (kOahuGridHeight - 1) * 6);
    for (int row = 0; row < kOahuGridHeight - 1; ++row) {
      for (int col = 0; col < kOahuGridWidth - 1; ++col) {
        const OahuTerrainSample& a = sampleAt(col, row);
        const OahuTerrainSample& b = sampleAt(col + 1, row);
        const OahuTerrainSample& c = sampleAt(col, row + 1);
        const OahuTerrainSample& d = sampleAt(col + 1, row + 1);

        if (a.land && b.land && c.land) {
          pushTerrainTriangle(terrain, a, b, c);
        }
        if (b.land && d.land && c.land) {
          pushTerrainTriangle(terrain, b, d, c);
        }
      }
    }
    submitColorVertices(*context.renderer, context.viewId, terrain, ColorPrimitive::Triangles, true, true);

    std::vector<ColorVertex> lines;
    lines.reserve(kOahuCoastlinePointCount * 2 + (kOahuGridWidth + kOahuGridHeight) * 2);
    const std::uint32_t coastColor = rgbaToAbgr(247, 228, 159, 255);
    for (int i = 0; i < kOahuCoastlinePointCount; ++i) {
      const OahuTopologyPoint& a = kOahuCoastline[static_cast<std::size_t>(i)];
      const OahuTopologyPoint& b = kOahuCoastline[static_cast<std::size_t>((i + 1) % kOahuCoastlinePointCount)];
      const OahuTerrainSample sa{a.x, a.y, 18.0f, 1};
      const OahuTerrainSample sb{b.x, b.y, 18.0f, 1};
      const bx::Vec3 pa = terrainPosition(sa);
      const bx::Vec3 pb = terrainPosition(sb);
      pushLine(lines, pa.x, pa.y + 0.015f, pa.z, pb.x, pb.y + 0.015f, pb.z, coastColor);
    }

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
    ImGui::Text("Max elevation: %.0f m", kOahuMaxElevationMeters);
  }
};

} // namespace

std::unique_ptr<IVisualizationModule> createOahuFlyoverVisualization() {
  return std::make_unique<OahuFlyoverVisualization>();
}

} // namespace prappy
