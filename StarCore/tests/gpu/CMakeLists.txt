set(TARGET_NAME "StarCoreGpuTests")

# Needs a Vulkan device with ray queries; tests SKIP without one.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(${TARGET_NAME} ${TEST_SOURCES})

target_link_libraries(
    ${TARGET_NAME} PRIVATE
    ${PROJECT_NAMESPACE}Gpu
    ${PROJECT_NAMESPACE}CompileOptions
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(${TARGET_NAME} DISCOVERY_MODE PRE_TEST
    # Skipped tests (no suitable GPU) are not failures.
    PROPERTIES SKIP_RETURN_CODE 4
)
