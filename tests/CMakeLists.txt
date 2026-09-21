# Test suites for ImHTML. Built as part of the library (IMHTML_BUILD_TESTS=ON)
# so they use the same dependency targets the library itself was given — no
# second CMake project, and no reaching into an embedding project's build tree.

set(IMHTML_TEST_DATA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/data")

add_library(imhtml_test_harness INTERFACE)
target_include_directories(imhtml_test_harness INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
)
target_link_libraries(imhtml_test_harness INTERFACE ImHTML)
target_compile_definitions(imhtml_test_harness INTERFACE
    IMHTML_TEST_DATA_DIR="${IMHTML_TEST_DATA_DIR}"
    # The suites predate the rename; keep the old spelling working.
    IMHTML_BENCH_DATA_DIR="${IMHTML_TEST_DATA_DIR}"
)

# White-box suites link the implementation directly, so they may include
# internal headers and call internal entry points.
function(imhtml_add_test name source)
    add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE imhtml_test_harness)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES TIMEOUT 300)
endfunction()

imhtml_add_test(imhtml_test_layout test_layout.cpp)
imhtml_add_test(imhtml_test_scroll test_scroll.cpp)
imhtml_add_test(imhtml_test_styles test_styles.cpp)
imhtml_add_test(imhtml_test_switching test_switching.cpp)
imhtml_add_test(imhtml_test_paint test_paint.cpp)

# Diagnostics, not tests: built on demand, never registered with CTest.
add_executable(imhtml_bench EXCLUDE_FROM_ALL bench_resize.cpp)
target_link_libraries(imhtml_bench PRIVATE imhtml_test_harness)

add_executable(imhtml_diag EXCLUDE_FROM_ALL diag_layout.cpp)
target_link_libraries(imhtml_diag PRIVATE imhtml_test_harness)

add_executable(imhtml_diag_switching EXCLUDE_FROM_ALL diag_switching.cpp)
target_link_libraries(imhtml_diag_switching PRIVATE imhtml_test_harness)
