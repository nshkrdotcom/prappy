#pragma once

#include <bgfx/bgfx.h>
#include <imgui.h>

#include <cstdarg>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace prappy {

class PrappyBgfxCallback final : public bgfx::CallbackI {
public:
  void queueScreenshot(const std::filesystem::path& path, const ImVec2& origin, const ImVec2& size);
  bool consumeScreenshotStatus(std::string& message);

  void fatal(
    const char* filePath,
    std::uint16_t line,
    bgfx::Fatal::Enum code,
    const char* message
  ) override;

  void traceVargs(
    const char* filePath,
    std::uint16_t line,
    const char* format,
    va_list args
  ) override;

  void profilerBegin(const char*, std::uint32_t, const char*, std::uint16_t) override;
  void profilerBeginLiteral(const char*, std::uint32_t, const char*, std::uint16_t) override;
  void profilerEnd() override;
  std::uint32_t cacheReadSize(std::uint64_t) override;
  bool cacheRead(std::uint64_t, void*, std::uint32_t) override;
  void cacheWrite(std::uint64_t, const void*, std::uint32_t) override;

  void screenShot(
    const char* filePath,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t pitch,
    bgfx::TextureFormat::Enum format,
    const void* data,
    std::uint32_t size,
    bool yflip
  ) override;

  void captureBegin(std::uint32_t, std::uint32_t, std::uint32_t, bgfx::TextureFormat::Enum, bool) override;
  void captureEnd() override;
  void captureFrame(const void*, std::uint32_t) override;

private:
  struct ScreenshotCapture {
    std::filesystem::path path;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool pending = false;
  };

  bool writeBmp(
    const ScreenshotCapture& request,
    std::uint32_t sourceWidth,
    std::uint32_t sourceHeight,
    std::uint32_t pitch,
    bgfx::TextureFormat::Enum format,
    const void* data,
    bool yflip,
    std::string& error
  ) const;

  mutable std::mutex mutex;
  ScreenshotCapture capture;
  std::string completedMessage;
  bool hasCompletedMessage = false;
};

std::filesystem::path nextScreenshotPath();

} // namespace prappy
