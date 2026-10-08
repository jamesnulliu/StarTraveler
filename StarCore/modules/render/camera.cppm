export module starcore.render:camera;

import std;
import starcore.math;

export namespace starcore
{

/**
 * @brief Perspective camera (right-handed, +Y up, yaw/pitch, no roll).
 *
 * Screen/pixel space: origin at the top-left corner, +y down; the center of
 * pixel (x, y) is (x + 0.5, y + 0.5). Default: at (0, 0, 10) looking down -Z
 * (same as the legacy camera).
 */
class Camera
{
public:
    explicit Camera(float verticalFovDegrees = 45.0F, float nearClip = 0.1F,
                    float farClip = 1000.0F);

    void setViewport(std::uint32_t width, std::uint32_t height);
    void setPosition(Vec3 position);
    /** @param pitchDegrees Clamped to [-89, 89]. */
    void setRotation(float yawDegrees, float pitchDegrees);
    void lookAt(Vec3 target);
    void setVerticalFov(float degrees);

    [[nodiscard]] auto position() const -> Vec3 { return m_position; }
    [[nodiscard]] auto forward() const -> Vec3 { return m_forward; }
    [[nodiscard]] auto right() const -> Vec3 { return m_right; }
    [[nodiscard]] auto up() const -> Vec3 { return m_up; }
    [[nodiscard]] auto yaw() const -> float { return m_yawDegrees; }
    [[nodiscard]] auto pitch() const -> float { return m_pitchDegrees; }
    [[nodiscard]] auto verticalFov() const -> float { return m_verticalFovDegrees; }
    [[nodiscard]] auto nearClip() const -> float { return m_nearClip; }
    [[nodiscard]] auto farClip() const -> float { return m_farClip; }
    [[nodiscard]] auto viewportSize() const -> UVec2 { return m_viewport; }
    [[nodiscard]] auto aspectRatio() const -> float;

    [[nodiscard]] auto view() const -> const Mat4& { return m_view; }
    [[nodiscard]] auto projection() const -> const Mat4& { return m_projection; }
    [[nodiscard]] auto inverseView() const -> const Mat4& { return m_inverseView; }

    /** @brief Primary ray through a (continuous) pixel position. */
    [[nodiscard]] auto generateRay(Vec2 pixelPosition) const -> Ray;

    /**
     * @brief Project a world point to pixel space.
     * @return nullopt if the point is not in front of the near plane. Points
     *         outside the viewport are still returned (needed for clipping).
     */
    [[nodiscard]] auto worldToScreen(Vec3 world) const -> std::optional<Vec2>;

    /**
     * @brief World point under a pixel at the given depth along forward().
     *
     * The legacy ScreenToWorld fed the distance in as an NDC z value, so
     * dragged control points ended up at the wrong depth.
     */
    [[nodiscard]] auto screenToWorld(Vec2 pixelPosition, float depth) const
        -> Vec3;

    /** @brief Signed distance of a world point along forward(). */
    [[nodiscard]] auto depthOf(Vec3 world) const -> float;

private:
    /** @brief Unnormalized direction through a pixel, with depth 1. */
    [[nodiscard]] auto pixelDirection(Vec2 pixelPosition) const -> Vec3;
    void updateView();
    void updateProjection();

    float m_verticalFovDegrees;
    float m_nearClip;
    float m_farClip;
    UVec2 m_viewport{1, 1};

    Vec3 m_position{0.0F, 0.0F, 10.0F};
    float m_yawDegrees = -90.0F;
    float m_pitchDegrees = 0.0F;
    Vec3 m_forward{0.0F, 0.0F, -1.0F};
    Vec3 m_right{1.0F, 0.0F, 0.0F};
    Vec3 m_up{0.0F, 1.0F, 0.0F};

    Mat4 m_view{1.0F};
    Mat4 m_inverseView{1.0F};
    Mat4 m_projection{1.0F};
    Mat4 m_viewProjection{1.0F};
    float m_tanHalfFov = 0.0F;
};

/** @brief Per-frame input for FlyCameraController (decoupled from GLFW). */
struct CameraInput
{
    bool forward = false;
    bool backward = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
    // Mouse look/movement are active only while this is held (e.g. RMB).
    bool look = false;
    Vec2 mouseDelta{0.0F};  // Pixels since the last update.
};

/**
 * @brief Free-flying camera controller.
 *
 * Fixes of the legacy Camera::OnUpdate: movement keys combine (diagonals
 * work), speed ramps up per second instead of per frame, and rotation uses
 * yaw/pitch instead of accumulating quaternion cross products.
 */
class FlyCameraController
{
public:
    float baseSpeed = 2.0F;        // Units per second (legacy start speed).
    float maxSpeed = 30.0F;        // Legacy cap.
    float acceleration = 12.0F;    // Units per second^2 while moving.
    float lookSensitivity = 0.15F; // Degrees per pixel.

    /** @return true if the camera moved or rotated. */
    auto update(Camera& camera, const CameraInput& input, float deltaSeconds)
        -> bool;

    [[nodiscard]] auto speed() const -> float
    {
        return m_speed;
    }

private:
    float m_speed = 2.0F;
};

}  // namespace starcore
