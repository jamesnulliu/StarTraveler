// NVIDIA DLSS Super Resolution and Ray Reconstruction through NGX (the DLSS SDK's static
// library, MSVC): the driver's NGX core loads nvngx_dlss.dll / nvngx_dlssd.dll from the
// directories libraryDirectories() gives; see cmake/libraries/upscalers.cmake.

#include "upscaler.hpp"

#ifdef STARCORE_HAS_DLSS

#include <algorithm>
#include <format>
#include <map>
#include <string_view>
#include <utility>

// The DLSS-RR helpers use the DLSS ones' types.
// clang-format off
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssd_vk.h>
// clang-format on

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

/**
 * @brief NGX on a device: initialized for the first of its features (DLSS, DLSS-RR) and
 *        shut down with the last (NGX is one per device: a feature shutting it down
 *        would end the others').
 */
class NgxSession
{
public:
    static auto open(vulkan::Context& context)
        -> std::expected<std::shared_ptr<NgxSession>, std::string>
    {
        static std::map<VkDevice, std::weak_ptr<NgxSession>> sessions;
        if (std::shared_ptr<NgxSession> open = sessions[context.device()].lock()) {
            return open;
        }
        NgxSetup setup;
        NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
            kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, setup.dataPath.c_str(),
            context.instance(), context.physicalDevice(), context.device(), vkGetInstanceProcAddr,
            vkGetDeviceProcAddr, &setup.common);
        if (NVSDK_NGX_FAILED(result)) {
            return std::unexpected(std::format("NGX did not start ({})", resultText(result)));
        }
        NVSDK_NGX_Parameter* capabilities = nullptr;
        result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&capabilities);
        if (NVSDK_NGX_FAILED(result)) {
            NVSDK_NGX_VULKAN_Shutdown1(context.device());
            return std::unexpected(std::format("NGX has no capabilities ({})", resultText(result)));
        }
        auto session = std::shared_ptr<NgxSession>(new NgxSession(context.device(), capabilities));
        sessions[context.device()] = session;
        return session;
    }

    ~NgxSession()
    {
        NVSDK_NGX_VULKAN_DestroyParameters(m_capabilities);
        NVSDK_NGX_VULKAN_Shutdown1(m_device);
    }
    NgxSession(const NgxSession&) = delete;
    auto operator=(const NgxSession&) -> NgxSession& = delete;

    /** @brief A capability (NVSDK_NGX_Parameter_*), 0 if NGX does not say. */
    [[nodiscard]] auto capability(const char* name) const -> int
    {
        int value = 0;
        NVSDK_NGX_Parameter_GetI(m_capabilities, name, &value);
        return value;
    }

private:
    NgxSession(VkDevice device, NVSDK_NGX_Parameter* capabilities)
        : m_device(device), m_capabilities(capabilities)
    {
    }

    VkDevice m_device;
    NVSDK_NGX_Parameter* m_capabilities;
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

// Linear radiance, motion vectors at the rendered size without the jitter, reversed depth.
constexpr int kCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                             NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                             NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;

/** @brief An NGX feature of a session: its parameters, the feature for the current sizes. */
class NgxUpscaler : public TemporalUpscaler
{
public:
    NgxUpscaler(vulkan::Context& context, std::shared_ptr<NgxSession> session,
                NVSDK_NGX_Parameter* parameters, const char* name)
        : m_context(&context), m_session(std::move(session)), m_parameters(parameters),
          m_name(name)
    {
    }

    ~NgxUpscaler() override
    {
        release();
        NVSDK_NGX_VULKAN_DestroyParameters(m_parameters);
    }

    NgxUpscaler(const NgxUpscaler&) = delete;
    auto operator=(const NgxUpscaler&) -> NgxUpscaler& = delete;

    auto resize(UVec2 renderSize, UVec2 outputSize) -> bool override
    {
        release();
        if (renderSize.x == 0 || renderSize.y == 0 || outputSize.x == 0 || outputSize.y == 0) {
            return false;
        }
        NVSDK_NGX_Result result = NVSDK_NGX_Result_Fail;
        m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
            result = create(commandBuffer, renderSize, outputSize);
        });
        m_renderSize = renderSize;
        if (NVSDK_NGX_FAILED(result)) {
            // Tiny sizes are refused (a window being laid out).
            SC_CORE_WARN("{}: no feature for {}x{} -> {}x{}: {}", m_name, renderSize.x,
                         renderSize.y, outputSize.x, outputSize.y, resultText(result));
            m_feature = nullptr;
            return false;
        }
        return true;
    }

    void record(VkCommandBuffer commandBuffer, const UpscaleFrame& frame) override
    {
        inputBarrier(commandBuffer);
        if (m_feature != nullptr) {
            const NVSDK_NGX_Result result = evaluate(commandBuffer, frame);
            if (NVSDK_NGX_FAILED(result) && !m_reported) {
                SC_CORE_ERROR("{}: evaluating failed: {}", m_name, resultText(result));
                m_reported = true;
            }
        }
        outputBarrier(commandBuffer);
    }

protected:
    /** @brief Create the feature (m_feature) between the sizes. */
    virtual auto create(VkCommandBuffer commandBuffer, UVec2 renderSize, UVec2 outputSize)
        -> NVSDK_NGX_Result = 0;
    virtual auto evaluate(VkCommandBuffer commandBuffer, const UpscaleFrame& frame)
        -> NVSDK_NGX_Result = 0;

    vulkan::Context* m_context;
    std::shared_ptr<NgxSession> m_session;  // Outlives the feature and the parameters.
    NVSDK_NGX_Parameter* m_parameters;
    NVSDK_NGX_Handle* m_feature = nullptr;
    UVec2 m_renderSize{0U};

private:
    void release()
    {
        if (m_feature != nullptr) {
            NVSDK_NGX_VULKAN_ReleaseFeature(m_feature);
            m_feature = nullptr;
        }
    }

    const char* m_name;
    bool m_reported = false;
};

class DlssUpscaler final : public NgxUpscaler
{
public:
    using NgxUpscaler::NgxUpscaler;

protected:
    auto create(VkCommandBuffer commandBuffer, UVec2 renderSize, UVec2 outputSize)
        -> NVSDK_NGX_Result override
    {
        NVSDK_NGX_DLSS_Create_Params create{};
        create.Feature.InWidth = renderSize.x;
        create.Feature.InHeight = renderSize.y;
        create.Feature.InTargetWidth = outputSize.x;
        create.Feature.InTargetHeight = outputSize.y;
        create.Feature.InPerfQualityValue = perfQuality(renderSize, outputSize);
        create.InFeatureCreateFlags = kCreateFlags | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        return NGX_VULKAN_CREATE_DLSS_EXT1(m_context->device(), commandBuffer, 1, 1, &m_feature,
                                           m_parameters, &create);
    }

    auto evaluate(VkCommandBuffer commandBuffer, const UpscaleFrame& frame)
        -> NVSDK_NGX_Result override
    {
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
        return NGX_VULKAN_EVALUATE_DLSS_EXT(commandBuffer, m_feature, m_parameters, &evaluate);
    }
};

/**
 * @brief DLSS Ray Reconstruction: denoises and upscales one frame's noisy samples, guided
 *        by the surfaces' albedos, normals and roughness (packed: the normals' w) and the
 *        motion of what they reflect; the overlay (the panes' reflections, denoised by the
 *        pipeline) is only upscaled.
 */
class DlssRrUpscaler final : public NgxUpscaler
{
public:
    using NgxUpscaler::NgxUpscaler;

    [[nodiscard]] auto denoises() const -> bool override
    {
        return true;
    }

protected:
    auto create(VkCommandBuffer commandBuffer, UVec2 renderSize, UVec2 outputSize)
        -> NVSDK_NGX_Result override
    {
        NVSDK_NGX_DLSSD_Create_Params create{};
        create.InDenoiseMode = NVSDK_NGX_DLSS_Denoise_Mode_DLUnified;
        create.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        create.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;
        create.InWidth = renderSize.x;
        create.InHeight = renderSize.y;
        create.InTargetWidth = outputSize.x;
        create.InTargetHeight = outputSize.y;
        create.InPerfQualityValue = perfQuality(renderSize, outputSize);
        create.InFeatureCreateFlags = kCreateFlags;  // No auto exposure: it has none.
        return NGX_VULKAN_CREATE_DLSSD_EXT1(m_context->device(), commandBuffer, 1, 1, &m_feature,
                                            m_parameters, &create);
    }

    auto evaluate(VkCommandBuffer commandBuffer, const UpscaleFrame& frame)
        -> NVSDK_NGX_Result override
    {
        if (frame.diffuseAlbedo == nullptr || frame.specularAlbedo == nullptr ||
            frame.normalRoughness == nullptr) {
            return NVSDK_NGX_Result_FAIL_InvalidParameter;  // No guides: nothing to denoise.
        }
        NVSDK_NGX_Resource_VK color = imageResource(*frame.color, false);
        NVSDK_NGX_Resource_VK depth = imageResource(*frame.depth, false);
        NVSDK_NGX_Resource_VK motion = imageResource(*frame.motion, false);
        NVSDK_NGX_Resource_VK output = imageResource(*frame.output, true);
        NVSDK_NGX_Resource_VK diffuse = imageResource(*frame.diffuseAlbedo, false);
        NVSDK_NGX_Resource_VK specular = imageResource(*frame.specularAlbedo, false);
        NVSDK_NGX_Resource_VK normals = imageResource(*frame.normalRoughness, false);
        NVSDK_NGX_Resource_VK reflectionMotion{};
        NVSDK_NGX_Resource_VK overlay{};
        NVSDK_NGX_VK_DLSSD_Eval_Params evaluate{};
        evaluate.pInColor = &color;
        evaluate.pInOutput = &output;
        evaluate.pInDepth = &depth;
        evaluate.pInMotionVectors = &motion;
        evaluate.pInDiffuseAlbedo = &diffuse;
        evaluate.pInSpecularAlbedo = &specular;
        evaluate.pInNormals = &normals;
        if (frame.reflectionMotion != nullptr) {
            reflectionMotion = imageResource(*frame.reflectionMotion, false);
            evaluate.pInMotionVectorsReflections = &reflectionMotion;
        }
        if (frame.overlay != nullptr) {
            overlay = imageResource(*frame.overlay, false);
            evaluate.pInTransparencyLayer = &overlay;
        }
        evaluate.InJitterOffsetX = 0.5F - frame.jitter.x;
        evaluate.InJitterOffsetY = 0.5F - frame.jitter.y;
        evaluate.InRenderSubrectDimensions = {m_renderSize.x, m_renderSize.y};
        evaluate.InReset = frame.reset ? 1 : 0;
        evaluate.InMVScaleX = 1.0F;
        evaluate.InMVScaleY = 1.0F;
        evaluate.InFrameTimeDeltaInMsec = frame.frameMs;
        return NGX_VULKAN_EVALUATE_DLSSD_EXT(commandBuffer, m_feature, m_parameters, &evaluate);
    }
};

using Requirements = std::expected<std::vector<std::string>, std::string>;

/** @brief discover() of one feature. */
auto discoverFeature(VkInstance instance, VkPhysicalDevice physicalDevice,
                     NVSDK_NGX_Feature feature, NgxSetup& setup) -> Requirements
{
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = feature;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion};
    discovery.ApplicationDataPath = setup.dataPath.c_str();
    discovery.FeatureInfo = &setup.common;
    NVSDK_NGX_FeatureRequirement requirement{};
    NVSDK_NGX_Result status =
        NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physicalDevice, &discovery, &requirement);
    if (NVSDK_NGX_FAILED(status)) {
        return std::unexpected(std::format("not supported here ({})", resultText(status)));
    }
    if (requirement.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        const auto support = static_cast<std::uint32_t>(requirement.FeatureSupported);
        if ((support & NVSDK_NGX_FeatureSupportResult_AdapterUnsupported) != 0U) {
            return std::unexpected("needs an NVIDIA RTX GPU");
        }
        if ((support & NVSDK_NGX_FeatureSupportResult_DriverVersionUnsupported) != 0U) {
            return std::unexpected("needs a newer NVIDIA driver");
        }
        return std::unexpected(std::format("not supported here (0x{:x})", support));
    }
    std::uint32_t extensionCount = 0;
    VkExtensionProperties* extensions = nullptr;
    status = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physicalDevice, &discovery, &extensionCount, &extensions);
    if (NVSDK_NGX_FAILED(status)) {
        return std::unexpected(std::format("no device extensions ({})", resultText(status)));
    }
    std::vector<std::string> names;
    for (std::uint32_t i = 0; i < extensionCount; ++i) {
        names.emplace_back(extensions[i].extensionName);
    }
    return names;
}

/**
 * @brief Whether this GPU and driver can run an NGX feature, and the device extensions it
 *        needs. Asked once per GPU, of all the features at once: NGX does not answer (not
 *        implemented) after it was initialized, even once shut down again.
 */
auto discover(VkInstance instance, VkPhysicalDevice physicalDevice, NVSDK_NGX_Feature feature,
              NgxSetup& setup) -> const Requirements&
{
    static std::map<std::pair<VkPhysicalDevice, NVSDK_NGX_Feature>, Requirements> discovered;
    if (!discovered.contains({physicalDevice, feature})) {
        for (const NVSDK_NGX_Feature each :
             {NVSDK_NGX_Feature_SuperSampling, NVSDK_NGX_Feature_RayReconstruction}) {
            discovered.try_emplace({physicalDevice, each},
                                   discoverFeature(instance, physicalDevice, each, setup));
        }
    }
    return discovered.at({physicalDevice, feature});
}

/**
 * @brief An NGX feature's upscaler, if this GPU, its driver and the device can run it and
 *        NGX finds its DLL (`available`, `needsDriver`: its capabilities).
 */
template <typename NgxFeature>
auto createNgx(vulkan::Context& context, NVSDK_NGX_Feature feature, const char* name,
               const char* available, const char* needsDriver, const char* dll)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    NgxSetup setup;
    const auto& required = discover(context.instance(), context.physicalDevice(), feature, setup);
    if (!required) {
        return std::unexpected(required.error());
    }
    const std::vector<std::string>& enabled = context.deviceInfo().extensions;
    std::string missing;
    for (const std::string& extension : *required) {
        if (std::ranges::find(enabled, extension) == enabled.end()) {
            missing += std::format("{}{}", missing.empty() ? "" : ", ", extension);
        }
    }
    if (!missing.empty()) {
        return std::unexpected(std::format("the device was created without {}", missing));
    }
    auto session = NgxSession::open(context);
    if (!session) {
        return std::unexpected(session.error());
    }
    if ((*session)->capability(available) == 0) {
        return std::unexpected((*session)->capability(needsDriver) != 0
                                   ? std::string("needs a newer NVIDIA driver")
                                   : std::format("{} not found or not supported", dll));
    }
    NVSDK_NGX_Parameter* parameters = nullptr;
    const NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_AllocateParameters(&parameters);
    if (NVSDK_NGX_FAILED(result)) {
        return std::unexpected(std::format("no parameters ({})", resultText(result)));
    }
    SC_CORE_INFO("{}: available", name);
    return std::make_unique<NgxFeature>(context, std::move(*session), parameters, name);
}

}  // namespace

auto createDlss(vulkan::Context& context)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    return createNgx<DlssUpscaler>(context, NVSDK_NGX_Feature_SuperSampling, "DLSS",
                                   NVSDK_NGX_Parameter_SuperSampling_Available,
                                   NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver,
                                   "nvngx_dlss.dll");
}

auto createDlssRr(vulkan::Context& context)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    return createNgx<DlssRrUpscaler>(context, NVSDK_NGX_Feature_RayReconstruction, "DLSS-RR",
                                     NVSDK_NGX_Parameter_SuperSamplingDenoising_Available,
                                     NVSDK_NGX_Parameter_SuperSamplingDenoising_NeedsUpdatedDriver,
                                     "nvngx_dlssd.dll");
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

auto createDlssRr(vulkan::Context& /*context*/)
    -> std::expected<std::unique_ptr<TemporalUpscaler>, std::string>
{
    return std::unexpected("not in this build (MSVC on Windows x64; STARCORE_DLSS)");
}

}  // namespace starcore::gpu

#endif  // STARCORE_HAS_DLSS
