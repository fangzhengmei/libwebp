# Standalone VP8L bitstream boundary regression test.
#
# Enabled only with -DWEBP_BUILD_VP8L_BOUNDS_TEST=ON so the default build and
# the existing fuzztest infrastructure are untouched. The test links the
# decoder-only static library and uses public decoding APIs.

enable_language(C)

add_executable(vp8l_bounds_test vp8l_bounds_test.c)
target_link_libraries(vp8l_bounds_test PRIVATE webpdecoder)
target_include_directories(vp8l_bounds_test PRIVATE ${CMAKE_SOURCE_DIR})

add_executable(vp8l_truncation_sweep vp8l_truncation_sweep.c)
target_link_libraries(vp8l_truncation_sweep PRIVATE webpdecoder)
target_include_directories(vp8l_truncation_sweep PRIVATE ${CMAKE_SOURCE_DIR})

add_test(
  NAME vp8l_bounds_test
  COMMAND
    vp8l_bounds_test "${CMAKE_CURRENT_SOURCE_DIR}/golden"
    "${CMAKE_SOURCE_DIR}/examples/test.webp")
add_test(
  NAME vp8l_truncation_sweep
  COMMAND
    vp8l_truncation_sweep
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/golden_1x1.webp"
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/golden_4x4.webp"
    "${CMAKE_CURRENT_SOURCE_DIR}/golden/golden_8x1.webp"
    "${CMAKE_SOURCE_DIR}/examples/test.webp")
