# ==================================================================================================
# StarCore -- the render engine. Builds standalone (this is also what `pip install StarCore/`
# drives through scikit-build-core) or as a subdirectory of a workspace (the repository root).
#
#   StarCore::Core     Headless: math, assets (meshes, models), scene/ECS, CPU path tracer,
#                      2D raster, collision detection, serialization.
#                      No window / GPU dependencies, so Python bindings and tests can use it.
#   StarCore::Runtime  Window, input, Vulkan context, ImGui, texture presentation, app/layers.
#   starcore._core     Python extension module (python/), see STARCORE_BUILD_PYTHON.
# ==================================================================================================

# Minimum required CMake version
cmake_minimum_required(VERSION 3.30)

# Options, C++ standard and vcpkg; a no-op when a workspace already included it.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/starcore-setup.cmake)

# Project name and version (also the Python package version, see pyproject.toml)
project(
    StarCore
    VERSION 0.1.0
    DESCRIPTION "StarCore render engine"
    LANGUAGES C CXX
)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# All libraries have an alias with this namespace.
set(PROJECT_NAMESPACE "StarCore::")

if(PROJECT_IS_TOP_LEVEL)
    # All executables (tests, examples) end up in one directory per configuration.
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}/bin)
endif()

# Compiler configurations for cpp, e.g., g++, clang++, msvc(cl), etc.:
include(${PROJECT_SOURCE_DIR}/cmake/compilers/cxx-compiler-configs.cmake)

# Libraries managed by vcpkg:
include(${PROJECT_SOURCE_DIR}/cmake/libraries/dependencies.cmake)

if(STARCORE_BUILD_TESTS)
    enable_testing()
endif()

configure_file(
    ${PROJECT_SOURCE_DIR}/include/starcore/common/version.hpp.in
    ${PROJECT_BINARY_DIR}/include/starcore/common/version.hpp
    @ONLY
)

# StarCore::Core -----------------------------------------------------------------------------------
set(CORE_MODULES common math asset scene render raster physics)
set(CORE_SOURCES)
foreach(MODULE IN LISTS CORE_MODULES)
    file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/src/${MODULE}/*.cpp)
    list(APPEND CORE_SOURCES ${MODULE_SOURCES})
endforeach()

add_library(StarCoreCore STATIC ${CORE_SOURCES})
add_library(${PROJECT_NAMESPACE}Core ALIAS StarCoreCore)
set_target_properties(StarCoreCore PROPERTIES
    OUTPUT_NAME starcore_core
    # Linked into the Python extension module (a shared library).
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(StarCoreCore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(StarCoreCore PUBLIC cxx_std_${STARCORE_CXX_STANDARD})
target_compile_definitions(StarCoreCore PUBLIC
    # Vulkan clip-space depth range [0, 1]; same convention for CPU and GPU renderers.
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    GLM_ENABLE_EXPERIMENTAL
)
target_link_libraries(StarCoreCore
    PUBLIC
        glm::glm
        EnTT::EnTT
        spdlog::spdlog
    PRIVATE
        ${PROJECT_NAMESPACE}CompileOptions
        ${PROJECT_NAMESPACE}Stb
        yaml-cpp::yaml-cpp
        fastgltf::fastgltf
)

# StarCore::Runtime --------------------------------------------------------------------------------
if(STARCORE_BUILD_RUNTIME)
    set(RUNTIME_MODULES app platform vulkan ui)
    set(RUNTIME_SOURCES)
    foreach(MODULE IN LISTS RUNTIME_MODULES)
        file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS
            ${PROJECT_SOURCE_DIR}/src/${MODULE}/*.cpp)
        list(APPEND RUNTIME_SOURCES ${MODULE_SOURCES})
    endforeach()

    add_library(StarCoreRuntime STATIC ${RUNTIME_SOURCES})
    add_library(${PROJECT_NAMESPACE}Runtime ALIAS StarCoreRuntime)
    set_target_properties(StarCoreRuntime PROPERTIES OUTPUT_NAME starcore_runtime)
    target_link_libraries(StarCoreRuntime
        PUBLIC
            ${PROJECT_NAMESPACE}Core
            Vulkan::Vulkan
            imgui::imgui
        PRIVATE
            ${PROJECT_NAMESPACE}CompileOptions
            glfw
            vk-bootstrap::vk-bootstrap
            GPUOpen::VulkanMemoryAllocator
    )
endif()

# Tests / examples / Python ------------------------------------------------------------------------
if(STARCORE_BUILD_TESTS)
    add_subdirectory(${PROJECT_SOURCE_DIR}/tests)
endif()

if(STARCORE_BUILD_EXAMPLES)
    add_subdirectory(${PROJECT_SOURCE_DIR}/examples)
endif()

if(STARCORE_BUILD_PYTHON)
    add_subdirectory(${PROJECT_SOURCE_DIR}/python)
endif()
