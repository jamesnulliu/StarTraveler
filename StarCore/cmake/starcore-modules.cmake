# ==================================================================================================
# @file starcore-modules.cmake
# @brief Checks that the toolchain can build StarCore's C++ modules and provide `import std;`, and
#        lets every target `import std;`. Must be included AFTER the first `project()` (it needs
#        the compiler), and before any target is created.
#
# Included by StarCore/CMakeLists.txt, and by a workspace right after its `project()` so that its
# own targets get the same setting (include_guard: the first include decides).
#
# @note Sets in the including scope (inherited by subdirectories):
#         - `CMAKE_CXX_MODULE_STD`: ON when CMake provides the standard library module (not with
#           MSBuild, which builds it itself).
# ==================================================================================================

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/utils/common.cmake)

# GCC 16.2 targeting Windows (MinGW) emits the function-local statics of inline functions that
# reach a unit through a module -- std::make_shared's type tag, EnTT's type ids, any `export
# inline` function with a static -- as ordinary definitions instead of COMDATs: every importer
# defines them again and linking fails. Linking anyway (--allow-multiple-definition) would leave
# each object with its own copy, silently breaking EnTT's type ids. Minimal repro:
#     m.cppm:  export module m; export inline int& counter() { static int c = 0; return c; }
#     a.cpp, b.cpp:  import m; int a() { return ++counter(); }   (b() alike)
#     -> "multiple definition of `counter@m()::c'"
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND WIN32)
    log_fatal("StarCore is made of C++ modules, which GCC ${CMAKE_CXX_COMPILER_VERSION} cannot link "
              "on Windows: it emits function-local statics of imported inline functions without "
              "COMDAT (see StarCore/cmake/starcore-modules.cmake). Use MSVC (preset msvc).")
endif()

if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND MSVC)
    # MSBuild builds the standard library module itself (CMake cannot for this generator).
elseif(CMAKE_CXX_STANDARD IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    # Experimental gate set in starcore-setup.cmake.
    set(CMAKE_CXX_MODULE_STD ON)
else()
    log_fatal("`import std;` is not available: StarCore needs the Visual Studio generator with "
              "MSVC, or CMake ${CMAKE_MAJOR_VERSION}.${CMAKE_MINOR_VERSION}'s experimental "
              "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD gate (known keys: starcore-setup.cmake) and a "
              "compiler that provides the module (CMAKE_CXX_COMPILER_IMPORT_STD: "
              "\"${CMAKE_CXX_COMPILER_IMPORT_STD}\").")
endif()
