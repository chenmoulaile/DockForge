cmake_minimum_required(VERSION 3.20)
project(spike_t010_hover CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

add_executable(spike_t010_hover main.cpp)
target_link_libraries(spike_t010_hover PRIVATE user32 gdi32)
target_link_options(spike_t010_hover PRIVATE -municode -static-libgcc -static-libstdc++)

add_executable(spike_t010_region region.cpp)
target_link_libraries(spike_t010_region PRIVATE user32 gdi32 ole32 uuid d3d11 dxgi d2d1 dcomp)
target_link_options(spike_t010_region PRIVATE -municode -static-libgcc -static-libstdc++)

add_executable(spike_t010_region_click region_click.cpp)
target_link_libraries(spike_t010_region_click PRIVATE user32 gdi32)
target_link_options(spike_t010_region_click PRIVATE -municode -static-libgcc -static-libstdc++)
