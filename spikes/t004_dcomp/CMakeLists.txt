cmake_minimum_required(VERSION 3.20)
project(spike_t004_dcomp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

add_compile_definitions(
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006
)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

add_executable(spike_t004_dcomp main.cpp)
# shcore provides GetDpiForMonitor (Windows 8.1+).
target_link_libraries(spike_t004_dcomp PRIVATE
  user32 gdi32 ole32 uuid d3d11 dxgi d2d1 dcomp shcore)

if(MINGW)
  # Console subsystem: the spike's whole product is its stdout report.
  target_link_options(spike_t004_dcomp PRIVATE -municode -static-libgcc -static-libstdc++)
endif()
