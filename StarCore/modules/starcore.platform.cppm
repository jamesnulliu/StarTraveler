// starcore.platform -- the operating system side of the runtime:
//
//   :window       a GLFW window without a graphics context
//   :input        keyboard and mouse queries (on ImGui's input state), cursor modes
//   :system_info  versions of GLFW, ImGui, the Vulkan headers and loader

export module starcore.platform;

export import :input;
export import :system_info;
export import :window;
