#include "../visualization_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <vector>

namespace prappy {
namespace {

constexpr std::uint32_t kParticleComputeGroupSize = 64;

struct ParticleVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float intensity = 1.0f;
  float r = 1.0f;
  float g = 1.0f;
  float b = 1.0f;
  float a = 1.0f;
};

struct GpuVec4 {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float w = 0.0f;
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
  bool preferComputeSimulation = true;
  bool computeSimulationActive = false;
  bool gpuStateDirty = true;
  ImVec2 lastSize{};

  bgfx::VertexLayout particleLayout;
  bgfx::VertexLayout computeStateLayout;
  bool layoutsReady = false;

  ShaderProgram particleProgram;
  ShaderProgram particleUpdateProgram;
  DynamicVertexBuffer particleVertexBuffer;
  DynamicVertexBuffer particleStateBuffer;
  bgfx::UniformHandle u_particleParams = BGFX_INVALID_HANDLE;

  std::uint32_t submittedVertexCount = 0;
  RenderPassDiagnostics passDiagnostics;

  const VisualizationDescriptor& descriptor() const override {
    return visualizationDescriptor(VisualizationId::ParticleField);
  }

  bool computeSupported() const {
    const bgfx::Caps* caps = bgfx::getCaps();
    return caps != nullptr && (caps->supported & BGFX_CAPS_COMPUTE) != 0;
  }

  void ensureLayouts() {
    if (layoutsReady) {
      return;
    }

    particleLayout
      .begin()
      .add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
      .add(bgfx::Attrib::Color0, 4, bgfx::AttribType::Float)
      .end();

    computeStateLayout
      .begin()
      .add(bgfx::Attrib::Position, 4, bgfx::AttribType::Float)
      .end();

    layoutsReady = true;
  }

  void ensureParticleProgram() {
    ensureLayouts();
    if (!particleProgram.isValid()) {
      particleProgram.loadGraphics(
        "particle_vs / particle_fs",
        "shaders/particle_vs.bin",
        "shaders/particle_fs.bin"
      );
    }
  }

  bool ensureComputeResources() {
    if (!computeSupported()) {
      return false;
    }

    ensureLayouts();
    ensureParticleProgram();

    if (!particleUpdateProgram.isValid()) {
      particleUpdateProgram.loadCompute("particle_update_cs", "shaders/particle_update_cs.bin");
    }

    if (!particleUpdateProgram.isValid()) {
      return false;
    }

    if (!bgfx::isValid(u_particleParams)) {
      u_particleParams = bgfx::createUniform("u_particleParams", bgfx::UniformType::Vec4, 3);
    }

    const std::uint32_t particleCount = static_cast<std::uint32_t>(particles.size());
    const std::uint32_t vertexCount = particleCount * 2u;
    particleVertexBuffer.ensure(
      vertexCount,
      static_cast<std::uint32_t>(sizeof(ParticleVertex)),
      particleLayout,
      BGFX_BUFFER_COMPUTE_READ_WRITE
    );

    if (
      gpuStateDirty ||
      !particleStateBuffer.isValid() ||
      particleStateBuffer.capacity() != particleCount * 2u
    ) {
      const std::vector<GpuVec4> state = makeGpuState();
      particleStateBuffer.createWithData(
        state.data(),
        static_cast<std::uint32_t>(state.size()),
        static_cast<std::uint32_t>(sizeof(GpuVec4)),
        computeStateLayout,
        BGFX_BUFFER_COMPUTE_READ_WRITE
      );
      gpuStateDirty = false;
    }

    return particleVertexBuffer.isValid() &&
      particleStateBuffer.isValid() &&
      bgfx::isValid(u_particleParams);
  }

  std::vector<GpuVec4> makeGpuState() const {
    std::vector<GpuVec4> state;
    state.reserve(particles.size() * 2u);

    for (const Particle& particle : particles) {
      state.push_back(GpuVec4{
        particle.position.x,
        particle.position.y,
        particle.position.z,
        particle.life
      });
      state.push_back(GpuVec4{
        particle.velocity.x,
        particle.velocity.y,
        particle.velocity.z,
        particle.hue
      });
    }

    return state;
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
    gpuStateDirty = true;
  }

  void reset(const ImVec2& size) override {
    lastSize = size;
    particles.clear();
    particles.resize(static_cast<std::size_t>(targetParticleCount));
    for (Particle& particle : particles) {
      resetParticle(particle, true);
    }

    submittedVertexCount = 0;
    gpuStateDirty = true;
  }

  void shutdown() override {
    particleStateBuffer.destroy();
    particleVertexBuffer.destroy();
    particleUpdateProgram.destroy();
    particleProgram.destroy();

    if (bgfx::isValid(u_particleParams)) {
      bgfx::destroy(u_particleParams);
      u_particleParams = BGFX_INVALID_HANDLE;
    }

    submittedVertexCount = 0;
    computeSimulationActive = false;
    passDiagnostics = {};
  }

  ImVec4 particleColor(float hue, float depth, float alpha) const {
    const ImVec4 color = ImColor::HSV(hue, 0.72f, 0.96f, alpha).Value;
    const float core = 0.78f + depth * 0.28f;
    return ImVec4(
      std::clamp(color.x * core, 0.0f, 1.0f),
      std::clamp(color.y * core, 0.0f, 1.0f),
      std::clamp(color.z * core, 0.0f, 1.0f),
      color.w
    );
  }

  void pushParticleLine(
    std::vector<ParticleVertex>& vertices,
    const bx::Vec3& a,
    const bx::Vec3& b,
    const ImVec4& color,
    float tailIntensity,
    float headIntensity
  ) const {
    vertices.push_back(ParticleVertex{a.x, a.y, a.z, tailIntensity, color.x, color.y, color.z, color.w});
    vertices.push_back(ParticleVertex{b.x, b.y, b.z, headIntensity, color.x, color.y, color.z, color.w});
  }

  void submitAxisLines(const VisualizationContext& context) const {
    std::vector<ColorVertex> axis;
    axis.reserve(4);
    const std::uint32_t axisColor = rgbaToAbgr(120, 180, 255, 42);
    pushLine(axis, -spread, 0.0f, -5.5f, spread, 0.0f, -5.5f, axisColor);
    pushLine(axis, 0.0f, -spread * 0.6f, -5.5f, 0.0f, spread * 0.6f, -5.5f, axisColor);
    submitColorVertices(*context.renderer, context.viewId, axis, ColorPrimitive::Lines, true, false);
  }

  void updateParticleBuffer(const std::vector<ParticleVertex>& vertices) {
    submittedVertexCount = static_cast<std::uint32_t>(vertices.size());
    if (submittedVertexCount == 0) {
      return;
    }

    ensureParticleProgram();
    particleVertexBuffer.update(
      vertices.data(),
      submittedVertexCount,
      static_cast<std::uint32_t>(sizeof(ParticleVertex)),
      particleLayout,
      BGFX_BUFFER_ALLOW_RESIZE
    );
  }

  void submitParticleBuffer(bgfx::ViewId viewId) const {
    if (
      submittedVertexCount == 0 ||
      !particleVertexBuffer.isValid() ||
      !particleProgram.isValid()
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

    bgfx::setVertexBuffer(0, particleVertexBuffer.get(), 0, submittedVertexCount);
    bgfx::setState(state);
    bgfx::submit(viewId, particleProgram.get());
  }

  bool drawComputeParticles(VisualizationContext& context) {
    if (!preferComputeSimulation || !ensureComputeResources()) {
      return false;
    }

    const float dt = std::min(context.deltaSeconds, 1.0f / 30.0f);
    const float params[12] = {
      dt,
      context.elapsedSeconds,
      speed,
      spread,
      turbulence,
      trailLength,
      hueShift,
      static_cast<float>(particles.size()),
      additiveTrails ? 1.0f : 0.0f,
      0.0f,
      0.0f,
      0.0f
    };

    bgfx::setUniform(u_particleParams, params, 3);
    bgfx::setBuffer(0, particleStateBuffer.get(), bgfx::Access::ReadWrite);
    bgfx::setBuffer(1, particleVertexBuffer.get(), bgfx::Access::Write);

    const std::uint32_t particleCount = static_cast<std::uint32_t>(particles.size());
    const std::uint32_t dispatchGroups =
      (particleCount + kParticleComputeGroupSize - 1u) / kParticleComputeGroupSize;
    bgfx::dispatch(context.viewId, particleUpdateProgram.get(), dispatchGroups, 1, 1);

    submittedVertexCount = particleCount * 2u;
    submitParticleBuffer(context.viewId);
    computeSimulationActive = true;

    passDiagnostics.dispatches = 1;
    passDiagnostics.drawCalls = 1;
    passDiagnostics.vertices = submittedVertexCount;
    passDiagnostics.uploadedBytes = particleStateBuffer.lastUploadBytes();
    passDiagnostics.bufferCapacity = particleVertexBuffer.capacity();
    return true;
  }

  void drawCpuParticles(VisualizationContext& context) {
    std::vector<ParticleVertex> vertices;
    vertices.reserve(particles.size() * 2u);

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
      const ImVec4 color = particleColor(hue, depth, alpha);

      const bx::Vec3 tail = {
        previous.x - particle.velocity.x * trailLength * dt,
        previous.y - particle.velocity.y * trailLength * dt,
        previous.z - particle.velocity.z * trailLength * dt
      };

      pushParticleLine(vertices, tail, particle.position, color, 0.34f, 1.0f);
    }

    updateParticleBuffer(vertices);
    submitParticleBuffer(context.viewId);
    computeSimulationActive = false;

    passDiagnostics.drawCalls = submittedVertexCount > 0 ? 1u : 0u;
    passDiagnostics.vertices = submittedVertexCount;
    passDiagnostics.uploadedBytes = particleVertexBuffer.lastUploadBytes();
    passDiagnostics.bufferCapacity = particleVertexBuffer.capacity();
  }

  void draw(VisualizationContext& context) override {
    if (
      particles.empty() ||
      static_cast<int>(particles.size()) != targetParticleCount ||
      distanceSquared(context.size, lastSize) > 96.0f * 96.0f
    ) {
      reset(context.size);
    }

    passDiagnostics = RenderPassDiagnostics{
      "Particle Field",
      particleProgram.label(),
      "retained dynamic vertex buffer",
      preferComputeSimulation && computeSupported()
        ? "GPU compute simulation"
        : "CPU simulation + GPU draw",
      nullptr,
      0,
      0,
      0,
      0,
      0,
      0,
      computeSupported(),
      false
    };

    if (!drawComputeParticles(context)) {
      drawCpuParticles(context);
    }

    passDiagnostics.shaderName = particleProgram.label();
    passDiagnostics.computeActive = computeSimulationActive;
    passDiagnostics.note = computeSimulationActive
      ? "particle state is updated by a bgfx compute dispatch"
      : "portable fallback updates particle state on CPU and uploads vertices";

    submitAxisLines(context);
  }

  void drawInspector() override {
    bool needsReset = false;
    const bool canCompute = computeSupported();

    ImGui::Text("Particles: %d", static_cast<int>(particles.size()));
    ImGui::Text("Backend: %s", computeSimulationActive ? "bgfx compute simulation" : "CPU simulation fallback");
    ImGui::Text("Compute support: %s", canCompute ? "available" : "unavailable");
    ImGui::Text("Submitted vertices: %u", submittedVertexCount);
    ImGui::Text("Buffer capacity: %u", particleVertexBuffer.capacity());
    ImGui::Text("Upload bytes: %u", particleVertexBuffer.lastUploadBytes());
    ImGui::Text("Shader: %s", particleProgram.label());
    if (canCompute) {
      ImGui::Checkbox("Prefer compute simulation", &preferComputeSimulation);
    } else {
      ImGui::BeginDisabled();
      ImGui::Checkbox("Prefer compute simulation", &preferComputeSimulation);
      ImGui::EndDisabled();
    }

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

  RenderPassDiagnostics renderPassDiagnostics() const override {
    return passDiagnostics;
  }
};

} // namespace

std::unique_ptr<IVisualizationModule> createParticleFieldVisualization() {
  return std::make_unique<ParticleFieldVisualization>();
}

} // namespace prappy
