# libuti120: the camera backend shared by all frontends.
#
# The including project provides libusb and zlib as targets named in
# UTI120_LIBUSB_TARGET and UTI120_ZLIB_TARGET (the desktop superbuild builds
# them statically; the Android build uses its own libusb and the NDK's zlib).

if(NOT UTI120_LIBUSB_TARGET OR NOT UTI120_ZLIB_TARGET)
  message(FATAL_ERROR "set UTI120_LIBUSB_TARGET and UTI120_ZLIB_TARGET before adding cpp/core")
endif()

find_package(Threads REQUIRED)

add_library(uti120core STATIC
  src/frame.cpp
  src/device.cpp
  src/process.cpp
  src/palette.cpp
  src/stream.cpp
  src/pipeline.cpp
  src/replay.cpp
  src/log.cpp)
target_compile_features(uti120core PUBLIC cxx_std_20)
target_include_directories(uti120core PUBLIC include)
target_compile_options(uti120core PRIVATE -Wall -Wextra)
target_link_libraries(uti120core PUBLIC ${UTI120_LIBUSB_TARGET} ${UTI120_ZLIB_TARGET}
                      Threads::Threads)
if(ANDROID)
  target_link_libraries(uti120core PUBLIC log)  # log.cpp writes to logcat
endif()

option(UTI120_BUILD_TESTS "Build the offline tests of the backend" ON)
if(UTI120_BUILD_TESTS)
  add_executable(uti120_tests tests/tests.cpp)
  target_compile_options(uti120_tests PRIVATE -Wall -Wextra)
  target_compile_definitions(uti120_tests PRIVATE
    TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../tests/data")
  target_link_libraries(uti120_tests PRIVATE uti120core)
  add_test(NAME uti120_tests COMMAND uti120_tests)
endif()
