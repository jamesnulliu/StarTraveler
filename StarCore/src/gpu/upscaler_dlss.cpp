// NVIDIA DLSS Super Resolution through NGX (the DLSS SDK's static library, MSVC): the
// driver's NGX core loads nvngx_dlss.dll from the directories libraryDirectories()
// gives; see cmake/libraries/upscalers.cmake.

#include "upscaler.hpp"

#ifdef STARCORE_HAS_DLSS

#include <algorithm>
#include <format>
#include <map>
#include <string_view>

#include <nvsdk_ngx_helpers_vk.h>

#include "starcore/common/log.hpp"
#include "starcore/vulkan/context.hpp"

namespace starcore::gpu
{

namespace
{

// NGX identifies applications without an id from NVIDIA by a project id (any GUID) and
// the engine.
constexpr const char* kProjectId = "6b9e3c2a-4f1d-4e8b-9a7c-5d2f0e1b3c4a";
constexpr const char* kEngineVersion = "0.1.0";

auto resultText(NVSDK_NGX_Result result) -> std::string
{
    return std::format("NGX error 0x{:08x}", static_cast<std::uint32_t>(result));
}

void NVSDK_CONV ngxLog(const char* message, NVSDK_NGX_Logging_Level /*level*/,
                       NVSDK_NGX_Feature /*component*/)
{
    std::string_view text(message);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    SC_CORE_DEBUG("DLSS: {}", text);
}

/** @brief What NGX is told on every call that discovers or initializes it. */
struct NgxSetup
{
    std::vector<std::wstring> directories;
    std::vector<const wchar_t*> paths;
    std::wstring dataPath;  // Where NGX may write its logs.
    NVSDK_NGX_FeatureCommonInfo common{};

    NgxSetup()
    {
        for (const std::filesystem::path& directory : libraryDirectories(STARCORE_DLSS_DLL_DIR)) {
            directories.push_back(directory.wstring());
        }
        for (const std::wstring& directory : directories) {
            paths.push_back(directory.c_str());
        }
        std::error_code error;
        dataPath = std::filesystem::temp_directory_path(error).wstring();
        common.PathListInfo.Path = paths.data();
        common.PathListInfo.Length = static_cast<unsigned int>(paths.size());
        common.LoggingInfo.LoggingCallback = ngxLog;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    }
    NgxSetup(const NgxSetup&) = delete;
    auto operator=(const NgxSetup&) -> NgxSetup& = delete;
};

auto imageResource(const vulkan::Image& image, bool readWrite) -> NVSDK_NGX_Resource_VK
{
    const VkImageSubresourceRange range{image.aspect(), 0, 1, 0, 1};
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view(), image.handle(), range,
                                                  image.format(), image.width(), image.height(),
                                                  readWrite);
}

/** @brief DLSS's mode of the ratio of the sizes (renderScale of UpscalerQuality). */
auto perfQuality(UVec2 renderSize, UVec2 outputSize) -> NVSDK_NGX_PerfQuality_Value
{
    const float ratio = static_cast<float>(outputSize.x) / static_cast<float>(renderSize.x);
    if (ratio > 2.5F) {
        return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    }
    if (ratio > 1.85F) {
        return NVSDK_NGX_PerfQuality_Value_MaxPerf;
    }
    if (ratio > 1.6F) {
        return NVSDK_NGX_PerfQuality_Value_Balanced;
    }
    if (ratio > 1.05F) {
        return NVSDK_NGX_PerfQuality_Value_MaxQuality;
    }
    return NVSDK_NGX_PerfQuality_Value_DLAA;
}

class DlssUpscaler final : public TemporalUpscaler
{
public:
    DlssUpscaler(vulkan::Context& context, NVSDK_NGX_Parameter* parameters)
        : m_context(&context), m_parameters(parameters)
    {
    }

    ~DlssUpscaler() override
    {
        release();
        NVSDK_NGX_VULKAN_DestroyParameters(m_parameters);
        NVSDK_NGX_VULKAN_Shutdown1(m_context->device());
    }

    DlssUpscaler(const DlssUpscaler&) = delete;
    auto operator=(const DlssUpscaler&) -> DlssUpscaler& = delete;

    auto resize(UVec2 renderSize, UVec2 outputSize) -> bool override
    {
        release();
        if (renderSize.x == 0 || renderSize.y == 0 || outputSize.x == 0 || outputSize.y == 0) {
            return false;
        }
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = renderSize.x;
        create.Feature.InHeight = renderSize.y;
        create.Feature.InTargetWidth = outputSize.x;
        create.Feature.InTargetHeight = outputSize.y;
        create.Feature.InPerfQualityValue = perfQuality(renderSize, outputSize);
        // Linear radiance, motion vectors at the rendered size without the jitter,
        // reversed depth.
        create.InFeatureCreateFlags =
            NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
        m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
            result = NGX_VULKAN_CREATE_DLSS_EXT1(m_context->device(), commandBuffer, 1, 1,
                                                 &m_feature, m_parameters, &create);
        });
        m_renderSize = renderSize;
        if (NVSDK_NGX_FAILED(result)) {
            // Tiny sizes are refused (a window being laid out).
            SC_CORE_WARN("DLSS: no feature for {}x{} -> {}x{}: {}", renderSize.x, renderSize.y,
                         outputSize.x, outputSize.y, resultText(result));
            m_feature = nullptr;
            return false;
        }
        return true;
    }

    void record(VkCommandBuffer commandBuffer, const UpscaleFrame& frame) override
    {
        inputBarrier(commandBuffer);
        if (m_feature != nullptr) {
            NVSDK_NGX_Resource_VK color = imageResource(*frame.color, false);
            NVSDK_NGX_Resource_VK depth = imageResource(*frame.depth, false);
            NVSDK_NGX_Resource_VK motion = imageResource(*frame.motion, false);
            NVSDK_NGX_Resource_VK output = imageResource(*frame.output, true);
            NVSDK_NGX_VK_DLSS_Eval_Params evaluate{};
            evaluate.Feature.pInColor = &color;
            evaluate.Feature.pInOutput = &output;
            evaluate.pInDepth = &depth;
            evaluate.pInMotionVectors = &motion;
            // In rendered pixels: the pixel center minus the sample position.
            evaluate.InJitterOffsetX = 0.5F - frame.jitter.x;
            evaluate.InJitterOffsetY = 0.5F - frame.jitter.y;
            evaluate.InRenderSubrectDimensions = {m_renderSize.x, m_renderSize.y};
            evaluate.InReset = frame.reset ? 1 : 0;
            evaluate.InMVScaleX = 1.0F;
            evaluate.InMVScaleY = 1.0F;
            evaluate.InFrameTimeDeltaInMsec = frame.frameMs;
            const NVSDK_NGX_Result result =
                NGX_VULKAN_EVALUATE_DLSS_EXT(commandBuffer, m_feature, m_parameters, &evaluate);
            if (NVSDK_NGX_FAILED(result) && !m_reported) {
                SC_CORE_ERROR("DLSS: evaluating failed: {}", resultText(result));
                m_reported = true;
            }
        }
        outputBarrier(commandBuffer);
    }

private:
    void release()
    {
        if (m_feature != nullptr) {
            NVSDK_NGX_VULKAN_ReleaseFeature(m_feature);
            m_feature = nullptr;
        }
    }

    vulkan::Context* m_context;
    NVSDK_NGX_Parameter* m_parameters;
    NVSDK_NGX_Handle* m_feature = nullptr;
    UVec2 m_renderSize{0U};
    bool m_reported = false;
};

/**
 * @brief Whether this GPU and driver can run DLSS, and the device extensions it needs.
 *        Asked once per GPU: NGX does not answer again after a shutdown.
 */
auto discover(VkInstance instance, VkPhysicalDevice physicalDevice, NgxSetup& setup)
    -> const std::expected<std::vector<std::string>, std::string>&
{
    static std::map<VkPhysicalDevice, std::expected<std::vector<std::string>, std::string>>
        discovered;
    if (const auto found = discovered.find(physicalDevice); found != discovered.end()) {
        return found->second;
    }
    auto& result = discovered[physicalDevice];
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion};
    discovery.ApplicationDataPath = setup.dataPath.c_str();
    discovery.FeatureInfo = &setup.common;
    NVSDK_NGX_FeatureRequirement requirement{};
    NVSDK_NGX_Result status =
        NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physicalDevice, &discovery, &requirement);
    if (NVSDK_NGX_FAILED(status)) {
        result = std::unexpected(std::format("not supported here ({})", resultText(status)));
        return result;
    }
    if (requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        const auto support = static_cast<std::uint32_t>(requirement.FeatureSupported);
        if ((support & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) != 0U) {
            result = std::unexpected("needs an NVIDIA RTX GPU");
        } else if ((support & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) != 0U) {
            result = std::unexpected("needs a newer NVIDIA driver");
        } else {
            result = std::unexpected(std::format("not supported here (0x{:x})", support));
        }
        return result;
    }
    std::uint32_t extensionCount = 0;
    VkExtensionProperties* extensions = nullptr;
    status = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physicalDevice, &discovery, &extensionCount, &extensions);
    if (NVSDK_NGX_FAILED(status)) {
        result = std::unexpected(std::format("no device extensions ({})", resultText(status)));
        return result;
    }
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < extensionCount; ++i) {
        names.emplace_back(extensions[i].extensionName);
    }
    result = std::move(names);
    return result;
}

}  // namespace

auto createDlss(vulkan::Context& context)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    const VkInstance instance = context.instance();
    const VkPhysicalDevice physicalDevice = context.physicalDevice();
    const VkDevice device = context.device();
    NgxSetup setup;

    // Whether this GPU and driver can, and the device has the extensions DLSS needs.
    const auto& required = discover(instance, physicalDevice, setup);
    if (!required) {
        return std::unexpected(required.error());
    }
    const std::vector<std::string>& enabled = context.deviceInfo().extensions;
    std::string missing;
    for (const std::string& name : *required) {
        if (std::ranges::find(enabled, name) == enabled.end()) {
            missing += std::format("{}{}", missing.empty() ? "" : ", ", name);
        }
    }
    if (!missing.empty()) {
        return std::unexpected(std::format("the device was created without {}", missing));
    }

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, setup.dataPath.c_str(), instance,
        physicalDevice, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, &setup.common);
    if (NVSDK_NGX_FAILED(result)) {
        return std::unexpected(std::format("NGX did not start ({})", resultText(result)));
    }
    NVSDK_NGX_Parameter* parameters = nullptr;
    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters);
    int available = 0;
    if (NVSDK_NGX_SUCCEED(result)) {
        NVSDK_NGX_Parameter_GetI(parameters, NVSDK_NGX_Parameter_SuperSampling_Available,
                                 &available);
    }
    if (available == 0) {
        int needsDriver = 0;
        if (parameters != nullptr) {
            NVSDK_NGX_Parameter_GetI(parameters,
                                     NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                                     &needsDriver);
            NVSDK_NGX_VULKAN_DestroyParameters(parameters);
        }
        NVSDK_NGX_VULKAN_Shutdown1(device);
        return std::unexpected(needsDriver != 0 ? "needs a newer NVIDIA driver"
                                                : "nvngx_dlss.dll not found or not supported");
    }
    SC_CORE_INFO("DLSS: available");
    return std::make_unique<DlssUpscaler>(context, parameters);
}

}  // namespace starcore::gpu

#else  // STARCORE_HAS_DLSS

namespace starcore::gpu
{

auto createDlss(vulkan::Context& /*context*/)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    return std::unexpected("not in this build (MSVC on Windows x64; STARCORE_DLSS)");
}

}  // namespace starcore::gpu

#endif  // STARCORE_HAS_DLSS
