cmake_minimum_required(VERSION 3.20)
project(spike_t005_material LANGUAGES CXX)

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

add_executable(spike_t005_material main.cpp)
target_link_libraries(spike_t005_material PRIVATE
  user32 gdi32 ole32 uuid d3d11 dxgi d2d1 dcomp dwmapi)

# Part 2 of the spike: can we capture the desktop behind the dock without
# capturing the dock itself? Shares the material probe's link set.
add_executable(spike_t005_capture capture.cpp)
target_link_libraries(spike_t005_capture PRIVATE
  user32 gdi32 ole32 uuid d3d11 dxgi d2d1 dcomp dwmapi)

# Part 3: self-drawn frosted plate (capture -> D2D blur -> rounded clip), with the
# frame-cost measurement that decides whether it can run every frame.
add_executable(spike_t005_blur blur.cpp)
target_link_libraries(spike_t005_blur PRIVATE
  user32 gdi32 ole32 uuid d3d11 dxgi d2d1 dcomp dwmapi)

# Part 4: DXGI Desktop Duplication, the GPU alternative to BitBlt. Part 3 measured
# BitBlt at a fixed ~78ms regardless of region, which rules it out for 60 FPS.
add_executable(spike_t005_dupl dupl.cpp)
target_link_libraries(spike_t005_dupl PRIVATE
  user32 gdi32 ole32 uuid d3d11 dxgi d2d1 dcomp)

# Part 5: explains the 78ms capture constant by separating compositor round-trip
# from GDI blit cost.
add_executable(spike_t005_capcost capcost.cpp)
target_link_libraries(spike_t005_capcost PRIVATE user32 gdi32)

# Part 6: a checkerboard backdrop, so "the plate blurs" is measurable rather than
# a claim about how it looks.
add_executable(spike_t005_checker checker.cpp)
target_link_libraries(spike_t005_checker PRIVATE user32 gdi32)

if(MINGW)
  # Console subsystem: the spike's product is its stdout report.
  target_link_options(spike_t005_material PRIVATE -municode -static-libgcc -static-libstdc++)
  target_link_options(spike_t005_capture PRIVATE -municode -static-libgcc -static-libstdc++)
  target_link_options(spike_t005_blur PRIVATE -municode -static-libgcc -static-libstdc++)
  target_link_options(spike_t005_dupl PRIVATE -municode -static-libgcc -static-libstdc++)
  target_link_options(spike_t005_capcost PRIVATE -municode -static-libgcc -static-libstdc++)
  target_link_options(spike_t005_checker PRIVATE -municode -static-libgcc -static-libstdc++)
endif()
