cmake_minimum_required(VERSION 3.24)
project(LimeX LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

option(LIMEX_WITH_AUDIO   "Build sound support (needs the irrKlang SDK)" ON)
option(LIMEX_WITH_TLS     "Build HTTPS support (needs OpenSSL 3)"        OFF)
option(LIMEX_WITH_PHYSICS "Build physics (needs Bullet and irrBullet)"   ON)
option(LIMEX_FETCH_DEPS   "Download the open-source dependencies"        ON)

if(MSVC)
  add_compile_options(/W3 /permissive- /Zc:preprocessor)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(LIMEX_FETCH_DEPS)
  FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE)
  set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        v3.3.0
    GIT_SHALLOW    TRUE)

  FetchContent_Declare(enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG        v1.3.18
    GIT_SHALLOW    TRUE)

  FetchContent_Declare(freetype
    GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
    GIT_TAG        VER-2-13-2
    GIT_SHALLOW    TRUE)

  FetchContent_Declare(lua
    URL      https://www.lua.org/ftp/lua-5.4.6.tar.gz
    URL_HASH SHA256=7d5ea1b9cb6aa0b59ca3dde1c6adcb57ef83a1ba8e5432c0ecd64730f7ce0896)

  FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        f0569113c93ad095470c54bf34a17b36646bbbb5
    GIT_SHALLOW    FALSE)

  if(LIMEX_WITH_PHYSICS)
    FetchContent_Declare(bullet
      GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git
      GIT_TAG        3.25
      GIT_SHALLOW    TRUE)
    set(BUILD_BULLET2_DEMOS  OFF CACHE BOOL "" FORCE)
    set(BUILD_CPU_DEMOS      OFF CACHE BOOL "" FORCE)
    set(BUILD_OPENGL3_DEMOS  OFF CACHE BOOL "" FORCE)
    set(BUILD_UNIT_TESTS     OFF CACHE BOOL "" FORCE)
    set(BUILD_EXTRAS         OFF CACHE BOOL "" FORCE)
  endif()

  FetchContent_MakeAvailable(glfw sol2 enet freetype lua stb)
  if(LIMEX_WITH_PHYSICS)
    FetchContent_MakeAvailable(bullet)
  endif()
endif()

if(LIMEX_FETCH_DEPS)
  file(GLOB LUA_SRC ${lua_SOURCE_DIR}/src/*.c)
  list(REMOVE_ITEM LUA_SRC
       ${lua_SOURCE_DIR}/src/lua.c
       ${lua_SOURCE_DIR}/src/luac.c)
  add_library(lua_static STATIC ${LUA_SRC})
  target_include_directories(lua_static PUBLIC ${lua_SOURCE_DIR}/src)
  if(MSVC)
    target_compile_options(lua_static PRIVATE /W0)
  endif()

  add_library(stb_headers INTERFACE)
  target_include_directories(stb_headers INTERFACE ${stb_SOURCE_DIR})
endif()

find_path(IRRLICHT_INCLUDE_DIR irrlicht.h
  HINTS ${CMAKE_SOURCE_DIR}/../irrlicht-1.8.5/include
        $ENV{IRRLICHT_ROOT}/include
  DOC "Directory containing irrlicht.h")
find_library(IRRLICHT_LIBRARY NAMES Irrlicht irrlicht
  HINTS ${CMAKE_SOURCE_DIR}/../irrlicht-1.8.5/lib/Win64-visualStudio
        $ENV{IRRLICHT_ROOT}/lib/Win64-visualStudio
  DOC "Irrlicht.lib")

if(LIMEX_WITH_PHYSICS)
  find_path(IRRBULLET_INCLUDE_DIR irrBullet.h
    HINTS ${CMAKE_SOURCE_DIR}/../irrBullet $ENV{IRRBULLET_ROOT})
  find_library(IRRBULLET_LIBRARY NAMES irrBullet
    HINTS ${CMAKE_SOURCE_DIR}/../irrBullet/x64/Release $ENV{IRRBULLET_ROOT}/lib)
endif()

if(LIMEX_WITH_AUDIO)
  find_path(IRRKLANG_INCLUDE_DIR irrKlang.h
    HINTS ${CMAKE_SOURCE_DIR}/../irrKlang-1.6.0/include
          $ENV{IRRKLANG_ROOT}/include)
  find_library(IRRKLANG_LIBRARY NAMES irrKlang
    HINTS ${CMAKE_SOURCE_DIR}/../irrKlang-1.6.0/lib/Winx64-visualStudio
          $ENV{IRRKLANG_ROOT}/lib/Winx64-visualStudio)
endif()

if(LIMEX_WITH_TLS)
  find_package(OpenSSL 3 REQUIRED)
endif()

set(_missing "")
if(NOT IRRLICHT_INCLUDE_DIR OR NOT IRRLICHT_LIBRARY)
  list(APPEND _missing "Irrlicht 1.8.5 (set IRRLICHT_ROOT)")
endif()
if(LIMEX_WITH_PHYSICS AND (NOT IRRBULLET_INCLUDE_DIR OR NOT IRRBULLET_LIBRARY))
  list(APPEND _missing "irrBullet (set IRRBULLET_ROOT, or -DLIMEX_WITH_PHYSICS=OFF)")
endif()
if(LIMEX_WITH_AUDIO AND (NOT IRRKLANG_INCLUDE_DIR OR NOT IRRKLANG_LIBRARY))
  list(APPEND _missing "irrKlang 1.6.0 (set IRRKLANG_ROOT, or -DLIMEX_WITH_AUDIO=OFF)")
endif()
if(_missing)
  message(FATAL_ERROR
    "LimeEngine cannot be configured. Missing:\n  ${_missing}\n"
    "See BUILDING.md. Everything else is downloaded automatically.")
endif()

file(GLOB_RECURSE LIMEENGINE_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/LimeEngine/src/*.cpp
  ${CMAKE_SOURCE_DIR}/LimeEngine/src/*.c)

if(NOT LIMEX_WITH_AUDIO)
  list(FILTER LIMEENGINE_SRC EXCLUDE REGEX "SoundManager\\.cpp$")
  list(FILTER LIMEENGINE_SRC EXCLUDE REGEX "SoundSource\\.cpp$")
endif()

add_library(LimeEngine SHARED ${LIMEENGINE_SRC})

target_include_directories(LimeEngine PRIVATE
  ${CMAKE_SOURCE_DIR}/LimeEngine/include
  ${CMAKE_SOURCE_DIR}/LimeEngine/api
  ${IRRLICHT_INCLUDE_DIR})

target_link_libraries(LimeEngine PRIVATE
  glfw sol2::sol2 lua_static stb_headers freetype enet
  ${IRRLICHT_LIBRARY}
  winmm ws2_32 crypt32 opengl32)

if(LIMEX_WITH_TLS)
  target_link_libraries(LimeEngine PRIVATE OpenSSL::SSL OpenSSL::Crypto)
else()
  target_compile_definitions(LimeEngine PRIVATE CPPHTTPLIB_NO_OPENSSL_SUPPORT=1)
endif()

if(LIMEX_WITH_AUDIO)
  target_include_directories(LimeEngine PRIVATE ${IRRKLANG_INCLUDE_DIR})
  target_link_libraries(LimeEngine PRIVATE ${IRRKLANG_LIBRARY})
  target_compile_definitions(LimeEngine PRIVATE LIMEX_AUDIO=1)
endif()

if(LIMEX_WITH_PHYSICS)
  target_include_directories(LimeEngine PRIVATE ${IRRBULLET_INCLUDE_DIR})
  target_link_libraries(LimeEngine PRIVATE
    ${IRRBULLET_LIBRARY} BulletDynamics BulletCollision BulletSoftBody LinearMath)
  target_compile_definitions(LimeEngine PRIVATE LIMEX_PHYSICS=1)
endif()

file(GLOB LIMEBUILDER_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/LimeBuilder/*.cpp)
add_executable(LimeBuilder ${LIMEBUILDER_SRC})
target_include_directories(LimeBuilder PRIVATE ${CMAKE_SOURCE_DIR}/LimeBuilder)
target_link_libraries(LimeBuilder PRIVATE lua_static)

file(GLOB LIMEPLAYER_SRC CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/LimePlayer/*.cpp)
add_executable(LimePlayer WIN32 ${LIMEPLAYER_SRC})
target_include_directories(LimePlayer PRIVATE
  ${CMAKE_SOURCE_DIR}/LimePlayer
  ${CMAKE_SOURCE_DIR}/LimeEngine/include)
add_dependencies(LimePlayer LimeEngine)

set_target_properties(LimeEngine LimeBuilder LimePlayer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
