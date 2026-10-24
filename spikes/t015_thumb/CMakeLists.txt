cmake_minimum_required(VERSION 3.20)
project(spike_t015_thumb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(spike_t015_thumb main.cpp)

target_compile_definitions(spike_t015_thumb PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)

target_link_libraries(spike_t015_thumb PRIVATE user32 gdi32 dwmapi shcore)

target_link_options(spike_t015_thumb PRIVATE -municode -static-libgcc -static-libstdc++)
