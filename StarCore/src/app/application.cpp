module;

// vulkan.h must precede glfw3.h to get glfwCreateWindowSurface.
#include <vulkan/vulkan.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include "starcore/common/assert.hpp"
#include "starcore/common/log.hpp"

module starcore.app;

import std;
import starcore.math;
import starcore.platform;
import starcore.ui;
import starcore.vulkan;

// Frame loop adapted from Dear ImGui's example_glfw_vulkan (v1.92).

namespace starcore
{

namespace
{

Application* g_instance = nullptr;
constexpr std::uint32_t kMinImageCount = 2;
// Descriptors for user textures (ImGui_ImplVulkan_AddTexture).
constexpr std::uint32_t kUserTextureDescriptors = 64;

void checkVkResult(VkResult result)
{
    vulkan::check(result);
}

}  // namespace

struct Application::Frame
{
    ImGui_ImplVulkanH_Window window;
    bool swapchainRebuild = false;
};

Application::Application(ApplicationConfig config) : m_config(std::move(config))
{
    SC_VERIFY(g_instance == nullptr, "only one Application may exist");
    g_instance = this;

    m_window = std::make_unique<Window>(WindowConfig{
        .title = m_config.name, .width = m_config.width, .height = m_config.height});

    std::uint32_t glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
    if (glfwExtensions == nullptr) {
        throw std::runtime_error("GLFW: Vulkan is not supported on this system");
    }
    GLFWwindow* window = m_window->handle();
    auto context = vulkan::Context::create({
        .applicationName = m_config.name,
        .enableValidation = m_config.enableValidation,
        .surfaceExtensions = {glfwExtensions, glfwExtensions + glfwExtensionCount},
        .createSurface =
            [window](VkInstance instance, VkSurfaceKHR* surface) {
                return glfwCreateWindowSurface(instance, window, nullptr, surface);
            },
    });
    if (!context) {
        throw std::runtime_error("Vulkan initialization failed: " + context.error());
    }
    m_vulkan = std::move(*context);

    // Swapchain, render pass and per-frame resources via the ImGui helpers.
    m_frame = std::make_unique<Frame>();
    ImGui_ImplVulkanH_Window& wd = m_frame->window;
    wd.Surface = m_vulkan->surface();

    const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM,
                                VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM};
    wd.SurfaceFormat = ImGui_ImplVulkanH_SelectSurfaceFormat(
        m_vulkan->physicalDevice(), wd.Surface, formats,
        static_cast<int>(std::size(formats)), VK_COLORSPACE_SRGB_NONLINEAR_KHR);

    const VkPresentModeKHR vsyncModes[] = {VK_PRESENT_MODE_FIFO_KHR};
    const VkPresentModeKHR fastModes[] = {VK_PRESENT_MODE_MAILBOX_KHR,
                                          VK_PRESENT_MODE_IMMEDIATE_KHR,
                                          VK_PRESENT_MODE_FIFO_KHR};
    wd.PresentMode = m_config.vsync
                         ? ImGui_ImplVulkanH_SelectPresentMode(
                               m_vulkan->physicalDevice(), wd.Surface,
                               vsyncModes, static_cast<int>(std::size(vsyncModes)))
                         : ImGui_ImplVulkanH_SelectPresentMode(
                               m_vulkan->physicalDevice(), wd.Surface,
                               fastModes, static_cast<int>(std::size(fastModes)));

    const IVec2 size = m_window->framebufferSize();
    ImGui_ImplVulkanH_CreateOrResizeWindow(
        m_vulkan->instance(), m_vulkan->physicalDevice(), m_vulkan->device(),
        &wd, m_vulkan->queueFamily(), nullptr, size.x, size.y, kMinImageCount, 0);

    initImGui();
}

Application::~Application()
{
    m_vulkan->waitIdle();
    // Layers own GPU resources (textures): destroy them while Vulkan and
    // ImGui are still alive, in reverse order of creation.
    for (auto& layer : std::views::reverse(m_layers)) {
        layer->onDetach();
    }
    while (!m_layers.empty()) {
        m_layers.pop_back();
    }
    shutdownImGui();
    ImGui_ImplVulkanH_DestroyWindow(m_vulkan->instance(), m_vulkan->device(),
                                    &m_frame->window, nullptr);
    m_frame.reset();
    m_vulkan.reset();
    m_window.reset();
    g_instance = nullptr;
}

auto Application::get() -> Application&
{
    SC_VERIFY(g_instance != nullptr, "no Application instance");
    return *g_instance;
}

void Application::initImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    if (m_config.enableViewports) {
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
    }

    const float scale =
        ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    ui::applyDarkTheme();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    io.ConfigDpiScaleFonts = true;
    io.ConfigDpiScaleViewports = true;
    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
        // Platform windows should look like regular ImGui windows.
        style.WindowRounding = 0.0F;
        style.Colors[ImGuiCol_WindowBg].w = 1.0F;
    }

    ImGui_ImplGlfw_InitForVulkan(m_window->handle(), true);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = m_vulkan->apiVersion();
    info.Instance = m_vulkan->instance();
    info.PhysicalDevice = m_vulkan->physicalDevice();
    info.Device = m_vulkan->device();
    info.QueueFamily = m_vulkan->queueFamily();
    info.Queue = m_vulkan->queue();
    info.DescriptorPoolSize =
        IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE + kUserTextureDescriptors;
    info.MinImageCount = kMinImageCount;
    info.ImageCount = m_frame->window.ImageCount;
    info.PipelineInfoMain.RenderPass = m_frame->window.RenderPass;
    info.PipelineInfoMain.Subpass = 0;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    info.CheckVkResultFn = checkVkResult;
    ImGui_ImplVulkan_Init(&info);
}

void Application::shutdownImGui()
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void Application::close()
{
    m_running = false;
}

void Application::rebuildSwapchainIfNeeded()
{
    ImGui_ImplVulkanH_Window& wd = m_frame->window;
    const IVec2 size = m_window->framebufferSize();
    if (size.x > 0 && size.y > 0 &&
        (m_frame->swapchainRebuild || wd.Width != size.x || wd.Height != size.y)) {
        ImGui_ImplVulkan_SetMinImageCount(kMinImageCount);
        ImGui_ImplVulkanH_CreateOrResizeWindow(
            m_vulkan->instance(), m_vulkan->physicalDevice(), m_vulkan->device(),
            &wd, m_vulkan->queueFamily(), nullptr, size.x, size.y,
            kMinImageCount, 0);
        wd.FrameIndex = 0;
        m_frame->swapchainRebuild = false;
    }
}

void Application::run()
{
    m_running = true;
    auto lastTime = std::chrono::steady_clock::now();

    while (m_running && !m_window->shouldClose()) {
        Window::pollEvents();
        rebuildSwapchainIfNeeded();
        if (m_window->isMinimized()) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        m_deltaTime = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;

        for (auto& layer : m_layers) {
            layer->onUpdate(m_deltaTime);
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (m_menubarCallback && ImGui::BeginMainMenuBar()) {
            m_menubarCallback();
            ImGui::EndMainMenuBar();
        }
        m_dockspaceId = ImGui::DockSpaceOverViewport(
            0, nullptr, ImGuiDockNodeFlags_PassthruCentralNode);

        for (auto& layer : m_layers) {
            layer->onUIRender();
        }

        ImGui::Render();
        renderFrame();
        if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        presentFrame();
    }
    m_running = false;
}

void Application::renderFrame()
{
    ImDrawData* drawData = ImGui::GetDrawData();
    if (drawData->DisplaySize.x <= 0.0F || drawData->DisplaySize.y <= 0.0F) {
        return;
    }
    ImGui_ImplVulkanH_Window& wd = m_frame->window;
    const VkDevice device = m_vulkan->device();

    VkSemaphore imageAcquired = wd.FrameSemaphores[wd.SemaphoreIndex].ImageAcquiredSemaphore;
    const VkResult acquired = vkAcquireNextImageKHR(
        device, wd.Swapchain, UINT64_MAX, imageAcquired, VK_NULL_HANDLE, &wd.FrameIndex);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR || acquired == VK_SUBOPTIMAL_KHR) {
        m_frame->swapchainRebuild = true;
    }
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }
    vulkan::check(acquired);
    // The semaphore the presentation waits on belongs to the image: presenting
    // only lets go of it once that image is acquired again.
    VkSemaphore renderComplete = wd.FrameSemaphores[wd.FrameIndex].RenderCompleteSemaphore;

    ImGui_ImplVulkanH_Frame& fd = wd.Frames[wd.FrameIndex];
    vulkan::check(vkWaitForFences(device, 1, &fd.Fence, VK_TRUE, UINT64_MAX));
    vulkan::check(vkResetFences(device, 1, &fd.Fence));
    vulkan::check(vkResetCommandPool(device, fd.CommandPool, 0));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vulkan::check(vkBeginCommandBuffer(fd.CommandBuffer, &beginInfo));

    // CPU-rendered images (path tracer output) -> GPU, before the UI samples them.
    m_vulkan->recordFrame(fd.CommandBuffer, wd.FrameIndex);

    VkRenderPassBeginInfo passInfo{};
    passInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    passInfo.renderPass = wd.RenderPass;
    passInfo.framebuffer = fd.Framebuffer;
    passInfo.renderArea.extent.width = static_cast<std::uint32_t>(wd.Width);
    passInfo.renderArea.extent.height = static_cast<std::uint32_t>(wd.Height);
    wd.ClearValue.color = {{0.1F, 0.105F, 0.11F, 1.0F}};
    passInfo.clearValueCount = 1;
    passInfo.pClearValues = &wd.ClearValue;
    vkCmdBeginRenderPass(fd.CommandBuffer, &passInfo, VK_SUBPASS_CONTENTS_INLINE);
    ImGui_ImplVulkan_RenderDrawData(drawData, fd.CommandBuffer);
    vkCmdEndRenderPass(fd.CommandBuffer);
    vulkan::check(vkEndCommandBuffer(fd.CommandBuffer));

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &imageAcquired;
    submitInfo.pWaitDstStageMask = &waitStage;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &fd.CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &renderComplete;
    vulkan::check(vkQueueSubmit(m_vulkan->queue(), 1, &submitInfo, fd.Fence));
}

void Application::presentFrame()
{
    ImDrawData* drawData = ImGui::GetDrawData();
    if (m_frame->swapchainRebuild || drawData->DisplaySize.x <= 0.0F ||
        drawData->DisplaySize.y <= 0.0F) {
        return;
    }
    ImGui_ImplVulkanH_Window& wd = m_frame->window;
    VkSemaphore renderComplete = wd.FrameSemaphores[wd.FrameIndex].RenderCompleteSemaphore;
    VkPresentInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &renderComplete;
    info.swapchainCount = 1;
    info.pSwapchains = &wd.Swapchain;
    info.pImageIndices = &wd.FrameIndex;
    const VkResult result = vkQueuePresentKHR(m_vulkan->queue(), &info);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        m_frame->swapchainRebuild = true;
    }
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        return;
    }
    vulkan::check(result);
    wd.SemaphoreIndex = (wd.SemaphoreIndex + 1) % wd.SemaphoreCount;
}

}  // namespace starcore
