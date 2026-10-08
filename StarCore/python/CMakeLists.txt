# ==================================================================================================
# Python frontend: the extension module `starcore._core` (nanobind bindings of StarCore::Core) and
# its type stub.
#
# Built by scikit-build-core (`pip install StarCore/`, see ../pyproject.toml), or directly with
# -DSTARCORE_BUILD_PYTHON=ON; the module and stub are then copied into python/starcore/ so that
# `pytest StarCore/python/tests` works from the source tree.
# ==================================================================================================

include(${PROJECT_SOURCE_DIR}/cmake/libraries/libnanobind.cmake)

file(GLOB BINDING_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bindings/*.cpp)

# STABLE_ABI: one abi3 wheel for CPython >= 3.12 (ignored for older interpreters).
nanobind_add_module(_core NB_STATIC STABLE_ABI ${BINDING_SOURCES})
target_link_libraries(_core PRIVATE ${PROJECT_NAMESPACE}Core ${PROJECT_NAMESPACE}CompileOptions)
# The sources are implementation units of the module `starcore.python` (casters, numpy helpers).
target_sources(_core PRIVATE
    FILE_SET CXX_MODULES BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/bindings
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/bindings/starcore.python.cppm
)

# Type stub for IDEs and type checkers (imports the freshly built module).
nanobind_add_stub(
    _core_stub
    MODULE _core
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/_core.pyi
    PYTHON_PATH $<TARGET_FILE_DIR:_core>
    DEPENDS _core
)

if(SKBUILD)
    install(TARGETS _core LIBRARY DESTINATION starcore)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/_core.pyi DESTINATION starcore)
else()
    set(PACKAGE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/starcore)
    add_custom_command(
        TARGET _core POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:_core> ${PACKAGE_DIR}/
        COMMENT "Copying the extension module into python/starcore"
    )
    add_custom_command(
        TARGET _core_stub POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${CMAKE_CURRENT_BINARY_DIR}/_core.pyi ${PACKAGE_DIR}/
        COMMENT "Copying the type stub into python/starcore"
    )
endif()
