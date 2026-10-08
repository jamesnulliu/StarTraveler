# ==================================================================================================
# StarTraveler -- a game built on StarCore (C++ frontend).
# ==================================================================================================

set(TARGET_NAME "StarTraveler")

# Imported targets are directory-scoped: find what the game uses directly (the packages come from
# the vcpkg manifest of StarCore).
find_package(CLI11 CONFIG REQUIRED)  # CLI11::CLI11

# Game logic as a library, so the executable and the tests share it: the modules
# startraveler.{editor,gameplay,game} (src/startraveler.*.cppm and their partitions in
# src/*/, src/game_layer.cppm) and their implementation units.
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM LIB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
file(GLOB_RECURSE LIB_MODULES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cppm)

add_library(StarTravelerLib STATIC ${LIB_SOURCES})
target_sources(StarTravelerLib PUBLIC
    FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/src FILES ${LIB_MODULES}
)
target_link_libraries(
    StarTravelerLib
    PUBLIC
        StarCore::Runtime
    PRIVATE
        StarCore::CompileOptions
)

add_executable(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(
    ${TARGET_NAME} PRIVATE
    StarTravelerLib
    StarCore::CompileOptions
    CLI11::CLI11
)

# Fallback asset location for runs from the build tree.
target_compile_definitions(${TARGET_NAME} PRIVATE
    STARTRAVELER_SOURCE_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
)

# Ship the assets next to the executable.
add_custom_command(
    TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:${TARGET_NAME}>/assets
    COMMENT "Copying assets next to ${TARGET_NAME}"
)

if(STARCORE_BUILD_TESTS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()
