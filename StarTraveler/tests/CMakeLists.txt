set(TARGET_NAME "StarTravelerTests")

find_package(Catch2 3 CONFIG REQUIRED)  # Catch2::Catch2WithMain, Catch.cmake

file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(${TARGET_NAME} ${TEST_SOURCES})

target_link_libraries(
    ${TARGET_NAME} PRIVATE
    StarTravelerLib
    StarCore::CompileOptions
    Catch2::Catch2WithMain
)
# The ship tests load the cockpit model from the source tree.
target_compile_definitions(${TARGET_NAME} PRIVATE
    STARTRAVELER_SOURCE_ASSET_DIR="${PROJECT_SOURCE_DIR}/StarTraveler/assets"
)

include(Catch)
catch_discover_tests(${TARGET_NAME} DISCOVERY_MODE PRE_TEST)
