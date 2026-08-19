cmake_minimum_required(VERSION 3.21)
project(LimeVSC VERSION 0.12.0 LANGUAGES CXX C RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:preprocessor /MP /utf-8 /EHsc)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()

set(TP ${CMAKE_SOURCE_DIR}/third_party)

file(GLOB_RECURSE LIMECORE_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/core/*.cpp
  ${CMAKE_SOURCE_DIR}/src/api/*.cpp
  ${CMAKE_SOURCE_DIR}/src/graph/*.cpp
  ${CMAKE_SOURCE_DIR}/src/lime/*.cpp
  ${CMAKE_SOURCE_DIR}/src/compile/*.cpp
  ${CMAKE_SOURCE_DIR}/src/project/*.cpp
  ${CMAKE_SOURCE_DIR}/src/scene/*.cpp
  ${CMAKE_SOURCE_DIR}/src/asset/*.cpp
  ${CMAKE_SOURCE_DIR}/src/mesh/*.cpp
)
add_library(limecore STATIC ${LIMECORE_SRC})
target_include_directories(limecore PUBLIC
  ${CMAKE_SOURCE_DIR}/src
  ${TP}/tomlplusplus
)
target_compile_definitions(limecore PUBLIC
  LIMEVSC_DATA_DIR="${CMAKE_SOURCE_DIR}/data"
  TOML_EXCEPTIONS=0
)

add_library(imgui STATIC
  ${TP}/imgui/imgui.cpp
  ${TP}/imgui/imgui_draw.cpp
  ${TP}/imgui/imgui_tables.cpp
  ${TP}/imgui/imgui_widgets.cpp
  ${TP}/imgui/backends/imgui_impl_dx11.cpp
  ${TP}/imgui/backends/imgui_impl_win32.cpp
  ${TP}/imgui-node-editor/imgui_node_editor.cpp
  ${TP}/imgui-node-editor/imgui_node_editor_api.cpp
  ${TP}/imgui-node-editor/imgui_canvas.cpp
  ${TP}/imgui-node-editor/crude_json.cpp
)
target_include_directories(imgui PUBLIC
  ${TP}/imgui ${TP}/imgui/backends ${TP}/imgui-node-editor
  ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(imgui PUBLIC IMGUI_DEFINE_MATH_OPERATORS)
target_link_libraries(imgui PUBLIC d3d11 dxgi d3dcompiler dwmapi)
if(MSVC)
  target_compile_options(imgui PRIVATE /W0)
endif()

file(GLOB_RECURSE LIMEVSC_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/ui/*.cpp
  ${CMAKE_SOURCE_DIR}/src/render/*.cpp
  ${CMAKE_SOURCE_DIR}/src/text/*.cpp
  ${CMAKE_SOURCE_DIR}/src/engine/*.cpp
  ${CMAKE_SOURCE_DIR}/src/app/*.cpp
)
add_executable(limevsc WIN32 ${LIMEVSC_SRC} ${CMAKE_SOURCE_DIR}/src/app/limevsc.rc)
target_compile_definitions(limevsc PRIVATE
  LIMEVSC_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(limevsc PRIVATE limecore imgui shell32 ole32
                      d3d11 dxgi d3dcompiler)

add_executable(limec    ${CMAKE_SOURCE_DIR}/src/tools/limec_main.cpp)
target_link_libraries(limec PRIVATE limecore)

add_executable(limeapi  ${CMAKE_SOURCE_DIR}/src/tools/limeapi_main.cpp)
target_link_libraries(limeapi PRIVATE limecore)

file(GLOB_RECURSE LIMETEST_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.cpp)
list(APPEND LIMETEST_SRC ${CMAKE_SOURCE_DIR}/src/app/editor.cpp)
list(APPEND LIMETEST_SRC ${CMAKE_SOURCE_DIR}/src/app/editor_scene.cpp)
list(APPEND LIMETEST_SRC ${CMAKE_SOURCE_DIR}/src/app/settings.cpp)
list(APPEND LIMETEST_SRC ${CMAKE_SOURCE_DIR}/src/app/loading.cpp)
add_executable(limetests ${LIMETEST_SRC})
target_link_libraries(limetests PRIVATE limecore shell32 ole32)
target_include_directories(limetests PRIVATE ${TP}/doctest ${TP}/imgui)
target_compile_definitions(limetests PRIVATE
  LIMEVSC_TEST_DIR="${CMAKE_SOURCE_DIR}/tests"
)

enable_testing()
add_test(NAME limetests COMMAND limetests)
