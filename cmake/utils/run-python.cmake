include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/common.cmake)

# @func  run_python(<command> <result-var> <output-var> <error-var>)
# @brief Run a python snippet with the interpreter found by `find_package(Python)`
#        (falls back to `python` on PATH) and capture result, stdout and stderr.
function(run_python PY_COMMAND PY_RESULT PY_OUTPUT PY_ERROR)
    if(DEFINED Python_EXECUTABLE)
        set(_PY_EXE "${Python_EXECUTABLE}")
    else()
        set(_PY_EXE "python")
    endif()
    execute_process(
        COMMAND "${_PY_EXE}" -c "${PY_COMMAND}"
        RESULT_VARIABLE _PY_RESULT
        OUTPUT_VARIABLE _PY_OUTPUT
        ERROR_VARIABLE _PY_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(NOT _PY_RESULT EQUAL 0)
        log_fatal("run_python: PY_COMMAND failed:\nCOMMAND:\n${PY_COMMAND}\nERROR:\n${_PY_ERROR}")
    endif()
    set(${PY_RESULT} "${_PY_RESULT}" PARENT_SCOPE)
    set(${PY_OUTPUT} "${_PY_OUTPUT}" PARENT_SCOPE)
    set(${PY_ERROR} "${_PY_ERROR}" PARENT_SCOPE)
endfunction()
