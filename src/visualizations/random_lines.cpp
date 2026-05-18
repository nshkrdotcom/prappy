#include "../visualization_core.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace prappy {
namespace {

struct RandomLinesVisualization final : IVisualizationModule {
  struct Endpoint {
    ImVec2 position{};
    ImVec2 velocity{};
  };

  struct Segment {
    Endpoint a{};
    Endpoint b{};
    float hue = 0.0f;
    float thickness = 1.0f;
  };

  std::mt19937 rng{0x5eed1234u};
  std::vector<Segment> segments;
  ImVec2 lastSize{};

  const VisualizationDescriptor& descriptor() const override {
    return visualizationDescriptor(VisualizationId::RandomLines2D);
  }

  Endpoint makeEndpoint(const ImVec2& size) {
    const float angle = randomFloat(rng, 0.0f, kPi * 2.0f);
    const float speed = randomFloat(rng, 70.0f, 260.0f);

    Endpoint endpoint;
    endpoint.position = ImVec2(
      randomFloat(rng, 0.0f, std::max(size.x, 1.0f)),
      randomFloat(rng, 0.0f, std::max(size.y, 1.0f))
    );
    endpoint.velocity = ImVec2(std::cos(angle) * speed, std::sin(angle) * speed);
    return endpoint;
  }

  void reset(const ImVec2& size) override {
    segments.clear();
    segments.reserve(72);
    lastSize = size;

    for (int i = 0; i < 72; ++i) {
      Segment segment;
      segment.a = makeEndpoint(size);
      segment.b = makeEndpoint(size);
      segment.hue = randomFloat(rng, 0.0f, 1.0f);
      segment.thickness = randomFloat(rng, 1.0f, 2.6f);
      segments.push_back(segment);
    }
  }

  void updateEndpoint(Endpoint& endpoint, const ImVec2& size, float deltaSeconds) {
    endpoint.position = addVec2(endpoint.position, scaleVec2(endpoint.velocity, deltaSeconds));

    if (endpoint.position.x < 0.0f) {
      endpoint.position.x = 0.0f;
      endpoint.velocity.x = std::abs(endpoint.velocity.x);
    } else if (endpoint.position.x > size.x) {
      endpoint.position.x = size.x;
      endpoint.velocity.x = -std::abs(endpoint.velocity.x);
    }

    if (endpoint.position.y < 0.0f) {
      endpoint.position.y = 0.0f;
      endpoint.velocity.y = std::abs(endpoint.velocity.y);
    } else if (endpoint.position.y > size.y) {
      endpoint.position.y = size.y;
      endpoint.velocity.y = -std::abs(endpoint.velocity.y);
    }
  }

  void pushGrid(const VisualizationContext& context, std::vector<ColorVertex>& vertices) {
    const std::uint32_t gridColor = rgbaToAbgr(255, 255, 255, 18);
    constexpr float spacing = 48.0f;

    for (float x = 0.0f; x <= context.size.x; x += spacing) {
      pushLine(vertices, x, 0.0f, 0.0f, x, context.size.y, 0.0f, gridColor);
    }

    for (float y = 0.0f; y <= context.size.y; y += spacing) {
      pushLine(vertices, 0.0f, y, 0.0f, context.size.x, y, 0.0f, gridColor);
    }
  }

  void draw(VisualizationContext& context) override {
    if (
      segments.empty() ||
      distanceSquared(context.size, lastSize) > 48.0f * 48.0f
    ) {
      reset(context.size);
    }

    std::vector<ColorVertex> vertices;
    vertices.reserve(segments.size() * 6u + 96u);
    pushGrid(context, vertices);

    for (Segment& segment : segments) {
      updateEndpoint(segment.a, context.size, context.deltaSeconds);
      updateEndpoint(segment.b, context.size, context.deltaSeconds);

      const float hue = std::fmod(segment.hue + context.elapsedSeconds * 0.035f, 1.0f);
      const std::uint32_t color = hsvToAbgr(hue, 0.78f, 0.95f, 0.78f);

      pushLine(
        vertices,
        segment.a.position.x,
        segment.a.position.y,
        0.0f,
        segment.b.position.x,
        segment.b.position.y,
        0.0f,
        color
      );

      const float marker = segment.thickness + 2.0f;
      pushLine(
        vertices,
        segment.a.position.x - marker,
        segment.a.position.y,
        0.0f,
        segment.a.position.x + marker,
        segment.a.position.y,
        0.0f,
        color
      );
      pushLine(
        vertices,
        segment.b.position.x,
        segment.b.position.y - marker,
        0.0f,
        segment.b.position.x,
        segment.b.position.y + marker,
        0.0f,
        color
      );
    }

    submitColorVertices(*context.renderer, context.viewId, vertices, ColorPrimitive::Lines);
  }

  void drawInspector() override {
    ImGui::Text("Segments: %d", static_cast<int>(segments.size()));
    ImGui::TextUnformatted("Primitive: line list");
    ImGui::TextUnformatted("Coordinates: screen pixels");
  }
};

} // namespace

std::unique_ptr<IVisualizationModule> createRandomLinesVisualization() {
  return std::make_unique<RandomLinesVisualization>();
}

} // namespace prappy
