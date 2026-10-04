# Third-party libraries, built from pinned sources into ${DEPS_PREFIX} as static
# archives and linked into the single uti120 executable.
#
#   libusb   USB access
#   zlib     CRC-32 of frames, PNG compression, gzip test fixtures
#   x264     H.264 encoder
#   FFmpeg   libavformat (MP4 muxer), libavcodec (libx264 + PNG encoders),
#            libswscale (upscaling, RGB -> YUV) -- configured with only those parts
#   SDL3     live window; display drivers (X11/Wayland) are loaded at run time

include(ExternalProject)
include(ProcessorCount)

ProcessorCount(NPROC)
if(NPROC EQUAL 0)
  set(NPROC 1)
endif()

set(DEPS_PREFIX ${CMAKE_BINARY_DIR}/deps)
set(DEPS_DOWNLOAD_DIR ${CMAKE_BINARY_DIR}/download CACHE PATH
    "Where dependency source archives are downloaded")
# Imported targets need their include directories to exist at configure time.
file(MAKE_DIRECTORY ${DEPS_PREFIX}/include/libusb-1.0 ${DEPS_PREFIX}/lib)

set(DEPS_CFLAGS "-O2 -fPIC")
set(DEPS_ENV ${CMAKE_COMMAND} -E env
    "PKG_CONFIG_PATH=${DEPS_PREFIX}/lib/pkgconfig"
    "PKG_CONFIG_LIBDIR=${DEPS_PREFIX}/lib/pkgconfig"
    "CC=${CMAKE_C_COMPILER}"
    "CFLAGS=${DEPS_CFLAGS}")

ExternalProject_Add(dep_zlib
  URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
  URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
  DOWNLOAD_DIR ${DEPS_DOWNLOAD_DIR}
  BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND ${DEPS_ENV} ./configure --static --prefix=${DEPS_PREFIX}
  BUILD_COMMAND make -j${NPROC}
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS ${DEPS_PREFIX}/lib/libz.a)

ExternalProject_Add(dep_libusb
  URL https://github.com/libusb/libusb/releases/download/v1.0.29/libusb-1.0.29.tar.bz2
  URL_HASH SHA256=5977fc950f8d1395ccea9bd48c06b3f808fd3c2c961b44b0c2e6e29fc3a70a85
  DOWNLOAD_DIR ${DEPS_DOWNLOAD_DIR}
  BUILD_IN_SOURCE 1
  # Without udev libusb enumerates through sysfs and gets hotplug events from
  # netlink, so no libudev is needed at run time.
  CONFIGURE_COMMAND ${DEPS_ENV} ./configure --prefix=${DEPS_PREFIX}
                    --enable-static --disable-shared --disable-udev
  BUILD_COMMAND make -j${NPROC}
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS ${DEPS_PREFIX}/lib/libusb-1.0.a)

ExternalProject_Add(dep_x264
  URL https://code.videolan.org/videolan/x264/-/archive/b35605ace3ddf7c1a5d67a2eb553f034aef41d55/x264-b35605ace3ddf7c1a5d67a2eb553f034aef41d55.tar.bz2
  URL_HASH SHA256=6eeb82934e69fd51e043bd8c5b0d152839638d1ce7aa4eea65a3fedcf83ff224
  DOWNLOAD_DIR ${DEPS_DOWNLOAD_DIR}
  BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND ${DEPS_ENV} ./configure --prefix=${DEPS_PREFIX}
                    --enable-static --enable-pic --disable-cli --disable-opencl
                    --disable-lavf --disable-swscale --disable-ffms --disable-gpac
                    --disable-lsmash
  BUILD_COMMAND make -j${NPROC}
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS ${DEPS_PREFIX}/lib/libx264.a)

ExternalProject_Add(dep_ffmpeg
  DEPENDS dep_zlib dep_x264
  URL https://ffmpeg.org/releases/ffmpeg-8.0.tar.xz
  URL_HASH SHA256=b2751fccb6cc4c77708113cd78b561059b6fa904b24162fa0be2d60273d27b8e
  DOWNLOAD_DIR ${DEPS_DOWNLOAD_DIR}
  BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND ${DEPS_ENV} ./configure --prefix=${DEPS_PREFIX}
                    --cc=${CMAKE_C_COMPILER}
                    --enable-static --disable-shared --enable-pic
                    --pkg-config-flags=--static
                    --extra-cflags=-I${DEPS_PREFIX}/include
                    --extra-ldflags=-L${DEPS_PREFIX}/lib
                    --enable-gpl --disable-autodetect --disable-everything
                    --disable-programs --disable-doc --disable-network
                    --disable-avdevice --disable-avfilter --disable-swresample
                    --enable-zlib --enable-libx264
                    --enable-encoder=libx264,png --enable-muxer=mp4
                    --enable-protocol=file
  BUILD_COMMAND make -j${NPROC}
  INSTALL_COMMAND make install
  BUILD_BYPRODUCTS ${DEPS_PREFIX}/lib/libavformat.a ${DEPS_PREFIX}/lib/libavcodec.a
                   ${DEPS_PREFIX}/lib/libswscale.a ${DEPS_PREFIX}/lib/libavutil.a)

ExternalProject_Add(dep_sdl3
  URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.24/SDL3-3.2.24.tar.gz
  URL_HASH SHA256=81cc0fc17e5bf2c1754eeca9af9c47a76789ac5efdd165b3b91cbbe4b90bfb76
  DOWNLOAD_DIR ${DEPS_DOWNLOAD_DIR}
  CMAKE_ARGS -DCMAKE_INSTALL_PREFIX=${DEPS_PREFIX}
             -DCMAKE_INSTALL_LIBDIR=lib
             -DCMAKE_BUILD_TYPE=Release
             -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
             -DCMAKE_POSITION_INDEPENDENT_CODE=ON
             -DSDL_STATIC=ON -DSDL_SHARED=OFF -DSDL_TEST_LIBRARY=OFF
             # Every optional dependency (X11, Wayland, libdecor, KMS/DRM,
             # udev, D-Bus, ...) is dlopen()ed at run time, so the static
             # archive needs nothing beyond -pthread -lm -ldl at link time
             # (see deps/lib/pkgconfig/sdl3.pc), whatever the build host has.
             -DSDL_DEPS_SHARED=ON
             -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
             -DSDL_AUDIO=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF
             -DSDL_SENSOR=OFF -DSDL_CAMERA=OFF -DSDL_POWER=OFF
  BUILD_BYPRODUCTS ${DEPS_PREFIX}/lib/libSDL3.a)

function(uti120_import name lib dep)
  add_library(${name} STATIC IMPORTED GLOBAL)
  set_target_properties(${name} PROPERTIES
    IMPORTED_LOCATION ${DEPS_PREFIX}/lib/${lib}
    INTERFACE_INCLUDE_DIRECTORIES ${DEPS_PREFIX}/include)
  add_dependencies(${name} ${dep})
endfunction()

uti120_import(deps::z libz.a dep_zlib)
uti120_import(deps::usb libusb-1.0.a dep_libusb)
uti120_import(deps::x264 libx264.a dep_x264)
uti120_import(deps::avformat libavformat.a dep_ffmpeg)
uti120_import(deps::avcodec libavcodec.a dep_ffmpeg)
uti120_import(deps::swscale libswscale.a dep_ffmpeg)
uti120_import(deps::avutil libavutil.a dep_ffmpeg)
uti120_import(deps::sdl3 libSDL3.a dep_sdl3)
target_include_directories(deps::usb INTERFACE ${DEPS_PREFIX}/include/libusb-1.0)

find_package(Threads REQUIRED)
set_property(TARGET deps::usb APPEND PROPERTY INTERFACE_LINK_LIBRARIES Threads::Threads)
set_property(TARGET deps::avcodec APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             deps::x264 deps::z deps::avutil)
set_property(TARGET deps::avformat APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             deps::avcodec deps::z deps::avutil)
set_property(TARGET deps::swscale APPEND PROPERTY INTERFACE_LINK_LIBRARIES deps::avutil)
set_property(TARGET deps::avutil APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             m Threads::Threads)
set_property(TARGET deps::x264 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             m Threads::Threads ${CMAKE_DL_LIBS})
set_property(TARGET deps::sdl3 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             m Threads::Threads ${CMAKE_DL_LIBS})
