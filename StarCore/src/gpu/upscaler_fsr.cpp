// AMD FidelityFX Super Resolution 3.1 through the FFX API: amd_fidelityfx_vk.dll (the
// FidelityFX SDK's prebuilt, signed DLL) is loaded at run time; see
// cmake/libraries/upscalers.cmake.

#include "upscaler.hpp"

#ifdef STARCORE_HAS_FSR

#include <algorithm>
#include <cfloat>
#include <format>

#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>

#include "starcore/common/log.hpp"
#include "starcore/vulkan/context.hpp"

// Last: its macros (near, far, min, max...) would break the headers above.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace starcore::gpu
{

namespace
{

constexpr const wchar_t* kLibrary = L"amd_fidelityfx_vk.dll";

/** @brief The FFX API of the DLL. */
struct FfxFunctions
{
    PfnFfxCreateContext createContext = nullptr;
    PfnFfxDestroyContext destroyContext = nullptr;
    PfnFfxDispatch dispatch = nullptr;
};

/** @brief The DLL's functions, loaded once (it stays loaded). */
auto ffxFunctions() -> const std::expected<FfxFunctions, std::string>&
{
    static const std::expected<FfxFunctions, std::string> functions =
        []() -> std::expected<FfxFunctions, std::string> {
        for (const std::filesystem::path& directory : libraryDirectories(STARCORE_FSR_DLL_DIR)) {
            const std::filesystem::path path = directory / kLibrary;
            HMODULE library =
                LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (library == nullptr) {
                continue;
            }
            FfxFunctions loaded{
                .createContext = reinterpret_cast<PfnFfxCreateContext>(
                    reinterpret_cast<void*>(GetProcAddress(library, "ffxCreateContext"))),
                .destroyContext = reinterpret_cast<PfnFfxDestroyContext>(
                    reinterpret_cast<void*>(GetProcAddress(library, "ffxDestroyContext"))),
                .dispatch = reinterpret_cast<PfnFfxDispatch>(
                    reinterpret_cast<void*>(GetProcAddress(library, "ffxDispatch"))),
            };
            if (loaded.createContext == nullptr || loaded.destroyContext == nullptr ||
                loaded.dispatch == nullptr) {
                return std::unexpected(std::format("{} has no FFX API", path.string()));
            }
            SC_CORE_INFO("FSR: {}", path.string());
            return loaded;
        }
        return std::unexpected("amd_fidelityfx_vk.dll not found");
    }();
    return functions;
}

auto narrow(const wchar_t* text) -> std::string
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(std::max(size - 1, 0)));
    return result;
}

void ffxMessage(std::uint32_t type, const wchar_t* message)
{
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        SC_CORE_ERROR("FSR: {}", narrow(message));
    } else {
        SC_CORE_WARN("FSR: {}", narrow(message));
    }
}

/** @brief An image in VK_IMAGE_LAYOUT_GENERAL (FSR returns it there). */
auto resource(const vulkan::Image& image, std::uint32_t usage) -> FfxApiResource
{
    FfxApiResourceDescription description{};
    description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    description.format = ffxApiGetSurfaceFormatVK(image.format());
    description.width = image.width();
    description.height = image.height();
    description.depth = 1;
    description.mipCount = 1;
    description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    description.usage = usage;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(image.handle()), description,
                               FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
}

class FsrUpscaler final : public TemporalUpscaler
{
public:
    FsrUpscaler(vulkan::Context& context, const FfxFunctions& functions)
        : m_context(&context), m_functions(functions)
    {
    }

    ~FsrUpscaler() override
    {
        destroy();
    }

    FsrUpscaler(const FsrUpscaler&) = delete;
    auto operator=(const FsrUpscaler&) -> FsrUpscaler& = delete;

    /** @return Why the context could not be created (empty if it was). */
    auto create(UVec2 renderSize, UVec2 outputSize) -> std::string
    {
        destroy();
        m_backend = {};
        m_backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        m_backend.vkDevice = m_context->device();
        m_backend.vkPhysicalDevice = m_context->physicalDevice();
        m_backend.vkDeviceProcAddr = vkGetDeviceProcAddr;
        m_description = {};
        m_description.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        m_description.header.pNext = &m_backend.header;
        // Linear radiance, reversed depth with an infinite far plane; motion vectors at
        // the rendered size, without the jitter.
        m_description.flags = FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE |
                              FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                              FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                              FFX_UPSCALE_ENABLE_AUTO_EXPOSURE;
        m_description.maxRenderSize = {renderSize.x, renderSize.y};
        m_description.maxUpscaleSize = {outputSize.x, outputSize.y};
        m_description.fpMessage = ffxMessage;
        const ffxReturnCode_t result =
            m_functions.createContext(&m_ffx, &m_description.header, nullptr);
        if (result != FFX_API_RETURN_OK) {
            m_ffx = nullptr;
            return std::format("ffxCreateContext failed ({})", result);
        }
        m_renderSize = renderSize;
        m_outputSize = outputSize;
        return {};
    }

    auto resize(UVec2 renderSize, UVec2 outputSize) -> bool override
    {
        destroy();
        if (renderSize.x == 0 || renderSize.y == 0 || outputSize.x == 0 || outputSize.y == 0) {
            return false;
        }
        if (const std::string error = create(renderSize, outputSize); !error.empty()) {
            SC_CORE_WARN("FSR ({}x{} -> {}x{}): {}", renderSize.x, renderSize.y, outputSize.x,
                         outputSize.y, error);
            return false;
        }
        return true;
    }

    void record(VkCommandBuffer commandBuffer, const UpscaleFrame& frame) override
    {
        inputBarrier(commandBuffer);
        if (m_ffx != nullptr) {
            ffxDispatchDescUpscale dispatch{};
            dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
            dispatch.commandList = commandBuffer;
            dispatch.color = resource(*frame.color, FFX_API_RESOURCE_USAGE_READ_ONLY);
            dispatch.depth = resource(*frame.depth, FFX_API_RESOURCE_USAGE_DEPTHTARGET);
            dispatch.motionVectors = resource(*frame.motion, FFX_API_RESOURCE_USAGE_READ_ONLY);
            dispatch.output = resource(*frame.output, FFX_API_RESOURCE_USAGE_UAV);
            // FSR samples at the pixel center minus the offset.
            dispatch.jitterOffset = {0.5F - frame.jitter.x, 0.5F - frame.jitter.y};
            // Pixels at the rendered size, from the current position to the previous one.
            dispatch.motionVectorScale = {1.0F, 1.0F};
            dispatch.renderSize = {m_renderSize.x, m_renderSize.y};
            dispatch.upscaleSize = {m_outputSize.x, m_outputSize.y};
            dispatch.enableSharpening = frame.sharpness > 0.0F;
            dispatch.sharpness = std::clamp(frame.sharpness, 0.0F, 1.0F);
            dispatch.frameTimeDelta = frame.frameMs;
            dispatch.preExposure = 1.0F;
            dispatch.reset = frame.reset;
            dispatch.cameraNear = frame.nearPlane;
            dispatch.cameraFar = FLT_MAX;
            dispatch.cameraFovAngleVertical = frame.verticalFov;
            dispatch.viewSpaceToMetersFactor = 1.0F;
            if (const ffxReturnCode_t result = m_functions.dispatch(&m_ffx, &dispatch.header);
                result != FFX_API_RETURN_OK && !m_reported) {
                SC_CORE_ERROR("FSR: ffxDispatch failed ({})", result);
                m_reported = true;
            }
        }
        outputBarrier(commandBuffer);
    }

private:
    void destroy()
    {
        if (m_ffx != nullptr) {
            m_functions.destroyContext(&m_ffx, nullptr);
            m_ffx = nullptr;
        }
    }

    vulkan::Context* m_context;
    FfxFunctions m_functions;
    // The context and what it was created with (it keeps pointers to them).
    ffxContext m_ffx = nullptr;
    ffxCreateBackendVKDesc m_backend{};
    ffxCreateContextDescUpscale m_description{};
    UVec2 m_renderSize{0U};
    UVec2 m_outputSize{0U};
    bool m_reported = false;
};

}  // namespace

auto createFsr(vulkan::Context& context)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    const auto& functions = ffxFunctions();
    if (!functions) {
        return std::unexpected(functions.error());
    }
    // A context of a small size: whether FSR runs on this device at all.
    auto upscaler = std::make_unique<FsrUpscaler>(context, *functions);
    if (const std::string error = upscaler->create({64U, 64U}, {128U, 128U}); !error.empty()) {
        return std::unexpected(error);
    }
    return upscaler;
}

}  // namespace starcore::gpu

#else  // STARCORE_HAS_FSR

namespace starcore::gpu
{

auto createFsr(vulkan::Context& /*context*/)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    return std::unexpected("not in this build (Windows only; STARCORE_FSR)");
}

}  // namespace starcore::gpu

#endif  // STARCORE_HAS_FSR
