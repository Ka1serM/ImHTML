cmake_minimum_required(VERSION 3.20)
project(ImHTML VERSION 0.1.0 LANGUAGES CXX)

# ImHTML ships two targets:
#
#   ImHTML::Core - the framework. Draws through an ImGui draw list, lays out
#                  through litehtml, owns no window and no render loop.
#   ImHTML::App  - an optional shell on top of Core (font setup, SVG image
#                  cache, fragments, theme). It adds lunasvg and nothing else.
#
# Neither target links a windowing library. ImHTML never creates a window, reads
# no display metrics of its own, and calls no platform API: the host installs
# the two optional services (open a URL, toggle IME) through SetPlatformHooks.
#
# Dependencies are injected by the embedding project, which keeps ImHTML out of
# the business of choosing versions. Every required target must exist before
# this directory is added.

option(IMHTML_BUILD_APP "Build the optional ImHTML::App shell (needs lunasvg)" ON)
option(IMHTML_BUILD_TESTS "Build the ImHTML test suites" OFF)
# Installing exports ImHTML's link interface, which only works if the injected
# dependency targets are themselves installed and exported. That is true when
# ImHTML is built and installed on its own, and usually false when it is one
# add_subdirectory inside a larger build — so the default follows that.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IMHTML_INSTALL_DEFAULT ON)
else()
    set(IMHTML_INSTALL_DEFAULT OFF)
endif()
option(IMHTML_INSTALL "Generate install and package rules for ImHTML" ${IMHTML_INSTALL_DEFAULT})

# Projects vendor dependencies under different target names, so they are named
# by variable rather than hardcoded.
set(IMHTML_LUNASVG_TARGET "lunasvg::lunasvg" CACHE STRING "CMake target providing lunasvg to ImHTML::App")

# The list API takes nlohmann/json values, so that header is public. Projects
# that already vendor nlohmann can point at their own target; otherwise inja
# brings its bundled copy along.
if(NOT IMHTML_JSON_TARGET)
    if(TARGET nlohmann_json::nlohmann_json)
        set(IMHTML_JSON_TARGET nlohmann_json::nlohmann_json)
    else()
        set(IMHTML_JSON_TARGET pantor::inja)
    endif()
endif()

set(IMHTML_CORE_DEPENDENCIES imgui litehtml pantor::inja ${IMHTML_JSON_TARGET})
if(IMHTML_BUILD_APP)
    list(APPEND IMHTML_APP_DEPENDENCIES ${IMHTML_LUNASVG_TARGET})
endif()

foreach(dependency IN LISTS IMHTML_CORE_DEPENDENCIES IMHTML_APP_DEPENDENCIES)
    if(NOT TARGET ${dependency})
        message(FATAL_ERROR
            "ImHTML requires the CMake target '${dependency}'. Provide it before "
            "add_subdirectory(ImHTML), or turn off IMHTML_BUILD_APP if you only need the core.")
    endif()
endforeach()

#
# Core
#

add_library(ImHTML
    src/core.cpp
    src/app/theme.cpp
    src/document.cpp
    src/scroll/scroll_state.cpp
    src/template/templates.cpp
    src/version.cpp
)
add_library(ImHTML::Core ALIAS ImHTML)
# Kept so existing `ImHTML::ImHTML` links keep working.
add_library(ImHTML::ImHTML ALIAS ImHTML)

target_compile_features(ImHTML PUBLIC cxx_std_23)
set_target_properties(ImHTML PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    EXPORT_NAME Core
)

target_include_directories(ImHTML
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# inja is an implementation detail: it never appears in a public header. Only
# the JSON value type it is built on does.
target_link_libraries(ImHTML
    PUBLIC imgui litehtml ${IMHTML_JSON_TARGET}
    PRIVATE pantor::inja
)

#
# App shell
#

if(IMHTML_BUILD_APP)
    add_library(ImHTMLApp
        src/app/window.cpp
        src/app/image_cache.cpp
    )
    add_library(ImHTML::App ALIAS ImHTMLApp)

    set_target_properties(ImHTMLApp PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        EXPORT_NAME App
    )

    target_include_directories(ImHTMLApp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(ImHTMLApp
        PUBLIC ImHTML
        PRIVATE ${IMHTML_LUNASVG_TARGET}
    )
endif()

#
# Tests
#

if(IMHTML_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

#
# Install and package
#

if(IMHTML_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    set(IMHTML_INSTALL_TARGETS ImHTML)
    if(IMHTML_BUILD_APP)
        list(APPEND IMHTML_INSTALL_TARGETS ImHTMLApp)
    endif()

    install(TARGETS ${IMHTML_INSTALL_TARGETS}
        EXPORT ImHTMLTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )

    # Headers keep their imhtml/ prefix so consumers write <imhtml/app.hpp> and
    # nothing flat lands on their include path.
    install(DIRECTORY include/imhtml
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp"
    )

    install(EXPORT ImHTMLTargets
        FILE ImHTMLTargets.cmake
        NAMESPACE ImHTML::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ImHTML
    )

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ImHTMLConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ImHTMLConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ImHTML
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ImHTMLConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ImHTMLConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ImHTMLConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ImHTML
    )
    install(FILES LICENSE THIRD_PARTY_NOTICES.md
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/licenses/ImHTML
    )
endif()
