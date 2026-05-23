cmake_minimum_required(VERSION 3.28...3.30)

include("cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI extensions" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

# libdatachannel provides the WebRTC transport. Resolved in this order:
#   1. System install (find_package) — devs with libdatachannel-devel.
#   2. Vendored submodule under deps/libdatachannel — pinned local copy.
#   3. FetchContent from upstream — CI runners without a system package.
set(_TETHER_LDC_TAG "v0.23.1")
find_package(LibDataChannel CONFIG QUIET)
if(NOT LibDataChannel_FOUND)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
  set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
  set(NO_MEDIA OFF CACHE BOOL "" FORCE)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/libdatachannel/CMakeLists.txt")
    add_subdirectory(deps/libdatachannel EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(
      libdatachannel
      GIT_REPOSITORY https://github.com/paullouisageneau/libdatachannel.git
      GIT_TAG ${_TETHER_LDC_TAG}
      GIT_SHALLOW TRUE
      GIT_SUBMODULES_RECURSE TRUE
    )
    FetchContent_MakeAvailable(libdatachannel)
  endif()
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)

target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/plugin-main.c
    src/token.c
    src/token.h
    src/signaling.c
    src/signaling.h
    src/admission.c
    src/admission.h
    src/webrtc.c
    src/webrtc.h
    src/audio.c
    src/audio.h
    src/source.c
    src/source.h
    src/sender.c
    src/sender.h
    src/properties.c
    src/properties.h
    src/log.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE bcrypt)
endif()

# Opus is required for our multi-track audio encode/decode (used directly in
# audio.c). Found via pkg-config on Linux/macOS; on Windows the obs-deps
# bundle ships it.
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::OPUS)
else()
  find_library(OPUS_LIBRARY NAMES opus REQUIRED)
  find_path(OPUS_INCLUDE_DIR opus/opus.h REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${OPUS_LIBRARY})
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${OPUS_INCLUDE_DIR})
endif()

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
