#include "screenshot.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace prappy {
namespace {

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

} // namespace

std::filesystem::path nextScreenshotPath() {
  const std::filesystem::path captureDir = std::filesystem::current_path() / "captures";
  std::filesystem::create_directories(captureDir);

  const auto now = std::chrono::system_clock::now();
  const std::time_t time = std::chrono::system_clock::to_time_t(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
    now.time_since_epoch()
  ).count() % 1000;

  std::tm localTime{};
#ifdef _WIN32
  localtime_s(&localTime, &time);
#else
  localtime_r(&time, &localTime);
#endif

  std::ostringstream filename;
  filename
    << "prappy_"
    << std::put_time(&localTime, "%Y%m%d_%H%M%S")
    << "_"
    << std::setw(3)
    << std::setfill('0')
    << millis
    << ".bmp";
  return captureDir / filename.str();
}

void PrappyBgfxCallback::queueScreenshot(
  const std::filesystem::path& path,
  const ImVec2& origin,
  const ImVec2& size
) {
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

bool PrappyBgfxCallback::consumeScreenshotStatus(std::string& message) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!hasCompletedMessage) {
    return false;
  }

  message = completedMessage;
  hasCompletedMessage = false;
  completedMessage.clear();
  return true;
}

void PrappyBgfxCallback::fatal(
  const char* filePath,
  std::uint16_t line,
  bgfx::Fatal::Enum code,
  const char* message
) {
  std::fprintf(stderr, "bgfx fatal %s:%u: %s\n", filePath, line, message);
  if (code != bgfx::Fatal::DebugCheck) {
    std::abort();
  }
}

void PrappyBgfxCallback::traceVargs(
  const char* filePath,
  std::uint16_t line,
  const char* format,
  va_list args
) {
  (void)filePath;
  (void)line;
  (void)format;
  (void)args;
}

void PrappyBgfxCallback::profilerBegin(const char*, std::uint32_t, const char*, std::uint16_t) {}
void PrappyBgfxCallback::profilerBeginLiteral(const char*, std::uint32_t, const char*, std::uint16_t) {}
void PrappyBgfxCallback::profilerEnd() {}
std::uint32_t PrappyBgfxCallback::cacheReadSize(std::uint64_t) { return 0; }
bool PrappyBgfxCallback::cacheRead(std::uint64_t, void*, std::uint32_t) { return false; }
void PrappyBgfxCallback::cacheWrite(std::uint64_t, const void*, std::uint32_t) {}

void PrappyBgfxCallback::screenShot(
  const char* filePath,
  std::uint32_t width,
  std::uint32_t height,
  std::uint32_t pitch,
  bgfx::TextureFormat::Enum format,
  const void* data,
  std::uint32_t,
  bool yflip
) {
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

void PrappyBgfxCallback::captureBegin(
  std::uint32_t,
  std::uint32_t,
  std::uint32_t,
  bgfx::TextureFormat::Enum,
  bool
) {}

void PrappyBgfxCallback::captureEnd() {}
void PrappyBgfxCallback::captureFrame(const void*, std::uint32_t) {}

bool PrappyBgfxCallback::writeBmp(
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

} // namespace prappy
