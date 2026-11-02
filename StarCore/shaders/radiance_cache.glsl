// The hybrid renderer's radiance cache (src/gpu/radiance_cache.cpp), after NVIDIA's
// spatially hashed radiance cache (SHaRC): cells of the surfaces in a hash table,
// each holding the light its surface reflects (direction-agnostic: as a diffuse
// surface), learnt from paths traced from a few pixels every frame (hybrid_trace.comp's
// cache stage) and averaged over the last frames (cache_resolve.comp). A path that
// finds a surface whose cell knows its light ends there with that light instead of
// tracing on: "a ray and a cache lookup", as engines gather light (Lumen's surface
// cache, DDGI's probes) -- far less noise than paths of several vertices, as many
// bounces as the cache learns over the frames.
//
// - Cells are in the space of each instance's object (its index, a level, the side its
//   normal faces, a cell of a size of that level): a cockpit's cells move with the ship,
//   whatever it flies through. The level follows the distance from the camera
//   (kCachePixels pixels a cell, a power of two of kCacheBase).
// - What the sphere lights give a cell's diffuse lobe straight is not cached: the
//   shaders add it in closed form, shadowed by a ray (sphere_light.glsl), at the point
//   looked up, so it follows the lights at once and its shadows are not blurred. The cache
//   holds the rest: the meshes' light and the light bounced around, which change slowly.
// - Glass and metals are not cached (their light depends on where they are seen from):
//   paths go on through them.
// Include after sphere_light.glsl.

#ifndef STARCORE_RADIANCE_CACHE_GLSL
#define STARCORE_RADIANCE_CACHE_GLSL

// The cache's buffers (frameData.cacheBuffers, cacheValues): per entry, its key (x:
// checksum, 0 for none; y: frames since it was last given samples), the samples of this
// frame (fixed point, w: their count) and its light (w: the samples it averages).
layout(buffer_reference, std430, buffer_reference_align = 8) buffer CacheKeys
{
    uvec2 cacheKeys[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) buffer CacheSums
{
    ivec4 cacheSums[];
};
layout(buffer_reference, std430, buffer_reference_align = 16) buffer CacheValues
{
    vec4 cacheValues[];
};

const uint kCacheBucket = 8u;            // Entries a key may take (linear probing).
const uint kNoCacheEntry = 0xFFFFFFFFu;
const float kCacheFixedPoint = 1024.0;   // Of the sums.
const float kCacheMaxSample = 64.0;      // A sample's light at most (fixed point range).
const float kCacheBase = 1.0 / 64.0;     // The finest cells' size (world units).
const float kCachePixels = 12.0;         // The cells' size on screen (pixels across).
const float kCacheMinSamples = 4.0;      // A cell is looked up with this many at least.

bool radianceCacheEnabled()
{
    return (frameData.options.x & kFlagRadianceCache) != 0u;
}

/** Whether a surface's light may be cached: not glass, not metal (seen alike from anywhere). */
bool cacheable(Material material)
{
    return material.transmission <= 0.0 && material.metallic < 0.5;
}

/**
 * Of a surface point: the entry its key starts at (x) and its checksum (y, not 0); its
 * cell's size (world units).
 */
uvec2 cacheKey(SurfaceHit hit, out float cellSize)
{
    InstanceTransform transform = transforms[hit.instanceIndex];
    vec3 objectPoint = transformPoint(transform.worldToObject, hit.position);
    vec3 objectNormal = transformVector(transform.worldToObject, hit.geometricNormal);
    // World units per object unit.
    float scale = length(vec3(transform.objectToWorld[0].x, transform.objectToWorld[1].x,
                              transform.objectToWorld[2].x));
    // The cell's size: kCachePixels pixels at its distance from the camera.
    float pixel = 2.0 * length(frameData.cameraUp.xyz) / viewportSize().y;
    float wanted = length(hit.position - frameData.cameraPosition.xyz) * pixel * kCachePixels;
    uint level = uint(clamp(ceil(log2(max(wanted, kCacheBase) / kCacheBase)), 0.0, 24.0));
    cellSize = kCacheBase * exp2(float(level));
    ivec3 cell = ivec3(floor(objectPoint * (scale / cellSize)));
    vec3 magnitude = abs(objectNormal);
    uint axis = magnitude.x >= magnitude.y && magnitude.x >= magnitude.z ? 0u
                : magnitude.y >= magnitude.z                             ? 1u
                                                                         : 2u;
    uint side = 2u * axis + (objectNormal[axis] < 0.0 ? 1u : 0u);
    uint h = pcgHash(hit.instanceIndex * 0x9E3779B9u + (level << 3u) + side);
    h = pcgHash(h ^ uint(cell.x));
    h = pcgHash(h ^ uint(cell.y));
    h = pcgHash(h ^ uint(cell.z));
    uint buckets = frameData.cacheValues.z / kCacheBucket;
    return uvec2((h % buckets) * kCacheBucket, pcgHash(h ^ 0x68E31DA4u) | 1u);
}

/** The entry of a key, taken if it has none yet; kNoCacheEntry if its bucket is full. */
uint cacheInsert(uvec2 key)
{
    CacheKeys keys = CacheKeys(frameData.cacheBuffers.xy);
    for (uint i = 0u; i < kCacheBucket; ++i) {
        uint previous = atomicCompSwap(keys.cacheKeys[key.x + i].x, 0u, key.y);
        if (previous == 0u || previous == key.y) {
            return key.x + i;
        }
    }
    return kNoCacheEntry;
}

/** The entry of a key; kNoCacheEntry if it has none (the whole bucket: entries may go). */
uint cacheFind(uvec2 key)
{
    CacheKeys keys = CacheKeys(frameData.cacheBuffers.xy);
    for (uint i = 0u; i < kCacheBucket; ++i) {
        if (keys.cacheKeys[key.x + i].x == key.y) {
            return key.x + i;
        }
    }
    return kNoCacheEntry;
}

/** Add a sample of the light an entry's surface reflects. */
void cacheAdd(uint entry, vec3 light)
{
    CacheSums sums = CacheSums(frameData.cacheBuffers.zw);
    ivec3 scaled =
        ivec3(round(clamp(light, -kCacheMaxSample, kCacheMaxSample) * kCacheFixedPoint));
    atomicAdd(sums.cacheSums[entry].x, scaled.x);
    atomicAdd(sums.cacheSums[entry].y, scaled.y);
    atomicAdd(sums.cacheSums[entry].z, scaled.z);
    atomicAdd(sums.cacheSums[entry].w, 1);
}

/**
 * The light a surface reflects towards `wo`, if its cell knows it and the path's
 * `footprint` there is as wide as the cell (else the cells would show, as in a mirror):
 * the cached light and the sphere lights' (not its own emission: the path's; `u` as
 * closedFormHitLight's).
 */
bool cachedReflection(SurfaceHit hit, Material material, vec3 wo, float footprint, vec2 u,
                      out vec3 reflected)
{
    reflected = vec3(0.0);
    if (!cacheable(material)) {
        return false;
    }
    float cellSize;
    uvec2 key = cacheKey(hit, cellSize);
    if (footprint < cellSize) {
        return false;
    }
    uint entry = cacheFind(key);
    if (entry == kNoCacheEntry) {
        return false;
    }
    vec4 value = CacheValues(frameData.cacheValues.xy).cacheValues[entry];
    if (value.w < kCacheMinSamples) {
        return false;
    }
    reflected = max(value.rgb + closedFormHitLight(hit, material, wo, u, false), vec3(0.0));
    return true;
}

#endif  // STARCORE_RADIANCE_CACHE_GLSL
