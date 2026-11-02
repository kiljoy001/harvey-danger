# CMake toolchain for building software that runs on Harvey (APEX userland).
#
#   cmake -DCMAKE_TOOLCHAIN_FILE=util/clang-harvey/harvey-toolchain.cmake ...
#
# Set HARVEY to the checkout root if it is not the directory containing this
# file's parent.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT DEFINED HARVEY)
  set(HARVEY "$ENV{HARVEY}")
endif()
if(NOT HARVEY)
  get_filename_component(HARVEY "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
message(STATUS "Harvey toolchain: HARVEY=${HARVEY}")

set(CMAKE_C_COMPILER   "${HARVEY}/util/clang-harvey/harvey-clang")
set(CMAKE_CXX_COMPILER "${HARVEY}/util/clang-harvey/harvey-clang++")

set(CMAKE_AR  "${HARVEY}/util/clang-harvey/harvey-ar")
set(CMAKE_RANLIB "${HARVEY}/util/clang-harvey/harvey-ranlib")

# Link tests are real: the wrapper knows the Harvey link recipe, and the
# result is what the build will actually produce.
set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)

set(CMAKE_FIND_ROOT_PATH "${HARVEY}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Do not let CMake add host libraries to link lines.
set(CMAKE_C_STANDARD_LIBRARIES "")
set(CMAKE_CXX_STANDARD_LIBRARIES "")
