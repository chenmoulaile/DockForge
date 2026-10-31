cmake_minimum_required(VERSION 3.20)

project(DockForge VERSION 0.1.1 LANGUAGES CXX)

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
  d2d1 dwrite dcomp psapi advapi32 d3d11 dxgi shcore taskschd dbghelp oleacc
  winhttp version)

# ---------------------------------------------------------------- executable
add_executable(DockForge ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(DockForge PRIVATE dockforge_lib)

# One source of truth for the version string: the project() call above. main.cpp
# reports it at startup so a log line identifies the build that produced it.
target_compile_definitions(DockForge PRIVATE DOCKFORGE_VERSION="${PROJECT_VERSION}")

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

  # T-068 hardware sidecar: GSMTC media control + Windows.Devices.Radios cannot
  # be driven from the MinGW process (WinRT async never completes there;
  # spikes/T-068_hw_probe), so a .NET Framework exe speaks a line protocol
  # over pipes. Built by the stock Windows csc (no SDK needed); when csc is
  # missing the target degrades to a message and the panel reports media/radio
  # as unavailable at runtime instead of failing the build.
  set(DOCKFORGE_CSC "" CACHE FILEPATH "Path to csc.exe for the hw_sidecar build")
  if(NOT DOCKFORGE_CSC)
    foreach(_csc_candidate
        "C:/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe"
        "C:/Windows/Microsoft.NET/Framework/v4.0.30319/csc.exe")
      if(EXISTS "${_csc_candidate}")
        set(DOCKFORGE_CSC "${_csc_candidate}")
        break()
      endif()
    endforeach()
  endif()
  if(DOCKFORGE_CSC)
    # csc.exe splits any argument containing forward slashes, so a path like
    # D:/MacDock/tools/x.cs arrives as a bare "x.cs" resolved against the build
    # directory and the compile fails with CS1504. Every path handed to csc
    # therefore goes through TO_NATIVE_PATH first - this is why a clean build
    # of these two targets used to fail while an incremental one succeeded.
    file(TO_NATIVE_PATH "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}" _df_csc_out_dir)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" _df_csc_src_dir)
    add_custom_command(OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/hw_sidecar.exe"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      COMMAND "${DOCKFORGE_CSC}" -nologo -target:exe
              "-out:${_df_csc_out_dir}\\hw_sidecar.exe"
              -r:System.Runtime.WindowsRuntime.dll
              -r:System.Runtime.InteropServices.WindowsRuntime.dll
              "-r:C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\System.Runtime.dll"
              "-r:C:\\Windows\\System32\\WinMetadata\\Windows.Foundation.winmd"
              "-r:C:\\Windows\\System32\\WinMetadata\\Windows.Media.winmd"
              "-r:C:\\Windows\\System32\\WinMetadata\\Windows.Devices.winmd"
              "${_df_csc_src_dir}\\tools\\hw_sidecar.cs"
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/hw_sidecar.cs
      COMMENT "csc: hw_sidecar.exe (T-068 media/radio bridge)"
      VERBATIM)
    add_custom_target(hw_sidecar ALL
      SOURCES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/hw_sidecar.exe")

    # T-076 notification sidecar: UserNotificationListener's async calls never
    # complete in this process even with RoInitialize(STA) and a real message
    # pump (evidence/T-076/spike_async_pump.log stayed Started for 8s), so the
    # listener lives in a .NET child and speaks the same line protocol as
    # hw_sidecar. Same csc dependency, same degradation rule: without csc the
    # centre runs with no history instead of failing the build.
    add_custom_command(
      OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/notification_sidecar.exe"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      COMMAND "${DOCKFORGE_CSC}" -nologo -target:exe
              "-out:${_df_csc_out_dir}\\notification_sidecar.exe"
              -r:System.Runtime.WindowsRuntime.dll
              -r:System.Runtime.InteropServices.WindowsRuntime.dll
              "-r:C:\\Windows\\Microsoft.NET\\Framework64\\v4.0.30319\\System.Runtime.dll"
              "-r:C:\\Windows\\System32\\WinMetadata\\Windows.Foundation.winmd"
              "-r:C:\\Windows\\System32\\WinMetadata\\Windows.UI.winmd"
              "-r:C:\\Windows\\System32\\WinMetadata\\Windows.ApplicationModel.winmd"
              "${_df_csc_src_dir}\\tools\\notification_sidecar.cs"
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/notification_sidecar.cs
      COMMENT "csc: notification_sidecar.exe (T-076 notification bridge)"
      VERBATIM)
    add_custom_target(notification_sidecar ALL
      SOURCES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/notification_sidecar.exe")

    # T-076 clear-path stub: same line protocol, fixed two-row list, no WinRT.
    # The probe points DOCKFORGE_NOTIFICATION_SIDECAR at this binary so the
    # clear chain can be exercised end to end without wiping the operator's real
    # Windows notification centre. Plain C++ because it must not depend on csc
    # succeeding - the probe still needs it on a machine without a .NET SDK.
    add_custom_command(
      OUTPUT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/notification_sidecar_stub.exe"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
              "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
      COMMAND "${CMAKE_CXX_COMPILER}" -std=c++17 -O2 -static
              -o "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/notification_sidecar_stub.exe"
              "${CMAKE_CURRENT_SOURCE_DIR}/spikes/T-076_notification_center/stub_sidecar.cpp"
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/spikes/T-076_notification_center/stub_sidecar.cpp
      COMMENT "g++: notification_sidecar_stub.exe (T-076 clear-path probe)"
      VERBATIM)
    add_custom_target(notification_sidecar_stub ALL
      SOURCES "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/notification_sidecar_stub.exe")
  else()
    message(STATUS "hw_sidecar: csc.exe not found - media/radio controls will be unavailable")
  endif()
endif()
