#include "visualization_core.h"

#include <algorithm>
#include <cmath>

namespace prappy {

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
      yaw = 0.0f;
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
  camera.resetFor(active);
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
    resetRequested = true;
    camera.resetFor(active);
  }
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

} // namespace prappy
