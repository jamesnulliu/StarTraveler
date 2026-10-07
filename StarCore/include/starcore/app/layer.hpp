#pragma once

namespace starcore
{

/**
 * @brief A unit of application logic driven by the Application frame loop:
 *        onUpdate(dt) for simulation, then onUIRender() inside the ImGui
 *        frame. Layers are owned by the Application.
 */
class Layer
{
public:
    Layer() = default;
    virtual ~Layer() = default;
    Layer(const Layer&) = delete;
    auto operator=(const Layer&) -> Layer& = delete;

    virtual void onAttach()
    {
    }
    virtual void onDetach()
    {
    }
    /** @param deltaSeconds Time since the previous frame. */
    virtual void onUpdate([[maybe_unused]] float deltaSeconds)
    {
    }
    virtual void onUIRender()
    {
    }
};

}  // namespace starcore
