#pragma once
// Space dust: faint specks hanging still in space around the camera, the
// classic cue for how fast you fly (with motion blur they streak past).

#include <cstdint>
#include <vector>

#include <starcore/math/random.hpp>
#include <starcore/math/types.hpp>
#include <starcore/render/particles.hpp>

namespace startraveler
{

class SpaceDust
{
public:
    explicit SpaceDust(std::uint32_t count = 800, float extent = 40.0F, std::uint32_t seed = 7);

    /**
     * @brief Keep the specks in a cube of `extent` around `center`: those
     *        left behind reappear on the opposite side.
     */
    void update(const starcore::Vec3& center);

    /**
     * @brief The specks, fading out near the center (inside the cockpit) and
     *        towards the cube's faces (where they wrap).
     */
    void appendParticles(std::vector<starcore::RenderParticle>& particles,
                         const starcore::Vec3& center) const;

private:
    std::vector<starcore::Vec3> m_positions;
    float m_extent;
    starcore::Rng m_rng;
    bool m_placed = false;
};

}  // namespace startraveler
