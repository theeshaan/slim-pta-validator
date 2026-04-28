cmake_minimum_required(VERSION 3.20)
project(PtaValidator CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Find LLVM (17+)
# ---------------------------------------------------------------------------
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "LLVM include dirs: ${LLVM_INCLUDE_DIRS}")
message(STATUS "LLVM library dirs: ${LLVM_LIBRARY_DIRS}")

if(LLVM_PACKAGE_VERSION VERSION_LESS "17.0")
  message(FATAL_ERROR "LLVM 17 or newer is required.")
endif()

include_directories(${LLVM_INCLUDE_DIRS})
link_directories(${LLVM_LIBRARY_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ---------------------------------------------------------------------------
# 1. Instrumentation tool: pta-validator
#    Reads input.ll + pta.txt, emits instrumented.ll
# ---------------------------------------------------------------------------
add_executable(pta-validator PtaValidator.cpp)

# Map LLVM component names to actual libraries
llvm_map_components_to_libnames(LLVM_LIBS
  core        # IR types, IRBuilder, Module, ...
  irreader    # parseIRFile
  support     # raw_ostream, CommandLine, InitLLVM, ...
  passes      # PassBuilder, new pass manager infra
  transformutils  # appendToGlobalCtors, etc.
  analysis    # AnalysisManager base
)

target_link_libraries(pta-validator PRIVATE ${LLVM_LIBS})

# LLVM requires RTTI to be off if it was built without RTTI
if(NOT LLVM_ENABLE_RTTI)
  target_compile_options(pta-validator PRIVATE -fno-rtti)
endif()

# ---------------------------------------------------------------------------
# 2. Runtime library: libpta_runtime.a
#    Linked with the instrumented program at compile time
# ---------------------------------------------------------------------------
add_library(pta_runtime STATIC runtime.c)
target_compile_options(pta_runtime PRIVATE -O2)

# ---------------------------------------------------------------------------
# Usage instructions (printed during build)
# ---------------------------------------------------------------------------
add_custom_target(usage
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "=== PTA Validator Usage ==="
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Step 1: Instrument the IR"
  COMMAND ${CMAKE_COMMAND} -E echo "  ./pta-validator input.ll points-to.txt -o instrumented.ll"
  COMMAND ${CMAKE_COMMAND} -E echo "  ./pta-validator --pta-mode=fs input.ll points-to.txt -o instrumented.ll"
  COMMAND ${CMAKE_COMMAND} -E echo "  ./pta-validator --pta-mode=fs --program-point-format=source-line input.ll points-to.txt -o instrumented.ll"
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Step 2: Compile + link instrumented IR with the runtime"
  COMMAND ${CMAKE_COMMAND} -E echo "  clang-18 instrumented.ll libpta_runtime.a -o program_instrumented"
  COMMAND ${CMAKE_COMMAND} -E echo ""
  COMMAND ${CMAKE_COMMAND} -E echo "Step 3: Run the instrumented program"
  COMMAND ${CMAKE_COMMAND} -E echo "  ./program_instrumented"
  COMMAND ${CMAKE_COMMAND} -E echo ""
)
