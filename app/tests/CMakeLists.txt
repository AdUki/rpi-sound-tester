function(add_st_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE soundtester_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_st_test(test_generators)
add_st_test(test_ring_buffer)
add_st_test(test_capture)
add_st_test(test_wav)
add_st_test(test_config)
add_st_test(test_opus)
add_st_test(test_dsp)
add_st_test(test_genie)
add_st_test(test_net_timeline)
add_st_test(test_net_protocol)
add_st_test(test_resample)
add_st_test(test_net_session)
add_st_test(test_soc_out)
add_st_test(test_engine)
add_st_test(test_board_profile)
add_st_test(test_analysis)
# The golden files a test compares against are read from the source tree, never copied.
target_compile_definitions(test_config PRIVATE ST_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
# That test encodes with libvorbis so the decoder is checked against a real encoder rather than
# against an assumption about one. The daemon itself only ever decodes, so vorbisenc is linked
# here and nowhere else.
pkg_check_modules(VORBISENC REQUIRED IMPORTED_TARGET vorbisenc)
target_link_libraries(test_net_session PRIVATE PkgConfig::VORBISENC)
