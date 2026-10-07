# ==================================================================================================
# StarCore -- the render engine.
#
#   StarCore::Core     Headless: math, scene/ECS, CPU path tracer, 2D raster, serialization.
#                      No window / GPU dependencies, so Python bindings and tests can use it.
#   StarCore::Runtime  Window, input, Vulkan context, ImGui, texture presentation, app/layers.
# ==================================================================================================

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/starcore/common/version.hpp.in
    ${CMAKE_CURRENT_BINARY_DIR}/include/starcore/common/version.hpp
    @ONLY
)

# StarCore::Core -----------------------------------------------------------------------------------
set(CORE_MODULES common math scene render raster)
set(CORE_SOURCES)
foreach(MODULE IN LISTS CORE_MODULES)
    file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/${MODULE}/*.cpp)
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
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
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
)

# StarCore::Runtime --------------------------------------------------------------------------------
if(STARCORE_BUILD_RUNTIME)
    set(RUNTIME_MODULES app platform vulkan ui)
    set(RUNTIME_SOURCES)
    foreach(MODULE IN LISTS RUNTIME_MODULES)
        file(GLOB_RECURSE MODULE_SOURCES CONFIGURE_DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/src/${MODULE}/*.cpp)
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

# Tests / examples ---------------------------------------------------------------------------------
if(STARCORE_BUILD_TESTS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()

if(STARCORE_BUILD_EXAMPLES)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/examples)
endif()
