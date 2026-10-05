# Native part of the Android app: the shared backend (cpp/core), libusb built
# from its pinned release with the NDK, and the JNI bridge.
cmake_minimum_required(VERSION 3.22)
project(uti120_android C CXX)

include(FetchContent)
FetchContent_Declare(libusb
  URL https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.tar.bz2
  URL_HASH SHA256=5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85
  # libusb has no CMake build; only fetch it.
  SOURCE_SUBDIR no-cmake-build)
FetchContent_MakeAvailable(libusb)
set(LIBUSB ${libusb_SOURCE_DIR})

# Source list and flags as in libusb's own android/jni/libusb.mk.
add_library(usb STATIC
  ${LIBUSB}/libusb/core.c
  ${LIBUSB}/libusb/descriptor.c
  ${LIBUSB}/libusb/hotplug.c
  ${LIBUSB}/libusb/io.c
  ${LIBUSB}/libusb/sync.c
  ${LIBUSB}/libusb/strerror.c
  ${LIBUSB}/libusb/os/linux_usbfs.c
  ${LIBUSB}/libusb/os/events_posix.c
  ${LIBUSB}/libusb/os/threads_posix.c
  ${LIBUSB}/libusb/os/linux_netlink.c)
target_include_directories(usb PRIVATE ${LIBUSB}/android ${LIBUSB}/libusb/os
                           PUBLIC ${LIBUSB}/libusb)
target_compile_options(usb PRIVATE -fvisibility=hidden -pthread)
target_link_libraries(usb PRIVATE log)

set(UTI120_LIBUSB_TARGET usb)
set(UTI120_ZLIB_TARGET z)
set(UTI120_BUILD_TESTS OFF)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../../cpp/core
                 ${CMAKE_CURRENT_BINARY_DIR}/core)

add_library(uti120_jni SHARED uti120_jni.cpp)
target_compile_options(uti120_jni PRIVATE -Wall -Wextra)
target_link_libraries(uti120_jni PRIVATE uti120core jnigraphics log)
# 16 KB page alignment, required for apps targeting Android 15+ devices.
target_link_options(uti120_jni PRIVATE -Wl,-z,max-page-size=16384)
