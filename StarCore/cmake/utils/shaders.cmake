# ==================================================================================================
# @file shaders.cmake
# @brief Compile GLSL shaders to SPIR-V at build time and embed them in a target.
#
# @note Values MUST be set BEFORE calling `starcore_add_shaders()`:
#         - `STARCORE_GLSLANG_EXECUTABLE`: glslang (vcpkg host tool, see dependencies.cmake).
# ==================================================================================================

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/common.cmake)

# @func  starcore_add_shaders(<target> SOURCES <file>... [INCLUDE_DIRS <dir>...] [DEPENDS <file>...])
# @brief Each SOURCES file (e.g. path_trace.comp) becomes the header
#        `shaders/path_trace.comp.spv.h` in the target's private include path, defining
#        `const uint32_t path_trace_comp_spv[]` (SPIR-V for Vulkan 1.3). DEPENDS lists included
#        files (changes recompile every shader).
function(starcore_add_shaders TARGET)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "" "SOURCES;INCLUDE_DIRS;DEPENDS")
    if(NOT STARCORE_GLSLANG_EXECUTABLE)
        log_fatal("starcore_add_shaders(): STARCORE_GLSLANG_EXECUTABLE is not set")
    endif()

    set(OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_shaders)
    set(INCLUDE_FLAGS)
    foreach(DIR IN LISTS ARG_INCLUDE_DIRS)
        list(APPEND INCLUDE_FLAGS -I${DIR})
    endforeach()

    set(HEADERS)
    foreach(SOURCE IN LISTS ARG_SOURCES)
        get_filename_component(NAME ${SOURCE} NAME)
        string(MAKE_C_IDENTIFIER "${NAME}_spv" VARIABLE)
        set(HEADER ${OUTPUT_DIR}/shaders/${NAME}.spv.h)
        add_custom_command(
            OUTPUT ${HEADER}
            COMMAND ${STARCORE_GLSLANG_EXECUTABLE} -V --target-env vulkan1.3 ${INCLUDE_FLAGS}
                    --vn ${VARIABLE} -o ${HEADER} ${SOURCE}
            DEPENDS ${SOURCE} ${ARG_DEPENDS}
            COMMENT "Compiling shader ${NAME}"
            VERBATIM
        )
        list(APPEND HEADERS ${HEADER})
    endforeach()

    target_sources(${TARGET} PRIVATE ${HEADERS})
    target_include_directories(${TARGET} PRIVATE ${OUTPUT_DIR})
endfunction()
