// Shared shader math: random numbers, sampling, BSDF building blocks and color
// conversions. Ports of starcore/math/random.hpp, starcore/render/sampling.hpp
// and starcore/math/color.hpp -- keep them in sync.

#ifndef STARCORE_COMMON_GLSL
#define STARCORE_COMMON_GLSL

const float kPi = 3.14159265358979323846;
const float kTwoPi = 6.28318530717958647692;
const float kFar = 1e30;
const uint kNoEntityId = 0xFFFFFFFFu;

// Random numbers ----------------------------------------------------------------------------------

/** PCG hash (RXS-M-XS 32), Jarzynski & Olano, JCGT 2020. */
uint pcgHash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

/** State of Rng::forPixel(x, y, frame, seed). */
uint rngForPixel(uint x, uint y, uint frame, uint seed)
{
    return pcgHash(pcgHash(x + pcgHash(y + pcgHash(frame + pcgHash(seed)))));
}

uint nextUint(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

/** Uniform float in [0, 1). */
float nextFloat(inout uint state)
{
    return float(nextUint(state) >> 8u) * (1.0 / 16777216.0);
}

vec3 onUnitSphere(inout uint state)
{
    float z = 1.0 - 2.0 * nextFloat(state);
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = kTwoPi * nextFloat(state);
    return vec3(r * cos(phi), r * sin(phi), z);
}

/** Cosine-weighted direction around a unit normal; pdf = cos / pi. */
vec3 cosineHemisphere(vec3 normal, inout uint state)
{
    vec3 direction = normal + onUnitSphere(state);
    float length2 = dot(direction, direction);
    return length2 > 1e-12 ? direction / sqrt(length2) : normal;
}

// Frames and BSDF helpers -------------------------------------------------------------------------

struct Frame
{
    vec3 tangent;
    vec3 bitangent;
    vec3 normal;
};

/** Branchless basis around a unit vector (Duff et al., JCGT 2017). */
Frame makeFrame(vec3 n)
{
    float s = n.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + n.z);
    float b = n.x * n.y * a;
    Frame frame;
    frame.tangent = vec3(1.0 + s * n.x * n.x * a, s * b, -s * n.x);
    frame.bitangent = vec3(b, s + n.y * n.y * a, -n.y);
    frame.normal = n;
    return frame;
}

vec3 toLocal(Frame frame, vec3 v)
{
    return vec3(dot(v, frame.tangent), dot(v, frame.bitangent), dot(v, frame.normal));
}

vec3 toWorld(Frame frame, vec3 v)
{
    return v.x * frame.tangent + v.y * frame.bitangent + v.z * frame.normal;
}

vec3 fresnelSchlick(vec3 f0, float cosTheta)
{
    float m = clamp(1.0 - cosTheta, 0.0, 1.0);
    float m2 = m * m;
    return f0 + (vec3(1.0) - f0) * (m2 * m2 * m);
}

/** Unpolarized dielectric Fresnel reflectance; eta = n_incident / n_transmitted; 1 on TIR. */
float fresnelDielectric(float cosThetaI, float eta)
{
    cosThetaI = clamp(cosThetaI, 0.0, 1.0);
    float sin2T = eta * eta * (1.0 - cosThetaI * cosThetaI);
    if (sin2T >= 1.0) {
        return 1.0;
    }
    float cosThetaT = sqrt(1.0 - sin2T);
    float rs = (eta * cosThetaI - cosThetaT) / (eta * cosThetaI + cosThetaT);
    float rp = (cosThetaI - eta * cosThetaT) / (cosThetaI + eta * cosThetaT);
    return 0.5 * (rs * rs + rp * rp);
}

/** Smith Lambda of GGX for a local direction (z = normal). */
float smithLambda(vec3 v, float alpha)
{
    float z2 = v.z * v.z;
    if (z2 <= 0.0) {
        return 1e30;
    }
    float tan2 = (v.x * v.x + v.y * v.y) / z2;
    return 0.5 * (-1.0 + sqrt(1.0 + alpha * alpha * tan2));
}

/** GGX distribution of microfacet normals D(h), h local. */
float ggxDistribution(vec3 h, float alpha)
{
    float a2 = alpha * alpha;
    float d = h.z * h.z * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

/** Density of sampleGgxVisibleNormal: G1(wo) max(0, wo.h) D(h) / wo.z. */
float ggxVisibleNormalPdf(vec3 wo, vec3 h, float alpha)
{
    float cosine = dot(wo, h);
    if (cosine <= 0.0 || h.z <= 0.0) {
        return 0.0;
    }
    return cosine * ggxDistribution(h, alpha) / ((1.0 + smithLambda(wo, alpha)) * wo.z);
}

/** GGX visible normal sample (Heitz, JCGT 2018); wo local with wo.z > 0. */
vec3 sampleGgxVisibleNormal(vec3 wo, float alpha, float u1, float u2)
{
    vec3 vh = normalize(vec3(alpha * wo.x, alpha * wo.y, wo.z));
    float lengthSquared = vh.x * vh.x + vh.y * vh.y;
    vec3 t1 = lengthSquared > 0.0 ? vec3(-vh.y, vh.x, 0.0) / sqrt(lengthSquared)
                                  : vec3(1.0, 0.0, 0.0);
    vec3 t2 = cross(vh, t1);
    float r = sqrt(u1);
    float phi = kTwoPi * u2;
    float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    float s = 0.5 * (1.0 + vh.z);
    p2 = (1.0 - s) * sqrt(max(0.0, 1.0 - p1 * p1)) + s * p2;
    vec3 nh = p1 * t1 + p2 * t2 + sqrt(max(0.0, 1.0 - p1 * p1 - p2 * p2)) * vh;
    return normalize(vec3(alpha * nh.x, alpha * nh.y, max(1e-6, nh.z)));
}

float roughnessToAlpha(float roughness)
{
    float clamped = clamp(roughness, 0.0, 1.0);
    return max(clamped * clamped, 1e-3);
}

/** Refract `incident` (towards the surface) through normal `n` on its side; false on TIR. */
bool refractDirection(vec3 incident, vec3 n, float eta, out vec3 transmitted)
{
    float cosI = -dot(incident, n);
    float sin2T = eta * eta * (1.0 - cosI * cosI);
    transmitted = vec3(0.0);
    if (sin2T >= 1.0) {
        return false;
    }
    transmitted = eta * incident + (eta * cosI - sqrt(1.0 - sin2T)) * n;
    return true;
}

/** 1 - cos(theta_max) of the cone subtended by a sphere. */
float coneOneMinusCos(float radius, float distance2)
{
    float sin2 = min(1.0, radius * radius / distance2);
    float cosMax = sqrt(1.0 - sin2);
    return sin2 / (1.0 + cosMax);
}

vec3 sampleCone(vec3 axis, float oneMinusCos, float u1, float u2)
{
    float cosTheta = 1.0 - u1 * oneMinusCos;
    float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    float phi = kTwoPi * u2;
    return toWorld(makeFrame(axis), vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
}

float conePdf(float oneMinusCos)
{
    return 1.0 / (kTwoPi * oneMinusCos);
}

float powerHeuristic(float pdfA, float pdfB)
{
    float a2 = pdfA * pdfA;
    float b2 = pdfB * pdfB;
    return a2 + b2 > 0.0 ? a2 / (a2 + b2) : 0.0;
}

float maxComponent(vec3 v)
{
    return max(v.r, max(v.g, v.b));
}

// Color -------------------------------------------------------------------------------------------

const uint kToneMapperNone = 0u;
const uint kToneMapperReinhard = 1u;
const uint kToneMapperAces = 2u;
const uint kToneMapperAgx = 3u;
const uint kToneMapperAgxPunchy = 4u;

/** AgX (math/color.hpp's detail::agx): linear Rec. 709 to linear display values. */
vec3 agx(vec3 color, bool punchy)
{
    const mat3 inset = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                            0.0784335999999992, 0.878468636469772, 0.0784336,
                            0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 outset = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                             -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                             -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float kMinEv = -12.47393;
    const float kMaxEv = 4.026069;
    vec3 x = inset * max(color, vec3(0.0));
    x = (clamp(log2(max(x, vec3(1e-10))), kMinEv, kMaxEv) - kMinEv) / (kMaxEv - kMinEv);
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    x = 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x + 0.4298 * x2 +
        0.1191 * x - 0.00232;
    if (punchy) {
        float luma = dot(x, vec3(0.2126, 0.7152, 0.0722));
        x = pow(max(x, vec3(0.0)), vec3(1.35));
        x = vec3(luma) + 1.4 * (x - vec3(luma));
    }
    x = outset * x;
    return pow(clamp(x, 0.0, 1.0), vec3(2.2));
}

vec3 toneMap(vec3 color, uint mapper)
{
    if (mapper == kToneMapperReinhard) {
        return color / (vec3(1.0) + color);
    }
    if (mapper == kToneMapperAces) {
        const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
        return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
    }
    if (mapper == kToneMapperAgx || mapper == kToneMapperAgxPunchy) {
        return agx(color, mapper == kToneMapperAgxPunchy);
    }
    return clamp(color, 0.0, 1.0);
}

float linearToSrgb(float linear)
{
    return linear <= 0.0031308 ? 12.92 * linear : 1.055 * pow(linear, 1.0 / 2.4) - 0.055;
}

vec3 linearToSrgb(vec3 linear)
{
    return vec3(linearToSrgb(linear.r), linearToSrgb(linear.g), linearToSrgb(linear.b));
}

#endif  // STARCORE_COMMON_GLSL
