#define SDL_MAIN_HANDLED

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <bgfx/bgfx.h>
#include <bgfx/platform.h>

#include <bx/math.h>

#include "oahu_topology.h"

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
#endif

namespace {

struct Texture {
  bgfx::TextureHandle handle = BGFX_INVALID_HANDLE;
};

struct Program {
  bgfx::ProgramHandle handle = BGFX_INVALID_HANDLE;
};

constexpr bgfx::ViewId kVisualizationView = 0;
constexpr bgfx::ViewId kUiView = 1;
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

struct VisualizationContext {
  VisualizationRenderer* renderer = nullptr;
  bgfx::ViewId viewId = kVisualizationView;
  ImVec2 size{};
  float deltaSeconds = 1.0f / 60.0f;
  float elapsedSeconds = 0.0f;
};

enum class ColorPrimitive {
  Lines,
  Triangles
};

void submitColorVertices(
  const VisualizationRenderer& renderer,
  bgfx::ViewId viewId,
  const std::vector<ColorVertex>& vertices,
  ColorPrimitive primitive,
  bool depthTest = false,
  bool depthWrite = false
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

enum class VisualizationId {
  RandomLines2D,
  Starfield3D,
  OahuFlyover
};

struct VisualizationDescriptor {
  VisualizationId id;
  const char* name;
  const char* shortName;
  const char* spaceLabel;
  const char* primitiveLabel;
  bool usesCamera;
  bool hasAutoCamera;
};

const VisualizationDescriptor& visualizationDescriptor(VisualizationId id) {
  static const VisualizationDescriptor descriptors[] = {
    {
      VisualizationId::RandomLines2D,
      "Random Lines 2D",
      "Lines",
      "2D screen-space line pass",
      "line list",
      false,
      false
    },
    {
      VisualizationId::Starfield3D,
      "Infinite Starfield",
      "Starfield",
      "3D line pass",
      "depth lines",
      true,
      false
    },
    {
      VisualizationId::OahuFlyover,
      "Oahu Flyover",
      "Oahu",
      "3D terrain pass",
      "triangles + coastline lines",
      true,
      true
    }
  };

  for (const VisualizationDescriptor& descriptor : descriptors) {
    if (descriptor.id == id) {
      return descriptor;
    }
  }

  return descriptors[0];
}

const char* visualizationName(VisualizationId id) {
  return visualizationDescriptor(id).name;
}

const char* visualizationShortName(VisualizationId id) {
  return visualizationDescriptor(id).shortName;
}

const char* visualizationSpaceLabel(VisualizationId id) {
  return visualizationDescriptor(id).spaceLabel;
}

struct IVisualizationModule {
  virtual ~IVisualizationModule() = default;
  virtual const VisualizationDescriptor& descriptor() const = 0;
  virtual void reset(const ImVec2& size) = 0;
  virtual void draw(VisualizationContext& context) = 0;
  virtual void drawInspector() const = 0;
};

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

  void drawInspector() const override {
    ImGui::Text("Segments: %d", static_cast<int>(segments.size()));
    ImGui::TextUnformatted("Primitive: line list");
    ImGui::TextUnformatted("Coordinates: screen pixels");
  }
};

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

  void drawInspector() const override {
    ImGui::Text("Stars: %d", static_cast<int>(stars.size()));
    ImGui::TextUnformatted("Primitive: depth lines");
    ImGui::TextUnformatted("Camera: perspective");
  }
};

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

  void drawInspector() const override {
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

struct CameraRig {
  bool manual = false;
  float yaw = 0.0f;
  float pitch = 0.0f;
  float distance = 10.0f;
  float fovDegrees = 70.0f;
  float routeSpeed = 1.0f;
  bx::Vec3 target = {0.0f, 0.0f, 0.0f};

  void resetFor(VisualizationId id) {
    manual = false;
    routeSpeed = 1.0f;

    switch (id) {
      case VisualizationId::RandomLines2D:
        yaw = 0.0f;
        pitch = 0.0f;
        distance = 10.0f;
        fovDegrees = 70.0f;
        target = {0.0f, 0.0f, 0.0f};
        break;
      case VisualizationId::Starfield3D:
        yaw = 0.0f;
        pitch = 0.0f;
        distance = 1.0f;
        fovDegrees = 70.0f;
        target = {0.0f, 0.0f, -1.0f};
        break;
      case VisualizationId::OahuFlyover:
        yaw = 0.0f;
        pitch = 0.25f;
        distance = 10.0f;
        fovDegrees = 62.0f;
        target = {0.0f, 0.55f, -0.6f};
        break;
    }
  }

  bx::Vec3 lookDirection() const {
    const float cp = std::cos(pitch);
    return {
      std::sin(yaw) * cp,
      std::sin(pitch),
      -std::cos(yaw) * cp
    };
  }

  bx::Vec3 orbitEye() const {
    const float cp = std::cos(pitch);
    return {
      target.x + std::sin(yaw) * cp * distance,
      target.y + std::sin(pitch) * distance,
      target.z + std::cos(yaw) * cp * distance
    };
  }

  void orbit(const ImVec2& delta) {
    yaw -= delta.x * 0.008f;
    pitch = std::clamp(pitch - delta.y * 0.006f, -1.15f, 1.25f);
    manual = true;
  }

  void pan(const ImVec2& delta) {
    const float scale = std::max(distance, 1.0f) * 0.0018f;
    const bx::Vec3 right = {std::cos(yaw), 0.0f, std::sin(yaw)};
    target.x -= right.x * delta.x * scale;
    target.z -= right.z * delta.x * scale;
    target.y += delta.y * scale;
    target.y = std::clamp(target.y, -1.0f, 4.0f);
    manual = true;
  }

  void zoom(VisualizationId id, float wheel) {
    if (id == VisualizationId::Starfield3D) {
      fovDegrees = std::clamp(fovDegrees - wheel * 4.0f, 38.0f, 96.0f);
    } else {
      distance = std::clamp(distance * std::pow(0.86f, wheel), 2.2f, 34.0f);
    }
    manual = true;
  }
};

struct VisualizationHost {
  VisualizationId active = VisualizationId::RandomLines2D;
  bool showStatus = true;
  bool resetRequested = true;
  CameraRig camera;
  std::vector<std::unique_ptr<IVisualizationModule>> modules;

  VisualizationHost() {
    camera.resetFor(active);
    modules.push_back(std::make_unique<RandomLinesVisualization>());
    modules.push_back(std::make_unique<StarfieldVisualization>());
    modules.push_back(std::make_unique<OahuFlyoverVisualization>());
  }

  IVisualizationModule& module(VisualizationId id) {
    for (const std::unique_ptr<IVisualizationModule>& candidate : modules) {
      if (candidate->descriptor().id == id) {
        return *candidate;
      }
    }

    return *modules.front();
  }

  const IVisualizationModule& module(VisualizationId id) const {
    for (const std::unique_ptr<IVisualizationModule>& candidate : modules) {
      if (candidate->descriptor().id == id) {
        return *candidate;
      }
    }

    return *modules.front();
  }

  IVisualizationModule& activeModule() {
    return module(active);
  }

  const IVisualizationModule& activeModule() const {
    return module(active);
  }

  void setActive(VisualizationId next) {
    if (active != next) {
      active = next;
      resetRequested = true;
      camera.resetFor(active);
    }
  }

  void resetActive(const ImVec2& size) {
    activeModule().reset(size);
    resetRequested = false;
  }

  void draw(VisualizationContext& context) {
    if (resetRequested) {
      resetActive(context.size);
    }

    activeModule().draw(context);
  }
};

#pragma pack(push, 1)
struct BmpInfoHeader {
  std::uint32_t size = 40;
  std::int32_t width = 0;
  std::int32_t height = 0;
  std::uint16_t planes = 1;
  std::uint16_t bitCount = 32;
  std::uint32_t compression = 0;
  std::uint32_t imageSize = 0;
  std::int32_t xPixelsPerMeter = 0;
  std::int32_t yPixelsPerMeter = 0;
  std::uint32_t colorsUsed = 0;
  std::uint32_t importantColors = 0;
};

struct BmpFileHeader {
  std::uint16_t type = 0x4d42;
  std::uint32_t size = 0;
  std::uint16_t reserved1 = 0;
  std::uint16_t reserved2 = 0;
  std::uint32_t offset = 0;
};
#pragma pack(pop)

struct ScreenshotCapture {
  std::filesystem::path path;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool pending = false;
};

class PrappyBgfxCallback final : public bgfx::CallbackI {
public:
  void queueScreenshot(const std::filesystem::path& path, const ImVec2& origin, const ImVec2& size) {
    std::lock_guard<std::mutex> lock(mutex);
    capture.path = path;
    capture.x = std::max(static_cast<int>(std::round(origin.x)), 0);
    capture.y = std::max(static_cast<int>(std::round(origin.y)), 0);
    capture.width = std::max(static_cast<int>(std::round(size.x)), 1);
    capture.height = std::max(static_cast<int>(std::round(size.y)), 1);
    capture.pending = true;
    hasCompletedMessage = false;
    completedMessage.clear();
  }

  bool consumeScreenshotStatus(std::string& message) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!hasCompletedMessage) {
      return false;
    }

    message = completedMessage;
    hasCompletedMessage = false;
    completedMessage.clear();
    return true;
  }

  void fatal(
    const char* filePath,
    std::uint16_t line,
    bgfx::Fatal::Enum code,
    const char* message
  ) override {
    std::fprintf(stderr, "bgfx fatal %s:%u: %s\n", filePath, line, message);
    if (code != bgfx::Fatal::DebugCheck) {
      std::abort();
    }
  }

  void traceVargs(
    const char* filePath,
    std::uint16_t line,
    const char* format,
    va_list args
  ) override {
    (void)filePath;
    (void)line;
    (void)format;
    (void)args;
  }

  void profilerBegin(
    const char*,
    std::uint32_t,
    const char*,
    std::uint16_t
  ) override {}

  void profilerBeginLiteral(
    const char*,
    std::uint32_t,
    const char*,
    std::uint16_t
  ) override {}

  void profilerEnd() override {}
  std::uint32_t cacheReadSize(std::uint64_t) override { return 0; }
  bool cacheRead(std::uint64_t, void*, std::uint32_t) override { return false; }
  void cacheWrite(std::uint64_t, const void*, std::uint32_t) override {}

  void screenShot(
    const char* filePath,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch,
    bgfx::TextureFormat::Enum format,
    const void* data,
    std::uint32_t,
    bool yflip
  ) override {
    ScreenshotCapture request;
    {
      std::lock_guard<std::mutex> lock(mutex);
      request = capture;
      capture.pending = false;
    }

    if (request.path.empty() && filePath) {
      request.path = filePath;
    }
    if (request.width <= 0 || request.height <= 0) {
      request.x = 0;
      request.y = 0;
      request.width = static_cast<int>(width);
      request.height = static_cast<int>(height);
    }

    std::string error;
    const bool ok = writeBmp(request, width, height, pitch, format, data, yflip, error);

    std::lock_guard<std::mutex> lock(mutex);
    completedMessage = ok
      ? std::string("Saved ") + request.path.string()
      : error;
    hasCompletedMessage = true;
  }

  void captureBegin(
    std::uint32_t,
    std::uint32_t,
    std::uint32_t,
    bgfx::TextureFormat::Enum,
    bool
  ) override {}

  void captureEnd() override {}
  void captureFrame(const void*, std::uint32_t) override {}

private:
  bool writeBmp(
    const ScreenshotCapture& request,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t pitch,
    bgfx::TextureFormat::Enum format,
    const void* data,
    bool yflip,
    std::string& error
  ) const {
    if (data == nullptr || sourceWidth == 0 || sourceHeight == 0) {
      error = "capture failed: empty framebuffer";
      return false;
    }

    if (format != bgfx::TextureFormat::BGRA8 && format != bgfx::TextureFormat::RGBA8) {
      error = "capture failed: unsupported framebuffer format";
      return false;
    }

    const int frameWidth = static_cast<int>(sourceWidth);
    const int frameHeight = static_cast<int>(sourceHeight);
    const int x = std::clamp(request.x, 0, frameWidth - 1);
    const int y = std::clamp(request.y, 0, frameHeight - 1);
    const int cropWidth = std::clamp(request.width, 1, frameWidth - x);
    const int cropHeight = std::clamp(request.height, 1, frameHeight - y);

    BmpInfoHeader info;
    info.width = cropWidth;
    info.height = -cropHeight;
    info.imageSize = static_cast<std::uint32_t>(cropWidth * cropHeight * 4);

    BmpFileHeader file;
    file.offset = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    file.size = file.offset + info.imageSize;

    std::ofstream output(request.path, std::ios::binary);
    if (!output) {
      error = "capture failed: could not open output file";
      return false;
    }

    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info), sizeof(info));

    const auto* source = static_cast<const std::uint8_t*>(data);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(cropWidth) * 4u);

    for (int rowIndex = 0; rowIndex < cropHeight; ++rowIndex) {
      const int logicalY = y + rowIndex;
      const int sourceY = yflip ? (frameHeight - 1 - logicalY) : logicalY;
      const std::uint8_t* sourceRow =
        source + static_cast<std::size_t>(sourceY) * pitch + static_cast<std::size_t>(x) * 4u;

      if (format == bgfx::TextureFormat::BGRA8) {
        std::memcpy(row.data(), sourceRow, row.size());
      } else {
        for (int col = 0; col < cropWidth; ++col) {
          const std::uint8_t* rgba = sourceRow + static_cast<std::size_t>(col) * 4u;
          std::uint8_t* bgra = row.data() + static_cast<std::size_t>(col) * 4u;
          bgra[0] = rgba[2];
          bgra[1] = rgba[1];
          bgra[2] = rgba[0];
          bgra[3] = rgba[3];
        }
      }

      output.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size()));
    }

    return true;
  }

  mutable std::mutex mutex;
  ScreenshotCapture capture;
  std::string completedMessage;
  bool hasCompletedMessage = false;
};

struct AppState {
  SDL_Window* window = nullptr;
  int width = 1280;
  int height = 720;
  bool running = true;
  bool smokeTest = false;
  bool screenshotSmoke = false;
  bool focusMode = false;
  bool showStackPanel = true;
  bool showInspectorPanel = true;
  bool showStatusStrip = true;
  bool screenshotRequested = false;
  int frameCount = 0;
  float deltaSeconds = 1.0f / 60.0f;
  float elapsedSeconds = 0.0f;
  float screenshotStatusSeconds = 0.0f;
  std::string screenshotStatus;
  bool bgfxReady = false;
  bool imguiReady = false;
  std::ofstream smokeLog;
  std::chrono::steady_clock::time_point lastFrameTime{};
  ImVec2 visualizationCanvasOrigin{};
  ImVec2 visualizationCanvasSize{1280.0f, 720.0f};
  PrappyBgfxCallback bgfxCallback;
  VisualizationRenderer visualizationRenderer;
  VisualizationHost visualizations;

  bgfx::VertexLayout imguiLayout;
  Program imguiProgram;
  Texture fontTexture;
  bgfx::UniformHandle s_tex = BGFX_INVALID_HANDLE;
};

void logSmoke(AppState& state, const char* message) {
  if (state.smokeLog.is_open()) {
    state.smokeLog << message << '\n';
    state.smokeLog.flush();
  }
}

void setScreenshotStatus(AppState& state, const std::string& message, float seconds = 5.0f) {
  state.screenshotStatus = message;
  state.screenshotStatusSeconds = seconds;
}

std::filesystem::path nextScreenshotPath() {
  const std::filesystem::path captureDir = std::filesystem::current_path() / "captures";
  std::filesystem::create_directories(captureDir);

  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm localTime{};
#ifdef _WIN32
  localtime_s(&localTime, &time);
#else
  localtime_r(&time, &localTime);
#endif

  std::ostringstream filename;
  filename << "prappy_" << std::put_time(&localTime, "%Y%m%d_%H%M%S") << ".bmp";
  return captureDir / filename.str();
}

void requestScreenshot(AppState& state) {
  state.screenshotRequested = true;
  setScreenshotStatus(state, "Capture queued", 2.0f);
}

bool queueVisualizationCanvasCapture(AppState& state, std::filesystem::path& outputPath, std::string& error) {
  const int width = static_cast<int>(std::round(state.visualizationCanvasSize.x));
  const int height = static_cast<int>(std::round(state.visualizationCanvasSize.y));
  if (width <= 0 || height <= 0) {
    error = "capture failed: visualization canvas is empty";
    return false;
  }

  outputPath = nextScreenshotPath();
  state.bgfxCallback.queueScreenshot(outputPath, state.visualizationCanvasOrigin, state.visualizationCanvasSize);

  const bgfx::FrameBufferHandle backbuffer = BGFX_INVALID_HANDLE;
  const std::string bgfxPath = outputPath.string();
  bgfx::requestScreenShot(backbuffer, bgfxPath.c_str());
  return true;
}

void processScreenshotRequest(AppState& state) {
  if (!state.screenshotRequested) {
    return;
  }

  state.screenshotRequested = false;
  std::filesystem::path outputPath;
  std::string error;
  if (queueVisualizationCanvasCapture(state, outputPath, error)) {
    setScreenshotStatus(state, std::string("Capture queued: ") + outputPath.string(), 3.0f);
  } else {
    setScreenshotStatus(state, error, 7.0f);
  }
}

void pollScreenshotStatus(AppState& state) {
  std::string message;
  if (state.bgfxCallback.consumeScreenshotStatus(message)) {
    setScreenshotStatus(state, message, 7.0f);
  }
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

void applyPrappyStyle();

void initVisualizationRenderer(AppState& state) {
  state.visualizationRenderer.colorLayout
    .begin()
    .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
    .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
    .end();

  state.visualizationRenderer.colorProgram = loadProgram(
    "shaders/color_vs.bin",
    "shaders/color_fs.bin"
  );
}

void shutdownVisualizationRenderer(AppState& state) {
  if (bgfx::isValid(state.visualizationRenderer.colorProgram.handle)) {
    bgfx::destroy(state.visualizationRenderer.colorProgram.handle);
    state.visualizationRenderer.colorProgram.handle = BGFX_INVALID_HANDLE;
  }
}

void fatalMessage(const char* title, const std::string& message) {
#ifdef _WIN32
  MessageBoxA(nullptr, message.c_str(), title, MB_OK | MB_ICONERROR);
#else
  std::fprintf(stderr, "%s: %s\n", title, message.c_str());
#endif
}

void initBgfx(AppState& state) {
  bgfx::PlatformData pd{};

#ifdef _WIN32
  SDL_PropertiesID props = SDL_GetWindowProperties(state.window);
  pd.nwh = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
#endif

  bgfx::Init init;
  init.type = bgfx::RendererType::Count;
  init.resolution.width = static_cast<std::uint32_t>(state.width);
  init.resolution.height = static_cast<std::uint32_t>(state.height);
  init.resolution.reset = BGFX_RESET_VSYNC;
  init.platformData = pd;
  init.callback = &state.bgfxCallback;

  if (!bgfx::init(init)) {
    throw std::runtime_error("bgfx::init failed");
  }

  state.bgfxReady = true;

  bgfx::setDebug(BGFX_DEBUG_TEXT);
  bgfx::setViewMode(kVisualizationView, bgfx::ViewMode::Sequential);
  bgfx::setViewMode(kUiView, bgfx::ViewMode::Sequential);
  bgfx::setViewClear(
    kVisualizationView,
    BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
    0x101018ff,
    1.0f,
    0
  );
}

void initImGui(AppState& state) {
  logSmoke(state, "smoke: initImGui/check version");
  IMGUI_CHECKVERSION();
  logSmoke(state, "smoke: initImGui/create context");
  ImGui::CreateContext();
  state.imguiReady = true;

  logSmoke(state, "smoke: initImGui/io");
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.DisplaySize = ImVec2(static_cast<float>(state.width), static_cast<float>(state.height));

  logSmoke(state, "smoke: initImGui/style");
  applyPrappyStyle();

  logSmoke(state, "smoke: initImGui/layout");
  state.imguiLayout
    .begin()
    .add(bgfx::Attrib::Position, 2, bgfx::AttribType::Float)
    .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
    .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
    .end();

  logSmoke(state, "smoke: initImGui/uniforms");
  state.s_tex = bgfx::createUniform("s_tex", bgfx::UniformType::Sampler);

  logSmoke(state, "smoke: initImGui/program");
  state.imguiProgram = loadProgram("shaders/imgui_vs.bin", "shaders/imgui_fs.bin");

  unsigned char* pixels = nullptr;
  int texWidth = 0;
  int texHeight = 0;

  logSmoke(state, "smoke: initImGui/font pixels");
  io.Fonts->GetTexDataAsRGBA32(&pixels, &texWidth, &texHeight);

  logSmoke(state, "smoke: initImGui/font texture memory");
  const bgfx::Memory* mem = bgfx::copy(
    pixels,
    static_cast<std::uint32_t>(texWidth * texHeight * 4)
  );

  logSmoke(state, "smoke: initImGui/font texture");
  state.fontTexture.handle = bgfx::createTexture2D(
    static_cast<std::uint16_t>(texWidth),
    static_cast<std::uint16_t>(texHeight),
    false,
    1,
    bgfx::TextureFormat::RGBA8,
    0,
    mem
  );

  logSmoke(state, "smoke: initImGui/set font texture id");
  io.Fonts->SetTexID(static_cast<ImTextureID>(
    static_cast<std::uint64_t>(state.fontTexture.handle.idx) + 1u
  ));
  logSmoke(state, "smoke: initImGui/done");
}

void shutdownImGui(AppState& state) {
  if (bgfx::isValid(state.fontTexture.handle)) {
    bgfx::destroy(state.fontTexture.handle);
  }

  if (bgfx::isValid(state.imguiProgram.handle)) {
    bgfx::destroy(state.imguiProgram.handle);
  }

  if (bgfx::isValid(state.s_tex)) {
    bgfx::destroy(state.s_tex);
  }

  if (state.imguiReady) {
    ImGui::DestroyContext();
    state.imguiReady = false;
  }
}

void beginImGuiFrame(AppState& state) {
  const auto now = std::chrono::steady_clock::now();
  if (state.lastFrameTime.time_since_epoch().count() != 0) {
    const std::chrono::duration<float> elapsed = now - state.lastFrameTime;
    state.deltaSeconds = std::clamp(elapsed.count(), 1.0f / 240.0f, 1.0f / 15.0f);
  }
  state.lastFrameTime = now;
  state.elapsedSeconds += state.deltaSeconds;
  if (state.screenshotStatusSeconds > 0.0f) {
    state.screenshotStatusSeconds = std::max(0.0f, state.screenshotStatusSeconds - state.deltaSeconds);
    if (state.screenshotStatusSeconds == 0.0f) {
      state.screenshotStatus.clear();
    }
  }

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(static_cast<float>(state.width), static_cast<float>(state.height));
  io.DeltaTime = state.deltaSeconds;

  ImGui::NewFrame();
}

ImTextureID commandTextureId(const ImDrawCmd* command) {
#if IMGUI_VERSION_NUM >= 19191
  return command->GetTexID();
#else
  return command->TextureId;
#endif
}

bgfx::TextureHandle textureHandleFromImGuiId(ImTextureID textureId) {
  bgfx::TextureHandle texture;
  texture.idx = static_cast<std::uint16_t>(
    static_cast<std::uint64_t>(textureId) - 1u
  );
  return texture;
}

void renderImGui(AppState& state, ImDrawData* drawData) {
  if (drawData == nullptr || drawData->TotalVtxCount == 0) {
    return;
  }

  const float left = drawData->DisplayPos.x;
  const float right = drawData->DisplayPos.x + drawData->DisplaySize.x;
  const float top = drawData->DisplayPos.y;
  const float bottom = drawData->DisplayPos.y + drawData->DisplaySize.y;

  float ortho[16];
  bx::mtxOrtho(
    ortho,
    left,
    right,
    bottom,
    top,
    0.0f,
    100.0f,
    0.0f,
    bgfx::getCaps()->homogeneousDepth
  );
  float view[16];
  bx::mtxIdentity(view);
  bgfx::setViewTransform(kUiView, view, ortho);
  bgfx::setViewRect(
    kUiView,
    0,
    0,
    static_cast<std::uint16_t>(state.width),
    static_cast<std::uint16_t>(state.height)
  );

  const ImVec2 clipOffset = drawData->DisplayPos;

  for (int n = 0; n < drawData->CmdListsCount; ++n) {
    const ImDrawList* cmdList = drawData->CmdLists[n];

    const std::uint32_t vtxSize =
      static_cast<std::uint32_t>(cmdList->VtxBuffer.Size * sizeof(ImDrawVert));

    const std::uint32_t idxSize =
      static_cast<std::uint32_t>(cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));

    const bgfx::Memory* vtxMem = bgfx::copy(cmdList->VtxBuffer.Data, vtxSize);
    const bgfx::Memory* idxMem = bgfx::copy(cmdList->IdxBuffer.Data, idxSize);

    bgfx::VertexBufferHandle vbh = bgfx::createVertexBuffer(vtxMem, state.imguiLayout);
    bgfx::IndexBufferHandle ibh = bgfx::createIndexBuffer(
      idxMem,
      sizeof(ImDrawIdx) == 4 ? BGFX_BUFFER_INDEX32 : 0
    );

    for (int cmdIndex = 0; cmdIndex < cmdList->CmdBuffer.Size; ++cmdIndex) {
      const ImDrawCmd* pcmd = &cmdList->CmdBuffer[cmdIndex];

      if (pcmd->UserCallback != nullptr) {
        pcmd->UserCallback(cmdList, pcmd);
      } else {
        const float clipMinX = bx::max(pcmd->ClipRect.x - clipOffset.x, 0.0f);
        const float clipMinY = bx::max(pcmd->ClipRect.y - clipOffset.y, 0.0f);
        const float clipMaxX = bx::max(pcmd->ClipRect.z - clipOffset.x, 0.0f);
        const float clipMaxY = bx::max(pcmd->ClipRect.w - clipOffset.y, 0.0f);

        bgfx::setScissor(
          static_cast<std::uint16_t>(clipMinX),
          static_cast<std::uint16_t>(clipMinY),
          static_cast<std::uint16_t>(clipMaxX - clipMinX),
          static_cast<std::uint16_t>(clipMaxY - clipMinY)
        );

        const std::uint32_t numVertices =
          static_cast<std::uint32_t>(cmdList->VtxBuffer.Size) - pcmd->VtxOffset;

        bgfx::setVertexBuffer(0, vbh, pcmd->VtxOffset, numVertices);
        bgfx::setIndexBuffer(ibh, pcmd->IdxOffset, pcmd->ElemCount);

        bgfx::TextureHandle texture = textureHandleFromImGuiId(commandTextureId(pcmd));

        bgfx::setTexture(0, state.s_tex, texture);
        bgfx::setState(
          BGFX_STATE_WRITE_RGB |
          BGFX_STATE_WRITE_A |
          BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA)
        );

        bgfx::submit(kUiView, state.imguiProgram.handle);
      }
    }

    bgfx::destroy(vbh);
    bgfx::destroy(ibh);
  }
}

void handleEvent(AppState& state, const SDL_Event& event) {
  ImGuiIO& io = ImGui::GetIO();

  switch (event.type) {
    case SDL_EVENT_QUIT:
      state.running = false;
      break;

    case SDL_EVENT_WINDOW_RESIZED:
      state.width = event.window.data1;
      state.height = event.window.data2;
      bgfx::reset(
        static_cast<std::uint32_t>(state.width),
        static_cast<std::uint32_t>(state.height),
        BGFX_RESET_VSYNC
      );
      state.visualizations.resetRequested = true;
      break;

    case SDL_EVENT_MOUSE_MOTION:
      io.AddMousePosEvent(static_cast<float>(event.motion.x), static_cast<float>(event.motion.y));
      break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      int button = -1;
      if (event.button.button == SDL_BUTTON_LEFT) {
        button = 0;
      }
      if (event.button.button == SDL_BUTTON_RIGHT) {
        button = 1;
      }
      if (event.button.button == SDL_BUTTON_MIDDLE) {
        button = 2;
      }
      if (button >= 0) {
        io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
      }
      break;
    }

    case SDL_EVENT_MOUSE_WHEEL:
      io.AddMouseWheelEvent(static_cast<float>(event.wheel.x), static_cast<float>(event.wheel.y));
      break;

    default:
      break;
  }
}

const char* vendorName(std::uint16_t vendorId) {
  switch (vendorId) {
    case BGFX_PCI_ID_NVIDIA:
      return "NVIDIA";
    case BGFX_PCI_ID_AMD:
      return "AMD";
    case BGFX_PCI_ID_INTEL:
      return "Intel";
    case BGFX_PCI_ID_MICROSOFT:
      return "Microsoft";
    case BGFX_PCI_ID_SOFTWARE_RASTERIZER:
      return "Software";
    default:
      return "Unknown";
  }
}

double timerMilliseconds(int64_t begin, int64_t end, int64_t frequency) {
  if (frequency <= 0 || end <= begin) {
    return 0.0;
  }

  return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency);
}

void applyPrappyStyle() {
  ImGui::StyleColorsDark();

  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = 0.0f;
  style.ChildRounding = 5.0f;
  style.FrameRounding = 4.0f;
  style.PopupRounding = 5.0f;
  style.GrabRounding = 4.0f;
  style.TabRounding = 4.0f;
  style.ScrollbarRounding = 8.0f;
  style.WindowBorderSize = 0.0f;
  style.ChildBorderSize = 1.0f;
  style.FrameBorderSize = 0.0f;
  style.WindowPadding = ImVec2(10.0f, 10.0f);
  style.FramePadding = ImVec2(9.0f, 5.0f);
  style.ItemSpacing = ImVec2(8.0f, 7.0f);
  style.ItemInnerSpacing = ImVec2(7.0f, 5.0f);

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = ImVec4(0.91f, 0.94f, 0.97f, 1.0f);
  colors[ImGuiCol_TextDisabled] = ImVec4(0.52f, 0.58f, 0.64f, 1.0f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.035f, 0.043f, 0.058f, 0.96f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.064f, 0.083f, 0.94f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.045f, 0.052f, 0.068f, 0.98f);
  colors[ImGuiCol_Border] = ImVec4(0.19f, 0.23f, 0.28f, 0.82f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.08f, 0.10f, 0.13f, 0.92f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.12f, 0.16f, 0.20f, 0.96f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.16f, 0.22f, 0.28f, 1.0f);
  colors[ImGuiCol_TitleBg] = ImVec4(0.04f, 0.05f, 0.06f, 1.0f);
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.05f, 0.07f, 0.09f, 1.0f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.035f, 0.043f, 0.058f, 0.98f);
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.035f, 0.043f, 0.058f, 0.78f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.24f, 0.29f, 0.34f, 0.9f);
  colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.38f, 0.44f, 0.95f);
  colors[ImGuiCol_CheckMark] = ImVec4(0.31f, 0.76f, 0.86f, 1.0f);
  colors[ImGuiCol_SliderGrab] = ImVec4(0.31f, 0.76f, 0.86f, 0.88f);
  colors[ImGuiCol_Button] = ImVec4(0.10f, 0.13f, 0.17f, 0.96f);
  colors[ImGuiCol_ButtonHovered] = ImVec4(0.16f, 0.22f, 0.28f, 1.0f);
  colors[ImGuiCol_ButtonActive] = ImVec4(0.23f, 0.36f, 0.42f, 1.0f);
  colors[ImGuiCol_Header] = ImVec4(0.11f, 0.16f, 0.20f, 0.92f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.23f, 0.29f, 1.0f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.22f, 0.36f, 0.42f, 1.0f);
  colors[ImGuiCol_Separator] = ImVec4(0.20f, 0.24f, 0.29f, 0.75f);
  colors[ImGuiCol_Tab] = ImVec4(0.08f, 0.10f, 0.13f, 0.96f);
  colors[ImGuiCol_TabHovered] = ImVec4(0.19f, 0.29f, 0.35f, 1.0f);
  colors[ImGuiCol_TabSelected] = ImVec4(0.14f, 0.23f, 0.28f, 1.0f);
  colors[ImGuiCol_TableHeaderBg] = ImVec4(0.08f, 0.10f, 0.13f, 1.0f);
  colors[ImGuiCol_TableBorderStrong] = ImVec4(0.22f, 0.27f, 0.32f, 1.0f);
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.18f, 0.22f, 1.0f);
}

const char* buildConfiguration() {
#ifdef NDEBUG
  return "Release";
#else
  return "Debug";
#endif
}

const char* compilerLabel() {
#ifdef _MSC_VER
  static char label[32];
  std::snprintf(label, sizeof(label), "MSVC %d", _MSC_VER);
  return label;
#else
  return "C++20";
#endif
}

bool drawModeButton(const char* label, bool active, const ImVec2& size) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.38f, 0.43f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.23f, 0.47f, 0.53f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.16f, 0.32f, 0.38f, 1.0f));
  }

  const bool pressed = ImGui::Button(label, size);

  if (active) {
    ImGui::PopStyleColor(3);
  }

  return pressed;
}

void drawKeyValue(const char* key, const char* value) {
  ImGui::TextDisabled("%s", key);
  ImGui::SameLine(136.0f);
  ImGui::TextUnformatted(value);
}

void drawKeyValueNumber(const char* key, int value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%d", value);
  drawKeyValue(key, buffer);
}

void drawKeyValueFloat(const char* key, double value, const char* suffix = "") {
  char buffer[48];
  std::snprintf(buffer, sizeof(buffer), "%.2f%s", value, suffix);
  drawKeyValue(key, buffer);
}

void drawMetricTile(const char* label, const char* value, const ImVec4& accent) {
  ImGui::BeginGroup();
  ImGui::PushStyleColor(ImGuiCol_Text, accent);
  ImGui::TextUnformatted(value);
  ImGui::PopStyleColor();
  ImGui::TextDisabled("%s", label);
  ImGui::EndGroup();
}

void drawLibraryModule(
  const char* name,
  const char* role,
  const char* primary,
  const char* secondary,
  const ImVec4& accent
) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.065f, 0.078f, 0.098f, 0.96f));
  ImGui::BeginChild(name, ImVec2(0.0f, 86.0f), ImGuiChildFlags_Borders);
  ImGui::PushStyleColor(ImGuiCol_Text, accent);
  ImGui::TextUnformatted(name);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("%s", role);
  ImGui::Separator();
  ImGui::TextUnformatted(primary);
  ImGui::TextDisabled("%s", secondary);
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void drawVisualizationSelector(AppState& state) {
  const float available = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
  const float gap = 6.0f;
  const float width = std::clamp((available - gap * 2.0f) / 3.0f, 92.0f, 154.0f);
  const ImVec2 size(width, 30.0f);

  if (drawModeButton(
    visualizationShortName(VisualizationId::RandomLines2D),
    state.visualizations.active == VisualizationId::RandomLines2D,
    size
  )) {
    state.visualizations.setActive(VisualizationId::RandomLines2D);
  }
  ImGui::SameLine(0.0f, gap);
  if (drawModeButton(
    visualizationShortName(VisualizationId::Starfield3D),
    state.visualizations.active == VisualizationId::Starfield3D,
    size
  )) {
    state.visualizations.setActive(VisualizationId::Starfield3D);
  }
  ImGui::SameLine(0.0f, gap);
  if (drawModeButton(
    visualizationShortName(VisualizationId::OahuFlyover),
    state.visualizations.active == VisualizationId::OahuFlyover,
    size
  )) {
    state.visualizations.setActive(VisualizationId::OahuFlyover);
  }
}

void drawCommandBar(AppState& state) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 10.0f));
  ImGui::BeginChild(
    "CommandBar",
    ImVec2(0.0f, 66.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );

  if (ImGui::BeginTable("CommandBarLayout", 3, ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Brand", ImGuiTableColumnFlags_WidthFixed, 210.0f);
    ImGui::TableSetupColumn("Modes", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 410.0f);
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted("PRAPPY");
    ImGui::TextDisabled("SDL3 / bgfx / Dear ImGui");

    ImGui::TableSetColumnIndex(1);
    drawVisualizationSelector(state);
    ImGui::TextDisabled("%s", visualizationSpaceLabel(state.visualizations.active));

    ImGui::TableSetColumnIndex(2);
    if (ImGui::Button(state.focusMode ? "Workspace" : "Focus", ImVec2(92.0f, 30.0f))) {
      state.focusMode = !state.focusMode;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(82.0f, 30.0f))) {
      state.visualizations.resetRequested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture", ImVec2(92.0f, 30.0f))) {
      requestScreenshot(state);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Diagnostics", &state.visualizations.showStatus);
    ImGui::TextDisabled("%s / %s", buildConfiguration(), compilerLabel());

    ImGui::EndTable();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void drawStackPanel(AppState& state) {
  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();

  char sdlPrimary[64];
  std::snprintf(sdlPrimary, sizeof(sdlPrimary), "%d x %d window", state.width, state.height);

  char bgfxPrimary[96];
  std::snprintf(
    bgfxPrimary,
    sizeof(bgfxPrimary),
    "%s / %s",
    caps ? bgfx::getRendererName(caps->rendererType) : "renderer",
    caps ? vendorName(caps->vendorId) : "GPU"
  );

  char imguiPrimary[64];
  std::snprintf(imguiPrimary, sizeof(imguiPrimary), "%.1f ms UI tick", state.deltaSeconds * 1000.0f);

  char buildPrimary[64];
  std::snprintf(buildPrimary, sizeof(buildPrimary), "%s C++20", buildConfiguration());

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
  ImGui::BeginChild("StackPanel", ImVec2(284.0f, 0.0f), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("Native Stack");
  ImGui::TextDisabled("runtime surface");
  ImGui::Separator();

  drawLibraryModule(
    "SDL3",
    "window/input",
    sdlPrimary,
    "event pump + native handle",
    ImVec4(0.37f, 0.78f, 0.92f, 1.0f)
  );
  drawLibraryModule(
    "bgfx",
    "renderer",
    bgfxPrimary,
    stats ? "custom transient vertex passes" : "initializing",
    ImVec4(0.58f, 0.84f, 0.46f, 1.0f)
  );
  drawLibraryModule(
    "Dear ImGui",
    "tool UI",
    imguiPrimary,
    "menus, tabs, tables, overlays",
    ImVec4(0.94f, 0.70f, 0.36f, 1.0f)
  );
  drawLibraryModule(
    "CMake/Ninja",
    compilerLabel(),
    buildPrimary,
    "scripted Windows build",
    ImVec4(0.84f, 0.62f, 0.92f, 1.0f)
  );

  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void drawRendererTab(AppState& state) {
  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();
  const double renderCpuMs = stats
    ? timerMilliseconds(stats->cpuTimeBegin, stats->cpuTimeEnd, stats->cpuTimerFreq)
    : 0.0;
  const double gpuMs = stats
    ? timerMilliseconds(stats->gpuTimeBegin, stats->gpuTimeEnd, stats->gpuTimerFreq)
    : 0.0;

  char frameValue[32];
  char cpuValue[32];
  char gpuValue[32];
  std::snprintf(frameValue, sizeof(frameValue), "%.2f ms", state.deltaSeconds * 1000.0f);
  std::snprintf(cpuValue, sizeof(cpuValue), "%.2f ms", renderCpuMs);
  std::snprintf(gpuValue, sizeof(gpuValue), "%.2f ms", gpuMs);

  if (ImGui::BeginTable("RendererMetrics", 3, ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    drawMetricTile("frame", frameValue, ImVec4(0.38f, 0.82f, 0.92f, 1.0f));
    ImGui::TableNextColumn();
    drawMetricTile("CPU", cpuValue, ImVec4(0.80f, 0.88f, 0.52f, 1.0f));
    ImGui::TableNextColumn();
    drawMetricTile("GPU", gpuValue, ImVec4(0.94f, 0.68f, 0.42f, 1.0f));
    ImGui::EndTable();
  }

  ImGui::Separator();
  if (caps) {
    drawKeyValue("Renderer", bgfx::getRendererName(caps->rendererType));
    drawKeyValue("Vendor", vendorName(caps->vendorId));
    drawKeyValueNumber("Device", caps->deviceId);
    drawKeyValueNumber("GPUs", static_cast<int>(caps->numGPUs));
    drawKeyValue("Compute", (caps->supported & BGFX_CAPS_COMPUTE) ? "available" : "unavailable");
  }
  if (stats) {
    drawKeyValueNumber("Draw calls", static_cast<int>(stats->numDraw));
    drawKeyValueNumber("Transient VB", static_cast<int>(stats->transientVbUsed));
  }
}

void drawCameraControls(AppState& state) {
  const VisualizationDescriptor& descriptor = visualizationDescriptor(state.visualizations.active);
  if (!descriptor.usesCamera) {
    ImGui::TextDisabled("Camera: not used by this module");
    return;
  }

  CameraRig& camera = state.visualizations.camera;
  ImGui::SeparatorText("Camera");

  if (descriptor.hasAutoCamera) {
    bool autoCamera = !camera.manual;
    if (ImGui::Checkbox("Auto route", &autoCamera)) {
      camera.manual = !autoCamera;
    }
    ImGui::SliderFloat("Route speed", &camera.routeSpeed, 0.15f, 3.0f, "%.2f");
  } else {
    ImGui::Checkbox("Manual look", &camera.manual);
  }

  ImGui::SliderFloat("FOV", &camera.fovDegrees, 38.0f, 96.0f, "%.0f");

  if (camera.manual) {
    ImGui::SliderFloat("Yaw", &camera.yaw, -kPi, kPi, "%.2f");
    ImGui::SliderFloat("Pitch", &camera.pitch, -1.15f, 1.25f, "%.2f");

    if (state.visualizations.active == VisualizationId::OahuFlyover) {
      ImGui::SliderFloat("Distance", &camera.distance, 2.2f, 34.0f, "%.1f");
      ImGui::SliderFloat("Target X", &camera.target.x, -8.0f, 8.0f, "%.2f");
      ImGui::SliderFloat("Target Y", &camera.target.y, -1.0f, 4.0f, "%.2f");
      ImGui::SliderFloat("Target Z", &camera.target.z, -8.0f, 8.0f, "%.2f");
    }
  }

  if (ImGui::Button("Reset Camera", ImVec2(-1.0f, 30.0f))) {
    camera.resetFor(state.visualizations.active);
  }
}

void drawVisualizationTab(AppState& state) {
  drawVisualizationSelector(state);
  ImGui::Separator();
  drawKeyValue("Active", visualizationName(state.visualizations.active));
  drawKeyValue("Space", visualizationSpaceLabel(state.visualizations.active));
  drawKeyValue("Primitive", visualizationDescriptor(state.visualizations.active).primitiveLabel);
  state.visualizations.activeModule().drawInspector();
  drawCameraControls(state);

  ImGui::Separator();
  ImGui::Checkbox("Renderer overlay", &state.visualizations.showStatus);
  ImGui::Checkbox("Focus mode", &state.focusMode);
  if (ImGui::Button("Reset Active Visualization", ImVec2(-1.0f, 30.0f))) {
    state.visualizations.resetRequested = true;
  }
  if (ImGui::Button("Capture Screenshot", ImVec2(-1.0f, 30.0f))) {
    requestScreenshot(state);
  }
  if (!state.screenshotStatus.empty()) {
    ImGui::TextWrapped("%s", state.screenshotStatus.c_str());
  }
}

void drawDataTab() {
  drawKeyValue("Oahu relation", "OSM 3489649");
  drawKeyValue("Coastline", "Nominatim GeoJSON");
  drawKeyValue("Elevation", "USGS EPQS meters");
  drawKeyValueNumber("Grid width", kOahuGridWidth);
  drawKeyValueNumber("Grid height", kOahuGridHeight);
  drawKeyValueNumber("Coast samples", kOahuCoastlinePointCount);
  ImGui::Separator();
  drawKeyValue("Topology file", "src/oahu_topology.h");
  drawKeyValue("Refresh tool", "tools/fetch_oahu_topology.py");
}

void drawInspectorPanel(AppState& state) {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 12.0f));
  ImGui::BeginChild("InspectorPanel", ImVec2(340.0f, 0.0f), ImGuiChildFlags_Borders);
  ImGui::TextUnformatted("Inspector");
  ImGui::TextDisabled("%s", visualizationName(state.visualizations.active));
  ImGui::Separator();

  if (ImGui::BeginTabBar("InspectorTabs")) {
    if (ImGui::BeginTabItem("Renderer")) {
      drawRendererTab(state);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Visual")) {
      drawVisualizationTab(state);
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Data")) {
      drawDataTab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void drawStatusStrip(AppState& state) {
  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 5.0f));
  ImGui::BeginChild(
    "StatusStrip",
    ImVec2(0.0f, 32.0f),
    ImGuiChildFlags_Borders,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
  );
  ImGui::Text(
    "%s | %s | %.0f x %.0f",
    caps ? bgfx::getRendererName(caps->rendererType) : "bgfx",
    visualizationName(state.visualizations.active),
    state.visualizationCanvasSize.x,
    state.visualizationCanvasSize.y
  );
  ImGui::SameLine();
  ImGui::TextDisabled(
    "| frame %.2f ms | draws %u | %s",
    state.deltaSeconds * 1000.0f,
    stats ? stats->numDraw : 0u,
    state.focusMode ? "focus" : "workspace"
  );
  if (!state.screenshotStatus.empty()) {
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", state.screenshotStatus.c_str());
  }
  ImGui::EndChild();
  ImGui::PopStyleVar();
}

void handleVisualizationCanvasInput(AppState& state) {
  const VisualizationDescriptor& descriptor = visualizationDescriptor(state.visualizations.active);
  if (!descriptor.usesCamera) {
    return;
  }

  const bool hovered = ImGui::IsItemHovered();
  const bool active = ImGui::IsItemActive();
  if (!hovered && !active) {
    return;
  }

  ImGuiIO& io = ImGui::GetIO();
  CameraRig& camera = state.visualizations.camera;

  if (hovered && std::abs(io.MouseWheel) > 0.001f) {
    camera.zoom(state.visualizations.active, io.MouseWheel);
  }

  if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
    camera.orbit(io.MouseDelta);
  }

  if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
    camera.pan(io.MouseDelta);
  }
}

void drawVisualizationStatus(AppState& state, const ImVec2& canvasOrigin) {
  if (!state.visualizations.showStatus) {
    return;
  }

  const bgfx::Caps* caps = bgfx::getCaps();
  const bgfx::Stats* stats = bgfx::getStats();
  const double renderCpuMs = stats
    ? timerMilliseconds(stats->cpuTimeBegin, stats->cpuTimeEnd, stats->cpuTimerFreq)
    : 0.0;
  const double gpuMs = stats
    ? timerMilliseconds(stats->gpuTimeBegin, stats->gpuTimeEnd, stats->gpuTimerFreq)
    : 0.0;
  const float desiredHeight = state.visualizations.active == VisualizationId::OahuFlyover
    ? 306.0f
    : 278.0f;
  const float maxHeight = std::max(state.visualizationCanvasSize.y - 32.0f, 160.0f);
  const float panelHeight = std::min(desiredHeight, maxHeight);
  const bool needsScrollbar = panelHeight + 0.5f < desiredHeight;
  const ImGuiWindowFlags panelFlags = needsScrollbar
    ? ImGuiWindowFlags_None
    : ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

  ImGui::SetCursorScreenPos(addVec2(canvasOrigin, ImVec2(16.0f, 16.0f)));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(8, 10, 18, 218));
  ImGui::BeginChild(
    "VisualizationStatus",
    ImVec2(390.0f, panelHeight),
    ImGuiChildFlags_Borders,
    panelFlags
  );

  ImGui::TextUnformatted(visualizationName(state.visualizations.active));
  ImGui::Separator();
  if (caps) {
    ImGui::Text("Renderer: %s", bgfx::getRendererName(caps->rendererType));
    ImGui::Text(
      "GPU: %s 0x%04x:0x%04x",
      vendorName(caps->vendorId),
      caps->vendorId,
      caps->deviceId
    );
    ImGui::Text("Enumerated GPUs: %u", static_cast<unsigned>(caps->numGPUs));
    ImGui::Text("Compute shaders: %s", (caps->supported & BGFX_CAPS_COMPUTE) ? "yes" : "no");
  }
  ImGui::Text("App frame: %.2f ms", state.deltaSeconds * 1000.0f);
  ImGui::Text("Render CPU: %.2f ms", renderCpuMs);
  ImGui::Text("GPU frame: %.2f ms", gpuMs);
  if (stats) {
    ImGui::Text("Draw calls: %u", stats->numDraw);
    ImGui::Text(
      "Transient VB: %d / %u",
      stats->transientVbUsed,
      caps ? caps->limits.maxTransientVbSize : 0u
    );
  }
  if (state.visualizations.active == VisualizationId::OahuFlyover) {
    ImGui::Text(
      "Oahu grid: %d x %d, max %.0f m",
      kOahuGridWidth,
      kOahuGridHeight,
      kOahuMaxElevationMeters
    );
  }
  ImGui::Text("Canvas: %.0f x %.0f", state.visualizationCanvasSize.x, state.visualizationCanvasSize.y);

  if (ImGui::Button("Reset")) {
    state.visualizations.resetRequested = true;
  }
  ImGui::SameLine();
  if (ImGui::Button("Hide")) {
    state.visualizations.showStatus = false;
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

void drawAppUi(AppState& state) {
  ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->WorkPos);
  ImGui::SetNextWindowSize(viewport->WorkSize);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

  const ImGuiWindowFlags windowFlags =
    ImGuiWindowFlags_NoTitleBar |
    ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoCollapse |
    ImGuiWindowFlags_NoBringToFrontOnFocus |
    ImGuiWindowFlags_NoNavFocus |
    ImGuiWindowFlags_NoBackground |
    ImGuiWindowFlags_MenuBar;

  ImGui::Begin("PrappyVisualizationHost", nullptr, windowFlags);

  if (ImGui::BeginMenuBar()) {
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("Reset Visualization")) {
        state.visualizations.resetRequested = true;
      }
      if (ImGui::MenuItem("Reset Camera")) {
        state.visualizations.camera.resetFor(state.visualizations.active);
      }
      if (ImGui::MenuItem("Capture Visualization")) {
        requestScreenshot(state);
      }
      ImGui::Separator();
      if (ImGui::MenuItem("Quit")) {
        state.running = false;
      }
      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Visualization")) {
      if (ImGui::MenuItem(
        "Random Lines 2D",
        nullptr,
        state.visualizations.active == VisualizationId::RandomLines2D
      )) {
        state.visualizations.setActive(VisualizationId::RandomLines2D);
      }

      if (ImGui::MenuItem(
        "Infinite Starfield",
        nullptr,
        state.visualizations.active == VisualizationId::Starfield3D
      )) {
        state.visualizations.setActive(VisualizationId::Starfield3D);
      }

      if (ImGui::MenuItem(
        "Oahu Flyover",
        nullptr,
        state.visualizations.active == VisualizationId::OahuFlyover
      )) {
        state.visualizations.setActive(VisualizationId::OahuFlyover);
      }

      ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
      ImGui::MenuItem("Renderer Diagnostics", nullptr, &state.visualizations.showStatus);
      ImGui::MenuItem("Focus Mode", nullptr, &state.focusMode);
      ImGui::Separator();
      ImGui::MenuItem("Stack Panel", nullptr, &state.showStackPanel);
      ImGui::MenuItem("Inspector Panel", nullptr, &state.showInspectorPanel);
      ImGui::MenuItem("Status Strip", nullptr, &state.showStatusStrip);
      ImGui::EndMenu();
    }

    ImGui::Separator();
    ImGui::TextUnformatted(visualizationName(state.visualizations.active));
    ImGui::EndMenuBar();
  }

  if (!state.focusMode) {
    drawCommandBar(state);
  }

  const float statusStripHeight = (!state.focusMode && state.showStatusStrip) ? 36.0f : 0.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 8.0f));
  ImGui::BeginChild(
    "Workspace",
    ImVec2(0.0f, -statusStripHeight),
    false,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground
  );

  const bool showStack = !state.focusMode && state.showStackPanel;
  const bool showInspector = !state.focusMode && state.showInspectorPanel;
  const int workspaceColumns = 1 + (showStack ? 1 : 0) + (showInspector ? 1 : 0);

  if (ImGui::BeginTable(
    "WorkspaceLayout",
    workspaceColumns,
    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoPadOuterX
  )) {
    if (showStack) {
      ImGui::TableSetupColumn("Stack", ImGuiTableColumnFlags_WidthFixed, 284.0f);
    }
    ImGui::TableSetupColumn("Visualization", ImGuiTableColumnFlags_WidthStretch);
    if (showInspector) {
      ImGui::TableSetupColumn("Inspector", ImGuiTableColumnFlags_WidthFixed, 340.0f);
    }

    ImGui::TableNextRow();
    int column = 0;

    if (showStack) {
      ImGui::TableSetColumnIndex(column++);
      drawStackPanel(state);
    }

    ImGui::TableSetColumnIndex(column++);
    ImGui::BeginChild(
      "VisualizationRegion",
      ImVec2(0.0f, 0.0f),
      false,
      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground
    );

    const ImVec2 canvasOrigin = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    canvasSize.x = std::max(canvasSize.x, 1.0f);
    canvasSize.y = std::max(canvasSize.y, 1.0f);

    ImGui::InvisibleButton("VisualizationCanvas", canvasSize);
    handleVisualizationCanvasInput(state);

    state.visualizationCanvasOrigin = canvasOrigin;
    state.visualizationCanvasSize = canvasSize;
    drawVisualizationStatus(state, canvasOrigin);

    ImGui::EndChild();

    if (showInspector) {
      ImGui::TableSetColumnIndex(column++);
      drawInspectorPanel(state);
    }

    ImGui::EndTable();
  }

  ImGui::EndChild();
  ImGui::PopStyleVar();

  if (!state.focusMode && state.showStatusStrip) {
    drawStatusStrip(state);
  }

  ImGui::End();
  ImGui::PopStyleVar(3);
}

std::uint32_t visualizationClearColor(VisualizationId id) {
  switch (id) {
    case VisualizationId::RandomLines2D:
      return 0x07090fff;
    case VisualizationId::Starfield3D:
      return 0x02040aff;
    case VisualizationId::OahuFlyover:
      return 0x78c7efff;
  }

  return 0x101018ff;
}

void renderVisualization(AppState& state) {
  const int x = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasOrigin.x)),
    0,
    std::max(state.width - 1, 0)
  );
  const int y = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasOrigin.y)),
    0,
    std::max(state.height - 1, 0)
  );
  const int width = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasSize.x)),
    1,
    std::max(state.width - x, 1)
  );
  const int height = std::clamp(
    static_cast<int>(std::round(state.visualizationCanvasSize.y)),
    1,
    std::max(state.height - y, 1)
  );

  bgfx::setViewRect(
    kVisualizationView,
    static_cast<std::uint16_t>(x),
    static_cast<std::uint16_t>(y),
    static_cast<std::uint16_t>(width),
    static_cast<std::uint16_t>(height)
  );
  bgfx::setViewClear(
    kVisualizationView,
    BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH,
    visualizationClearColor(state.visualizations.active),
    1.0f,
    0
  );

  const bgfx::Caps* caps = bgfx::getCaps();
  const CameraRig& camera = state.visualizations.camera;
  float view[16];
  float projection[16];

  if (state.visualizations.active == VisualizationId::OahuFlyover) {
    bx::Vec3 eye = {0.0f, 0.0f, 0.0f};
    bx::Vec3 at = {0.0f, 0.0f, 0.0f};
    if (camera.manual) {
      eye = camera.orbitEye();
      at = camera.target;
    } else {
      const float cycle = std::fmod(state.elapsedSeconds * 0.025f * camera.routeSpeed, 1.0f);
      const float route = cycle * 2.0f - 1.0f;
      const float sway = std::sin(state.elapsedSeconds * 0.35f) * 0.55f;
      eye = {sway, 2.65f, 7.4f - route * 8.5f};
      at = {sway * 0.25f, 0.42f, 4.1f - route * 8.5f};
    }
    bx::mtxLookAt(view, eye, at);
    bx::mtxProj(
      projection,
      camera.fovDegrees,
      static_cast<float>(width) / static_cast<float>(height),
      0.05f,
      80.0f,
      caps->homogeneousDepth
    );
  } else if (state.visualizations.active == VisualizationId::Starfield3D) {
    const bx::Vec3 eye = {0.0f, 0.0f, 0.0f};
    const bx::Vec3 direction = camera.manual ? camera.lookDirection() : bx::Vec3{0.0f, 0.0f, -1.0f};
    const bx::Vec3 at = {direction.x, direction.y, direction.z};
    bx::mtxLookAt(view, eye, at);
    bx::mtxProj(
      projection,
      camera.fovDegrees,
      static_cast<float>(width) / static_cast<float>(height),
      0.05f,
      60.0f,
      caps->homogeneousDepth
    );
  } else {
    bx::mtxIdentity(view);
    bx::mtxOrtho(
      projection,
      0.0f,
      static_cast<float>(width),
      static_cast<float>(height),
      0.0f,
      0.0f,
      100.0f,
      0.0f,
      caps->homogeneousDepth
    );
  }

  bgfx::setViewTransform(kVisualizationView, view, projection);
  bgfx::touch(kVisualizationView);

  VisualizationContext context;
  context.renderer = &state.visualizationRenderer;
  context.viewId = kVisualizationView;
  context.size = ImVec2(static_cast<float>(width), static_cast<float>(height));
  context.deltaSeconds = state.deltaSeconds;
  context.elapsedSeconds = state.elapsedSeconds;

  state.visualizations.draw(context);
}

bool hasArg(int argc, char** argv, const char* expected) {
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == expected) {
      return true;
    }
  }

  return false;
}

VisualizationId visualizationFromArgs(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--visualization=starfield" || arg == "--starfield") {
      return VisualizationId::Starfield3D;
    }
    if (arg == "--visualization=random-lines" || arg == "--random-lines") {
      return VisualizationId::RandomLines2D;
    }
    if (arg == "--visualization=oahu" || arg == "--oahu") {
      return VisualizationId::OahuFlyover;
    }
  }

  return VisualizationId::RandomLines2D;
}

void cleanup(AppState& state) {
  if (state.imguiReady) {
    shutdownImGui(state);
  }

  if (state.bgfxReady) {
    shutdownVisualizationRenderer(state);
    bgfx::shutdown();
    state.bgfxReady = false;
  }

  if (state.window) {
    SDL_DestroyWindow(state.window);
    state.window = nullptr;
  }

  SDL_Quit();
}

} // namespace

int main(int argc, char** argv) {
  AppState state{};
  state.smokeTest = hasArg(argc, argv, "--smoke-test");
  state.screenshotSmoke = hasArg(argc, argv, "--screenshot-smoke");
  state.visualizations.setActive(visualizationFromArgs(argc, argv));
  if (state.smokeTest) {
    state.smokeLog.open("prappy_smoke.log", std::ios::out | std::ios::trunc);
    logSmoke(state, "smoke: start");
  }

  try {
    logSmoke(state, "smoke: SDL_SetMainReady");
    SDL_SetMainReady();

    logSmoke(state, "smoke: SDL_Init");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
      throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
    }

    logSmoke(state, "smoke: SDL_CreateWindow");
    state.window = SDL_CreateWindow(
      "prappy-native",
      state.width,
      state.height,
      SDL_WINDOW_RESIZABLE
    );

    if (!state.window) {
      throw std::runtime_error(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
    }

    logSmoke(state, "smoke: initBgfx");
    initBgfx(state);
    logSmoke(state, "smoke: initVisualizationRenderer");
    initVisualizationRenderer(state);
    logSmoke(state, "smoke: initImGui");
    initImGui(state);
    logSmoke(state, "smoke: entering main loop");

    while (state.running) {
      logSmoke(state, "smoke: poll events");
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        handleEvent(state, event);
      }

      logSmoke(state, "smoke: frame setup");
      bgfx::setViewRect(
        kUiView,
        0,
        0,
        static_cast<std::uint16_t>(state.width),
        static_cast<std::uint16_t>(state.height)
      );

      bgfx::touch(kUiView);

      logSmoke(state, "smoke: imgui frame");
      beginImGuiFrame(state);
      drawAppUi(state);
      logSmoke(state, "smoke: render visualization");
      renderVisualization(state);
      ImGui::Render();
      logSmoke(state, "smoke: render imgui");
      renderImGui(state, ImGui::GetDrawData());

      if (state.screenshotSmoke && state.frameCount == 1) {
        requestScreenshot(state);
      }

      processScreenshotRequest(state);
      logSmoke(state, "smoke: bgfx frame");
      bgfx::frame();
      pollScreenshotStatus(state);

      ++state.frameCount;
      if (state.smokeTest && state.frameCount >= 3) {
        state.running = false;
      }
    }

    logSmoke(state, "smoke: cleanup");
    cleanup(state);
    logSmoke(state, "smoke: done");
    return 0;
  } catch (const std::exception& ex) {
    logSmoke(state, ex.what());
    fatalMessage("prappy-native fatal error", ex.what());
    cleanup(state);
    return 1;
  }
}
