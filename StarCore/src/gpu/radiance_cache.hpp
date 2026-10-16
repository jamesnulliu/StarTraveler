#pragma once
// The hybrid pipeline's radiance cache (private, GpuRenderOptions::radianceCache;
// shaders/radiance_cache.glsl): a hash table of the surfaces' cells and the light they
// reflect, learnt from a few paths every frame (hybrid_trace.comp's cache stage) and
// averaged here (cache_resolve.comp); the other paths end where it knows the light.

#include <cstdint>

#include <vulkan/vulkan.h>

#include "rasterization.hpp"
#include "starcore/vulkan/pipeline.hpp"
#include "starcore/vulkan/resources.hpp"

namespace starcore::gpu
{

class RadianceCache
{
public:
    explicit RadianceCache(vulkan::Context& context);

    /** @brief Its buffers' addresses and capacity into the frame uniforms. */
    void describe(FrameUniforms& uniforms) const;

    /** @brief Record forgetting everything (new content: the light learnt is wrong). */
    void clear(VkCommandBuffer commandBuffer) const;

    /**
     * @brief Record averaging this frame's samples into the entries' light (after the
     *        cache stage wrote them, before the paths look it up).
     */
    void resolve(VkCommandBuffer commandBuffer) const;

private:
    vulkan::Context* m_context;
    vulkan::ComputePipeline m_resolve;
    vulkan::Buffer m_keys;
    vulkan::Buffer m_sums;
    vulkan::Buffer m_values;
};

}  // namespace starcore::gpu
