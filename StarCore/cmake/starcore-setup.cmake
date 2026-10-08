# ==================================================================================================
# @file starcore-setup.cmake
# @brief Settings that must be known BEFORE the first `project()` call: build options (they select
#        vcpkg manifest features), the C++ standard and the vcpkg toolchain.
#
# Included by StarCore/CMakeLists.txt when StarCore is the top-level project (e.g. when
# scikit-build-core builds the Python package), and by a workspace that adds StarCore as a
# subdirectory (the StarTraveler repository root) before its own `project()`.
#
# @note Values MAY be set BEFORE including this file (or on the command line):
#         - `STARCORE_CXX_STANDARD`: 23 (default) or 26 (experimental: MSVC maps it to
#           /std:c++latest; C++26 library features must be guarded by feature-test macros).
#         - `VCPKG_TARGET_TRIPLET`: Default on Windows: x64-windows-static-md.
# ==================================================================================================

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/utils/common.cmake)

# Options -------------------------------------------------------------------------------------------
option(STARCORE_BUILD_GPU      "Build StarCore::Gpu (headless Vulkan, GPU path tracer)" ON)
option(STARCORE_BUILD_RUNTIME  "Build StarCore::Runtime (window, input, ImGui)"         ON)
option(STARCORE_BUILD_TESTS    "Build the C++ unit tests"                               ON)
option(STARCORE_BUILD_EXAMPLES "Build the StarCore examples (e.g. starcore-render)"     ON)
option(STARCORE_BUILD_PYTHON   "Build the Python extension module (starcore._core)"    OFF)
# Development aid: build VK_LAYER_KHRONOS_validation with vcpkg; tests and `--validation` use it.
option(STARCORE_VULKAN_VALIDATION "Install the Vulkan validation layers (vcpkg)"        OFF)

if(STARCORE_BUILD_RUNTIME AND NOT STARCORE_BUILD_GPU)
    log_fatal("STARCORE_BUILD_RUNTIME requires STARCORE_BUILD_GPU (the runtime presents with Vulkan)")
endif()

# C++ standard --------------------------------------------------------------------------------------
set(STARCORE_CXX_STANDARD 23 CACHE STRING "C++ standard: 23, or 26 (experimental)")
set_property(CACHE STARCORE_CXX_STANDARD PROPERTY STRINGS 23 26)
if(NOT STARCORE_CXX_STANDARD MATCHES "^(23|26)$")
    log_fatal("STARCORE_CXX_STANDARD must be 23 or 26, got: \"${STARCORE_CXX_STANDARD}\"")
endif()
set(CMAKE_CXX_STANDARD          ${STARCORE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)
# Header units / C++20 modules are not used (yet); skip dependency scanning.
set(CMAKE_CXX_SCAN_FOR_MODULES  OFF)

# vcpkg ---------------------------------------------------------------------------------------------
# Use $ENV{VCPKG_ROOT} when no toolchain was given (plain `cmake -B build`, scikit-build-core).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()
# Static libraries + dynamic CRT on Windows: no third-party DLLs next to executables, and the
# same CRT as CPython for the extension module.
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg target triplet")
endif()
# The manifest belongs to the engine, also when a workspace builds it as a subdirectory.
set(VCPKG_MANIFEST_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
# Only install the dependencies of the parts we build.
set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON)
if(STARCORE_BUILD_GPU)
    list(APPEND VCPKG_MANIFEST_FEATURES "gpu")
endif()
if(STARCORE_BUILD_GPU AND STARCORE_VULKAN_VALIDATION)
    list(APPEND VCPKG_MANIFEST_FEATURES "validation")
endif()
if(STARCORE_BUILD_RUNTIME)
    list(APPEND VCPKG_MANIFEST_FEATURES "runtime")
endif()
if(STARCORE_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()
