cmake_minimum_required(VERSION 3.20)
project(spike_t013_drag CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(spike_t013_drag main.cpp)

target_compile_definitions(spike_t013_drag PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
    _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)

target_link_libraries(spike_t013_drag PRIVATE user32 gdi32 comctl32)

target_link_options(spike_t013_drag PRIVATE -municode -static-libgcc -static-libstdc++)
