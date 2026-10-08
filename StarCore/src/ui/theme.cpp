module;

#include <imgui.h>

module starcore.ui;

import std;

namespace starcore::ui
{

void applyDarkTheme()
{
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    auto& colors = style.Colors;

    colors[ImGuiCol_WindowBg] = ImVec4{0.1F, 0.105F, 0.11F, 1.0F};

    colors[ImGuiCol_Header] = ImVec4{0.2F, 0.205F, 0.21F, 1.0F};
    colors[ImGuiCol_HeaderHovered] = ImVec4{0.3F, 0.305F, 0.31F, 1.0F};
    colors[ImGuiCol_HeaderActive] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};

    colors[ImGuiCol_Button] = ImVec4{0.2F, 0.205F, 0.21F, 1.0F};
    colors[ImGuiCol_ButtonHovered] = ImVec4{0.3F, 0.305F, 0.31F, 1.0F};
    colors[ImGuiCol_ButtonActive] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};

    colors[ImGuiCol_FrameBg] = ImVec4{0.2F, 0.205F, 0.21F, 1.0F};
    colors[ImGuiCol_FrameBgHovered] = ImVec4{0.3F, 0.305F, 0.31F, 1.0F};
    colors[ImGuiCol_FrameBgActive] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};

    colors[ImGuiCol_Tab] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};
    colors[ImGuiCol_TabHovered] = ImVec4{0.38F, 0.3805F, 0.381F, 1.0F};
    colors[ImGuiCol_TabSelected] = ImVec4{0.28F, 0.2805F, 0.281F, 1.0F};
    colors[ImGuiCol_TabDimmed] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};
    colors[ImGuiCol_TabDimmedSelected] = ImVec4{0.2F, 0.205F, 0.21F, 1.0F};

    colors[ImGuiCol_TitleBg] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};
    colors[ImGuiCol_TitleBgActive] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};
    colors[ImGuiCol_TitleBgCollapsed] = ImVec4{0.15F, 0.1505F, 0.151F, 1.0F};

    style.FrameRounding = 1.0F;
}

}  // namespace starcore::ui
