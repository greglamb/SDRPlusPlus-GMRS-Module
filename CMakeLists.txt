cmake_minimum_required(VERSION 3.13)
project(frsgmrs_monitor)

# Builds in-tree (copied/symlinked into SDR++'s decoder_modules/ with an
# add_subdirectory in the root CMakeLists) or out-of-tree with
#   cmake -B build -DSDRPP_MODULE_CMAKE=/path/to/sdrpp/build/sdrpp_module.cmake
file(GLOB_RECURSE SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

target_include_directories(frsgmrs_monitor PRIVATE "src/")
set_target_properties(frsgmrs_monitor PROPERTIES CXX_STANDARD 17)

# FFTW3 (single precision) for the detectors
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3F REQUIRED fftw3f)
target_include_directories(frsgmrs_monitor PRIVATE ${FFTW3F_INCLUDE_DIRS})
target_link_directories(frsgmrs_monitor PRIVATE ${FFTW3F_LIBRARY_DIRS})
target_link_libraries(frsgmrs_monitor PRIVATE ${FFTW3F_LIBRARIES})
