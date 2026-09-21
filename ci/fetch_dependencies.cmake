# Fetches the dependency sources CI builds ImHTML against.
#
# ImHTML never vendors these: an embedding project owns its own copies and
# versions. CI plays the part of that project, and this file records the
# revisions the library is actually tested against.

set(IMHTML_IMGUI_TAG "v1.92.6")
set(IMHTML_LITEHTML_TAG "master")
set(IMHTML_INJA_TAG "v3.5.0")

function(imhtml_clone name repository tag)
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/_deps/${name}")
        message(STATUS "${name} already present")
        return()
    endif()
    message(STATUS "Cloning ${name} @ ${tag}")
    execute_process(
        COMMAND git clone --depth 1 --branch ${tag} ${repository} "${CMAKE_CURRENT_LIST_DIR}/_deps/${name}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Failed to clone ${name}")
    endif()
endfunction()

imhtml_clone(imgui https://github.com/ocornut/imgui.git ${IMHTML_IMGUI_TAG})
imhtml_clone(litehtml https://github.com/litehtml/litehtml.git ${IMHTML_LITEHTML_TAG})
imhtml_clone(inja https://github.com/pantor/inja.git ${IMHTML_INJA_TAG})
