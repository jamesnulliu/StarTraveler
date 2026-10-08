set(TARGET_NAME "StarCoreRender")

add_executable(${TARGET_NAME} ${CMAKE_CURRENT_SOURCE_DIR}/render.cpp)
set_target_properties(${TARGET_NAME} PROPERTIES OUTPUT_NAME starcore-render)

target_link_libraries(
    ${TARGET_NAME} PRIVATE
    ${PROJECT_NAMESPACE}Core
    ${PROJECT_NAMESPACE}CompileOptions
    CLI11::CLI11
)

# --gpu: render headless with the GPU path tracer.
if(STARCORE_BUILD_GPU)
    target_link_libraries(${TARGET_NAME} PRIVATE ${PROJECT_NAMESPACE}Gpu)
    target_compile_definitions(${TARGET_NAME} PRIVATE STARCORE_RENDER_GPU)
endif()
