cmake_minimum_required(VERSION 3.7)
project(CH9326LinuxTest C)

set(CMAKE_C_STANDARD 99)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)
find_package(Threads REQUIRED)

# Built from source so it works on any architecture (x86, ARM/Raspberry Pi, ...).
# lib/libch9326.so is the original x86-64 vendor binary, kept for reference only.
add_library(ch9326 SHARED lib/ch9326.c)
target_include_directories(ch9326 PUBLIC lib ${LIBUSB_INCLUDE_DIRS})
target_link_libraries(ch9326 PRIVATE ${LIBUSB_LDFLAGS} Threads::Threads)

add_executable(CH9326LinuxTest main.c)
target_link_libraries(CH9326LinuxTest ch9326)
