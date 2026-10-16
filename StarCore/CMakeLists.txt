# ==================================================================================================
# StarCore -- the render engine. Builds standalone (this is also what `pip install StarCore/`
# drives through scikit-build-core) or as a subdirectory of a workspace (the repository root).
#
#   StarCore::Core     Headless: math, assets (meshes, models), scene/ECS, CPU path tracer,
#                      2D raster, collision detection, serialization.
#                      No window / GPU dependencies, so Python bindings and tests can use it.
#   StarCore::Gpu      Headless Vulkan (context, buffers, pipelines) and the GPU renderer
#                      (path tracing with ray queries, hybrid and raster pipelines).
#   StarCore::Runtime  Window, input, ImGui, texture presentation, app/layers.
#   starcore._core     Python extension module (python/), see STARCORE_BUILD_PYTHON.
# ==================================================================================================

# Minimum required CMake version
cmake_minimum_required(VERSION 4.0)

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
# C++20 named modules (their implementation units are among the sources above).
target_sources(StarCoreCore PUBLIC
    FILE_SET CXX_MODULES BASE_DIRS ${PROJECT_SOURCE_DIR}/modules
    FILES ${PROJECT_SOURCE_DIR}/modules/starcore.raster.cppm
)
# `import std;` in them: built by MSBuild (Visual Studio generators) or by CMake (gate set in
# starcore-setup.cmake, compiler support in CMAKE_CXX_COMPILER_IMPORT_STD).
if(STARCORE_IMPORT_STD AND (CMAKE_GENERATOR MATCHES "^Visual Studio" AND MSVC))
    set(STARCORE_IMPORT_STD_USED ON)
elseif(STARCORE_IMPORT_STD AND CMAKE_CXX_STANDARD IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    set(STARCORE_IMPORT_STD_USED ON)
    set_target_properties(StarCoreCore PROPERTIES CXX_MODULE_STD ON)
else()
    set(STARCORE_IMPORT_STD_USED OFF)
endif()
if(STARCORE_IMPORT_STD_USED)
    target_compile_definitions(StarCoreCore PRIVATE STARCORE_IMPORT_STD)
endif()
log_info("import std in StarCore's modules: ${STARCORE_IMPORT_STD_USED}")

# StarCore::Gpu ------------------------------------------------------------------------------------
if(STARCORE_BUILD_GPU)
    include(${PROJECT_SOURCE_DIR}/cmake/utils/shaders.cmake)

    set(GPU_MODULES vulkan gpu)
    set(GPU_SOURCES)
    foreach(MODULE IN LISTS GPU_MODULES)
        file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS
            ${PROJECT_SOURCE_DIR}/src/${MODULE}/*.cpp)
        list(APPEND GPU_SOURCES ${MODULE_SOURCES})
    endforeach()

    add_library(StarCoreGpu STATIC ${GPU_SOURCES})
    add_library(${PROJECT_NAMESPACE}Gpu ALIAS StarCoreGpu)
    set_target_properties(StarCoreGpu PROPERTIES OUTPUT_NAME starcore_gpu)
    target_link_libraries(StarCoreGpu
        PUBLIC
            ${PROJECT_NAMESPACE}Core
            Vulkan::Vulkan
        PRIVATE
            ${PROJECT_NAMESPACE}CompileOptions
            vk-bootstrap::vk-bootstrap
            GPUOpen::VulkanMemoryAllocator
    )
    # The vendors' upscalers, where available (cmake/libraries/upscalers.cmake).
    foreach(UPSCALER IN ITEMS Fsr Dlss)
        if(TARGET ${PROJECT_NAMESPACE}${UPSCALER})
            target_link_libraries(StarCoreGpu PRIVATE ${PROJECT_NAMESPACE}${UPSCALER})
        endif()
    endforeach()

    file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/shaders/*.glsl)
    file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
        ${PROJECT_SOURCE_DIR}/shaders/*.comp
        ${PROJECT_SOURCE_DIR}/shaders/*.vert
        ${PROJECT_SOURCE_DIR}/shaders/*.frag
    )
    starcore_add_shaders(StarCoreGpu
        SOURCES ${SHADER_SOURCES}
        INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/shaders
        DEPENDS ${SHADER_INCLUDES}
    )

    if(STARCORE_VULKAN_VALIDATION)
        # Context::create points the Vulkan loader here when validation is requested.
        target_compile_definitions(StarCoreGpu PRIVATE
            STARCORE_VULKAN_LAYER_DIR="${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
        )
    endif()
endif()

# StarCore::Runtime --------------------------------------------------------------------------------
if(STARCORE_BUILD_RUNTIME)
    set(RUNTIME_MODULES app platform ui)
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
            ${PROJECT_NAMESPACE}Gpu
            imgui::imgui
        PRIVATE
            ${PROJECT_NAMESPACE}CompileOptions
            glfw
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
