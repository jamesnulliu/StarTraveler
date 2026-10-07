set(TARGET_NAME "StarTravelerTests")

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(${TARGET_NAME} ${TEST_SOURCES})

target_link_libraries(
    ${TARGET_NAME} PRIVATE
    StarTravelerLib
    ${PROJECT_NAMESPACE}CompileOptions
    Catch2::Catch2WithMain
)

include(Catch)
catch_discover_tests(${TARGET_NAME} DISCOVERY_MODE PRE_TEST)
