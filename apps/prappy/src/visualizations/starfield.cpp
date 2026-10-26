#include "../visualization_core.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace prappy {
namespace {

struct StarfieldVisualization final : IVisualizationModule {
  struct Star {
    float x = 0.0f;
    float y = 0.0f;
    float z = 1.0f;
    float speed = 1.0f;
    float radius = 1.0f;
    float tint = 0.0f;
  };

  std::mt19937 rng{0xc0ffee42u};
  std::vector<Star> stars;
  ImVec2 lastSize{};

  const VisualizationDescriptor& descriptor() const override {
    return visualizationDescriptor(VisualizationId::Starfield3D);
  }

  void resetStar(Star& star, const ImVec2& size, bool spreadDepth) {
    const float aspect = std::max(size.x / std::max(size.y, 1.0f), 1.0f);

    star.x = randomFloat(rng, -aspect * 2.0f, aspect * 2.0f);
    star.y = randomFloat(rng, -2.0f, 2.0f);
    star.z = spreadDepth ? randomFloat(rng, 0.35f, 18.0f) : randomFloat(rng, 14.0f, 18.0f);
    star.speed = randomFloat(rng, 2.2f, 8.0f);
    star.radius = randomFloat(rng, 0.7f, 2.2f);
    star.tint = randomFloat(rng, 0.0f, 1.0f);
  }

  void reset(const ImVec2& size) override {
    stars.clear();
    stars.resize(900);
    lastSize = size;

    for (Star& star : stars) {
      resetStar(star, size, true);
    }
  }

  void draw(VisualizationContext& context) override {
    if (
      stars.empty() ||
      distanceSquared(context.size, lastSize) > 64.0f * 64.0f
    ) {
      reset(context.size);
    }

    std::vector<ColorVertex> vertices;
    vertices.reserve(stars.size() * 2u + 96u);

    for (Star& star : stars) {
      const float previousZ = star.z;
      star.z -= star.speed * context.deltaSeconds;

      if (star.z <= 0.18f) {
        resetStar(star, context.size, false);
        continue;
      }

      const float depthAlpha = std::clamp(1.0f - star.z / 18.0f, 0.08f, 1.0f);
      const float warm = star.tint > 0.82f ? 1.0f : 0.82f;
      const float cool = star.tint < 0.18f ? 1.0f : 0.86f;
      const std::uint32_t color = rgbaFloatToAbgr(
        std::clamp(warm * (0.65f + depthAlpha * 0.45f), 0.0f, 1.0f),
        std::clamp(0.72f + depthAlpha * 0.28f, 0.0f, 1.0f),
        std::clamp(cool, 0.0f, 1.0f),
        std::clamp(depthAlpha, 0.0f, 1.0f)
      );

      const float halfWidth = std::max(star.radius * 0.004f * depthAlpha, 0.0012f);
      pushLine(
        vertices,
        star.x - halfWidth,
        star.y,
        -previousZ,
        star.x + halfWidth,
        star.y,
        -star.z,
        color
      );
    }

    const std::uint32_t tunnelColor = rgbaToAbgr(90, 128, 255, 42);
    for (float z = 3.0f; z <= 18.0f; z += 3.0f) {
      constexpr int kRingSegments = 32;
      const float radius = z * 0.12f;
      for (int i = 0; i < kRingSegments; ++i) {
        const float a0 = static_cast<float>(i) / static_cast<float>(kRingSegments) * kPi * 2.0f;
        const float a1 = static_cast<float>(i + 1) / static_cast<float>(kRingSegments) * kPi * 2.0f;
        pushLine(
          vertices,
          std::cos(a0) * radius,
          std::sin(a0) * radius,
          -z,
          std::cos(a1) * radius,
          std::sin(a1) * radius,
          -z,
          tunnelColor
        );
      }
    }

    submitColorVertices(*context.renderer, context.viewId, vertices, ColorPrimitive::Lines);
  }

  void drawInspector() override {
    ImGui::Text("Stars: %d", static_cast<int>(stars.size()));
    ImGui::TextUnformatted("Primitive: depth lines");
    ImGui::TextUnformatted("Camera: perspective");
  }
};

} // namespace

std::unique_ptr<IVisualizationModule> createStarfieldVisualization() {
  return std::make_unique<StarfieldVisualization>();
}

} // namespace prappy
