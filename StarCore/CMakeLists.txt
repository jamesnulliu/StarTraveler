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

# C++ modules and `import std;` for every target (checked once per build, see the file).
include(${PROJECT_SOURCE_DIR}/cmake/starcore-modules.cmake)

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

# Every component is a named module: modules/starcore.<component>.cppm (the primary interface)
# exports the partitions in modules/<component>/*.cppm; src/<component>/ holds the implementation
# units (`module starcore.<component>;`) and internal partitions.
include(${PROJECT_SOURCE_DIR}/cmake/utils/modules.cmake)

configure_file(
    ${PROJECT_SOURCE_DIR}/modules/common/version.cppm.in
    ${PROJECT_BINARY_DIR}/modules/common/version.cppm
    @ONLY
)

# StarCore::Core -----------------------------------------------------------------------------------
set(CORE_COMPONENTS common math raster asset scene render physics)

add_library(StarCoreCore STATIC)
add_library(${PROJECT_NAMESPACE}Core ALIAS StarCoreCore)
starcore_add_components(StarCoreCore ${CORE_COMPONENTS})
target_sources(StarCoreCore PUBLIC
    FILE_SET CXX_MODULES BASE_DIRS ${PROJECT_BINARY_DIR}/modules
    FILES ${PROJECT_BINARY_DIR}/modules/common/version.cppm
)
# glm's own module (`import glm;`), shipped with glm >= 1.0 but compiled by its users.
target_sources(StarCoreCore PUBLIC
    FILE_SET glm TYPE CXX_MODULES BASE_DIRS ${STARCORE_GLM_MODULE_DIR}
    FILES ${STARCORE_GLM_MODULE_DIR}/glm.cppm
)
set_target_properties(StarCoreCore PROPERTIES
    OUTPUT_NAME starcore_core
    # Linked into the Python extension module (a shared library).
    POSITION_INDEPENDENT_CODE ON
)
# Headers: what modules cannot export, the logging and assertion macros.
target_include_directories(StarCoreCore PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_compile_features(StarCoreCore PUBLIC cxx_std_${STARCORE_CXX_STANDARD})
target_compile_definitions(StarCoreCore
    PUBLIC
        # Vulkan clip-space depth range [0, 1]; same convention for CPU and GPU renderers.
        GLM_FORCE_DEPTH_ZERO_TO_ONE
        GLM_ENABLE_EXPERIMENTAL
    PRIVATE
        # glm.cppm exports the stable extensions' functions as glm::f, not glm::gtc::f. The
        # experimental ones stay in glm::gtx: among them are unconstrained begin()/end() (gtx/range)
        # that argument-dependent lookup would find for any type with glm template arguments.
        GLM_GTC_INLINE_NAMESPACE
        GLM_EXT_INLINE_NAMESPACE
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

# StarCore::Gpu ------------------------------------------------------------------------------------
if(STARCORE_BUILD_GPU)
    include(${PROJECT_SOURCE_DIR}/cmake/utils/shaders.cmake)

    add_library(StarCoreGpu STATIC)
    add_library(${PROJECT_NAMESPACE}Gpu ALIAS StarCoreGpu)
    starcore_add_components(StarCoreGpu vulkan gpu)
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
    add_library(StarCoreRuntime STATIC)
    add_library(${PROJECT_NAMESPACE}Runtime ALIAS StarCoreRuntime)
    starcore_add_components(StarCoreRuntime platform ui app)
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
