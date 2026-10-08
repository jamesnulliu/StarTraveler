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

# C++ modules ---------------------------------------------------------------------------------------
# StarCore::Core provides named modules (modules/*.cppm, e.g. `import starcore.raster;`); their
# importers need dependency scanning too.
set(CMAKE_CXX_SCAN_FOR_MODULES  ON)
# `import std;` inside those modules (experimental): MSBuild builds the standard library module
# itself; with the other generators CMake does, behind a gate whose key changes with CMake's
# version -- known keys below, other versions fall back to including the standard headers.
option(STARCORE_IMPORT_STD "StarCore's modules `import std;` where the toolchain can" ON)
set(_STARCORE_IMPORT_STD_GATES
    "4.2" "d0edc3af-4c50-42ea-a356-e2862fe7a444"
    "4.4" "f35a9ac6-8463-4d38-8eec-5d6008153e7d"
)
if(STARCORE_IMPORT_STD AND NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
    set(_version "${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}")
    list(FIND _STARCORE_IMPORT_STD_GATES "${_version}" _index)
    if(_index GREATER_EQUAL 0)
        math(EXPR _index "${_index} + 1")
        list(GET _STARCORE_IMPORT_STD_GATES ${_index} CMAKE_EXPERIMENTAL_CXX_IMPORT_STD)
    endif()
    unset(_index)
    unset(_version)
endif()

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
