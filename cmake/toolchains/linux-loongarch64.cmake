# Cross toolchain used by the GitHub Actions Linux LoongArch64 job.
#
# Ubuntu provides the compilers and target sysroot in the
# gcc-14-loongarch64-linux-gnu / g++-14-loongarch64-linux-gnu packages.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR loongarch64)

set(CMAKE_C_COMPILER loongarch64-linux-gnu-gcc-14)
set(CMAKE_CXX_COMPILER loongarch64-linux-gnu-g++-14)

set(CMAKE_FIND_ROOT_PATH /usr/loongarch64-linux-gnu)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
