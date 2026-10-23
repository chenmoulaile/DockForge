cmake_minimum_required(VERSION 3.20)
project(spike_t014_drop LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(-Wall -Wextra)

add_executable(spike_t014_drop main.cpp)
target_link_libraries(spike_t014_drop PRIVATE user32 gdi32 ole32 oleaut32 uuid shell32 shlwapi)
target_link_options(spike_t014_drop PRIVATE -municode -static-libgcc -static-libstdc++)
