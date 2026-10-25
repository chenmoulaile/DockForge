cmake_minimum_required(VERSION 3.20)

project(DockForge VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_definitions(
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00
  NTDDI_VERSION=0x0A000006
)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()

# ---------------------------------------------------------------- third_party
add_library(thirdparty_nlohmann INTERFACE)
target_include_directories(thirdparty_nlohmann INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann/include)

# ---------------------------------------------------------------- core static lib
file(GLOB_RECURSE DOCKFORGE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER DOCKFORGE_SOURCES EXCLUDE REGEX "/src/main\\.cpp$")

add_library(dockforge_lib STATIC ${DOCKFORGE_SOURCES})
target_include_directories(dockforge_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dockforge_lib PUBLIC thirdparty_nlohmann)

target_link_libraries(dockforge_lib PUBLIC
  user32 gdi32 shell32 shlwapi ole32 oleaut32 uuid dwmapi
  d2d1 dwrite dcomp psapi advapi32 d3d11 dxgi shcore taskschd dbghelp)

# ---------------------------------------------------------------- executable
add_executable(DockForge ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(DockForge PRIVATE dockforge_lib)

if(MINGW)
  # wWinMain needs the unicode CRT entry point; -mwindows hides the console.
  target_link_options(DockForge PRIVATE -municode -mwindows -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------- tests
option(DOCKFORGE_BUILD_TESTS "Build unit tests" ON)
if(DOCKFORGE_BUILD_TESTS)
  enable_testing()
  file(GLOB_RECURSE DOCKFORGE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
  if(DOCKFORGE_TEST_SOURCES)
    add_executable(dockforge_tests
      ${DOCKFORGE_TEST_SOURCES}
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2/catch_amalgamated.cpp)
    target_include_directories(dockforge_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/third_party/catch2)
    # The global UNICODE define makes Catch2 emit wmain(); the console
    # subsystem entry point wants plain main, so pin it explicitly.
    target_compile_definitions(dockforge_tests PRIVATE DO_NOT_USE_WMAIN)
    target_link_libraries(dockforge_tests PRIVATE dockforge_lib)
    add_test(NAME dockforge_tests COMMAND dockforge_tests)
  endif()
endif()

# ---------------------------------------------------------------- benchmarks
# Console tools, kept out of the default build target so a normal build does not
# pay for them, but reproducible: `cmake --build build --target dockforge_bench`.
option(DOCKFORGE_BUILD_BENCH "Build benchmark tools" ON)
if(DOCKFORGE_BUILD_BENCH)
  add_executable(layout_bench ${CMAKE_CURRENT_SOURCE_DIR}/tools/layout_bench.cpp)
  target_link_libraries(layout_bench PRIVATE dockforge_lib)
  if(MINGW)
    target_link_options(layout_bench PRIVATE -static-libgcc -static-libstdc++)
  endif()

  # T-060 system probe: drives the real Task Scheduler through the production
  # autostart module so tools/autostart_probe.ps1 can cross-check with schtasks.
  add_executable(autostart_probe ${CMAKE_CURRENT_SOURCE_DIR}/tools/autostart_probe.cpp)
  target_link_libraries(autostart_probe PRIVATE dockforge_lib)
  if(MINGW)
    target_link_options(autostart_probe PRIVATE -static-libgcc -static-libstdc++)
  endif()

  # T-062 log-rotation probe: drives the production Logger against a scratch
  # directory (DOCKFORGE_LOG_DIR) so size/age rotation is observable in seconds.
  add_executable(log_probe ${CMAKE_CURRENT_SOURCE_DIR}/tools/log_probe.cpp)
  target_link_libraries(log_probe PRIVATE dockforge_lib)
  if(MINGW)
    target_link_options(log_probe PRIVATE -static-libgcc -static-libstdc++)
  endif()

  # T-065 badge probe target: a GUI window titled "(4) badge probe" owned by
  # this process, so the title-badge adapter's process-name match sees it (a
  # console would be owned by WindowsTerminal and invisible to the adapter).
  # tools/message_counts_probe.ps1 puts a Start-Menu shortcut next to it, which
  # turns the exe into a real dock tile and closes the full badge chain.
  add_executable(message_badge_probe_app
                 ${CMAKE_CURRENT_SOURCE_DIR}/tools/message_badge_probe_app.cpp)
  if(MINGW)
    target_link_options(message_badge_probe_app PRIVATE -municode -mwindows
                        -static-libgcc -static-libstdc++)
  endif()
endif()
