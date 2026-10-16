// The part of a sphere light above a surface point's plane, as its shadows sample it:
// the shadow maps' taps (shadows.glsl) and the shadow rays (sphere_light.glsl). Of a big
// light low over a planet only its upper part lights the ground: shadows of all of it
// were too dark there, or (a ray to its center, below the horizon) of the ground itself.
//
// The light is taken as a disk of its radius through its center, facing the point: of
// its points center + radius * (x tangent + y bitangent), in a frame around the direction
// from its center to the point (as the taps'), those above the plane are past a chord,
// and what is sampled is an ellipse inside that segment, from the chord to the edge (all
// of the disk while all of it is above). Each point weighs as the cosine its light comes
// in at, as the closed forms weigh the light. Include after common.glsl.

#ifndef STARCORE_LIGHT_PART_GLSL
#define STARCORE_LIGHT_PART_GLSL

struct LightPart
{
    vec2 axis;     // Unit: across the chord, towards the part above the plane.
    float chord;   // Its offset along the axis (< -1: all of the disk above; > 1: none).
    vec3 ellipse;  // The ellipse sampled: x its center along the axis, y its half-axis
                   // along it, z across.
};

/**
 * The part of a sphere light (`center`, `radius`) above the plane of a surface point
 * (`point`, unit `normal` on the side lit), in `frame` (made around the direction from
 * the light's center to the point).
 */
LightPart lightPartAbove(vec3 center, float radius, vec3 point, vec3 normal, Frame frame)
{
    // A point x along the axis is radius * across * (x - chord) above the plane.
    vec2 slope = vec2(dot(frame.tangent, normal), dot(frame.bitangent, normal));
    float across = length(slope);
    LightPart part;
    part.axis = across > 1e-6 ? slope / across : vec2(1.0, 0.0);
    part.chord = dot(point - center, normal) / (radius * max(across, 1e-6));
    // A sliver at least: what sets last, while the closed form still has some of it.
    float chord = clamp(part.chord, -1.0, 0.98);
    float middle = 0.5 * (1.0 + chord);
    part.ellipse = vec3(middle, 0.5 * (1.0 - chord), sqrt(1.0 - middle * middle));
    return part;
}

/**
 * A point of the unit disk moved onto a light's part: its offset on the light's disk
 * (xy, unit: its radius) and its weight (z: the cosine its light comes in at, relatively).
 */
vec3 onLightPart(LightPart part, vec2 disk)
{
    float along = part.ellipse.x + part.ellipse.y * disk.x;
    vec2 offset = part.axis * along + vec2(-part.axis.y, part.axis.x) * (part.ellipse.z * disk.y);
    return vec3(offset, max(along - part.chord, 1e-3));
}

#endif  // STARCORE_LIGHT_PART_GLSL
