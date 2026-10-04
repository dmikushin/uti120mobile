cmake_minimum_required(VERSION 3.20)
project(uti120 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(cmake/Dependencies.cmake)

add_library(uti120core STATIC
  src/frame.cpp
  src/device.cpp
  src/process.cpp
  src/palette.cpp
  src/stream.cpp
  src/log.cpp)
target_include_directories(uti120core PUBLIC src)
target_compile_options(uti120core PRIVATE -Wall -Wextra)
target_link_libraries(uti120core PUBLIC deps::usb deps::z Threads::Threads)

add_executable(uti120
  src/main.cpp
  src/scaler.cpp
  src/video.cpp
  src/png.cpp
  src/display.cpp)
target_compile_options(uti120 PRIVATE -Wall -Wextra)
target_compile_definitions(uti120 PRIVATE UTI120_VERSION="${PROJECT_VERSION}")
# Everything but the C/C++ runtime is linked statically.
target_link_options(uti120 PRIVATE -static-libstdc++ -static-libgcc)
target_link_libraries(uti120 PRIVATE uti120core deps::avformat deps::avcodec
                      deps::swscale deps::avutil deps::sdl3)
install(TARGETS uti120)

enable_testing()
add_executable(uti120_tests tests/tests.cpp)
target_compile_options(uti120_tests PRIVATE -Wall -Wextra)
target_compile_definitions(uti120_tests PRIVATE
  TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../tests/data")
target_link_libraries(uti120_tests PRIVATE uti120core)
add_test(NAME uti120_tests COMMAND uti120_tests)
