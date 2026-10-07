# ==================================================================================================
# @file cxx-compiler-configs.cmake
# @brief C++ standard and compiler/linker options for StarTraveler targets.
#
# Options are NOT appended to global CMAKE_CXX_FLAGS; they live on the INTERFACE target
# `${PROJECT_NAMESPACE}CompileOptions`, which every first-party target links PRIVATE-ly.
# Third-party code (vcpkg ports, imgui, ...) is therefore not affected by our warning level.
#
# @note Values SHOULD be set BEFORE including this file:
#         - `STARCORE_CXX_STANDARD`: 23 (default) or 26 (experimental: MSVC maps it to
#           /std:c++latest; C++26 library features must be guarded by feature-test macros).
#         - `STARCORE_WARNINGS_AS_ERRORS`: Treat warnings as errors. Default: OFF.
#         - `STACK_SIZE`: Stack size (bytes) of executables. Default: 8388608 (8 MiB).
#         - `STDOUT_IS_TERMINAL`: Colored diagnostics. Default: OFF.
# ==================================================================================================

include(${CMAKE_CURRENT_LIST_DIR}/../utils/common.cmake)

set_default_values(
    STARCORE_CXX_STANDARD         23
    STARCORE_WARNINGS_AS_ERRORS   OFF
    STACK_SIZE                    8388608
    # Header units / C++20 modules are not used (yet); skip dependency scanning.
    CMAKE_CXX_SCAN_FOR_MODULES    OFF
)

if(NOT STARCORE_CXX_STANDARD MATCHES "^(23|26)$")
    log_fatal("STARCORE_CXX_STANDARD must be 23 or 26, got: \"${STARCORE_CXX_STANDARD}\"")
endif()

set(CMAKE_CXX_STANDARD          ${STARCORE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

log_info("C++ compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
log_info("STARCORE_CXX_STANDARD: ${STARCORE_CXX_STANDARD}")
log_info("STARCORE_WARNINGS_AS_ERRORS: ${STARCORE_WARNINGS_AS_ERRORS}")
log_info("STACK_SIZE: ${STACK_SIZE}")

set(_OPTIONS_TARGET starcore_compile_options)
add_library(${_OPTIONS_TARGET} INTERFACE)
add_library(${PROJECT_NAMESPACE}CompileOptions ALIAS ${_OPTIONS_TARGET})

# MSVC ---------------------------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    target_compile_options(${_OPTIONS_TARGET} INTERFACE
        /W4 /permissive- /utf-8 /EHsc
        /Zc:__cplusplus /Zc:preprocessor /Zc:inline /Zc:throwingNew
        $<$<BOOL:${STARCORE_WARNINGS_AS_ERRORS}>:/WX>
    )
    if(CMAKE_GENERATOR MATCHES "Visual Studio")
        # Parallel compilation inside one MSBuild project
        target_compile_options(${_OPTIONS_TARGET} INTERFACE /MP)
    endif()
    target_link_options(${_OPTIONS_TARGET} INTERFACE
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:/STACK:${STACK_SIZE}>
    )
# GNU / Clang --------------------------------------------------------------------------------------
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(${_OPTIONS_TARGET} INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual
        -Wimplicit-fallthrough
        $<$<BOOL:${STARCORE_WARNINGS_AS_ERRORS}>:-Werror>
    )
    if(WIN32)
        # MinGW: self-contained executables (no libstdc++/libgcc/winpthread DLLs needed)
        target_link_options(${_OPTIONS_TARGET} INTERFACE
            $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:-static -Wl,--stack,${STACK_SIZE}>
        )
    endif()
else()
    log_fatal("Unsupported C++ compiler: ${CMAKE_CXX_COMPILER_ID}")
endif()

# Platform definitions -----------------------------------------------------------------------------
target_compile_definitions(${_OPTIONS_TARGET} INTERFACE
    $<$<PLATFORM_ID:Windows>:NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS>
)

if(STDOUT_IS_TERMINAL)
    add_compile_options(
        $<$<CXX_COMPILER_ID:GNU>:-fdiagnostics-color=always>
        $<$<CXX_COMPILER_ID:Clang>:-fcolor-diagnostics>
        $<$<CXX_COMPILER_ID:MSVC>:/FC>
    )
endif()

unset(_OPTIONS_TARGET)
