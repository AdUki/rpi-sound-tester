cmake_minimum_required(VERSION 3.16)
project(soundtester_alsa_plugin C)

# Builds for the machine that SENDS audio — a laptop, a build box — not for the Pi.

option(ST_VORBIS "Vorbis encoding in the plugin (links libvorbis)" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

if(ST_VORBIS)
  pkg_check_modules(VORBIS IMPORTED_TARGET vorbis vorbisenc)
  if(NOT VORBIS_FOUND)
    message(FATAL_ERROR "libvorbis not found: apt install libvorbis-dev, or cmake -DST_VORBIS=OFF")
  endif()
endif()
message(STATUS "soundtester plugin: Vorbis ${ST_VORBIS}")

# alsa-lib dlopens exactly these names.
set(ST_PCM_SO libasound_module_pcm_soundtester.so)
set(ST_CTL_SO libasound_module_ctl_soundtester.so)

# Both halves in one object; its only external symbols are the two _snd_*_soundtester_open.
add_library(soundtester_alsa MODULE pcm_soundtester.c ctl_soundtester.c)
set_target_properties(soundtester_alsa PROPERTIES
                      OUTPUT_NAME asound_module_pcm_soundtester PREFIX lib SUFFIX .so)

target_include_directories(soundtester_alsa PRIVATE ../app/src)
target_compile_options(soundtester_alsa PRIVATE -O2 -Wall -Wextra)
# SND_DLSYM_BUILD_VERSION keys off the PIC macro being DEFINED, not off -fPIC being passed:
# without it the build takes the static-link branch and fails on snd_dlsym_start.
target_compile_definitions(soundtester_alsa PRIVATE PIC)
# The PCM half connects and reconnects off the audio path.
find_package(Threads REQUIRED)
target_link_libraries(soundtester_alsa PRIVATE PkgConfig::ALSA Threads::Threads -Wl,--no-undefined)

if(ST_VORBIS)
  target_compile_definitions(soundtester_alsa PRIVATE ST_HAVE_VORBIS)
  target_link_libraries(soundtester_alsa PRIVATE PkgConfig::VORBIS)
endif()

# One object answers to both names.
add_custom_command(TARGET soundtester_alsa POST_BUILD
                   COMMAND ${CMAKE_COMMAND} -E create_symlink ${ST_PCM_SO} ${ST_CTL_SO}
                   WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# Where alsa-lib looks, per this machine's alsa.pc.
pkg_get_variable(ALSA_LIBDIR alsa libdir)
if(NOT ALSA_PLUGIN_DIR)
  set(ALSA_PLUGIN_DIR "${ALSA_LIBDIR}/alsa-lib")
endif()

install(TARGETS soundtester_alsa DESTINATION "${ALSA_PLUGIN_DIR}")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${ST_CTL_SO} DESTINATION "${ALSA_PLUGIN_DIR}")
install(FILES 50-soundtester.conf DESTINATION "share/alsa/alsa.conf.d")
