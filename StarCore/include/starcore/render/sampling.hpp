#pragma once
// Sampling and BSDF building blocks of the path tracer, written in a
// shader-like style (plain functions on vectors) so they port 1:1 to GLSL.

#include <algorithm>
#include <cmath>

#include <glm/geometric.hpp>

#include "starcore/math/types.hpp"

namespace starcore::sampling
{

/** @brief Orthonormal basis with `normal` as its z axis. */
struct Frame
{
    Vec3 tangent;
    Vec3 bitangent;
    Vec3 normal;

    [[nodiscard]] auto toLocal(Vec3 v) const -> Vec3
    {
        return {glm::dot(v, tangent), glm::dot(v, bitangent), glm::dot(v, normal)};
    }

    [[nodiscard]] auto toWorld(Vec3 v) const -> Vec3
    {
        return v.x * tangent + v.y * bitangent + v.z * normal;
    }
};

/** @brief Branchless basis around a unit vector (Duff et al., JCGT 2017). */
[[nodiscard]] inline auto makeFrame(Vec3 n) -> Frame
{
    const float sign = std::copysign(1.0F, n.z);
    const float a = -1.0F / (sign + n.z);
    const float b = n.x * n.y * a;
    return {Vec3(1.0F + sign * n.x * n.x * a, sign * b, -sign * n.x),
            Vec3(b, sign + n.y * n.y * a, -n.y), n};
}

[[nodiscard]] inline auto fresnelSchlick(Vec3 f0, float cosTheta) -> Vec3
{
    const float m = std::clamp(1.0F - cosTheta, 0.0F, 1.0F);
    const float m2 = m * m;
    return f0 + (Vec3(1.0F) - f0) * (m2 * m2 * m);
}

/**
 * @brief Unpolarized Fresnel reflectance of a dielectric interface.
 * @param cosThetaI Cosine between the incident direction and the normal (>= 0).
 * @param eta Ratio n_incident / n_transmitted.
 * @return 1 on total internal reflection.
 */
[[nodiscard]] inline auto fresnelDielectric(float cosThetaI, float eta) -> float
{
    cosThetaI = std::clamp(cosThetaI, 0.0F, 1.0F);
    const float sin2T = eta * eta * (1.0F - cosThetaI * cosThetaI);
    if (sin2T >= 1.0F) {
        return 1.0F;
    }
    const float cosThetaT = std::sqrt(1.0F - sin2T);
    const float rs = (eta * cosThetaI - cosThetaT) / (eta * cosThetaI + cosThetaT);
    const float rp = (cosThetaI - eta * cosThetaT) / (cosThetaI + eta * cosThetaT);
    return 0.5F * (rs * rs + rp * rp);
}

/** @brief Smith Lambda of GGX for a direction in the local frame (z = normal). */
[[nodiscard]] inline auto smithLambda(Vec3 v, float alpha) -> float
{
    const float z2 = v.z * v.z;
    if (z2 <= 0.0F) {
        return 1e30F;
    }
    const float tan2 = (v.x * v.x + v.y * v.y) / z2;
    return 0.5F * (-1.0F + std::sqrt(1.0F + alpha * alpha * tan2));
}

/** @brief GGX distribution of microfacet normals D(h), h in the local frame. */
[[nodiscard]] inline auto ggxDistribution(Vec3 h, float alpha) -> float
{
    const float a2 = alpha * alpha;
    const float d = h.z * h.z * (a2 - 1.0F) + 1.0F;
    return a2 / (kPi * d * d);
}

/** @brief Density of sampleGgxVisibleNormal: G1(wo) max(0, wo.h) D(h) / wo.z. */
[[nodiscard]] inline auto ggxVisibleNormalPdf(Vec3 wo, Vec3 h, float alpha) -> float
{
    const float cosine = glm::dot(wo, h);
    if (cosine <= 0.0F || h.z <= 0.0F) {
        return 0.0F;
    }
    return cosine * ggxDistribution(h, alpha) / ((1.0F + smithLambda(wo, alpha)) * wo.z);
}

/** @brief Weight G2 / G1(wo) of a VNDF-sampled GGX direction (height-correlated Smith). */
[[nodiscard]] inline auto smithWeight(Vec3 wo, Vec3 wi, float alpha) -> float
{
    const float lambdaO = smithLambda(wo, alpha);
    const float lambdaI = smithLambda(wi, alpha);
    return (1.0F + lambdaO) / (1.0F + lambdaO + lambdaI);
}

/**
 * @brief Microfacet normal sampled from the GGX distribution of normals
 *        visible from `wo` (Heitz, JCGT 2018). `wo` is in the local frame
 *        with wo.z > 0; u1, u2 in [0, 1).
 */
[[nodiscard]] inline auto sampleGgxVisibleNormal(Vec3 wo, float alpha, float u1, float u2)
    -> Vec3
{
    // Stretch the view direction to the hemisphere configuration.
    const Vec3 vh = glm::normalize(Vec3(alpha * wo.x, alpha * wo.y, wo.z));
    const float lengthSquared = vh.x * vh.x + vh.y * vh.y;
    const Vec3 t1 = lengthSquared > 0.0F ? Vec3(-vh.y, vh.x, 0.0F) / std::sqrt(lengthSquared)
                                         : Vec3(1.0F, 0.0F, 0.0F);
    const Vec3 t2 = glm::cross(vh, t1);
    // Sample the projected area.
    const float r = std::sqrt(u1);
    const float phi = kTwoPi * u2;
    const float p1 = r * std::cos(phi);
    float p2 = r * std::sin(phi);
    const float s = 0.5F * (1.0F + vh.z);
    p2 = (1.0F - s) * std::sqrt(std::max(0.0F, 1.0F - p1 * p1)) + s * p2;
    const Vec3 nh = p1 * t1 + p2 * t2 +
                    std::sqrt(std::max(0.0F, 1.0F - p1 * p1 - p2 * p2)) * vh;
    // Unstretch.
    return glm::normalize(Vec3(alpha * nh.x, alpha * nh.y, std::max(1e-6F, nh.z)));
}

/** @brief GGX alpha from perceptual roughness (kept away from 0 for stability). */
[[nodiscard]] inline auto roughnessToAlpha(float roughness) -> float
{
    const float clamped = std::clamp(roughness, 0.0F, 1.0F);
    return std::max(clamped * clamped, 1e-3F);
}

/**
 * @brief Refract `incident` (pointing towards the surface) through a normal
 *        `n` on the incident side; false on total internal reflection.
 */
[[nodiscard]] inline auto refract(Vec3 incident, Vec3 n, float eta, Vec3& transmitted) -> bool
{
    const float cosI = -glm::dot(incident, n);
    const float sin2T = eta * eta * (1.0F - cosI * cosI);
    if (sin2T >= 1.0F) {
        return false;
    }
    transmitted = eta * incident + (eta * cosI - std::sqrt(1.0F - sin2T)) * n;
    return true;
}

/**
 * @brief 1 - cos(theta_max) of the cone subtended by a sphere of `radius`
 *        at squared distance `distance2` (stable for small/far spheres).
 */
[[nodiscard]] inline auto coneOneMinusCos(float radius, float distance2) -> float
{
    const float sin2 = std::min(1.0F, radius * radius / distance2);
    const float cosMax = std::sqrt(1.0F - sin2);
    return sin2 / (1.0F + cosMax);
}

/** @brief Uniform direction in a cone around `axis` (unit); pdf = 1 / (2 pi oneMinusCos). */
[[nodiscard]] inline auto sampleCone(Vec3 axis, float oneMinusCos, float u1, float u2) -> Vec3
{
    const float cosTheta = 1.0F - u1 * oneMinusCos;
    const float sinTheta = std::sqrt(std::max(0.0F, 1.0F - cosTheta * cosTheta));
    const float phi = kTwoPi * u2;
    return makeFrame(axis).toWorld(
        Vec3(std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta));
}

[[nodiscard]] inline auto conePdf(float oneMinusCos) -> float
{
    return 1.0F / (kTwoPi * oneMinusCos);
}

/** @brief Power heuristic (beta = 2) weight of strategy `a`. */
[[nodiscard]] inline auto powerHeuristic(float pdfA, float pdfB) -> float
{
    const float a2 = pdfA * pdfA;
    const float b2 = pdfB * pdfB;
    return a2 + b2 > 0.0F ? a2 / (a2 + b2) : 0.0F;
}

[[nodiscard]] inline auto luminance(Vec3 color) -> float
{
    return glm::dot(color, Vec3(0.2126F, 0.7152F, 0.0722F));
}

}  // namespace starcore::sampling
