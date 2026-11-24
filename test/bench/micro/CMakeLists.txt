# Copyright 2021 The Lynx Authors. All rights reserved.
# Licensed under the Apache License Version 2.0 that can be found in the
# LICENSE file in the root directory of this source tree.

add_executable(skity_micro_bench
    array_list_benchmarks.cc
    hw_path_raster_benchmarks.cc
    matrix_benchmarks.cc
    micro_bench_main.cc
    sw_benchmarks.cc
    ${CMAKE_SOURCE_DIR}/example/case/basic/example.cc
    ${CMAKE_SOURCE_DIR}/example/case/basic/example.hpp
)

target_include_directories(skity_micro_bench
    PUBLIC
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/example
)

target_compile_options(skity_micro_bench PUBLIC -fno-rtti)
target_compile_options(skity_micro_bench PUBLIC -std=c++17)
target_compile_definitions(skity_micro_bench PUBLIC -DDISABLE_SKITY_EXPERIMENTAL_WARNINGS)

# On MSVC the accelerator archive must precede the skity import library:
# skity.dll exports the runtime switch (SW_ACCEL_API reuses SKITY_API), so
# skity.lib provides it as well, and link.exe reports LNK2005 when the
# archive's definition is scanned after the import-library thunk. With this
# order the bench binds to its own statically linked copy, so the
# runtime-switch A/B arms stay meaningless on Windows (as they already were
# before the export existed). Everywhere else skity comes first and the
# toggle reaches the shared library's switch, which is what the A/B arms
# are supposed to measure.
if(MSVC AND TARGET skity_sw_accel)
  target_link_libraries(skity_micro_bench PRIVATE skity_sw_accel)
endif()

target_link_libraries(skity_micro_bench
    PUBLIC
    skity::skity
    benchmark::benchmark
)

target_link_libraries(skity_micro_bench PRIVATE glm::glm-header-only)

# sw_benchmarks.cc drives FillSolidSpans directly; it is hidden from the
# shared library (only the runtime switch is exported), so the bench needs
# the accelerator statically, like skity_unit_test.
if(NOT MSVC AND TARGET skity_sw_accel)
  target_link_libraries(skity_micro_bench PRIVATE skity_sw_accel)
endif()
