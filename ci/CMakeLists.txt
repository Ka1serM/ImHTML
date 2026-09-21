# The CI harness: builds the dependency targets ImHTML expects, then adds the
# library. It doubles as the smallest working example of how to embed ImHTML.

cmake_minimum_required(VERSION 3.20)
project(ImHTMLCI LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(IMHTML_DEPS "${CMAKE_CURRENT_SOURCE_DIR}/_deps")

# Dear ImGui: core only, no platform or renderer backend.
add_library(imgui STATIC
    "${IMHTML_DEPS}/imgui/imgui.cpp"
    "${IMHTML_DEPS}/imgui/imgui_draw.cpp"
    "${IMHTML_DEPS}/imgui/imgui_tables.cpp"
    "${IMHTML_DEPS}/imgui/imgui_widgets.cpp"
)
target_include_directories(imgui PUBLIC "${IMHTML_DEPS}/imgui")

set(LITEHTML_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LITEHTML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LITEHTML_ENABLE_LINT OFF CACHE BOOL "" FORCE)
add_subdirectory("${IMHTML_DEPS}/litehtml" litehtml EXCLUDE_FROM_ALL)

set(INJA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INJA_INSTALL OFF CACHE BOOL "" FORCE)
set(INJA_EXPORT OFF CACHE BOOL "" FORCE)
add_subdirectory("${IMHTML_DEPS}/inja" inja EXCLUDE_FROM_ALL)

# CTest only walks subdirectories when the top-level project enables testing.
enable_testing()

# The App shell needs lunasvg; CI covers the core and the test suites.
set(IMHTML_BUILD_APP OFF CACHE BOOL "" FORCE)
set(IMHTML_BUILD_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(.. imhtml)
