#pragma once

#include "starcore/math/types.hpp"

namespace starcore
{

/** @brief Translation * Rotation * Scale. */
[[nodiscard]] auto composeTransform(Vec3 translation, Quat rotation,
                                    Vec3 scale) -> Mat4;

struct DecomposedTransform
{
    Vec3 translation{0.0F};
    Quat rotation{1.0F, 0.0F, 0.0F, 0.0F};
    Vec3 scale{1.0F};
};

/**
 * @brief Split an affine matrix (no shear/perspective) into T, R, S.
 *
 * Fixes the legacy ChildEntities code, which ran quat_cast on a scaled matrix.
 */
[[nodiscard]] auto decomposeTransform(const Mat4& matrix)
    -> DecomposedTransform;

/**
 * @brief Shortest rotation taking direction `from` onto `to` (neither needs to
 *        be normalized); handles (anti)parallel input. Returns identity for
 *        zero-length input.
 *
 * Replaces legacy GetRotationQuat, which returned identity whenever
 * `dest.x == 0 && dest.y == 0` (e.g. for any direction along Z).
 */
[[nodiscard]] auto rotationBetween(Vec3 from, Vec3 to) -> Quat;

}  // namespace starcore
