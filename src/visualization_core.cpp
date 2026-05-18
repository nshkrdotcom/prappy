#include "visualization_core.h"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace prappy {

namespace {

const VisualizationPresetDescriptor kPresetDescriptors[] = {
  {
    VisualizationPresetId::RandomLinesHero,
    VisualizationId::RandomLines2D,
    "Random Lines / Hero",
    "Hero",
    "random-lines-hero",
    "Dense 2D line field framed for screenshots and demos"
  },
  {
    VisualizationPresetId::StarfieldHero,
    VisualizationId::Starfield3D,
    "Starfield / Hero",
    "Hero",
    "starfield-hero",
    "Forward-facing infinite travel view"
  },
  {
    VisualizationPresetId::OahuFlyover,
    VisualizationId::OahuFlyover,
    "Oahu / Flyover",
    "Flyover",
    "oahu-flyover",
    "Low bird-flight route over terrain with ocean horizon"
  },
  {
    VisualizationPresetId::OahuCenteredTopDown,
    VisualizationId::OahuFlyover,
    "Oahu / Centered Top Down",
    "Centered",
    "oahu-centered-top-down",
    "North-up terrain composition centered in the canvas"
  },
  {
    VisualizationPresetId::OahuDebugMesh,
    VisualizationId::OahuFlyover,
    "Oahu / Debug Mesh",
    "Debug Mesh",
    "oahu-debug-mesh",
    "North-up topology view with grid and landmark controls"
  },
  {
    VisualizationPresetId::ParticleFieldHero,
    VisualizationId::ParticleField,
    "Particles / Hero",
    "Hero",
    "particles-hero",
    "Orbiting particle field framed as a GPU demo"
  }
};

} // namespace

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
    },
    {
      VisualizationId::ParticleField,
      "GPU Particle Field",
      "Particles",
      "3D particle pass",
      "particle streak lines",
      true,
      false
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

const VisualizationPresetDescriptor& visualizationPresetDescriptor(VisualizationPresetId id) {
  for (const VisualizationPresetDescriptor& descriptor : kPresetDescriptors) {
    if (descriptor.id == id) {
      return descriptor;
    }
  }

  return kPresetDescriptors[0];
}

std::vector<VisualizationPresetDescriptor> visualizationPresetsFor(VisualizationId id) {
  std::vector<VisualizationPresetDescriptor> presets;
  for (const VisualizationPresetDescriptor& descriptor : kPresetDescriptors) {
    if (descriptor.visualization == id) {
      presets.push_back(descriptor);
    }
  }
  return presets;
}

VisualizationPresetId defaultVisualizationPreset(VisualizationId id) {
  switch (id) {
    case VisualizationId::RandomLines2D:
      return VisualizationPresetId::RandomLinesHero;
    case VisualizationId::Starfield3D:
      return VisualizationPresetId::StarfieldHero;
    case VisualizationId::OahuFlyover:
      return VisualizationPresetId::OahuFlyover;
    case VisualizationId::ParticleField:
      return VisualizationPresetId::ParticleFieldHero;
  }

  return VisualizationPresetId::RandomLinesHero;
}

bool tryParseVisualizationPreset(std::string_view slug, VisualizationPresetId& preset) {
  for (const VisualizationPresetDescriptor& descriptor : kPresetDescriptors) {
    if (slug == descriptor.slug) {
      preset = descriptor.id;
      return true;
    }
  }

  return false;
}

void CameraRig::resetFor(VisualizationId id) {
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
      yaw = kPi;
      pitch = 0.25f;
      distance = 10.0f;
      fovDegrees = 62.0f;
      target = {0.0f, 0.55f, -0.6f};
      break;
    case VisualizationId::ParticleField:
      yaw = -0.15f;
      pitch = 0.18f;
      distance = 7.0f;
      fovDegrees = 68.0f;
      target = {0.0f, 0.0f, -4.5f};
      break;
  }
}

void CameraRig::applyPreset(VisualizationPresetId id) {
  resetFor(visualizationPresetDescriptor(id).visualization);

  switch (id) {
    case VisualizationPresetId::RandomLinesHero:
      break;
    case VisualizationPresetId::StarfieldHero:
      manual = false;
      fovDegrees = 66.0f;
      target = {0.0f, 0.0f, -1.0f};
      break;
    case VisualizationPresetId::OahuFlyover:
      manual = false;
      routeSpeed = 0.82f;
      fovDegrees = 60.0f;
      target = {0.0f, 0.55f, -0.6f};
      break;
    case VisualizationPresetId::OahuCenteredTopDown:
      manual = false;
      routeSpeed = 1.0f;
      fovDegrees = 58.0f;
      target = {0.0f, 0.0f, 0.0f};
      break;
    case VisualizationPresetId::OahuDebugMesh:
      manual = false;
      routeSpeed = 1.0f;
      fovDegrees = 58.0f;
      target = {0.0f, 0.0f, 0.0f};
      break;
    case VisualizationPresetId::ParticleFieldHero:
      manual = true;
      yaw = -0.34f;
      pitch = 0.22f;
      distance = 7.4f;
      fovDegrees = 62.0f;
      target = {0.0f, 0.0f, -4.8f};
      break;
  }
}

bx::Vec3 CameraRig::lookDirection() const {
  const float cp = std::cos(pitch);
  return {
    std::sin(yaw) * cp,
    std::sin(pitch),
    -std::cos(yaw) * cp
  };
}

bx::Vec3 CameraRig::orbitEye() const {
  const float cp = std::cos(pitch);
  return {
    target.x + std::sin(yaw) * cp * distance,
    target.y + std::sin(pitch) * distance,
    target.z + std::cos(yaw) * cp * distance
  };
}

void CameraRig::orbit(const ImVec2& delta) {
  yaw -= delta.x * 0.008f;
  pitch = std::clamp(pitch - delta.y * 0.006f, -1.15f, 1.25f);
  manual = true;
}

void CameraRig::pan(const ImVec2& delta) {
  const float scale = std::max(distance, 1.0f) * 0.0018f;
  const bx::Vec3 right = {std::cos(yaw), 0.0f, std::sin(yaw)};
  target.x -= right.x * delta.x * scale;
  target.z -= right.z * delta.x * scale;
  target.y += delta.y * scale;
  target.y = std::clamp(target.y, -1.0f, 4.0f);
  manual = true;
}

void CameraRig::zoom(VisualizationId id, float wheel) {
  if (id == VisualizationId::Starfield3D) {
    fovDegrees = std::clamp(fovDegrees - wheel * 4.0f, 38.0f, 96.0f);
  } else {
    distance = std::clamp(distance * std::pow(0.86f, wheel), 2.2f, 34.0f);
  }
  manual = true;
}

VisualizationHost::VisualizationHost() {
  camera.applyPreset(activePreset);
  modules.push_back(createRandomLinesVisualization());
  modules.push_back(createStarfieldVisualization());
  modules.push_back(createOahuFlyoverVisualization());
  modules.push_back(createParticleFieldVisualization());
}

IVisualizationModule& VisualizationHost::module(VisualizationId id) {
  for (const std::unique_ptr<IVisualizationModule>& candidate : modules) {
    if (candidate->descriptor().id == id) {
      return *candidate;
    }
  }

  return *modules.front();
}

const IVisualizationModule& VisualizationHost::module(VisualizationId id) const {
  for (const std::unique_ptr<IVisualizationModule>& candidate : modules) {
    if (candidate->descriptor().id == id) {
      return *candidate;
    }
  }

  return *modules.front();
}

IVisualizationModule& VisualizationHost::activeModule() {
  return module(active);
}

const IVisualizationModule& VisualizationHost::activeModule() const {
  return module(active);
}

void VisualizationHost::setActive(VisualizationId next) {
  if (active != next) {
    active = next;
    activePreset = defaultVisualizationPreset(active);
    resetRequested = true;
    camera.applyPreset(activePreset);
  }
}

void VisualizationHost::setPreset(VisualizationPresetId preset) {
  const VisualizationPresetDescriptor& presetDescriptor = visualizationPresetDescriptor(preset);
  active = presetDescriptor.visualization;
  activePreset = preset;
  camera.applyPreset(preset);
  resetRequested = true;
}

void VisualizationHost::resetActive(const ImVec2& size) {
  activeModule().reset(size);
  resetRequested = false;
}

void VisualizationHost::draw(VisualizationContext& context) {
  if (resetRequested) {
    resetActive(context.size);
  }

  activeModule().draw(context);
}

void VisualizationHost::shutdown() {
  for (const std::unique_ptr<IVisualizationModule>& module : modules) {
    module->shutdown();
  }
}

} // namespace prappy
