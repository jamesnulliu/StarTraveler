set(TARGET_NAME "StarCoreTests")

# Headless CPU tests; the GPU tests (gpu/) are a separate executable.
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

add_executable(${TARGET_NAME} ${TEST_SOURCES})

target_link_libraries(
    ${TARGET_NAME} PRIVATE
    ${PROJECT_NAMESPACE}Core
    ${PROJECT_NAMESPACE}CompileOptions
    Catch2::Catch2WithMain
)

# Tests feed constant axis-parallel rays (1/0 = inf is intended in slab tests)
# and degenerate triangles; MSVC's constant propagation then reports C4723
# "potential divide by 0" on paths that are guarded or IEEE-defined.
target_compile_options(${TARGET_NAME} PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/wd4723>)

include(Catch)
# PRE_TEST: discover test cases when ctest runs instead of after every build.
catch_discover_tests(${TARGET_NAME} DISCOVERY_MODE PRE_TEST)

if(STARCORE_BUILD_GPU)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/gpu)
endif()
