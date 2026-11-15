# usbcam-recv-c: portable IUCM receiver (Windows, macOS, Linux). MIT.
# Compiles the shared/ core sources directly, like obs-plugin/ does, so the
# shared/ test targets do not leak into this project.
#
#   cmake -S tools/recv-c -B tools/recv-c/build && cmake --build tools/recv-c/build
#   (Windows, Visual Studio: add  --config Release  to the build step)
cmake_minimum_required(VERSION 3.16)
project(usbcam_recv_c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(IUCM_SHARED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")

add_executable(usbcam-recv-c
    recv.c
    "${IUCM_SHARED_DIR}/frame_parser.c"
    "${IUCM_SHARED_DIR}/usbmux.c"
)
target_include_directories(usbcam-recv-c PRIVATE "${IUCM_SHARED_DIR}")

if(WIN32)
    target_link_libraries(usbcam-recv-c PRIVATE ws2_32)
endif()

if(MSVC)
    target_compile_options(usbcam-recv-c PRIVATE /W4 /WX)
else()
    target_compile_options(usbcam-recv-c PRIVATE -Wall -Wextra -Werror)
endif()
