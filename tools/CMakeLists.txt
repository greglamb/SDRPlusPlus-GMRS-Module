# Standalone build of the detector library + parity test (no SDR++ needed).
#   cmake -S tools -B build-tools && cmake --build build-tools
cmake_minimum_required(VERSION 3.13)
project(frsmon_tools CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif ()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED fftw3f)

add_library(frsmon_detect STATIC ../src/detect/detector.cpp)
target_include_directories(frsmon_detect PUBLIC ../src/detect ${FFTW3F_INCLUDE_DIRS})
target_link_libraries(frsmon_detect PUBLIC ${FFTW3F_LIBRARIES})
target_link_directories(frsmon_detect PUBLIC ${FFTW3F_LIBRARY_DIRS})
target_compile_options(frsmon_detect PRIVATE -O2 -Wall -Wextra)

add_executable(parity_test parity_test.cpp)
target_link_libraries(parity_test PRIVATE frsmon_detect)
