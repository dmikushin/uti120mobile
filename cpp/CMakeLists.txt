# Desktop superbuild: third-party libraries (cmake/Dependencies.cmake), the
# backend (core/) and the command-line frontend (desktop/), linked into one
# static executable.
cmake_minimum_required(VERSION 3.20)
project(uti120 VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(cmake/Dependencies.cmake)

enable_testing()
set(UTI120_LIBUSB_TARGET deps::usb)
set(UTI120_ZLIB_TARGET deps::z)
add_subdirectory(core)

add_executable(uti120
  desktop/main.cpp
  desktop/scaler.cpp
  desktop/video.cpp
  desktop/png.cpp
  desktop/display.cpp)
target_compile_options(uti120 PRIVATE -Wall -Wextra)
target_compile_definitions(uti120 PRIVATE UTI120_VERSION="${PROJECT_VERSION}")
# Everything but the C/C++ runtime is linked statically.
target_link_options(uti120 PRIVATE -static-libstdc++ -static-libgcc)
target_link_libraries(uti120 PRIVATE uti120core deps::avformat deps::avcodec
                      deps::swscale deps::avutil deps::sdl3)
install(TARGETS uti120)
