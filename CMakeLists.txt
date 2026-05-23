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
    PRIVATE
      $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES
      AUTOMOC ON
      AUTOUIC ON
      AUTORCC ON
  )
endif()

# libdatachannel provides the WebRTC transport. Either system-installed (find_package)
# or vendored as a submodule under deps/libdatachannel — the latter wins if present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/libdatachannel/CMakeLists.txt")
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
  add_subdirectory(deps/libdatachannel EXCLUDE_FROM_ALL)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
else()
  find_package(LibDataChannel REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
endif()

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

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
