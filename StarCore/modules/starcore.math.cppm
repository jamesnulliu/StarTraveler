// starcore.math -- vectors and geometry (on glm, which it re-exports):
//
//   :types      Vec2/3/4, Mat3/4, Quat (glm types), constants, Rect
//   :geometry   rays, boxes, ray-box/sphere/triangle intersection, closest points
//   :bvh        bounding volume hierarchies over boxes (BLAS, TLAS, overlaps)
//   :color      RGBA8 packing, sRGB, tone mapping
//   :random     PCG hashing and a random generator (same streams as the shaders)
//   :transform  compose/decompose translation, rotation, scale

export module starcore.math;

export import :bvh;
export import :color;
export import :geometry;
export import :random;
export import :transform;
export import :types;
