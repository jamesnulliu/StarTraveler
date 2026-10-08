// starcore.vulkan -- a thin layer over Vulkan 1.3 (headless or with a window):
//
//   :check      VkResult names, checking results
//   :context    instance, device, queues, allocator (vk-bootstrap, VMA)
//   :resources  buffers, images, samplers, barriers
//   :pipeline   compute and graphics pipelines, push descriptor writes
//
// Code that calls Vulkan includes <vulkan/vulkan.h> itself (a C header: its
// functions and macros cannot come from a module).

export module starcore.vulkan;

export import :check;
export import :context;
export import :pipeline;
export import :resources;
