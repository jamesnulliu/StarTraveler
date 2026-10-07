# ==================================================================================================
# @file dependencies.cmake
# @brief Find third-party packages (provided by vcpkg, see `vcpkg.json`).
#
# @note Values MUST be set BEFORE including this file:
#         - `STARCORE_BUILD_RUNTIME`, `STARCORE_BUILD_TESTS`: Which optional parts are built.
#           They select the vcpkg manifest features, see `../starcore-setup.cmake`.
# ==================================================================================================

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../utils/common.cmake)

# Core (headless) ----------------------------------------------------------------------------------
find_package(glm      CONFIG REQUIRED)  # glm::glm
find_package(EnTT     CONFIG REQUIRED)  # EnTT::EnTT
find_package(spdlog   CONFIG REQUIRED)  # spdlog::spdlog (std::format backend)
find_package(yaml-cpp CONFIG REQUIRED)  # yaml-cpp::yaml-cpp
find_package(CLI11    CONFIG REQUIRED)  # CLI11::CLI11
find_package(fastgltf CONFIG REQUIRED)  # fastgltf::fastgltf (glTF 2.0 models)
find_package(Stb             REQUIRED)  # Stb_INCLUDE_DIR

add_library(starcore_stb INTERFACE)
add_library(${PROJECT_NAMESPACE}Stb ALIAS starcore_stb)
target_include_directories(starcore_stb SYSTEM INTERFACE ${Stb_INCLUDE_DIR})

# Runtime (window / Vulkan / ImGui) ----------------------------------------------------------------
if(STARCORE_BUILD_RUNTIME)
    find_package(Vulkan                         REQUIRED)  # Vulkan::Vulkan, Vulkan::Headers
    find_package(glfw3                   CONFIG REQUIRED)  # glfw
    find_package(imgui                   CONFIG REQUIRED)  # imgui::imgui
    find_package(vk-bootstrap            CONFIG REQUIRED)  # vk-bootstrap::vk-bootstrap
    find_package(VulkanMemoryAllocator   CONFIG REQUIRED)  # GPUOpen::VulkanMemoryAllocator
    log_info("Vulkan headers: ${Vulkan_VERSION}")
endif()

# Tests --------------------------------------------------------------------------------------------
if(STARCORE_BUILD_TESTS)
    find_package(Catch2 3 CONFIG REQUIRED)  # Catch2::Catch2WithMain
endif()
