# Minimum required CMake version
cmake_minimum_required(VERSION 3.30)

# Options (must be known before `project()`, they select vcpkg manifest features) -----------------
option(STARCORE_BUILD_RUNTIME "Build StarCore::Runtime (window, input, Vulkan, ImGui)" ON)
option(STARCORE_BUILD_TESTS   "Build C++ unit tests"                                 ON)
option(STARCORE_BUILD_PYTHON  "Build the Python extension module (starcore._core)"  OFF)
include(CMakeDependentOption)
cmake_dependent_option(
    STARCORE_BUILD_GAME "Build the StarTraveler executable" ON
    "STARCORE_BUILD_RUNTIME" OFF
)

# vcpkg ---------------------------------------------------------------------------------------------
# Use $ENV{VCPKG_ROOT} when no toolchain was given (e.g. plain `cmake -B build` or scikit-build-core).
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain file")
endif()
# Static libraries + dynamic CRT on Windows: no third-party DLLs to deploy next to executables.
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET "x64-windows-static-md" CACHE STRING "vcpkg target triplet")
endif()
# Only install the dependencies of the parts we build.
set(VCPKG_MANIFEST_NO_DEFAULT_FEATURES ON)
if(STARCORE_BUILD_RUNTIME)
    list(APPEND VCPKG_MANIFEST_FEATURES "runtime")
endif()
if(STARCORE_BUILD_TESTS)
    list(APPEND VCPKG_MANIFEST_FEATURES "tests")
endif()

# Project name and version
project(
    StarTraveler
    VERSION 0.1.0
    DESCRIPTION "StarCore render engine and the StarTraveler game"
    LANGUAGES C CXX
)

# Common utility functions:
include(${PROJECT_SOURCE_DIR}/cmake/utils/common.cmake)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# @see "./cmake/utils/common.cmake"
set_default_values(
    # CMake project namespace:
    # All the libs should have an alias with this namespace
    PROJECT_NAMESPACE "StarCore::"
    STDOUT_IS_TERMINAL OFF
)

# All executables (game, tests) end up in one directory per configuration.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

# Compiler configurations for cpp, e.g., g++, clang++, msvc(cl), etc.:
include(${PROJECT_SOURCE_DIR}/cmake/compilers/cxx-compiler-configs.cmake)

# Libraries managed by vcpkg:
include(${PROJECT_SOURCE_DIR}/cmake/libraries/dependencies.cmake)

if(STARCORE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(${PROJECT_SOURCE_DIR}/StarCore)

if(STARCORE_BUILD_GAME)
    add_subdirectory(${PROJECT_SOURCE_DIR}/StarTraveler)
endif()

if(STARCORE_BUILD_PYTHON)
    add_subdirectory(${PROJECT_SOURCE_DIR}/bindings/python)
endif()
