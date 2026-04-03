cmake_minimum_required(VERSION 3.5)
set (CMAKE_CXX_STANDARD 14)
project(llir-to-slim)

set(ENV{LLVM_DIR} /usr/lib/llvm-14/lib/cmake/llvm)

find_package(LLVM REQUIRED CONFIG)
add_definitions(${LLVM_DEFINITIONS})
include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})

include_directories(/usr/local/include/slim)

add_library(slim SHARED IMPORTED)

set_target_properties(slim PROPERTIES IMPORTED_LOCATION "/usr/local/lib/libslim.so")

add_executable(${PROJECT_NAME} main.cpp)

target_link_libraries(${PROJECT_NAME} slim LLVM)