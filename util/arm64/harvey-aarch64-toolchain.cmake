# CMake toolchain for building software that runs on Harvey/ARM64 (APEX).
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=util/arm64/harvey-aarch64-toolchain.cmake ...
#
# Set HARVEY to the checkout root if it is not the directory containing this
# file's parent.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(NOT DEFINED HARVEY)
  set(HARVEY "$ENV{HARVEY}")
endif()
if(NOT HARVEY)
  get_filename_component(HARVEY "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
message(STATUS "Harvey ARM64 toolchain: HARVEY=${HARVEY}")

set(CMAKE_C_COMPILER   "${HARVEY}/util/arm64/aarch64-clang")
set(CMAKE_CXX_COMPILER "${HARVEY}/util/arm64/aarch64-clang++")

find_program(ARM64_AR llvm-ar REQUIRED)
find_program(ARM64_RANLIB llvm-ranlib REQUIRED)
set(CMAKE_AR "${ARM64_AR}")
set(CMAKE_RANLIB "${ARM64_RANLIB}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)

set(CMAKE_FIND_ROOT_PATH "${HARVEY}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
