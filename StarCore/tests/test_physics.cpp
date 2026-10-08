#include <algorithm>
#include <cmath>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <starcore/math/random.hpp>
#include <starcore/physics/physics_world.hpp>
#include <starcore/scene/entity.hpp>

using namespace starcore;
using Catch::Matchers::WithinAbs;

namespace
{

auto addSphere(Scene& scene, Vec3 position, float radius, std::uint32_t layer = 0) -> Entity
{
    Entity entity = scene.createEntity("Sphere");
    entity.get<TransformComponent>().translation = position;
    entity.add<ColliderComponent>(ColliderComponent{.shape = ColliderShape::Sphere, .radius = radius});
    scene.setLayer(entity, layer);
    return entity;
}

auto addBox(Scene& scene, Vec3 position, Vec3 halfExtents, Quat rotation = Quat(1, 0, 0, 0))
    -> Entity
{
    Entity entity = scene.createEntity("Box");
    entity.get<TransformComponent>().translation = position;
    entity.get<TransformComponent>().rotation = rotation;
    entity.add<ColliderComponent>(
        ColliderComponent{.shape = ColliderShape::Box, .halfExtents = halfExtents});
    return entity;
}

/** @brief A 20 x 20 plane through `position` facing `normal`, as a mesh collider. */
auto addWall(Scene& scene, Vec3 position, Vec3 normal) -> Entity
{
    Entity entity = scene.createEntity("Wall");
    entity.get<TransformComponent>().translation = position;
    entity.get<TransformComponent>().rotation = rotationBetween({0, 1, 0}, normal);
    entity.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
    entity.add<ColliderComponent>(ColliderComponent{.shape = ColliderShape::Mesh});
    return entity;
}

}  // namespace

TEST_CASE("Closest point on a triangle matches dense sampling", "[physics]")
{
    Rng rng(8);
    for (int i = 0; i < 200; ++i) {
        const Vec3 a = rng.inUnitSphere() * 3.0F;
        const Vec3 b = rng.inUnitSphere() * 3.0F;
        const Vec3 c = rng.inUnitSphere() * 3.0F;
        const Vec3 p = rng.inUnitSphere() * 6.0F;
        const Vec3 q = closestPointOnTriangle(p, a, b, c);
        float sampled = kInfinity;
        constexpr int kSteps = 60;
        for (int u = 0; u <= kSteps; ++u) {
            for (int v = 0; u + v <= kSteps; ++v) {
                const float fu = static_cast<float>(u) / kSteps;
                const float fv = static_cast<float>(v) / kSteps;
                sampled = std::min(sampled, glm::distance(p, a + (b - a) * fu + (c - a) * fv));
            }
        }
        REQUIRE(glm::distance(p, q) <= sampled + 1e-4F);
        REQUIRE(glm::distance(p, q) >= sampled - 0.15F);  // Sampling resolution.
    }
}

TEST_CASE("Raycasts hit spheres, boxes and meshes with layer and trigger filters",
          "[physics]")
{
    Scene scene;
    Entity sphere = addSphere(scene, {0, 0, 0}, 1.0F);
    Entity box = addBox(scene, {5, 0, 0}, {1, 1, 1}, glm::angleAxis(kPi / 4, Vec3{0, 1, 0}));
    Entity wall = addWall(scene, {0, -5, 0}, {0, 1, 0});
    Entity trigger = addSphere(scene, {0, 3, 0}, 0.5F);
    trigger.patch<ColliderComponent>([](auto& c) { c.trigger = true; });
    Entity hidden = addSphere(scene, {-5, 0, 0}, 1.0F, 6);
    const PhysicsWorld world = PhysicsWorld::build(scene);
    REQUIRE(world.colliders().size() == 5);

    auto hit = world.raycast(Ray{{0, 0, 10}, {0, 0, -1}}, kInfinity);
    REQUIRE(hit.has_value());
    REQUIRE(hit->entity == sphere.handle());
    REQUIRE_THAT(hit->distance, WithinAbs(9.0, 1e-4));
    REQUIRE(glm::distance(hit->normal, Vec3{0, 0, 1}) < 1e-4F);

    // Box rotated 45 degrees: its corner points at +Z, at distance sqrt(2).
    hit = world.raycast(Ray{{5, 0, 10}, {0, 0, -1}}, kInfinity);
    REQUIRE(hit->entity == box.handle());
    REQUIRE_THAT(hit->distance, WithinAbs(10.0 - std::sqrt(2.0), 1e-4));

    hit = world.raycast(Ray{{0, 0.5F, -3}, {0, -1, 0}}, kInfinity);
    REQUIRE(hit->entity == wall.handle());
    REQUIRE_THAT(hit->distance, WithinAbs(5.5, 1e-4));
    REQUIRE(glm::distance(hit->normal, Vec3{0, 1, 0}) < 1e-4F);
    REQUIRE_FALSE(world.raycast(Ray{{0, 0.5F, -3}, {0, -1, 0}}, 5.0F));  // Too short.

    // Triggers only with filter.triggers; ignored entities and other layers never.
    const Ray down{{0, 10, 0}, {0, -1, 0}};
    REQUIRE(world.raycast(down, kInfinity)->entity == sphere.handle());
    REQUIRE(world.raycast(down, kInfinity, {.triggers = true})->entity == trigger.handle());
    REQUIRE(world.raycast(down, kInfinity, {.ignore = sphere.handle()})->entity == wall.handle());
    REQUIRE(world.raycast(Ray{{-5, 10, 0}, {0, -1, 0}}, kInfinity)->entity == hidden.handle());
    REQUIRE(world.raycast(Ray{{-5, 10, 0}, {0, -1, 0}}, kInfinity,
                          {.layerMask = kAllLayers & ~layerBit(6)})
                ->entity == wall.handle());
    // Rays starting inside a collider do not hit it.
    REQUIRE(world.raycast(Ray{{0, 0, 0}, {0, 0, 1}}, 2.0F) == std::nullopt);
}

TEST_CASE("Sphere casts find the first contact exactly", "[physics]")
{
    Scene scene;
    Entity ball = addSphere(scene, {0, 0, 0}, 1.0F);
    Entity box = addBox(scene, {10, 0, 0}, {1, 1, 1});
    addWall(scene, {0, -10, 0}, {0, 1, 0});
    const PhysicsWorld world = PhysicsWorld::build(scene);

    SECTION("sphere: radii add up")
    {
        const auto hit = world.sphereCast({0, 0, 10}, 0.5F, {0, 0, -20});
        REQUIRE(hit.has_value());
        REQUIRE(hit->entity == ball.handle());
        REQUIRE_THAT(hit->fraction * 20.0F, WithinAbs(10.0 - 1.5, 1e-3));
        REQUIRE(glm::distance(hit->normal, Vec3{0, 0, 1}) < 1e-3F);
    }
    SECTION("box face and edge")
    {
        auto hit = world.sphereCast({10, 0, 10}, 0.5F, {0, 0, -20});
        REQUIRE(hit->entity == box.handle());
        REQUIRE_THAT(hit->fraction * 20.0F, WithinAbs(10.0 - 1.5, 1e-3));
        // Grazing the edge x = 11: contact where the sphere center is 0.5 from the edge line.
        hit = world.sphereCast({11.3F, 0, 10}, 0.5F, {0, 0, -20});
        REQUIRE(hit.has_value());
        const float expected = 10.0F - 1.0F - std::sqrt(0.5F * 0.5F - 0.3F * 0.3F);
        REQUIRE_THAT(hit->fraction * 20.0F, WithinAbs(expected, 1e-3));
        // Passing just outside the edge misses.
        REQUIRE_FALSE(world.sphereCast({11.6F, 0, 10}, 0.5F, {0, 0, -20}));
    }
    SECTION("fast spheres do not tunnel through thin meshes")
    {
        const auto hit = world.sphereCast({3, 0, 3}, 0.25F, {0, -1000, 0});
        REQUIRE(hit.has_value());
        REQUIRE_THAT(hit->fraction * 1000.0F, WithinAbs(10.0 - 0.25, 1e-3));
        REQUIRE(glm::distance(hit->normal, Vec3{0, 1, 0}) < 1e-3F);
    }
    SECTION("overlapping spheres may leave but not go deeper")
    {
        REQUIRE_FALSE(world.sphereCast({0, 0, 1.2F}, 0.5F, {0, 0, 2}));
        const auto deeper = world.sphereCast({0, 0, 1.2F}, 0.5F, {0, 0, -1});
        REQUIRE(deeper.has_value());
        REQUIRE(deeper->fraction == 0.0F);
    }
}

TEST_CASE("Overlap queries report penetration", "[physics]")
{
    Scene scene;
    addSphere(scene, {0, 0, 0}, 1.0F);
    addBox(scene, {10, 0, 0}, {1, 1, 1});
    addWall(scene, {0, -10, 0}, {0, 1, 0});
    const PhysicsWorld world = PhysicsWorld::build(scene);

    auto contacts = world.overlapSphere({0, 1.25F, 0}, 0.5F);
    REQUIRE(contacts.size() == 1);
    REQUIRE_THAT(contacts[0].depth, WithinAbs(0.25, 1e-5));
    REQUIRE(glm::distance(contacts[0].normal, Vec3{0, 1, 0}) < 1e-5F);

    contacts = world.overlapSphere({10.8F, 0.2F, 0}, 0.5F);  // Center inside the box.
    REQUIRE(contacts.size() == 1);
    REQUIRE(glm::distance(contacts[0].normal, Vec3{1, 0, 0}) < 1e-5F);
    REQUIRE_THAT(contacts[0].depth, WithinAbs(0.5 + 0.2, 1e-5));

    contacts = world.overlapSphere({2, -9.9F, 2}, 0.5F);
    REQUIRE(contacts.size() == 1);
    REQUIRE_THAT(contacts[0].depth, WithinAbs(0.4, 1e-4));

    REQUIRE(world.overlapSphere({5, 5, 5}, 0.5F).empty());
}

TEST_CASE("Collide and slide keeps spheres out of colliders", "[physics]")
{
    Scene scene;
    Entity wall = addWall(scene, {0, 0, 0}, {1, 0, 0});  // The plane x = 0, facing +X.
    const PhysicsWorld world = PhysicsWorld::build(scene);
    constexpr float kRadius = 0.5F;

    SECTION("sliding along a wall keeps the tangential motion")
    {
        const auto moved = world.moveSphere({2, 0, 2}, kRadius, {-4, 0, -3});
        REQUIRE(moved.collided);
        REQUIRE(moved.touched == std::vector{wall.handle()});
        REQUIRE(moved.position.x >= kRadius);
        REQUIRE_THAT(moved.position.x, WithinAbs(kRadius, 0.01));
        REQUIRE_THAT(moved.position.z, WithinAbs(2.0 - 3.0, 0.01));
        REQUIRE(glm::distance(moved.normals.front(), Vec3{1, 0, 0}) < 1e-3F);
    }
    SECTION("free movement is unchanged")
    {
        const auto moved = world.moveSphere({2, 0, 2}, kRadius, {1, 2, 3});
        REQUIRE_FALSE(moved.collided);
        REQUIRE(moved.position == Vec3{3, 2, 5});
    }
    SECTION("an overlapping sphere is pushed out")
    {
        const auto moved = world.moveSphere({0.2F, 0, 0}, kRadius, Vec3(0.0F));
        REQUIRE(moved.collided);
        REQUIRE(moved.position.x >= kRadius);
    }
    SECTION("corners stop the motion")
    {
        Scene corner;
        addWall(corner, {0, 0, 0}, {1, 0, 0});
        addWall(corner, {0, 0, 0}, {0, 0, 1});
        const PhysicsWorld cornerWorld = PhysicsWorld::build(corner);
        const auto moved = cornerWorld.moveSphere({3, 0, 3}, kRadius, {-10, 0, -10});
        REQUIRE(moved.position.x >= kRadius - 1e-3F);
        REQUIRE(moved.position.z >= kRadius - 1e-3F);
        REQUIRE(moved.position.x < kRadius + 0.05F);
        REQUIRE(moved.position.z < kRadius + 0.05F);
    }
}

TEST_CASE("The layer collision matrix filters physics queries", "[physics][layers]")
{
    Scene scene;
    addSphere(scene, {0, 0, 0}, 1.0F, 2);
    scene.layers().setCollides(5, 2, false);
    const PhysicsWorld world = PhysicsWorld::build(scene);
    const QueryFilter fromLayer5{.layerMask = scene.layers().collisionMask(5)};
    REQUIRE_FALSE(world.sphereCast({0, 0, 5}, 0.5F, {0, 0, -10}, fromLayer5));
    const QueryFilter fromLayer1{.layerMask = scene.layers().collisionMask(1)};
    REQUIRE(world.sphereCast({0, 0, 5}, 0.5F, {0, 0, -10}, fromLayer1));
}
