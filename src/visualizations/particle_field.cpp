#include "../visualization_core.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <random>
#include <vector>

namespace prappy {
namespace {

struct ParticleVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  std::uint32_t abgr = 0xffffffffu;
  float intensity = 1.0f;
  float age = 0.0f;
};

struct ParticleFieldVisualization final : IVisualizationModule {
  struct Particle {
    bx::Vec3 position = {0.0f, 0.0f, 0.0f};
    bx::Vec3 velocity = {0.0f, 0.0f, 0.0f};
    float life = 1.0f;
    float hue = 0.0f;
  };

  std::mt19937 rng{0x9a771c1eu};
  std::vector<Particle> particles;
  int targetParticleCount = 1800;
  float speed = 1.0f;
  float spread = 5.0f;
  float turbulence = 0.42f;
  float trailLength = 0.18f;
  float hueShift = 0.08f;
  bool additiveTrails = true;
  ImVec2 lastSize{};
  bgfx::VertexLayout particleLayout;
  Program particleProgram;
  bgfx::DynamicVertexBufferHandle particleVertexBuffer = BGFX_INVALID_HANDLE;
  std::uint32_t particleBufferCapacity = 0;
  std::uint32_t submittedVertexCount = 0;
  bool particleLayoutReady = false;

  const VisualizationDescriptor& descriptor() const override {
    return visualizationDescriptor(VisualizationId::ParticleField);
  }

  void ensureParticleLayout() {
    if (particleLayoutReady) {
      return;
    }

    particleLayout
      .begin()
      .add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Uint8, true)
      .add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
      .end();
    particleLayoutReady = true;
  }

  void ensureParticleResources(std::uint32_t requiredVertexCount) {
    ensureParticleLayout();

    if (!bgfx::isValid(particleProgram.handle)) {
      particleProgram = loadProgram("shaders/particle_vs.bin", "shaders/particle_fs.bin");
    }

    if (
      !bgfx::isValid(particleVertexBuffer) ||
      particleBufferCapacity < requiredVertexCount
    ) {
      if (bgfx::isValid(particleVertexBuffer)) {
        bgfx::destroy(particleVertexBuffer);
        particleVertexBuffer = BGFX_INVALID_HANDLE;
      }

      particleBufferCapacity = requiredVertexCount;
      particleVertexBuffer = bgfx::createDynamicVertexBuffer(
        particleBufferCapacity,
        particleLayout,
        BGFX_BUFFER_ALLOW_RESIZE
      );
    }
  }

  void shutdown() override {
    if (bgfx::isValid(particleVertexBuffer)) {
      bgfx::destroy(particleVertexBuffer);
      particleVertexBuffer = BGFX_INVALID_HANDLE;
    }

    if (bgfx::isValid(particleProgram.handle)) {
      bgfx::destroy(particleProgram.handle);
      particleProgram.handle = BGFX_INVALID_HANDLE;
    }

    particleBufferCapacity = 0;
    submittedVertexCount = 0;
  }

  bx::Vec3 randomDirection() {
    const float yaw = randomFloat(rng, 0.0f, kPi * 2.0f);
    const float y = randomFloat(rng, -0.42f, 0.42f);
    const float radius = std::sqrt(std::max(1.0f - y * y, 0.0f));
    return {std::cos(yaw) * radius, y, std::sin(yaw) * radius};
  }

  void resetParticle(Particle& particle, bool anywhere) {
    const bx::Vec3 direction = randomDirection();
    const float depth = anywhere ? randomFloat(rng, 0.0f, 11.0f) : randomFloat(rng, 9.0f, 11.0f);
    const float orbit = randomFloat(rng, 0.0f, kPi * 2.0f);
    const float radius = randomFloat(rng, 0.08f, spread);

    particle.position = {
      std::cos(orbit) * radius,
      randomFloat(rng, -spread * 0.55f, spread * 0.55f),
      -depth
    };
    particle.velocity = {
      direction.x * randomFloat(rng, 0.12f, 0.72f),
      direction.y * randomFloat(rng, 0.12f, 0.52f),
      randomFloat(rng, 2.4f, 6.8f)
    };
    particle.life = randomFloat(rng, 0.35f, 1.0f);
    particle.hue = randomFloat(rng, 0.0f, 1.0f);
  }

  void resizeParticles() {
    targetParticleCount = std::clamp(targetParticleCount, 256, 6000);
    const std::size_t previousSize = particles.size();
    particles.resize(static_cast<std::size_t>(targetParticleCount));
    for (std::size_t i = previousSize; i < particles.size(); ++i) {
      resetParticle(particles[i], true);
    }
  }

  void reset(const ImVec2& size) override {
    lastSize = size;
    particles.clear();
    particles.resize(static_cast<std::size_t>(targetParticleCount));
    for (Particle& particle : particles) {
      resetParticle(particle, true);
    }
    submittedVertexCount = 0;
  }

  void pushParticleLine(
    std::vector<ParticleVertex>& vertices,
    const bx::Vec3& a,
    const bx::Vec3& b,
    std::uint32_t color,
    float tailIntensity,
    float headIntensity,
    float age
  ) const {
    vertices.push_back(ParticleVertex{a.x, a.y, a.z, color, tailIntensity, age});
    vertices.push_back(ParticleVertex{b.x, b.y, b.z, color, headIntensity, age});
  }

  void updateParticleBuffer(const std::vector<ParticleVertex>& vertices) {
    submittedVertexCount = static_cast<std::uint32_t>(vertices.size());
    if (submittedVertexCount == 0) {
      return;
    }

    ensureParticleResources(submittedVertexCount);
    if (!bgfx::isValid(particleVertexBuffer)) {
      return;
    }

    const bgfx::Memory* memory = bgfx::copy(
      vertices.data(),
      submittedVertexCount * static_cast<std::uint32_t>(sizeof(ParticleVertex))
    );
    bgfx::update(particleVertexBuffer, 0, memory);
  }

  void submitParticleBuffer(bgfx::ViewId viewId) const {
    if (
      submittedVertexCount == 0 ||
      !bgfx::isValid(particleVertexBuffer) ||
      !bgfx::isValid(particleProgram.handle)
    ) {
      return;
    }

    std::uint64_t state =
      BGFX_STATE_WRITE_RGB |
      BGFX_STATE_WRITE_A |
      BGFX_STATE_DEPTH_TEST_LESS |
      BGFX_STATE_PT_LINES;

    if (additiveTrails) {
      state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_ONE);
    } else {
      state |= BGFX_STATE_BLEND_FUNC(BGFX_STATE_BLEND_SRC_ALPHA, BGFX_STATE_BLEND_INV_SRC_ALPHA);
    }

    bgfx::setVertexBuffer(0, particleVertexBuffer, 0, submittedVertexCount);
    bgfx::setState(state);
    bgfx::submit(viewId, particleProgram.handle);
  }

  void draw(VisualizationContext& context) override {
    if (
      particles.empty() ||
      static_cast<int>(particles.size()) != targetParticleCount ||
      distanceSquared(context.size, lastSize) > 96.0f * 96.0f
    ) {
      reset(context.size);
    }

    std::vector<ParticleVertex> vertices;
    vertices.reserve(particles.size() * 2u + 128u);

    const std::uint32_t axisColor = rgbaToAbgr(120, 180, 255, 42);
    pushParticleLine(
      vertices,
      bx::Vec3{-spread, 0.0f, -5.5f},
      bx::Vec3{spread, 0.0f, -5.5f},
      axisColor,
      0.24f,
      0.24f,
      0.0f
    );
    pushParticleLine(
      vertices,
      bx::Vec3{0.0f, -spread * 0.6f, -5.5f},
      bx::Vec3{0.0f, spread * 0.6f, -5.5f},
      axisColor,
      0.24f,
      0.24f,
      0.0f
    );

    const float dt = std::min(context.deltaSeconds, 1.0f / 30.0f);
    for (Particle& particle : particles) {
      const bx::Vec3 previous = particle.position;
      const float swirl = std::sin(context.elapsedSeconds * 0.8f + particle.hue * kPi * 2.0f);

      particle.velocity.x += (-particle.position.y * 0.08f + swirl * 0.05f) * turbulence * dt;
      particle.velocity.y += (particle.position.x * 0.05f) * turbulence * dt;

      particle.position.x += particle.velocity.x * dt * speed;
      particle.position.y += particle.velocity.y * dt * speed;
      particle.position.z += particle.velocity.z * dt * speed;
      particle.life -= dt * 0.18f * speed;

      const float radial = std::sqrt(particle.position.x * particle.position.x + particle.position.y * particle.position.y);
      if (particle.position.z > 1.0f || radial > spread * 1.42f || particle.life <= 0.0f) {
        resetParticle(particle, false);
        continue;
      }

      const float depth = std::clamp((particle.position.z + 11.0f) / 12.0f, 0.0f, 1.0f);
      const float hue = std::fmod(particle.hue + context.elapsedSeconds * hueShift, 1.0f);
      const float alpha = additiveTrails
        ? std::clamp(0.18f + depth * 0.78f, 0.0f, 1.0f)
        : 0.66f;
      const std::uint32_t color = hsvToAbgr(hue, 0.72f, 0.96f, alpha);

      const bx::Vec3 tail = {
        previous.x - particle.velocity.x * trailLength * dt,
        previous.y - particle.velocity.y * trailLength * dt,
        previous.z - particle.velocity.z * trailLength * dt
      };

      pushParticleLine(
        vertices,
        tail,
        particle.position,
        color,
        0.34f,
        1.0f,
        1.0f - particle.life
      );
    }

    updateParticleBuffer(vertices);
    submitParticleBuffer(context.viewId);
  }

  void drawInspector() override {
    bool needsReset = false;
    ImGui::Text("Particles: %d", static_cast<int>(particles.size()));
    ImGui::TextUnformatted("Primitive: dynamic bgfx particle line buffer");
    ImGui::Text("Submitted vertices: %u", submittedVertexCount);
    ImGui::Text("Buffer capacity: %u", particleBufferCapacity);
    ImGui::TextUnformatted("Shader: particle_vs / particle_fs");

    int nextCount = targetParticleCount;
    if (ImGui::SliderInt("Count", &nextCount, 256, 6000)) {
      targetParticleCount = nextCount;
      resizeParticles();
    }
    needsReset |= ImGui::SliderFloat("Speed", &speed, 0.15f, 3.0f, "%.2f");
    needsReset |= ImGui::SliderFloat("Spread", &spread, 2.0f, 9.0f, "%.1f");
    ImGui::SliderFloat("Turbulence", &turbulence, 0.0f, 1.4f, "%.2f");
    ImGui::SliderFloat("Trail", &trailLength, 0.04f, 0.42f, "%.2f");
    ImGui::SliderFloat("Hue drift", &hueShift, 0.0f, 0.6f, "%.2f");
    ImGui::Checkbox("Bright trails", &additiveTrails);

    if (needsReset || ImGui::Button("Reset Particles", ImVec2(-1.0f, 28.0f))) {
      reset(lastSize);
    }
  }
};

} // namespace

std::unique_ptr<IVisualizationModule> createParticleFieldVisualization() {
  return std::make_unique<ParticleFieldVisualization>();
}

} // namespace prappy
