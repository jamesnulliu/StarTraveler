set(TARGET_NAME "StarTraveler")

# Game logic as a library, so the executable and the tests share it.
file(GLOB_RECURSE LIB_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(REMOVE_ITEM LIB_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(StarTravelerLib STATIC ${LIB_SOURCES})
target_include_directories(StarTravelerLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(
    StarTravelerLib
    PUBLIC
        ${PROJECT_NAMESPACE}Runtime
    PRIVATE
        ${PROJECT_NAMESPACE}CompileOptions
)

add_executable(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(
    ${TARGET_NAME} PRIVATE
    StarTravelerLib
    ${PROJECT_NAMESPACE}CompileOptions
    CLI11::CLI11
)

# Fallback asset location for runs from the build tree.
target_compile_definitions(${TARGET_NAME} PRIVATE
    STARTRAVELER_SOURCE_ASSET_DIR="${PROJECT_SOURCE_DIR}/assets"
)

# Ship the assets next to the executable.
add_custom_command(
    TARGET ${TARGET_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different
            ${PROJECT_SOURCE_DIR}/assets $<TARGET_FILE_DIR:${TARGET_NAME}>/assets
    COMMENT "Copying assets next to ${TARGET_NAME}"
)

if(STARCORE_BUILD_TESTS)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()
