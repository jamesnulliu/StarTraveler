# ==================================================================================================
# StarTraveler workspace: the StarCore engine (StarCore/, also buildable on its own) and the
# StarTraveler game built on it (StarTraveler/).
# ==================================================================================================

# Minimum required CMake version
cmake_minimum_required(VERSION 4.0)

# Engine options, C++ standard and vcpkg (manifest: StarCore/vcpkg.json) -- before `project()`.
include(${CMAKE_CURRENT_LIST_DIR}/StarCore/cmake/starcore-setup.cmake)

include(CMakeDependentOption)
cmake_dependent_option(
    STARTRAVELER_BUILD_GAME "Build the StarTraveler game" ON
    "STARCORE_BUILD_RUNTIME" OFF
)

# Project name and version
project(
    StarTraveler
    VERSION 0.1.0
    DESCRIPTION "The StarCore render engine and the StarTraveler game"
    LANGUAGES C CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# C++ modules and `import std;` for the engine and the game alike (checked here, before any target
# exists).
include(${PROJECT_SOURCE_DIR}/StarCore/cmake/starcore-modules.cmake)

# All executables (game, tests, tools) end up in one directory per configuration.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)

if(STARCORE_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(${PROJECT_SOURCE_DIR}/StarCore)

if(STARTRAVELER_BUILD_GAME)
    add_subdirectory(${PROJECT_SOURCE_DIR}/StarTraveler)
endif()
