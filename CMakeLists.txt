cmake_minimum_required(VERSION 3.10)
project(ARFPlayer C CXX)

# Prefer the GLVND libGLX/libOpenGL split over the legacy libGL.
set(OpenGL_GL_PREFERENCE GLVND)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(ARFPLAYER_BUILD_PLAYER "Build the OpenGL player (needs GL, GLEW and X11)" ON)
option(ARFPLAYER_BUILD_EXAMPLES "Build the C++ binding example" ON)

# ---------------------------------------------------------------------------
# libzip -- the ZIP container.  System package rather than a vendored copy:
# Debian/Ubuntu  sudo apt install libzip-dev
# Fedora         sudo dnf install libzip-devel
# macOS          brew install libzip
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBZIP IMPORTED_TARGET libzip)
endif()

if(TARGET PkgConfig::LIBZIP)
    set(ARF_ZIP PkgConfig::LIBZIP)
else()
    find_package(libzip QUIET CONFIG)
    if(TARGET libzip::zip)
        set(ARF_ZIP libzip::zip)
    else()
        find_path(LIBZIP_INCLUDE_DIR NAMES zip.h)
        find_library(LIBZIP_LIBRARY NAMES zip)
        if(NOT LIBZIP_INCLUDE_DIR OR NOT LIBZIP_LIBRARY)
            message(FATAL_ERROR
                "libzip not found. Install it first — Debian/Ubuntu: sudo apt install libzip-dev")
        endif()
        add_library(arf_libzip INTERFACE)
        target_include_directories(arf_libzip INTERFACE ${LIBZIP_INCLUDE_DIR})
        target_link_libraries(arf_libzip INTERFACE ${LIBZIP_LIBRARY})
        set(ARF_ZIP arf_libzip)
    endif()
endif()

# ---------------------------------------------------------------------------
# libarf -- the core.  Depends on nothing but libzip and libm, which is what
# lets the Python binding load it as a plain shared object.
# ---------------------------------------------------------------------------
set(LIBARF_SOURCES
    src/libarf/arf_json.c
    src/libarf/arf_glb.c
    src/libarf/arf_reader.c
    src/libarf/arf_writer.c
    src/libarf/arf_skin.c
    src/libarf/arf_despike.c
)

add_library(arf_static STATIC ${LIBARF_SOURCES})
target_include_directories(arf_static PUBLIC src/libarf)
target_link_libraries(arf_static PUBLIC ${ARF_ZIP} PRIVATE m)
set_target_properties(arf_static PROPERTIES OUTPUT_NAME arf POSITION_INDEPENDENT_CODE ON)

# The Python ctypes binding loads this one.
add_library(arf SHARED ${LIBARF_SOURCES})
target_include_directories(arf PUBLIC src/libarf)
target_link_libraries(arf PRIVATE ${ARF_ZIP} m)

foreach(target arf arf_static)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
endforeach()

# ---------------------------------------------------------------------------
# arfplay -- the viewer
# ---------------------------------------------------------------------------
if(ARFPLAYER_BUILD_PLAYER)
    find_package(OpenGL)
    find_package(GLEW)
    find_package(X11)

    if(OPENGL_FOUND AND GLEW_FOUND AND X11_FOUND)
        add_executable(arfplay
            src/player/arfplay.c
            src/player/arf_window.c
            src/player/arf_render.c
        )
        target_compile_options(arfplay PRIVATE -Wall -Wextra)
        target_compile_definitions(arfplay PRIVATE
            ARF_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders")
        # OpenGL::GLX carries glX*, which the window backend calls directly;
        # OpenGL::GL alone is only the GL entry points under GLVND.
        target_link_libraries(arfplay PRIVATE
            arf_static m
            GLEW::GLEW OpenGL::GL ${X11_LIBRARIES})
        if(TARGET OpenGL::GLX)
            target_link_libraries(arfplay PRIVATE OpenGL::GLX)
        endif()

        # So that ./arfplay works from the build directory without --shaders.
        add_custom_command(TARGET arfplay POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                    ${CMAKE_CURRENT_SOURCE_DIR}/shaders
                    $<TARGET_FILE_DIR:arfplay>/shaders)
    else()
        message(WARNING "OpenGL, GLEW or X11 not found -- building libarf only, skipping arfplay")
    endif()
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(ARFPLAYER_BUILD_EXAMPLES)
    add_executable(arfinfo_cpp examples/arfinfo.cpp)
    target_include_directories(arfinfo_cpp PRIVATE bindings/cpp src/libarf)
    target_link_libraries(arfinfo_cpp PRIVATE arf_static m)
endif()

install(TARGETS arf arf_static
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib)
install(FILES src/libarf/arf.h src/libarf/arf_format.h DESTINATION include/libarf)
if(TARGET arfplay)
    install(TARGETS arfplay RUNTIME DESTINATION bin)
    install(DIRECTORY shaders/ DESTINATION share/arfplayer/shaders)
endif()
