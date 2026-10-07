set(TARGET_NAME "StarTraveler")

file(GLOB_RECURSE CXX_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_executable(${TARGET_NAME} ${CXX_SOURCES})

target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(
    ${TARGET_NAME} PRIVATE
    ${PROJECT_NAMESPACE}Runtime
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
