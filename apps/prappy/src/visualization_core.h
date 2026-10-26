#pragma once

#include "renderer.h"

#include <bgfx/bgfx.h>
#include <bx/math.h>
#include <imgui.h>

#include <memory>
#include <string_view>
#include <vector>

namespace prappy {

struct VisualizationContext {
  VisualizationRenderer* renderer = nullptr;
  bgfx::ViewId viewId = kVisualizationView;
  ImVec2 size{};
  float deltaSeconds = 1.0f / 60.0f;
  float elapsedSeconds = 0.0f;
  const struct OahuDiagnosticSettings* oahuDiagnostics = nullptr;
  const struct OahuRenderSettings* oahuRenderSettings = nullptr;
};

enum class VisualizationId {
  RandomLines2D,
  Starfield3D,
  OahuFlyover,
  ParticleField
};

enum class VisualizationPresetId {
  RandomLinesHero,
  StarfieldHero,
  OahuFlyover,
  OahuCenteredTopDown,
  OahuDebugMesh,
  ParticleFieldHero
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

struct VisualizationPresetDescriptor {
  VisualizationPresetId id;
  VisualizationId visualization;
  const char* name;
  const char* shortName;
  const char* slug;
  const char* description;
};

struct OahuDiagnosticSettings {
  bool topDown = false;
  bool showBackground = true;
  bool showFilledTerrain = true;
  bool showCoastline = true;
  bool showGrid = false;
  bool showRidges = true;
  bool showLandmarks = false;
};

struct OahuEnvironmentSettings {
  ImVec4 waterNear = ImVec4(0.04f, 0.39f, 0.62f, 1.0f);
  ImVec4 waterFar = ImVec4(0.52f, 0.80f, 0.90f, 1.0f);
  ImVec4 horizon = ImVec4(0.72f, 0.88f, 0.95f, 1.0f);
  float hazeStart = 7.0f;
  float hazeEnd = 34.0f;
  float hazeDensity = 0.58f;
  float waveStrength = 0.035f;
};

struct OahuLightingSettings {
  float sunYaw = -0.72f;
  float sunPitch = 0.92f;
  float ambient = 0.36f;
  float diffuse = 0.68f;
  float contrast = 1.10f;
};

struct OahuColorRampSettings {
  ImVec4 beach = ImVec4(0.72f, 0.67f, 0.45f, 1.0f);
  ImVec4 lowland = ImVec4(0.20f, 0.58f, 0.25f, 1.0f);
  ImVec4 ridge = ImVec4(0.47f, 0.58f, 0.33f, 1.0f);
  ImVec4 peak = ImVec4(0.72f, 0.68f, 0.58f, 1.0f);
  float lowlandStart = 0.08f;
  float ridgeStart = 0.38f;
  float peakStart = 0.72f;
};

struct OahuCameraPathSettings {
  float altitude = 2.70f;
  float routeSpeed = 0.82f;
  float lookahead = 3.35f;
  float bankStrength = 0.24f;
  float lateralSway = 0.55f;
};

struct OahuRenderSettings {
  OahuEnvironmentSettings environment;
  OahuLightingSettings lighting;
  OahuColorRampSettings colorRamp;
  OahuCameraPathSettings cameraPath;
};

const VisualizationDescriptor& visualizationDescriptor(VisualizationId id);
const char* visualizationName(VisualizationId id);
const char* visualizationShortName(VisualizationId id);
const char* visualizationSpaceLabel(VisualizationId id);
const VisualizationPresetDescriptor& visualizationPresetDescriptor(VisualizationPresetId id);
std::vector<VisualizationPresetDescriptor> visualizationPresetsFor(VisualizationId id);
VisualizationPresetId defaultVisualizationPreset(VisualizationId id);
bool tryParseVisualizationPreset(std::string_view slug, VisualizationPresetId& preset);

struct IVisualizationModule {
  virtual ~IVisualizationModule() = default;
  virtual const VisualizationDescriptor& descriptor() const = 0;
  virtual void shutdown() {}
  virtual void reset(const ImVec2& size) = 0;
  virtual void draw(VisualizationContext& context) = 0;
  virtual void drawInspector() = 0;
  virtual RenderPassDiagnostics renderPassDiagnostics() const { return {}; }
};

struct CameraRig {
  bool manual = false;
  float yaw = 0.0f;
  float pitch = 0.0f;
  float distance = 10.0f;
  float fovDegrees = 70.0f;
  float routeSpeed = 1.0f;
  bx::Vec3 target = {0.0f, 0.0f, 0.0f};

  void resetFor(VisualizationId id);
  void applyPreset(VisualizationPresetId id);
  bx::Vec3 lookDirection() const;
  bx::Vec3 orbitEye() const;
  void orbit(const ImVec2& delta);
  void pan(const ImVec2& delta);
  void zoom(VisualizationId id, float wheel);
};

struct VisualizationHost {
  VisualizationId active = VisualizationId::RandomLines2D;
  VisualizationPresetId activePreset = VisualizationPresetId::RandomLinesHero;
  bool showStatus = true;
  bool resetRequested = true;
  CameraRig camera;
  std::vector<std::unique_ptr<IVisualizationModule>> modules;

  VisualizationHost();
  IVisualizationModule& module(VisualizationId id);
  const IVisualizationModule& module(VisualizationId id) const;
  IVisualizationModule& activeModule();
  const IVisualizationModule& activeModule() const;
  void setActive(VisualizationId next);
  void setPreset(VisualizationPresetId preset);
  void resetActive(const ImVec2& size);
  void draw(VisualizationContext& context);
  void shutdown();
};

std::unique_ptr<IVisualizationModule> createRandomLinesVisualization();
std::unique_ptr<IVisualizationModule> createStarfieldVisualization();
std::unique_ptr<IVisualizationModule> createOahuFlyoverVisualization();
std::unique_ptr<IVisualizationModule> createParticleFieldVisualization();

} // namespace prappy
