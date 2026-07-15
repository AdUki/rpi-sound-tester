cmake_minimum_required(VERSION 3.16)
project(soundtesterd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# The audio path is all float32 block loops (int<->float conversion, gain, mix, min/max,
# FFT butterflies), which the vectorizer handles well.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ftree-vectorize -fno-math-errno")

# On 32-bit ARM, NEON floats are not fully IEEE-754 conformant, so GCC refuses to
# auto-vectorize float loops with NEON unless unsafe-math is allowed -- without this, -O3
# quietly produces scalar VFP code on the Pi.
# AArch64 NEON is conformant and vectorizes floats without any such flag.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm|ARM)" AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "64")
  set(ST_ARM32_NEON_DEFAULT ON)
else()
  set(ST_ARM32_NEON_DEFAULT OFF)
endif()
option(ST_UNSAFE_MATH "Allow reassociation so 32-bit ARM can use NEON for float loops"
       ${ST_ARM32_NEON_DEFAULT})

if(ST_UNSAFE_MATH)
  # Deliberately NOT -ffast-math: -ffinite-math-only would break the NaN/Inf guards in the
  # analysis code. This only permits reassociation and drops signed-zero/trapping rules.
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " -funsafe-math-optimizations")
  message(STATUS "NEON float vectorization enabled (-funsafe-math-optimizations)")
endif()

option(ST_VECTORIZE_REPORT "Print which loops got vectorized" OFF)
if(ST_VECTORIZE_REPORT)
  string(APPEND CMAKE_CXX_FLAGS_RELEASE " -fopt-info-vec-optimized")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED alsa)
# Opus encoder + Ogg muxer for the encoded listen stream. On a dev box:
#   sudo apt install libopus-dev libogg-dev
pkg_check_modules(OPUS REQUIRED opus)
pkg_check_modules(OGG REQUIRED ogg)
find_package(Threads REQUIRED)

# third_party/* are git submodules pinned at a tag (pocketfft: a commit — it has no tags). All
# header-only: nothing here is built or linked, this just points the include path into their
# trees. Missing headers here mean the submodules were never initialised; see the error below.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib/httplib.h)
  # The command goes on its own line: message() hard-wraps, and wrapping it mid-command would
  # hand the reader something that does not copy-paste.
  message(FATAL_ERROR "third_party submodules are empty. Run:\n"
                      "    git submodule update --init\n")
endif()

# CLI11 publishes the amalgamated CLI11.hpp only as a release asset — it is not in the repo,
# so the submodule carries the multi-header include/CLI layout instead. Shim it rather than
# make the source spell the library's packaging: <CLI11.hpp> keeps meaning what it always did.
# file(GENERATE), not file(WRITE): WRITE re-touches the header on every configure, and main.cpp
# (which pulls all of CLI11) would be recompiled each time for nothing.
set(CLI11_SHIM ${CMAKE_CURRENT_BINARY_DIR}/cli11-shim)
file(GENERATE OUTPUT ${CLI11_SHIM}/CLI11.hpp CONTENT "#include <CLI/CLI.hpp>\n")

add_library(third_party INTERFACE)
target_include_directories(third_party SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pocketfft
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cli11/include
  ${CLI11_SHIM}
)
# Header-only spdlog: no library to link, no fmt to build. CLI11 is header-only too as long as
# CLI11_COMPILE stays undefined.
target_compile_definitions(third_party INTERFACE SPDLOG_HEADER_ONLY)

add_library(soundtester_core STATIC
  src/analysis.cpp
  src/audio_engine.cpp
  src/capture.cpp
  src/config.cpp
  src/generators.cpp
  src/kmsg_watch.cpp
  src/listen_encoder.cpp
  src/listen_stream.cpp
  src/util/log.cpp
  src/util/sysinfo.cpp
  src/webserver.cpp
  src/ws_hub.cpp
)
target_include_directories(soundtester_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# Opus headers live under an opus/ subdir the .pc exposes, so the include dirs are required.
target_include_directories(soundtester_core SYSTEM PUBLIC
  ${ALSA_INCLUDE_DIRS} ${OPUS_INCLUDE_DIRS} ${OGG_INCLUDE_DIRS})
target_link_libraries(soundtester_core PUBLIC third_party
  ${ALSA_LIBRARIES} ${OPUS_LIBRARIES} ${OGG_LIBRARIES} Threads::Threads)
target_compile_options(soundtester_core PRIVATE -Wall -Wextra)

add_executable(soundtesterd src/main.cpp)
target_link_libraries(soundtesterd PRIVATE soundtester_core)

include(GNUInstallDirs)
install(TARGETS soundtesterd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY www/ DESTINATION ${CMAKE_INSTALL_DATADIR}/soundtester/www)
install(FILES config/default-config.json DESTINATION ${CMAKE_INSTALL_SYSCONFDIR}/soundtester
        RENAME config.json)

enable_testing()
add_subdirectory(tests)
