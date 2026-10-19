cmake_minimum_required(VERSION 3.16)
project(SignAnalysisPass)

# ── LLVM discovery ────────────────────────────────────────────────────────────
# Looks for llvm-config-XX in PATH (e.g. llvm-config-18, llvm-config-17 …)
# or the generic llvm-config.  Override via -DLLVM_DIR=... if needed.
find_package(LLVM REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake from ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

# Use the same C++ standard LLVM was built with
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

# ── Plugin target ─────────────────────────────────────────────────────────────
add_library(SignAnalysisPass MODULE passes/sign_analysis.cpp)

# Required flags for an LLVM out-of-tree plugin
target_compile_options(SignAnalysisPass PRIVATE
    -fno-rtti          # LLVM is built without RTTI by default
    -fPIC
)

# Don't link against LLVM libraries (loaded at runtime by opt)
set_target_properties(SignAnalysisPass PROPERTIES
    PREFIX ""                    # no "lib" prefix wanted by opt
)

# ── Other passes ─────────────────────────────────────────────────────────────
foreach(P InsCounter BFFAPass)
  add_library(${P} MODULE passes/${P}.cpp)
  target_compile_options(${P} PRIVATE -fno-rtti -fPIC)
  set_target_properties(${P} PROPERTIES PREFIX "")
endforeach()
