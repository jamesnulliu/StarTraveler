set(TARGET_NAME "StarCoreTests")

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(${TARGET_NAME} ${TEST_SOURCES})

target_link_libraries(
    ${TARGET_NAME} PRIVATE
    ${PROJECT_NAMESPACE}Core
    ${PROJECT_NAMESPACE}CompileOptions
    Catch2::Catch2WithMain
)

include(Catch)
# PRE_TEST: discover test cases when ctest runs instead of after every build.
catch_discover_tests(${TARGET_NAME} DISCOVERY_MODE PRE_TEST)
