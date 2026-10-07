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
