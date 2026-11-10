# CMake platform description for Harvey/APEX.
#
# Harvey's uname reports "Harvey", so CMake looks for Platform/Harvey when a
# native build runs without a toolchain file.  There is no dynamic linker in
# the default static model, and the APEX archives are .a files.
set(UNIX 1)
include(Platform/UnixPaths)

set(CMAKE_DL_LIBS "")
set(CMAKE_EXECUTABLE_SUFFIX "")
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_SHARED_MODULE_SUFFIX ".so")
