cmake_minimum_required(VERSION 3.25)
project(prappy_player LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Use the Windows x64 MSVC toolchain.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(PRAPPY_ANALYZE "Run MSVC static analysis on player code" OFF)
option(PRAPPY_ASAN "Instrument player, SDL and ImGui with AddressSanitizer" OFF)
if(PRAPPY_ASAN)
  # CMake's default Debug runtime checks conflict with ASan.
  foreach(lang C CXX)
    string(REGEX REPLACE "/RTC[1su]+" "" CMAKE_${lang}_FLAGS_DEBUG "${CMAKE_${lang}_FLAGS_DEBUG}")
  endforeach()
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/fsanitize=address>" "$<$<COMPILE_LANGUAGE:C,CXX>:/Zi>")
  add_link_options(/INCREMENTAL:NO /DEBUG)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype/CMakeLists.txt")
  message(FATAL_ERROR "Missing pinned FreeType source. Run scripts/deps.ps1.")
endif()
foreach(dependency ZLIB BZIP2 PNG HARFBUZZ BROTLI)
  set(FT_DISABLE_${dependency} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(third_party/freetype EXCLUDE_FROM_ALL)
add_library(imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_sdl3.cpp
  third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp
  third_party/imgui/misc/freetype/imgui_freetype.cpp)
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32 IMGUI_ENABLE_FREETYPE)
target_include_directories(imgui SYSTEM PUBLIC third_party/imgui third_party/imgui/backends third_party/imgui/misc/freetype)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static freetype)
add_library(implot STATIC third_party/implot/implot.cpp third_party/implot/implot_items.cpp)
target_include_directories(implot SYSTEM PUBLIC third_party/implot)
target_link_libraries(implot PUBLIC imgui)
add_executable(prappy_player WIN32 src/main.cpp src/player_ui.cpp src/analysis.cpp src/text.cpp src/tests.cpp src/sample_client.cpp src/sample_workspace.cpp assets/player.rc)
target_include_directories(prappy_player SYSTEM PRIVATE third_party/json)
set_property(SOURCE assets/player.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/player.ico")
set_target_properties(prappy_player PROPERTIES OUTPUT_NAME "PrappyPlayer")
target_include_directories(prappy_player SYSTEM PRIVATE third_party/mpv/include)
target_compile_definitions(prappy_player PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(prappy_player PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /external:W0)
if(PRAPPY_ANALYZE)
  target_compile_options(prappy_player PRIVATE /analyze /analyze:external-)
endif()
target_link_libraries(prappy_player PRIVATE implot imgui shell32 ole32 comctl32 bcrypt dwmapi dwrite winhttp)
target_link_options(prappy_player PRIVATE /INCREMENTAL:NO /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/assets/player.manifest)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/libmpv-2.dll")
  message(FATAL_ERROR "Missing libmpv runtime. Run scripts/deps.ps1.")
endif()
add_custom_command(TARGET prappy_player POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts" "$<TARGET_FILE_DIR:prappy_player>/fonts"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/player.ico" "$<TARGET_FILE_DIR:prappy_player>/PrappyPlayer.ico"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/libmpv-2.dll" "$<TARGET_FILE_DIR:prappy_player>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/licenses" "$<TARGET_FILE_DIR:prappy_player>/licenses/mpv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/LICENSE.txt" "$<TARGET_FILE_DIR:prappy_player>/licenses/SDL.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/freetype/docs/FTL.TXT" "$<TARGET_FILE_DIR:prappy_player>/licenses/FreeType.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/LICENSE.txt" "$<TARGET_FILE_DIR:prappy_player>/licenses/imgui.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/implot/LICENSE" "$<TARGET_FILE_DIR:prappy_player>/licenses/implot.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/LICENSE.MIT" "$<TARGET_FILE_DIR:prappy_player>/licenses/json.txt")
