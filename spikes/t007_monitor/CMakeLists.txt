cmake_minimum_required(VERSION 3.20)
project(spike_t007_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(spike_t007_monitor main.cpp)
target_compile_definitions(spike_t007_monitor PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN
                           NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
target_link_libraries(spike_t007_monitor PRIVATE user32 gdi32 shcore dwmapi)
target_link_options(spike_t007_monitor PRIVATE -municode -static-libgcc -static-libstdc++)
