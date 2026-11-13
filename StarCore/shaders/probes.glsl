// Irradiance probes (DDGI: Majercik et al., "Dynamic Diffuse Global
// Illumination with Ray-Traced Irradiance Fields", JCGT 2019), see
// src/gpu/probe_field.hpp: grids of probes (volumes), each probe with two
// octahedral maps over the directions around it:
//   - irradiance: the mean light arriving from around a direction, weighted by
//     the cosine (irradiance / pi): what a diffuse surface facing that way
//     reflects, times its albedo. The light of other surfaces only: what next
//     event estimation finds (the lights) is left out, the surface samples it
//     itself;
//   - distance: the mean distance to the surfaces in a direction and its
//     square, to tell whether a probe sees a point (Chebyshev's inequality):
//     probes behind a wall must not light it.
// Each map has a border of one texel copying the opposite edge (octahedral
// wrap), so bilinear lookups need no wrapping. All in buffers, their addresses in
// the frame uniforms (probeBuffers, probeMaps): a probe's map is a tile of texels
// row by row, the probes' tiles one after the other.
//
// Include after hybrid.glsl. The including shader defines PROBE_ACCESS (empty)
// if it updates the probes (readonly otherwise).

#ifndef STARCORE_PROBES_GLSL
#define STARCORE_PROBES_GLSL

#ifndef PROBE_ACCESS
#define PROBE_ACCESS readonly
#endif

// Texels per side of a probe's maps, without the border, and with it.
const int kIrradianceTexels = 6;
const int kDistanceTexels = 14;
const int kIrradianceTile = kIrradianceTexels + 2;
const int kDistanceTile = kDistanceTexels + 2;
// Probes seeing more back faces than this are inside something: unused.
const float kProbeMaxBackfaces = 0.25;

/** gpu::ProbeVolumeRecord (192 bytes). */
struct ProbeVolume
{
    vec4 worldToGrid[3];  // Rows (affine): probes at integer grid coordinates.
    vec4 gridToWorld[3];
    vec4 axes[3];         // Rows: grid (local) directions to world ones, orthonormal.
    ivec4 probes;         // xyz: per axis; w: index of its first probe.
    ivec4 origin;         // xyz: grid cell of the probes at (0, 0, 0) (scrolling); w: -
    vec4 bias;            // x: along the normal, y: towards the viewer, z: max ray distance
};

/** Per probe: the grid cell it holds (w: updates since then, 0: none) and x: back faces seen. */
struct ProbeState
{
    ivec4 cell;
    vec4 info;
};

// gpu::ProbeHeader (80 bytes), then the volumes, most detailed first.
layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer ProbeVolumes
{
    uvec4 info;         // x: volumes, y: rays per probe, z: probes, w: -
    vec4 rotation[3];   // Rows: this frame's random rotation of the rays.
    vec4 blend;         // x: hysteresis
    ProbeVolume volumes[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) PROBE_ACCESS buffer ProbeStates
{
    ProbeState states[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) PROBE_ACCESS buffer ProbeIrradiance
{
    vec4 irradiance[];  // rgb; a: -
};
layout(buffer_reference, std430, buffer_reference_align = 8) PROBE_ACCESS buffer ProbeDistance
{
    vec2 distances[];  // Mean distance, mean squared distance.
};

ProbeVolumes probeVolumes()
{
    return ProbeVolumes(frameData.probeBuffers.xy);
}

ProbeStates probeStates()
{
    return ProbeStates(frameData.probeBuffers.zw);
}

ProbeIrradiance probeIrradiance()
{
    return ProbeIrradiance(frameData.probeMaps.xy);
}

ProbeDistance probeDistance()
{
    return ProbeDistance(frameData.probeMaps.zw);
}

/** Index of a texel (border included) of a probe's irradiance map. */
uint irradianceTexel(uint probe, ivec2 texel)
{
    return probe * uint(kIrradianceTile * kIrradianceTile) +
           uint(texel.y * kIrradianceTile + texel.x);
}

/** Index of a texel (border included) of a probe's distance map. */
uint distanceTexel(uint probe, ivec2 texel)
{
    return probe * uint(kDistanceTile * kDistanceTile) + uint(texel.y * kDistanceTile + texel.x);
}

ivec3 positiveMod(ivec3 a, ivec3 b)
{
    return ((a % b) + b) % b;
}

/** Index of the probe at grid coordinates `grid` of a volume. */
uint probeIndex(ProbeVolume volume, ivec3 grid)
{
    ivec3 slot = positiveMod(grid + volume.origin.xyz, volume.probes.xyz);
    return uint(volume.probes.w + slot.x +
                volume.probes.x * (slot.y + volume.probes.y * slot.z));
}

/** The volume of a probe and its grid coordinates (false: no probe). */
bool probeOf(uint probe, out uint volume, out ivec3 grid)
{
    volume = 0u;
    grid = ivec3(0);
    ProbeVolumes data = probeVolumes();
    for (uint i = 0u; i < data.info.x; ++i) {
        ivec4 probes = data.volumes[i].probes;
        int local = int(probe) - probes.w;
        if (local >= 0 && local < probes.x * probes.y * probes.z) {
            ivec3 slot = ivec3(local % probes.x, (local / probes.x) % probes.y,
                               local / (probes.x * probes.y));
            volume = i;
            grid = positiveMod(slot - data.volumes[i].origin.xyz, probes.xyz);
            return true;
        }
    }
    return false;
}

vec3 toProbeLocal(ProbeVolume volume, vec3 world)
{
    return volume.axes[0].xyz * world.x + volume.axes[1].xyz * world.y +
           volume.axes[2].xyz * world.z;
}

vec3 toProbeWorld(ProbeVolume volume, vec3 local)
{
    return vec3(dot(volume.axes[0].xyz, local), dot(volume.axes[1].xyz, local),
                dot(volume.axes[2].xyz, local));
}

/** Direction of ray `ray` of every probe this frame (local): a rotated spherical Fibonacci set. */
vec3 probeRayDirection(uint ray)
{
    ProbeVolumes data = probeVolumes();
    float n = float(data.info.y);
    float i = float(ray);
    float phi = kTwoPi * fract(i * 0.61803398875);
    float cosTheta = 1.0 - (2.0 * i + 1.0) / n;
    float sinTheta = sqrt(clamp(1.0 - cosTheta * cosTheta, 0.0, 1.0));
    vec3 d = vec3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    return vec3(dot(data.rotation[0].xyz, d), dot(data.rotation[1].xyz, d),
                dot(data.rotation[2].xyz, d));
}

/**
 * The texel of a map's interior a texel of its tile (border included) holds:
 * itself, or for the border the one across the octahedron's edge.
 */
ivec2 probeBorderSource(ivec2 texel, int texels)
{
    int last = texels + 1;
    bool left = texel.x == 0;
    bool right = texel.x == last;
    bool top = texel.y == 0;
    bool bottom = texel.y == last;
    if ((left || right) && (top || bottom)) {
        return ivec2(left ? texels : 1, top ? texels : 1);
    }
    if (top || bottom) {
        return ivec2(last - texel.x, top ? 1 : texels);
    }
    if (left || right) {
        return ivec2(left ? 1 : texels, last - texel.y);
    }
    return texel;
}

/** The direction of an interior texel (1..texels) of a map. */
vec3 probeTexelDirection(ivec2 texel, int texels)
{
    return fromOctahedral((vec2(texel - 1) + 0.5) / float(texels) * 2.0 - 1.0);
}

/** Bilinear lookup position in a probe's tile: base texel (border included) and weights. */
void probeLookup(vec3 direction, int texels, out ivec2 base, out vec2 f)
{
    vec2 position = (octahedral(direction) * 0.5 + 0.5) * float(texels) + 0.5;
    base = ivec2(floor(position));
    f = position - vec2(base);
    base = clamp(base, ivec2(0), ivec2(texels));
}

vec3 probeIrradianceAt(uint probe, vec3 direction)
{
    ivec2 base;
    vec2 f;
    probeLookup(direction, kIrradianceTexels, base, f);
    ProbeIrradiance maps = probeIrradiance();
    vec3 a = maps.irradiance[irradianceTexel(probe, base)].rgb;
    vec3 b = maps.irradiance[irradianceTexel(probe, base + ivec2(1, 0))].rgb;
    vec3 c = maps.irradiance[irradianceTexel(probe, base + ivec2(0, 1))].rgb;
    vec3 d = maps.irradiance[irradianceTexel(probe, base + ivec2(1, 1))].rgb;
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

vec2 probeDistanceAt(uint probe, vec3 direction)
{
    ivec2 base;
    vec2 f;
    probeLookup(direction, kDistanceTexels, base, f);
    ProbeDistance maps = probeDistance();
    vec2 a = maps.distances[distanceTexel(probe, base)];
    vec2 b = maps.distances[distanceTexel(probe, base + ivec2(1, 0))];
    vec2 c = maps.distances[distanceTexel(probe, base + ivec2(0, 1))];
    vec2 d = maps.distances[distanceTexel(probe, base + ivec2(1, 1))];
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

/** Whether a probe holds the light of its cell and is not inside something. */
bool probeUsable(uint probe, ivec3 cell)
{
    ProbeState state = probeStates().states[probe];
    return state.cell.w > 0 && state.cell.xyz == cell && state.info.x <= kProbeMaxBackfaces;
}

/**
 * How far inside a volume a point is: 1 from a cell in, 0 on and outside its
 * outer probes (the light fades to the next volume's, or to the paths').
 */
float volumeCoverage(ProbeVolume volume, vec3 grid)
{
    vec3 inside = min(grid, vec3(volume.probes.xyz - 1) - grid);
    return clamp(min(inside.x, min(inside.y, inside.z)), 0.0, 1.0);
}

/**
 * A volume's light at a point (DDGI's interpolation): of the 8 probes around
 * it, weighted by distance (trilinear), by how much they face the surface and
 * by whether they see it. `weights`: their sum, 0 if none does.
 */
vec3 volumeLight(ProbeVolume volume, vec3 position, vec3 normal, vec3 toViewer,
                 out float weights)
{
    // Off the surface, towards the viewer: the probes' distances are to surfaces.
    vec3 biased = position + normal * volume.bias.x + toViewer * volume.bias.y;
    vec3 grid = transformPoint(volume.worldToGrid, biased);
    ivec3 base = clamp(ivec3(floor(grid)), ivec3(0), volume.probes.xyz - 2);
    vec3 alpha = clamp(grid - vec3(base), 0.0, 1.0);
    vec3 localNormal = toProbeLocal(volume, normal);
    vec3 sum = vec3(0.0);
    weights = 0.0;
    for (int i = 0; i < 8; ++i) {
        ivec3 offset = ivec3(i & 1, (i >> 1) & 1, i >> 2);
        ivec3 probeGrid = base + offset;
        uint probe = probeIndex(volume, probeGrid);
        if (!probeUsable(probe, probeGrid + volume.origin.xyz)) {
            continue;
        }
        vec3 probePosition = transformPoint(volume.gridToWorld, vec3(probeGrid));
        // Probes behind the surface count less (smoothly: not at all is too sharp).
        vec3 toProbe = normalize(probePosition - position);
        float facing = 0.5 * (dot(toProbe, normal) + 1.0);
        float weight = facing * facing + 0.2;
        // Chebyshev: how likely the probe sees the point, from its distances that way.
        vec3 fromProbe = biased - probePosition;
        float distance = length(fromProbe);
        vec2 moments =
            probeDistanceAt(probe, toProbeLocal(volume, fromProbe / max(distance, 1e-6)));
        if (distance > moments.x) {
            float variance = abs(moments.y - moments.x * moments.x);
            float excess = distance - moments.x;
            float visible = variance / (variance + excess * excess);
            weight *= max(0.05, visible * visible * visible);
        }
        // Crush small weights: a probe that barely sees the point should not light it.
        const float kCrush = 0.2;
        weight = max(weight, 1e-6);
        if (weight < kCrush) {
            weight *= weight * weight / (kCrush * kCrush);
        }
        vec3 trilinear = mix(1.0 - alpha, alpha, vec3(offset));
        weight *= trilinear.x * trilinear.y * trilinear.z;
        sum += weight * probeIrradianceAt(probe, localNormal);
        weights += weight;
    }
    return weights > 0.0 ? sum / weights : vec3(0.0);
}

/**
 * The probes' light at a surface point: what a diffuse surface there reflects
 * of the light of other surfaces, per unit albedo. Volumes are blended by how
 * far inside each the point is, the most detailed first; `coverage` is how
 * much of the light they give (0 outside all of them).
 */
vec3 probeLight(vec3 position, vec3 normal, vec3 toViewer, out float coverage)
{
    ProbeVolumes data = probeVolumes();
    vec3 light = vec3(0.0);
    float remaining = 1.0;
    for (uint i = 0u; i < data.info.x && remaining > 1e-3; ++i) {
        ProbeVolume volume = data.volumes[i];
        float inside = volumeCoverage(volume, transformPoint(volume.worldToGrid, position));
        if (inside <= 0.0) {
            continue;
        }
        float weights;
        vec3 l = volumeLight(volume, position, normal, toViewer, weights);
        if (weights <= 0.0) {
            continue;
        }
        light += remaining * inside * l;
        remaining *= 1.0 - inside;
    }
    coverage = 1.0 - remaining;
    return coverage > 0.0 ? light / coverage : vec3(0.0);
}

#endif  // STARCORE_PROBES_GLSL
