cmake_minimum_required(VERSION 3.20)
project(MinFlyout LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 20)   # std::format for the logging macros
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT WIN32)
    message(FATAL_ERROR "MinFlyout is a pure Win32 application.")
endif()

# ---------------------------------------------------------------------------
# Version information: res/Version.h from Version.txt and git, written by the
# same script the Visual Studio pre-build event runs. Once at configure time so
# the header exists, and before every build so a new commit shows up without
# re-running CMake. The script only rewrites the file when its content
# changes, so an unchanged tree does not rebuild the resources.
# ---------------------------------------------------------------------------
find_program(MFLY_POWERSHELL NAMES pwsh powershell REQUIRED)
set(MFLY_VERSION_COMMAND
    ${MFLY_POWERSHELL} -NoProfile -NonInteractive -ExecutionPolicy Bypass
    -File ${CMAKE_CURRENT_SOURCE_DIR}/tools/GenerateVersion.ps1
    -ProjectDir ${CMAKE_CURRENT_SOURCE_DIR}
    -OutputPath ${CMAKE_CURRENT_SOURCE_DIR}/res/Version.h
    -DoxygenPath ${CMAKE_CURRENT_SOURCE_DIR}/Version.doxyfile
    -VersionFile ${CMAKE_CURRENT_SOURCE_DIR}/Version.txt)
execute_process(COMMAND ${MFLY_VERSION_COMMAND})
add_custom_target(version
    COMMAND ${MFLY_VERSION_COMMAND}
    BYPRODUCTS ${CMAKE_CURRENT_SOURCE_DIR}/res/Version.h
               ${CMAKE_CURRENT_SOURCE_DIR}/Version.doxyfile
    COMMENT "Generating res/Version.h and Version.doxyfile from Version.txt and git")

# ---------------------------------------------------------------------------
# Main application
# ---------------------------------------------------------------------------
add_executable(minflyout WIN32
    src/main.cpp
    src/Common.cpp
    src/AboutDialog.cpp
    src/AppController.cpp
    src/CaptionProbe.cpp
    src/CaptionGeometry.cpp
    src/Diagnostics.cpp
    src/DragWatcher.cpp
    src/HookThread.cpp
    src/Hotkey.cpp
    src/FlyoutWindow.cpp
    src/OverlayGeometry.cpp
    src/Painting.cpp
    src/ZoneOverlay.cpp
    src/ItemRegistry.cpp
    src/BuiltinProviders.cpp
    src/TrayStash.cpp
    src/Config.cpp
    src/ConfigWatcher.cpp
    src/SettingsBackup.cpp
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
# comctl32: list view and tooltip of the About dialog.
target_link_libraries(minflyout PRIVATE user32 gdi32 shell32 advapi32 ole32 uuid comctl32)
add_dependencies(minflyout version)

# The log viewer (tray menu "Open log as HTML") looks for its scripts next to
# the executable. The pCloud deployment exists only in minflyout.vcxproj.
add_custom_command(TARGET minflyout POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/Debug-Log.cmd
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/Debug-Log.ps1
            $<TARGET_FILE_DIR:minflyout>
    COMMENT "Copying the log viewer scripts next to the executable")

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

# The overlay arithmetic, likewise free of Windows dependencies. Its own target
# because it carries its own main; fold it into json_tests once the three
# sources above are back in the working tree.
add_executable(overlay_tests
    tests/OverlayGeometryTest.cpp
    src/OverlayGeometry.cpp
)
target_include_directories(overlay_tests PRIVATE src)
target_compile_definitions(overlay_tests PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(overlay_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(overlay_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME overlay COMMAND overlay_tests)

# The key combination parser behind "openHotkey", same arrangement.
add_executable(hotkey_tests
    tests/HotkeyTest.cpp
    src/Hotkey.cpp
)
target_include_directories(hotkey_tests PRIVATE src)
target_compile_definitions(hotkey_tests PRIVATE UNICODE _UNICODE)
if(MSVC)
    target_compile_options(hotkey_tests PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(hotkey_tests PRIVATE -Wall -Wextra)
endif()

add_test(NAME hotkey COMMAND hotkey_tests)

# ---------------------------------------------------------------------------
# Documentation:  cmake --build build --target docs
# ---------------------------------------------------------------------------
find_package(Doxygen)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/MinFlyout.dox
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating Doxygen documentation into build/docs/html"
        VERBATIM)
    # MinFlyout.dox pulls in Version.doxyfile, which the version target writes.
    add_dependencies(docs version)
else()
    message(STATUS "Doxygen not found - target 'docs' is not available.")
endif()
