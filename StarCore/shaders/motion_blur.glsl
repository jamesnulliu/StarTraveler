// Motion blur after McGuire et al., "A Reconstruction Filter for Plausible
// Motion Blur" (I3D 2012): each pixel's blur is half the screen distance it
// moved while the shutter was open, the neighborhood's largest blur sets the
// direction to gather along, and depths decide what may smear over what.
// Motion images (rgba32f): xy = previous minus current position in pixels,
// w = current view depth (far for the sky).

#ifndef STARCORE_MOTION_BLUR_GLSL
#define STARCORE_MOTION_BLUR_GLSL

const int kMotionTile = 16;  // Pixels; the largest blur radius should not exceed ~1.5 tiles.

/**
 * The motion of an output pixel from a motion image of the rendered size
 * (`scale`: its size over the output's, less than 1 when the frame was
 * upscaled): that of the rendered pixel it is in, in output pixels.
 */
vec4 outputMotion(vec4 rendered, vec2 scale)
{
    return vec4(rendered.xy / scale, rendered.zw);
}

ivec2 renderedPixel(ivec2 pixel, vec2 scale)
{
    return ivec2((vec2(pixel) + 0.5) * scale);
}

/** Half the screen motion during the exposure (the blur's radius as a vector), clamped. */
vec2 halfBlur(vec4 motion, float shutter, float maxRadius)
{
    if (!(abs(motion.x) < 1e8 && abs(motion.y) < 1e8)) {
        return vec2(0.0);  // Unknown (not on screen the frame before).
    }
    vec2 v = -motion.xy * (0.5 * shutter);
    float radius = length(v);
    return radius > maxRadius ? v * (maxRadius / radius) : v;
}

#endif  // STARCORE_MOTION_BLUR_GLSL
