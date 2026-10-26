cmake_minimum_required(VERSION 3.25)
project(prappy_player LANGUAGES C CXX RC)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Use the Windows x64 MSVC toolchain.")
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/SDL EXCLUDE_FROM_ALL)
add_library(imgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp
  third_party/imgui/backends/imgui_impl_sdl3.cpp
  third_party/imgui/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(imgui PUBLIC third_party/imgui third_party/imgui/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3-static)
add_executable(prappy_player WIN32 src/main.cpp assets/player.rc)
set_property(SOURCE assets/player.rc APPEND PROPERTY OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/player.ico")
set_target_properties(prappy_player PROPERTIES OUTPUT_NAME "PrappyPlayer")
target_include_directories(prappy_player PRIVATE third_party/mpv/include)
target_compile_definitions(prappy_player PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(prappy_player PRIVATE /W4 /utf-8 /EHsc)
target_link_libraries(prappy_player PRIVATE imgui shell32 ole32 comctl32 bcrypt)
target_link_options(prappy_player PRIVATE /MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/assets/player.manifest)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/libmpv-2.dll")
  message(FATAL_ERROR "Missing libmpv runtime. Run scripts/deps.ps1.")
endif()
add_custom_command(TARGET prappy_player POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/libmpv-2.dll" "$<TARGET_FILE_DIR:prappy_player>"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mpv/licenses" "$<TARGET_FILE_DIR:prappy_player>/licenses/mpv"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL/LICENSE.txt" "$<TARGET_FILE_DIR:prappy_player>/licenses/SDL.txt"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/imgui/LICENSE.txt" "$<TARGET_FILE_DIR:prappy_player>/licenses/imgui.txt")
