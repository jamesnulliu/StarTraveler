#include <array>

#include "rasterization.hpp"
#include "upscaler.hpp"
#include "starcore/vulkan/context.hpp"
#include "starcore/vulkan/pipeline.hpp"

// Last: its macros (near, far, min, max...) would break the headers above.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace starcore::gpu
{

namespace
{

#include "shaders/taau.comp.spv.h"

constexpr VkDescriptorType kImage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
constexpr VkDescriptorType kSampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
// taau.comp: color, depth, motion, history, history out, output.
constexpr VkDescriptorType kTaauBindings[] = {kImage, kSampled, kImage, kSampled, kImage, kImage};
constexpr VkFormat kHistoryFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Samples a history holds at most: more is smoother and sharper while still, fewer
// follows changes faster (the clip catches most of them anyway).
constexpr float kMaxHistory = 16.0F;

/** @brief Push constants of taau.comp. */
struct TaauParams
{
    Vec2 jitter{0.5F};
    std::uint32_t flags = 0;
    float maxHistory = kMaxHistory;
};
constexpr std::uint32_t kTaauReset = 1U;

class TaauUpscaler final : public TemporalUpscaler
{
public:
    explicit TaauUpscaler(vulkan::Context& context)
        : m_context(&context),
          m_pipeline(context, taau_comp_spv, kTaauBindings, sizeof(TaauParams)),
          m_linear(context, VK_FILTER_LINEAR), m_nearest(context, VK_FILTER_NEAREST)
    {
    }

    auto resize(UVec2 /*renderSize*/, UVec2 outputSize) -> bool override
    {
        for (vulkan::Image& history : m_history) {
            history = {};
        }
        if (outputSize.x == 0 || outputSize.y == 0) {
            return false;
        }
        for (vulkan::Image& history : m_history) {
            history = vulkan::Image(*m_context, outputSize.x, outputSize.y, kHistoryFormat,
                                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        }
        m_context->submitAndWait([&](VkCommandBuffer commandBuffer) {
            for (const vulkan::Image& history : m_history) {
                vulkan::transitionImage(commandBuffer, history.handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                                        VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                        0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
            }
        });
        m_current = 0;
        return true;
    }

    void record(VkCommandBuffer commandBuffer, const UpscaleFrame& frame) override
    {
        inputBarrier(commandBuffer);
        const vulkan::Image& previous = m_history[1 - m_current];
        const vulkan::Image& current = m_history[m_current];
        vulkan::DescriptorWrites writes;
        writes.storageImage(0, frame.color->view())
            .sampledImage(1, frame.depth->view(), m_nearest.handle())
            .storageImage(2, frame.motion->view())
            .sampledImage(3, previous.view(), m_linear.handle())
            .storageImage(4, current.view())
            .storageImage(5, frame.output->view());
        const TaauParams params{.jitter = frame.jitter, .flags = frame.reset ? kTaauReset : 0U};
        m_pipeline.bind(commandBuffer);
        m_pipeline.pushDescriptors(commandBuffer, writes.writes());
        m_pipeline.pushConstants(commandBuffer, &params, sizeof(params));
        vkCmdDispatch(commandBuffer, (current.width() + kWorkgroupSize - 1) / kWorkgroupSize,
                      (current.height() + kWorkgroupSize - 1) / kWorkgroupSize, 1);
        outputBarrier(commandBuffer);
        m_current = 1 - m_current;
    }

private:
    vulkan::Context* m_context;
    vulkan::ComputePipeline m_pipeline;
    vulkan::Sampler m_linear;
    vulkan::Sampler m_nearest;
    std::array<vulkan::Image, 2> m_history;  // Ping-pong: written this frame / read.
    std::uint32_t m_current = 0;
};

}  // namespace

auto createTaau(vulkan::Context& context) -> std::unique_ptr<TemporalUpscaler>
{
    return std::make_unique<TaauUpscaler>(context);
}

void inputBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT |
                              VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

void outputBarrier(VkCommandBuffer commandBuffer)
{
    vulkan::memoryBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
}

auto libraryDirectories(const char* buildDirectory) -> std::vector<std::filesystem::path>
{
    std::vector<std::filesystem::path> directories;
#ifdef _WIN32
    // The module this function is in.
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&libraryDirectories), &module) != 0) {
        std::wstring path(MAX_PATH, L'\0');
        const DWORD length =
            GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
        if (length > 0 && length < path.size()) {
            path.resize(length);
            directories.push_back(std::filesystem::path(path).parent_path());
        }
    }
#endif
    if (buildDirectory != nullptr && *buildDirectory != '\0') {
        directories.emplace_back(buildDirectory);
    }
    return directories;
}

}  // namespace starcore::gpu
