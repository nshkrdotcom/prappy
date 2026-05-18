cmake_minimum_required(VERSION 3.25)

project(prappy_native LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(EXTERNAL_DIR "${ROOT_DIR}/external")

set(BX_DIR "${EXTERNAL_DIR}/bx")
set(BIMG_DIR "${EXTERNAL_DIR}/bimg")
set(BGFX_DIR "${EXTERNAL_DIR}/bgfx")
set(IMGUI_DIR "${EXTERNAL_DIR}/imgui")
set(SDL_DIR "${EXTERNAL_DIR}/SDL")

foreach(required_dir IN ITEMS "${BX_DIR}" "${BIMG_DIR}" "${BGFX_DIR}" "${IMGUI_DIR}" "${SDL_DIR}")
  if(NOT EXISTS "${required_dir}")
    message(FATAL_ERROR "Missing dependency directory: ${required_dir}. Run: pwsh scripts/deps.ps1")
  endif()
endforeach()

set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${SDL_DIR}" external/sdl)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(BGFX_CONFIG_SUFFIX "Debug")
else()
  set(BGFX_CONFIG_SUFFIX "Release")
endif()

set(BGFX_LIB_DIR "${BGFX_DIR}/.build/win64_vs2022/bin")
set(BGFX_SHADERC "${BGFX_LIB_DIR}/shaderc${BGFX_CONFIG_SUFFIX}.exe")

foreach(required_artifact IN ITEMS
    "${BGFX_LIB_DIR}/bgfx${BGFX_CONFIG_SUFFIX}.lib"
    "${BGFX_LIB_DIR}/bimg${BGFX_CONFIG_SUFFIX}.lib"
    "${BGFX_LIB_DIR}/bx${BGFX_CONFIG_SUFFIX}.lib"
    "${BGFX_SHADERC}")
  if(NOT EXISTS "${required_artifact}")
    message(FATAL_ERROR "Missing bgfx build artifact: ${required_artifact}. Run: pwsh scripts/configure.ps1 -Config ${CMAKE_BUILD_TYPE}")
  endif()
endforeach()

add_library(bgfx_prebuilt STATIC IMPORTED GLOBAL)
set_target_properties(bgfx_prebuilt PROPERTIES
  IMPORTED_LOCATION "${BGFX_LIB_DIR}/bgfx${BGFX_CONFIG_SUFFIX}.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${BGFX_DIR}/include"
)

add_library(bimg_prebuilt STATIC IMPORTED GLOBAL)
set_target_properties(bimg_prebuilt PROPERTIES
  IMPORTED_LOCATION "${BGFX_LIB_DIR}/bimg${BGFX_CONFIG_SUFFIX}.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${BIMG_DIR}/include"
)

add_library(bx_prebuilt STATIC IMPORTED GLOBAL)
set_target_properties(bx_prebuilt PROPERTIES
  IMPORTED_LOCATION "${BGFX_LIB_DIR}/bx${BGFX_CONFIG_SUFFIX}.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${BX_DIR}/include"
)

add_library(imgui STATIC
  "${IMGUI_DIR}/imgui.cpp"
  "${IMGUI_DIR}/imgui_draw.cpp"
  "${IMGUI_DIR}/imgui_tables.cpp"
  "${IMGUI_DIR}/imgui_widgets.cpp"
  "${IMGUI_DIR}/imgui_demo.cpp"
)

target_include_directories(imgui PUBLIC
  "${IMGUI_DIR}"
)

add_executable(prappy_native
  src/main.cpp
)

target_include_directories(prappy_native PRIVATE
  "${IMGUI_DIR}"
  "${BGFX_DIR}/include"
  "${BX_DIR}/include"
  "${BIMG_DIR}/include"
  "${SDL_DIR}/include"
)

target_link_libraries(prappy_native PRIVATE
  imgui
  bgfx_prebuilt
  bimg_prebuilt
  bx_prebuilt
  SDL3-static
  psapi
  gdi32
  user32
  shell32
  ole32
  advapi32
  uuid
)

target_compile_definitions(prappy_native PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _CRT_SECURE_NO_WARNINGS
  BX_CONFIG_DEBUG=$<IF:$<CONFIG:Debug>,1,0>
)

if(MSVC)
  target_compile_options(prappy_native PRIVATE
    /W4
    /permissive-
    /Zc:__cplusplus
    /Zc:preprocessor
    /EHsc
    /MP
  )
endif()

set(SHADER_OUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY "${SHADER_OUT_DIR}")

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/imgui_vs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/imgui.vs.sc"
    -o "${SHADER_OUT_DIR}/imgui_vs.bin"
    --type vertex
    --platform windows
    -p s_5_0
    -i "${BGFX_DIR}/src"
  DEPENDS "${BGFX_SHADERC}" "${ROOT_DIR}/src/shaders/imgui.vs.sc"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/imgui_fs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/imgui.fs.sc"
    -o "${SHADER_OUT_DIR}/imgui_fs.bin"
    --type fragment
    --platform windows
    -p s_5_0
    -i "${BGFX_DIR}/src"
  DEPENDS "${BGFX_SHADERC}" "${ROOT_DIR}/src/shaders/imgui.fs.sc"
  VERBATIM
)

add_custom_target(shaders ALL
  DEPENDS
    "${SHADER_OUT_DIR}/imgui_vs.bin"
    "${SHADER_OUT_DIR}/imgui_fs.bin"
)

add_dependencies(prappy_native shaders)

add_custom_command(TARGET prappy_native POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:prappy_native>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/imgui_vs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/imgui_vs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/imgui_fs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/imgui_fs.bin"
)
