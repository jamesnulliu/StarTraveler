include_guard(GLOBAL)

# @func  starcore_add_components(<target> <component>...)
# @brief Add the named modules of StarCore components to a target. A component is the module
#        `starcore.<component>`:
#          - modules/starcore.<component>.cppm      primary interface (exports the partitions)
#          - modules/<component>/*.cppm             interface partitions
#          - src/<component>/*.cppm                 internal partitions (only its units import)
#          - src/<component>/*.cpp                  implementation units (and plain sources)
#        Interfaces go into the PUBLIC file set (importable by linked targets), internal
#        partitions into a PRIVATE one.
function(starcore_add_components TARGET)
    set(INTERFACES)
    set(INTERNAL_PARTITIONS)
    set(SOURCES)
    foreach(COMPONENT IN LISTS ARGN)
        file(GLOB PARTITIONS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/modules/${COMPONENT}/*.cppm)
        file(GLOB INTERNAL CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/${COMPONENT}/*.cppm)
        file(GLOB UNITS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/src/${COMPONENT}/*.cpp)
        list(APPEND INTERFACES ${PROJECT_SOURCE_DIR}/modules/starcore.${COMPONENT}.cppm ${PARTITIONS})
        list(APPEND INTERNAL_PARTITIONS ${INTERNAL})
        list(APPEND SOURCES ${UNITS})
    endforeach()

    target_sources(${TARGET}
        PRIVATE
            ${SOURCES}
        PUBLIC
            FILE_SET CXX_MODULES BASE_DIRS ${PROJECT_SOURCE_DIR}/modules FILES ${INTERFACES}
    )
    if(INTERNAL_PARTITIONS)
        target_sources(${TARGET} PRIVATE
            FILE_SET internal_partitions TYPE CXX_MODULES
            BASE_DIRS ${PROJECT_SOURCE_DIR}/src FILES ${INTERNAL_PARTITIONS}
        )
    endif()
endfunction()
