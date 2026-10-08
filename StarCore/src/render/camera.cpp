module starcore.render;

import std;
import glm;
import starcore.math;

namespace starcore
{

namespace
{

constexpr Vec3 kWorldUp{0.0F, 1.0F, 0.0F};
constexpr float kMaxPitch = 89.0F;

}  // namespace

Camera::Camera(float verticalFovDegrees, float nearClip, float farClip)
    : m_verticalFovDegrees(verticalFovDegrees), m_nearClip(nearClip),
      m_farClip(farClip)
{
    updateView();
    updateProjection();
}

auto Camera::aspectRatio() const -> float
{
    return static_cast<float>(m_viewport.x) / static_cast<float>(m_viewport.y);
}

void Camera::setViewport(std::uint32_t width, std::uint32_t height)
{
    const UVec2 viewport{std::max(1U, width), std::max(1U, height)};
    if (viewport == m_viewport) {
        return;
    }
    m_viewport = viewport;
    updateProjection();
}

void Camera::setPosition(Vec3 position)
{
    m_position = position;
    updateView();
}

void Camera::setRotation(float yawDegrees, float pitchDegrees)
{
    m_yawDegrees = std::remainder(yawDegrees, 360.0F);
    m_pitchDegrees = std::clamp(pitchDegrees, -kMaxPitch, kMaxPitch);
    updateView();
}

void Camera::lookAt(Vec3 target)
{
    const Vec3 direction = target - m_position;
    if (glm::dot(direction, direction) < 1e-12F) {
        return;
    }
    const Vec3 d = glm::normalize(direction);
    setRotation(glm::degrees(std::atan2(d.z, d.x)),
                glm::degrees(std::asin(std::clamp(d.y, -1.0F, 1.0F))));
}

void Camera::setVerticalFov(float degrees)
{
    m_verticalFovDegrees = std::clamp(degrees, 1.0F, 179.0F);
    updateProjection();
}

void Camera::updateView()
{
    const float yaw = glm::radians(m_yawDegrees);
    const float pitch = glm::radians(m_pitchDegrees);
    m_forward = glm::normalize(Vec3{std::cos(pitch) * std::cos(yaw),
                                    std::sin(pitch),
                                    std::cos(pitch) * std::sin(yaw)});
    m_right = glm::normalize(glm::cross(m_forward, kWorldUp));
    m_up = glm::cross(m_right, m_forward);

    m_view = glm::lookAt(m_position, m_position + m_forward, kWorldUp);
    m_inverseView = glm::inverse(m_view);
    m_viewProjection = m_projection * m_view;
}

void Camera::updateProjection()
{
    // GLM_FORCE_DEPTH_ZERO_TO_ONE: clip depth in [0, 1] (Vulkan convention).
    m_projection = glm::perspective(glm::radians(m_verticalFovDegrees),
                                    aspectRatio(), m_nearClip, m_farClip);
    m_tanHalfFov = std::tan(glm::radians(m_verticalFovDegrees) * 0.5F);
    m_viewProjection = m_projection * m_view;
}

auto Camera::pixelDirection(Vec2 pixelPosition) const -> Vec3
{
    // NDC with +y up; pixel space has +y down.
    const float ndcX = 2.0F * pixelPosition.x / static_cast<float>(m_viewport.x) - 1.0F;
    const float ndcY = 1.0F - 2.0F * pixelPosition.y / static_cast<float>(m_viewport.y);
    return m_forward + m_right * (ndcX * aspectRatio() * m_tanHalfFov) +
           m_up * (ndcY * m_tanHalfFov);
}

auto Camera::generateRay(Vec2 pixelPosition) const -> Ray
{
    return {m_position, glm::normalize(pixelDirection(pixelPosition))};
}

auto Camera::worldToScreen(Vec3 world) const -> std::optional<Vec2>
{
    if (depthOf(world) < m_nearClip * 0.999F) {
        return std::nullopt;
    }
    const Vec4 clip = m_viewProjection * Vec4(world, 1.0F);
    const Vec2 ndc = Vec2(clip) / clip.w;
    return Vec2{(ndc.x + 1.0F) * 0.5F * static_cast<float>(m_viewport.x),
                (1.0F - ndc.y) * 0.5F * static_cast<float>(m_viewport.y)};
}

auto Camera::screenToWorld(Vec2 pixelPosition, float depth) const -> Vec3
{
    // pixelDirection has unit length along forward, so this lands on the
    // plane at `depth` in front of the camera.
    return m_position + pixelDirection(pixelPosition) * depth;
}

auto Camera::depthOf(Vec3 world) const -> float
{
    return glm::dot(world - m_position, m_forward);
}

auto FlyCameraController::update(Camera& camera, const CameraInput& input,
                                 float deltaSeconds) -> bool
{
    if (!input.look) {
        m_speed = baseSpeed;
        return false;
    }

    bool changed = false;
    if (input.mouseDelta.x != 0.0F || input.mouseDelta.y != 0.0F) {
        camera.setRotation(
            camera.yaw() + input.mouseDelta.x * lookSensitivity,
            camera.pitch() - input.mouseDelta.y * lookSensitivity);
        changed = true;
    }

    Vec3 direction{0.0F};
    direction += camera.forward() * (static_cast<float>(input.forward) -
                                     static_cast<float>(input.backward));
    direction += camera.right() * (static_cast<float>(input.right) -
                                   static_cast<float>(input.left));
    direction += kWorldUp * (static_cast<float>(input.up) -
                             static_cast<float>(input.down));

    if (glm::dot(direction, direction) > 0.0F) {
        camera.setPosition(camera.position() +
                           glm::normalize(direction) * m_speed * deltaSeconds);
        m_speed = std::min(maxSpeed, m_speed + acceleration * deltaSeconds);
        changed = true;
    } else {
        m_speed = baseSpeed;
    }
    return changed;
}

}  // namespace starcore
