# ==================================================================================================
# @file upscalers.cmake
# @brief The vendors' temporal upscalers of StarCore::Gpu (GpuRenderOptions::upscaling), both
#        optional: the renderer has its own (TAAU) for any GPU.
#
#   AMD FSR 3.1       FidelityFX SDK 1.1.4 (MIT): the FFX API headers and its prebuilt, signed
#                     Vulkan DLL, loaded at run time (any compiler, any GPU; Windows only).
#   NVIDIA DLSS       DLSS SDK 310.9.1 (NVIDIA RTX SDK license): the NGX headers, the static NGX
#                     library (MSVC only) and the DLLs of DLSS Super Resolution and Ray
#                     Reconstruction (NVIDIA RTX GPUs; Windows x64).
#
# Their files are downloaded at configure time into the build tree (not part of the repository),
# checked against the hashes below. Without network access, or with -DSTARCORE_FSR=OFF /
# -DSTARCORE_DLSS=OFF, the renderer simply does not offer them.
#
# Defines (if enabled and downloaded):
#   - `StarCore::Fsr`:  INTERFACE target; STARCORE_HAS_FSR, STARCORE_FSR_DLL_DIR.
#   - `StarCore::Dlss`: INTERFACE target; STARCORE_HAS_DLSS, STARCORE_DLSS_DLL_DIR, links NGX.
# The DLLs are looked for next to the executable first, then in those directories.
# ==================================================================================================

include_guard(GLOBAL)
include(${CMAKE_CURRENT_LIST_DIR}/../utils/common.cmake)

# @func  starcore_download_files(<OK var> <base URL> <directory> <path> <sha256> ...)
# @brief Download <base URL>/<path> to <directory>/<path> for each (path, SHA-256) pair, unless
#        the file is there with that hash. <OK var>: whether all of them are.
function(starcore_download_files OK BASE_URL DIRECTORY)
    set(${OK} TRUE PARENT_SCOPE)
    set(FILES ${ARGN})
    list(LENGTH FILES COUNT)
    math(EXPR LAST "${COUNT} - 1")
    foreach(INDEX RANGE 0 ${LAST} 2)
        math(EXPR HASH_INDEX "${INDEX} + 1")
        list(GET FILES ${INDEX} FILE_PATH)
        list(GET FILES ${HASH_INDEX} FILE_HASH)
        set(TARGET_FILE "${DIRECTORY}/${FILE_PATH}")
        if(EXISTS "${TARGET_FILE}")
            file(SHA256 "${TARGET_FILE}" EXISTING_HASH)
            string(TOUPPER "${EXISTING_HASH}" EXISTING_HASH)
            if(EXISTING_HASH STREQUAL FILE_HASH)
                continue()
            endif()
        endif()
        log_info("Downloading ${BASE_URL}/${FILE_PATH}")
        file(DOWNLOAD "${BASE_URL}/${FILE_PATH}" "${TARGET_FILE}"
            EXPECTED_HASH SHA256=${FILE_HASH}
            TLS_VERIFY ON
            STATUS STATUS
        )
        list(GET STATUS 0 CODE)
        if(NOT CODE EQUAL 0)
            list(GET STATUS 1 MESSAGE)
            log_warning("Could not download ${BASE_URL}/${FILE_PATH}: ${MESSAGE}")
            file(REMOVE "${TARGET_FILE}")
            set(${OK} FALSE PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

set(STARCORE_UPSCALERS_DIR "${CMAKE_BINARY_DIR}/_deps/upscalers")

# AMD FSR ------------------------------------------------------------------------------------------
if(STARCORE_FSR AND NOT WIN32)
    log_info("FSR: only on Windows (the SDK's prebuilt Vulkan DLL)")
elseif(STARCORE_FSR)
    set(FSR_DIR "${STARCORE_UPSCALERS_DIR}/fidelityfx-sdk-1.1.4")
    starcore_download_files(FSR_OK
        "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v1.1.4"
        "${FSR_DIR}"
        LICENSE.txt
        82CF74FC23885107C7F69249AC3F1DAC55898FF68320EB2C753984CAE6B2D78A
        ffx-api/include/ffx_api/ffx_api.h
        408B4203545E8299ED188FBB1D9DDFE0932B73EA6E8B14FE10EDCF1F45771561
        ffx-api/include/ffx_api/ffx_api_types.h
        DEE2782813D22047C068E0FB27D1347324F8CE79D280A4672848B3B294CB12FE
        ffx-api/include/ffx_api/ffx_upscale.h
        1155BCDCEA9C10E6A95507BC9422CA062A853AC84250F0B09A637A66836B3AC1
        ffx-api/include/ffx_api/vk/ffx_api_vk.h
        A50A8D5FF5689A10A6CE378F2DF5569FD880C89EC46E8356AAF35EB40512EDD9
        PrebuiltSignedDLL/amd_fidelityfx_vk.dll
        A1624CC4238FEF046F30C4D80CE3F47BE63FC5F5373F49E3EE9EDB9960F54C78
    )
    if(FSR_OK)
        add_library(starcore_fsr INTERFACE)
        add_library(${PROJECT_NAMESPACE}Fsr ALIAS starcore_fsr)
        target_include_directories(starcore_fsr SYSTEM INTERFACE "${FSR_DIR}/ffx-api/include")
        target_compile_definitions(starcore_fsr INTERFACE
            STARCORE_HAS_FSR
            STARCORE_FSR_DLL_DIR="${FSR_DIR}/PrebuiltSignedDLL"
        )
        log_info("FSR: FidelityFX SDK 1.1.4 (FSR 3.1) in ${FSR_DIR}")
    endif()
endif()

# NVIDIA DLSS --------------------------------------------------------------------------------------
if(STARCORE_DLSS AND NOT (WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8))
    log_info("DLSS: only with MSVC on Windows x64 (the SDK's NGX library)")
elseif(STARCORE_DLSS)
    set(DLSS_DIR "${STARCORE_UPSCALERS_DIR}/dlss-sdk-310.9.1")
    starcore_download_files(DLSS_OK
        "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.1"
        "${DLSS_DIR}"
        LICENSE.txt
        D4216E39EBEF5F9B50A6712EBB37BEEB5379862A67733A9999C651F21592AAF0
        include/nvsdk_ngx.h
        DC38E7467CF415379C9D12AE1B6E4A494C453ED92720FB53E92AECB523E7B848
        include/nvsdk_ngx_defs.h
        EA23F33497CD274860D1C25A97644FCE807DCB0037C594547203343103FAD03E
        include/nvsdk_ngx_defs_vk.h
        0A24D0861ACE7D6B9362A67B7F08BEA1B33EA123C5CE730B19133DE8A891D031
        include/nvsdk_ngx_params.h
        943BC8CC5CDAE03B6303016FBAD3183636F2335AE27A2D18776798C3B4EFABBC
        include/nvsdk_ngx_vk.h
        2D364CE7132881EB669E9498FD570D74CBA563B1FBEBC82C235F8E1AD2DD8B6D
        include/nvsdk_ngx_helpers_vk.h
        C192BFF72138F12F770DB48E12C1D8F712DFC81EA0E8E055EBB27D8DD1B31623
        include/nvsdk_ngx_defs_dlssd.h
        2E97FE1595B1334F71B51724F2CB67496E8C296A7A2EEC5606DA53055882E716
        include/nvsdk_ngx_params_dlssd.h
        D41E3BA8F1D72583B55273092C99BF426DC4A0D46DE827229E25D5B1325A370E
        include/nvsdk_ngx_helpers_dlssd_vk.h
        11DF8FBDA532A97FE867C8EF2B40058B49B7898183710F3468E55B1EEDB8F40B
        lib/Windows_x86_64/x64/nvsdk_ngx_d.lib
        4B6CECAD7F1906571C94010241F650E4A5457E64FAD49DDACCB82DE79F6C2999
        lib/Windows_x86_64/x64/nvsdk_ngx_d_dbg.lib
        712F18162C8C8766C92C98031B52D2BA34EE0AD027BA79731278EE6B63CD3E02
        lib/Windows_x86_64/rel/nvngx_dlss.dll
        3975567B8943C53ACCE397F2B72380092F84F162D00B0D2C7D08A1025C563983
        lib/Windows_x86_64/rel/nvngx_dlssd.dll
        4BC7EA5FCB2F32CF86BC2CB072E8D2860914A0BE511CBDCB2FC206C1D9D83B80
    )
    if(DLSS_OK)
        add_library(starcore_dlss INTERFACE)
        add_library(${PROJECT_NAMESPACE}Dlss ALIAS starcore_dlss)
        target_include_directories(starcore_dlss SYSTEM INTERFACE "${DLSS_DIR}/include")
        target_compile_definitions(starcore_dlss INTERFACE
            STARCORE_HAS_DLSS
            STARCORE_DLSS_DLL_DIR="${DLSS_DIR}/lib/Windows_x86_64/rel"
        )
        # The NGX library of the dynamic C runtime (/MD, /MDd).
        set(NGX_LIB_DIR "${DLSS_DIR}/lib/Windows_x86_64/x64")
        target_link_libraries(starcore_dlss INTERFACE
            $<IF:$<CONFIG:Debug>,${NGX_LIB_DIR}/nvsdk_ngx_d_dbg.lib,${NGX_LIB_DIR}/nvsdk_ngx_d.lib>
        )
        log_info("DLSS: DLSS SDK 310.9.1 in ${DLSS_DIR}")
    endif()
endif()
