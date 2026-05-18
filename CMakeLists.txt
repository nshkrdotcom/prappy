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

set(PRAPPY_ICON_ICO "${ROOT_DIR}/assets/prappy_icon.ico")
set(PRAPPY_ICON_BMP "${ROOT_DIR}/assets/prappy_icon.bmp")
set(PRAPPY_PLATFORM_RESOURCES)

if(WIN32)
  if(NOT EXISTS "${PRAPPY_ICON_ICO}")
    message(FATAL_ERROR "Missing Windows icon: ${PRAPPY_ICON_ICO}. Run: python tools/generate_app_icon.py")
  endif()

  if(NOT EXISTS "${PRAPPY_ICON_BMP}")
    message(FATAL_ERROR "Missing runtime icon: ${PRAPPY_ICON_BMP}. Run: python tools/generate_app_icon.py")
  endif()

  enable_language(RC)
  set(PRAPPY_ICON_ICO_RC "${PRAPPY_ICON_ICO}")
  configure_file(
    "${ROOT_DIR}/src/platform/windows_resources.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/windows_resources.rc"
    @ONLY
  )
  list(APPEND PRAPPY_PLATFORM_RESOURCES "${CMAKE_CURRENT_BINARY_DIR}/windows_resources.rc")
endif()

add_executable(prappy_native
  src/app.cpp
  src/main.cpp
  src/platform/screenshot.cpp
  src/renderer.cpp
  src/visualization_core.cpp
  src/visualizations/oahu_flyover.cpp
  src/visualizations/particle_field.cpp
  src/visualizations/random_lines.cpp
  src/visualizations/starfield.cpp
  ${PRAPPY_PLATFORM_RESOURCES}
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
    --varyingdef "${ROOT_DIR}/src/shaders/varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/imgui.vs.sc"
    "${ROOT_DIR}/src/shaders/varying.def.sc"
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
    --varyingdef "${ROOT_DIR}/src/shaders/varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/imgui.fs.sc"
    "${ROOT_DIR}/src/shaders/varying.def.sc"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/color_vs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/color.vs.sc"
    -o "${SHADER_OUT_DIR}/color_vs.bin"
    --type vertex
    --platform windows
    -p s_5_0
    --varyingdef "${ROOT_DIR}/src/shaders/visualization_varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/color.vs.sc"
    "${ROOT_DIR}/src/shaders/visualization_varying.def.sc"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/color_fs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/color.fs.sc"
    -o "${SHADER_OUT_DIR}/color_fs.bin"
    --type fragment
    --platform windows
    -p s_5_0
    --varyingdef "${ROOT_DIR}/src/shaders/visualization_varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/color.fs.sc"
    "${ROOT_DIR}/src/shaders/visualization_varying.def.sc"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/oahu_terrain_vs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/oahu_terrain.vs.sc"
    -o "${SHADER_OUT_DIR}/oahu_terrain_vs.bin"
    --type vertex
    --platform windows
    -p s_5_0
    --varyingdef "${ROOT_DIR}/src/shaders/oahu_terrain_varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/oahu_terrain.vs.sc"
    "${ROOT_DIR}/src/shaders/oahu_terrain_varying.def.sc"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/oahu_terrain_fs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/oahu_terrain.fs.sc"
    -o "${SHADER_OUT_DIR}/oahu_terrain_fs.bin"
    --type fragment
    --platform windows
    -p s_5_0
    --varyingdef "${ROOT_DIR}/src/shaders/oahu_terrain_varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/oahu_terrain.fs.sc"
    "${ROOT_DIR}/src/shaders/oahu_terrain_varying.def.sc"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/particle_vs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/particle.vs.sc"
    -o "${SHADER_OUT_DIR}/particle_vs.bin"
    --type vertex
    --platform windows
    -p s_5_0
    --varyingdef "${ROOT_DIR}/src/shaders/particle_varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/particle.vs.sc"
    "${ROOT_DIR}/src/shaders/particle_varying.def.sc"
  VERBATIM
)

add_custom_command(
  OUTPUT "${SHADER_OUT_DIR}/particle_fs.bin"
  COMMAND "${BGFX_SHADERC}"
    -f "${ROOT_DIR}/src/shaders/particle.fs.sc"
    -o "${SHADER_OUT_DIR}/particle_fs.bin"
    --type fragment
    --platform windows
    -p s_5_0
    --varyingdef "${ROOT_DIR}/src/shaders/particle_varying.def.sc"
    -i "${BGFX_DIR}/src"
  DEPENDS
    "${BGFX_SHADERC}"
    "${ROOT_DIR}/src/shaders/particle.fs.sc"
    "${ROOT_DIR}/src/shaders/particle_varying.def.sc"
  VERBATIM
)

add_custom_target(shaders ALL
  DEPENDS
    "${SHADER_OUT_DIR}/imgui_vs.bin"
    "${SHADER_OUT_DIR}/imgui_fs.bin"
    "${SHADER_OUT_DIR}/color_vs.bin"
    "${SHADER_OUT_DIR}/color_fs.bin"
    "${SHADER_OUT_DIR}/oahu_terrain_vs.bin"
    "${SHADER_OUT_DIR}/oahu_terrain_fs.bin"
    "${SHADER_OUT_DIR}/particle_vs.bin"
    "${SHADER_OUT_DIR}/particle_fs.bin"
)

add_dependencies(prappy_native shaders)

add_custom_command(TARGET prappy_native POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:prappy_native>/assets"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${PRAPPY_ICON_BMP}"
    "$<TARGET_FILE_DIR:prappy_native>/assets/prappy_icon.bmp"
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:prappy_native>/shaders"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/imgui_vs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/imgui_vs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/imgui_fs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/imgui_fs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/color_vs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/color_vs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/color_fs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/color_fs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/oahu_terrain_vs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/oahu_terrain_vs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/oahu_terrain_fs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/oahu_terrain_fs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/particle_vs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/particle_vs.bin"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SHADER_OUT_DIR}/particle_fs.bin"
    "$<TARGET_FILE_DIR:prappy_native>/shaders/particle_fs.bin"
)
