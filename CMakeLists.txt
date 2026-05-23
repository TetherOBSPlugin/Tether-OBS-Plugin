cmake_minimum_required(VERSION 3.28...3.30)

include("cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI extensions" ON)
option(ENABLE_QT "Use Qt functionality" ON)

# We ship as a shared module (loaded by OBS), so every static library we pull
# in via FetchContent (opus, libdatachannel, usrsctp, libsrtp, libjuice, ...)
# must be built position-independent. aarch64 enforces this strictly; x86_64
# is laxer but the variable harmonises behaviour across runners.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
#
# libdatachannel and its vendored deps (usrsctp, libsrtp, libjuice, ...) were
# not written under the OBS template's -Wunused-parameter -Werror umbrella, so
# we drop warnings-as-errors just for that subtree. Our own target was already
# created above with the strict default, so this only affects the deps.
# Helper: strip every warning-as-error setting from a foreign target tree.
# CMAKE_COMPILE_WARNING_AS_ERROR alone isn't enough on macOS because Xcode picks
# up CMAKE_XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS / WARNING_CFLAGS from the
# OBS template at the project level and applies them to every target including
# FetchContent imports.
function(_tether_relax_warnings_in_dir _dir)
  if(NOT _dir OR NOT EXISTS ${_dir})
    return()
  endif()
  get_property(_targets DIRECTORY ${_dir} PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_t IN LISTS _targets)
    set_target_properties(
      ${_t}
      PROPERTIES
        COMPILE_WARNING_AS_ERROR OFF
        XCODE_ATTRIBUTE_GCC_TREAT_WARNINGS_AS_ERRORS NO
        XCODE_ATTRIBUTE_WARNING_CFLAGS ""
        # usrsctp ships its own struct inpcb that clashes with macOS'
        # <netinet/in_pcb.h>. With Clang Modules enabled (the Xcode default),
        # the compiler enforces cross-TU struct compatibility and rejects the
        # build. Disable modules for foreign targets so they compile like
        # vanilla C.
        XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES NO
    )
    # Belt-and-suspenders: even after killing the per-target werror
    # property, some sub-deps (libsrtp's srtp2, usrsctp) inject -Werror
    # directly into CMAKE_C_FLAGS or via their own target_compile_options.
    # Append -Wno-error so the final compile command demotes everything
    # back to warnings. INTERFACE_LIBRARY targets cannot carry compile
    # options, so skip them.
    get_target_property(_t_type ${_t} TYPE)
    if(NOT _t_type STREQUAL "INTERFACE_LIBRARY")
      # MSVC uses /WX (and /WX-) instead of -Werror/-Wno-error; CMake would
      # mangle a bare -Wno-error into /Wno-error which cl.exe parses as a
      # numeric warning-level (D8021). Pick the right syntax per compiler.
      if(MSVC)
        target_compile_options(${_t} PRIVATE /WX-)
      else()
        target_compile_options(${_t} PRIVATE -Wno-error)
      endif()
    endif()
  endforeach()
  get_property(_subdirs DIRECTORY ${_dir} PROPERTY SUBDIRECTORIES)
  foreach(_sd IN LISTS _subdirs)
    _tether_relax_warnings_in_dir(${_sd})
  endforeach()
endfunction()

# obs-deps ships LibDataChannel 0.21.0 for macOS and Windows, which predates
# rtcCreateWebSocketEx / rtcWsConfiguration. Require the version that has the
# WebSocket API we depend on so find_package skips the obs-deps copy and
# FetchContent takes over.
set(_TETHER_LDC_MIN "0.23")
set(_TETHER_LDC_TAG "v0.23.1")
find_package(LibDataChannel ${_TETHER_LDC_MIN} CONFIG QUIET)
if(NOT LibDataChannel_FOUND)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(USE_GNUTLS OFF CACHE BOOL "" FORCE)
  set(NO_WEBSOCKET OFF CACHE BOOL "" FORCE)
  set(NO_MEDIA OFF CACHE BOOL "" FORCE)
  # Explicit overrides for each vendored sub-dep's own warnings-as-errors
  # option — libdatachannel's CMakeLists honours these before we get a
  # chance to set per-target properties:
  #   libsrtp  → ENABLE_WARNINGS_AS_ERRORS (default ON in cisco/libsrtp)
  #   usrsctp  → sctp_werror               (default 1 in paullouisageneau/usrsctp)
  #   libjuice → WARNINGS_AS_ERRORS        (default OFF, set explicit for safety)
  #   libdatachannel host → WARNINGS_AS_ERRORS (default OFF, same)
  set(ENABLE_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  set(sctp_werror OFF CACHE BOOL "" FORCE)
  set(WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  set(_tether_saved_werror ${CMAKE_COMPILE_WARNING_AS_ERROR})
  set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/deps/libdatachannel/CMakeLists.txt")
    add_subdirectory(deps/libdatachannel EXCLUDE_FROM_ALL)
    _tether_relax_warnings_in_dir(${CMAKE_CURRENT_SOURCE_DIR}/deps/libdatachannel)
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
    FetchContent_GetProperties(libdatachannel SOURCE_DIR _ldc_src)
    _tether_relax_warnings_in_dir(${_ldc_src})
  endif()
  set(CMAKE_COMPILE_WARNING_AS_ERROR ${_tether_saved_werror})
endif()
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE LibDataChannel::LibDataChannel)
# libdatachannel adds RTC_ENABLE_WEBSOCKET / RTC_ENABLE_MEDIA as PUBLIC
# compile definitions on its targets (datachannel, datachannel-static), so they
# propagate to us through the link. Do not redefine them here — Apple-Clang
# promotes the duplicate to -Werror=macro-redefined.

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
    src/sender-dialog.cpp
    src/sender-dialog.hpp
    src/hub-dialog.cpp
    src/hub-dialog.hpp
    src/receive-dialog.cpp
    src/receive-dialog.hpp
    src/properties.c
    src/properties.h
    src/log.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(WIN32)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE bcrypt)
endif()

# libavcodec / libavutil / libswscale provide the H.264 decoder we use on the
# receiver side to turn libdatachannel-depayloaded NAL units into raw video
# frames OBS can render. They come from FFmpeg, which the obs-deps bundles
# ship on Windows / macOS, and which Linux pulls in via system pkg-config.
find_package(PkgConfig QUIET)
set(_libav_found FALSE)
if(PKG_CONFIG_FOUND)
  pkg_check_modules(LIBAV QUIET IMPORTED_TARGET libavcodec libavutil libswscale)
  if(LIBAV_FOUND)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE PkgConfig::LIBAV)
    set(_libav_found TRUE)
  endif()
endif()
if(NOT _libav_found)
  find_library(AVCODEC_LIBRARY NAMES avcodec HINTS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib lib64 REQUIRED)
  find_library(AVUTIL_LIBRARY NAMES avutil HINTS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib lib64 REQUIRED)
  find_library(SWSCALE_LIBRARY NAMES swscale HINTS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES lib lib64 REQUIRED)
  find_path(AVCODEC_INCLUDE_DIR libavcodec/avcodec.h HINTS ${CMAKE_PREFIX_PATH} PATH_SUFFIXES include REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY} ${SWSCALE_LIBRARY})
  target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${AVCODEC_INCLUDE_DIR})
endif()

# Opus is required for our multi-track audio encode/decode (used directly in
# audio.c). The obs-deps Windows bundle ships only opus headers, not the .lib,
# so we cannot rely on find_library across all three platforms. Fetch opus from
# upstream at a pinned tag instead — same source on every runner, ~30s extra
# configure/build time. Save/restore CMAKE_COMPILE_WARNING_AS_ERROR around the
# fetch for the same reason we do for libdatachannel: opus' warning hygiene
# does not match the OBS template's strict umbrella.
include(FetchContent)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
# Opus' CMake mis-detects NEON support on macOS arm64 (xiph/opus GitLab
# issue #2372): it triggers OPUS_MAY_HAVE_NEON without passing the
# -mfloat-abi flag clang needs to compile <arm_neon.h>, so celt/arm/*.c
# fails with "NEON intrinsics not available with the soft-float ABI".
# Disable all CPU intrinsics to fall back to portable C. The performance
# hit is irrelevant for a 96 kbps voice/audio codec on modern hardware.
set(OPUS_DISABLE_INTRINSICS ON CACHE BOOL "" FORCE)
set(_tether_saved_werror_opus ${CMAKE_COMPILE_WARNING_AS_ERROR})
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
FetchContent_Declare(opus GIT_REPOSITORY https://github.com/xiph/opus.git GIT_TAG v1.5.2 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(opus)
FetchContent_GetProperties(opus SOURCE_DIR _opus_src)
_tether_relax_warnings_in_dir(${_opus_src})
set(CMAKE_COMPILE_WARNING_AS_ERROR ${_tether_saved_werror_opus})
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE opus)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
