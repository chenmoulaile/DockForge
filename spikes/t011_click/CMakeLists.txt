cmake_minimum_required(VERSION 3.20)
project(spike_t011_click CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)

add_executable(spike_t011_click main.cpp)
target_link_libraries(spike_t011_click PRIVATE user32 gdi32 dwmapi ole32 oleaut32 uuid)
target_link_options(spike_t011_click PRIVATE -municode -static-libgcc -static-libstdc++)

add_executable(spike_t011_foreground foreground.cpp)
target_link_libraries(spike_t011_foreground PRIVATE user32 gdi32 dwmapi)
target_link_options(spike_t011_foreground PRIVATE -municode -static-libgcc -static-libstdc++)
