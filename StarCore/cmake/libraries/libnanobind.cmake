# ==================================================================================================
# @file libnanobind.cmake
# @brief Find Python and nanobind (installed as a Python package: `pip install nanobind`, which
#        scikit-build-core does automatically via [build-system].requires).
#
# nanobind is taken from Python rather than vcpkg: the vcpkg port depends on vcpkg's own CPython,
# whereas the extension must match the interpreter that imports it.
#
# @note Values MAY be set BEFORE including this file:
#         - `Python_EXECUTABLE`: Interpreter to build for (e.g. a virtual environment's python).
#         - `SKBUILD_SABI_COMPONENT`: Set by scikit-build-core when building a stable-ABI wheel.
# ==================================================================================================

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../utils/run-python.cmake)

# @see "https://cmake.org/cmake/help/latest/module/FindPython.html"
find_package(Python 3.10 REQUIRED
    COMPONENTS Interpreter Development.Module ${SKBUILD_SABI_COMPONENT}
)

# @see "../utils/run-python.cmake"
run_python(
    "import nanobind; print(nanobind.cmake_dir())"
    PY_RESULT PY_OUTPUT PY_ERROR
)
list(APPEND CMAKE_PREFIX_PATH "${PY_OUTPUT}")
find_package(nanobind CONFIG REQUIRED)

log_info("Python: ${Python_EXECUTABLE} (${Python_VERSION})")
log_info("nanobind: ${nanobind_DIR}")
