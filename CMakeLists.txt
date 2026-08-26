cmake_minimum_required(VERSION 3.20)
project(MinFlyout LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)   # std::format for the logging macros
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "MinFlyout is a pure Win32 application.")
endif()

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
add_executable(minflyout WIN32
    src/main.cpp
    src/Common.cpp
    src/AppController.cpp
    src/CaptionProbe.cpp
    src/CaptionGeometry.cpp
    src/Diagnostics.cpp
    src/HookThread.cpp
    src/FlyoutWindow.cpp
    src/ItemRegistry.cpp
    src/BuiltinProviders.cpp
    src/TrayStash.cpp
    src/Config.cpp
    src/Json.cpp
    src/Log.cpp
    src/Monitors.cpp
    src/MonitorSelector.cpp
    src/WindowSizer.cpp
    res/minflyout.rc          # application icon and version info
)

# res is on the include path so that resource.h is found both by the compiler
# and by the resource compiler.
target_include_directories(minflyout PRIVATE src res)
target_compile_definitions(minflyout PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
# ole32/uuid: SHGetKnownFolderPath and FOLDERID_RoamingAppData.
# DPI and DWM functions are resolved at run time via GetProcAddress.
target_link_libraries(minflyout PRIVATE user32 gdi32 shell32 advapi32 ole32 uuid)

if(MSVC)
    target_compile_options(minflyout PRIVATE /W4 /permissive- /utf-8)
    # With MSVC the manifest tool binds in the manifest.
    target_sources(minflyout PRIVATE res/minflyout.manifest)
else()
    target_compile_options(minflyout PRIVATE -Wall -Wextra -municode)
    target_link_options(minflyout PRIVATE -municode -static -static-libgcc -static-libstdc++)
    # MinGW has no manifest tool - there it comes from the resource file.
    set_source_files_properties(res/minflyout.rc PROPERTIES
                                COMPILE_DEFINITIONS MFLY_EMBED_MANIFEST)
endif()

# ---------------------------------------------------------------------------
# Unit tests (console program, exit code 0 = passed). Everything tested here is
# free of Windows dependencies: the JSONC parser, the monitor selectors and
# the caption button arithmetic.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(json_tests
    tests/JsonTest.cpp
    tests/SelectorTest.cpp
    tests/GeometryTest.cpp
    src/Json.cpp
    src/MonitorSelector.cpp
    src/CaptionGeometry.cpp
)
target_include_directories(json_tests PRIVATE src)
target_compile_definitions(json_tests PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(json_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(json_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME units COMMAND json_tests)

# ---------------------------------------------------------------------------
# Documentation:  cmake --build build --target docs
# ---------------------------------------------------------------------------
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating Doxygen documentation into build/docs/html"
        VERBATIM)
else()
    message(STATUS "Doxygen not found - target 'docs' is not available.")
endif()
