#pragma once
#include "paths.h"
#include <exception>
#include <string_view>

// Fatal diagnostics are independent of the optional playback debug log.
namespace reporting {
void initialize(const fs::path& executable, const fs::path& reportRoot = {},bool interactive=true);
void finish(int code) noexcept;
void phase(const char* operation) noexcept;
void heartbeat() noexcept;
void pauseHang() noexcept;
void breadcrumb(const std::string& category,const std::string& message) noexcept;
void fatal(const char* handler,std::exception_ptr error = std::current_exception()) noexcept;
[[noreturn]] void assertion(const char* expression,const char* file,int line) noexcept;
void imguiError(const char* message,const char* window,int frame) noexcept;
void probe(std::string_view kind);
}
