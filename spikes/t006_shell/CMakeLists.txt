cmake_minimum_required(VERSION 3.20)
project(dockforge_spike_t006 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(spike_t006_shell main.cpp)
target_compile_definitions(spike_t006_shell PRIVATE
  UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
target_link_libraries(spike_t006_shell PRIVATE
  user32 ole32 oleaut32 uuid shell32 shlwapi windowscodecs)

if(MINGW)
  target_link_options(spike_t006_shell PRIVATE -municode -static-libgcc -static-libstdc++)
endif()
