#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <glm/gtc/quaternion.hpp>

#include <starcore/scene/entity.hpp>
#include <starcore/scene/serializer.hpp>

using namespace starcore;
using Catch::Matchers::WithinAbs;

TEST_CASE("Entities get identity components", "[scene]")
{
    Scene scene;
    Entity entity = scene.createEntity("Ball");
    REQUIRE(entity);
    REQUIRE(entity.has<IdComponent, NameComponent, TransformComponent>());
    REQUIRE(entity.name() == "Ball");
    REQUIRE(scene.findEntity(entity.uuid()) == entity);
    REQUIRE(scene.findEntityByName("Ball") == entity);
    REQUIRE(scene.entityCount() == 1);

    entity.add<SphereComponent>(2.0F);
    REQUIRE(entity.get<SphereComponent>().radius == 2.0F);
    REQUIRE(entity.tryGet<MeshComponent>() == nullptr);
    entity.remove<SphereComponent>();
    REQUIRE_FALSE(entity.has<SphereComponent>());
}

TEST_CASE("Scene version tracks changes", "[scene]")
{
    Scene scene;
    const auto v0 = scene.version();
    Entity entity = scene.createEntity();
    const auto v1 = scene.version();
    REQUIRE(v1 > v0);

    entity.patch<TransformComponent>([](auto& t) { t.translation.x = 3.0F; });
    REQUIRE(scene.version() > v1);

    const auto v2 = scene.version();
    entity.get<TransformComponent>().translation.y = 1.0F;  // In-place edit...
    REQUIRE(scene.version() == v2);
    scene.markDirty();  // ...must be reported explicitly.
    REQUIRE(scene.version() > v2);
}

TEST_CASE("Hierarchy keeps world transforms and rejects cycles", "[scene]")
{
    Scene scene;
    Entity parent = scene.createEntity("Parent");
    Entity child = scene.createEntity("Child");
    parent.get<TransformComponent>().translation = {10.0F, 0.0F, 0.0F};
    parent.get<TransformComponent>().scale = Vec3(2.0F);
    child.get<TransformComponent>().translation = {12.0F, 4.0F, 0.0F};

    REQUIRE(scene.setParent(child, parent));
    REQUIRE(scene.parentOf(child) == parent);
    REQUIRE(scene.childrenOf(parent).size() == 1);

    // World position unchanged; local is relative to the scaled parent.
    const Vec3 world = Vec3(child.worldTransform()[3]);
    REQUIRE_THAT(world.x, WithinAbs(12.0, 1e-5));
    REQUIRE_THAT(world.y, WithinAbs(4.0, 1e-5));
    REQUIRE_THAT(child.get<TransformComponent>().translation.x, WithinAbs(1.0, 1e-5));

    // Moving the parent moves the child.
    parent.get<TransformComponent>().translation.x = 0.0F;
    REQUIRE_THAT(Vec3(child.worldTransform()[3]).x, WithinAbs(2.0, 1e-5));

    REQUIRE_FALSE(scene.setParent(parent, child));  // Cycle.
    REQUIRE_FALSE(scene.setParent(parent, parent));

    // Detach.
    REQUIRE(scene.setParent(child, Entity{}));
    REQUIRE_FALSE(scene.parentOf(child));
    REQUIRE(scene.childrenOf(parent).empty());
}

TEST_CASE("Destroying an entity destroys its children", "[scene]")
{
    Scene scene;
    Entity root = scene.createEntity("Root");
    Entity child = scene.createEntity("Child");
    Entity grandChild = scene.createEntity("GrandChild");
    Entity other = scene.createEntity("Other");
    scene.setParent(child, root);
    scene.setParent(grandChild, child);

    const Uuid grandChildId = grandChild.uuid();
    scene.destroyEntity(child);
    REQUIRE_FALSE(child);
    REQUIRE_FALSE(grandChild);
    REQUIRE_FALSE(scene.findEntity(grandChildId));
    REQUIRE(scene.childrenOf(root).empty());
    REQUIRE(other);
    REQUIRE(scene.entityCount() == 2);
}

TEST_CASE("copyEntity duplicates data components across scenes", "[scene]")
{
    Scene source;
    Entity original = source.createEntity("Arc");
    original.get<TransformComponent>().translation = {1.0F, 2.0F, 3.0F};
    original.add<ArcShapeComponent>(ArcShapeComponent{2.0F, 0.5F, 1.0F});
    original.add<StrokeComponent>(StrokeComponent{.thickness = 5});
    Entity child = source.createEntity("Child");
    source.setParent(child, original);

    Scene target;
    Entity copy = target.copyEntity(original);
    REQUIRE(copy.scene() == &target);
    REQUIRE(copy.uuid() != original.uuid());
    REQUIRE(copy.name() == "Arc");
    REQUIRE(copy.get<TransformComponent>().translation == Vec3{1.0F, 2.0F, 3.0F});
    REQUIRE(copy.get<ArcShapeComponent>().radius == 2.0F);
    REQUIRE(copy.get<StrokeComponent>().thickness == 5);
    REQUIRE(target.childrenOf(copy).empty());  // Hierarchy is not copied.
    REQUIRE(target.entityCount() == 1);
}

TEST_CASE("Scene YAML round-trip", "[scene][serializer]")
{
    Scene scene;
    scene.environment().skyColor = {0.1F, 0.2F, 0.3F};
    scene.environment().space = SpaceSky{.enabled = true,
                                         .seed = 42,
                                         .brightness = 0.5F,
                                         .milkyWay = 1.5F,
                                         .starMagnitude = 7.0F,
                                         .orientation = {10.0F, 20.0F, 30.0F},
                                         .nebula = 0.8F,
                                         .nebulaCenter = {0.6F, 0.0F, 0.8F},
                                         .corona = 3.5F};

    Entity ball = scene.createEntity("Ball");
    ball.get<TransformComponent>().translation = {1.0F, 2.0F, 3.0F};
    ball.get<TransformComponent>().rotation =
        glm::angleAxis(0.5F, Vec3{0.0F, 1.0F, 0.0F});
    ball.add<SphereComponent>(1.5F);
    ball.add<MaterialComponent>(MaterialComponent{
        .albedo = {1.0F, 0.0F, 0.0F}, .emissionColor = {1.0F, 1.0F, 0.0F},
        .emissionPower = 3.0F});

    Entity group = scene.createEntity("Group");
    group.add<PointShapeComponent>();
    group.add<StrokeComponent>(StrokeComponent{.color = {0, 1, 0, 1}, .thickness = 9});

    Entity curve = scene.createEntity("Curve");
    curve.add<CurveShapeComponent>(CurveShapeComponent{
        .controlPoints = {{0, 0, 0}, {1, 1, 0}, {2, 0, 0}},
        .type = CurveType::Bezier,
        .order = 4});
    curve.add<StrokeComponent>(StrokeComponent{.pattern = LinePattern::Dash});
    scene.setParent(curve, group);

    Entity mesh = scene.createEntity("Triangle");
    mesh.add<MeshComponent>(MeshComponent{
        .mesh = makeMesh({{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}, {0, 1, 2}).value()});

    const std::string yaml = serializeScene(scene);

    Scene loaded;
    REQUIRE(deserializeScene(yaml, loaded).has_value());
    REQUIRE(loaded.entityCount() == 4);
    REQUIRE(loaded.environment().skyColor == scene.environment().skyColor);
    REQUIRE(loaded.environment().space == scene.environment().space);
    // Without a space sky the file has none (and loads without one).
    Scene plain;
    REQUIRE(serializeScene(plain).find("Space") == std::string::npos);

    Entity ball2 = loaded.findEntity(ball.uuid());
    REQUIRE(ball2.name() == "Ball");
    REQUIRE(ball2.get<TransformComponent>().translation == Vec3{1.0F, 2.0F, 3.0F});
    REQUIRE(ball2.get<TransformComponent>().rotation ==
            ball.get<TransformComponent>().rotation);
    REQUIRE(ball2.get<SphereComponent>().radius == 1.5F);
    REQUIRE(ball2.get<MaterialComponent>().emissionPower == 3.0F);

    Entity curve2 = loaded.findEntity(curve.uuid());
    REQUIRE(loaded.parentOf(curve2).uuid() == group.uuid());
    REQUIRE(curve2.get<CurveShapeComponent>().type == CurveType::Bezier);
    REQUIRE(curve2.get<CurveShapeComponent>().controlPoints.size() == 3);
    REQUIRE(curve2.get<StrokeComponent>().pattern == LinePattern::Dash);
    // Local transform restored as-is (not recomputed from world).
    REQUIRE(curve2.get<TransformComponent>().translation ==
            curve.get<TransformComponent>().translation);

    REQUIRE(loaded.findEntity(mesh.uuid()).get<MeshComponent>().mesh->indices.size() == 3);
    REQUIRE(loaded.findEntity(group.uuid()).get<StrokeComponent>().thickness == 9);

    // Serializing again yields the same document.
    REQUIRE(serializeScene(loaded) == yaml);
}

TEST_CASE("Transient entities and their children are not saved", "[scene][serializer]")
{
    Scene scene;
    scene.createEntity("World");
    Entity ship = scene.createEntity("Ship");
    ship.add<TransientComponent>();
    Entity seat = scene.createEntity("Seat");
    scene.setParent(seat, ship);

    Scene loaded;
    REQUIRE(deserializeScene(serializeScene(scene), loaded).has_value());
    REQUIRE(loaded.entityCount() == 1);
    REQUIRE(loaded.findEntityByName("World"));
}

TEST_CASE("Invalid scene documents are rejected", "[scene][serializer]")
{
    Scene scene;
    scene.createEntity("Leftover");

    REQUIRE_FALSE(deserializeScene("foo: [1, 2", scene).has_value());
    REQUIRE(scene.entityCount() == 0);

    REQUIRE_FALSE(deserializeScene("Hello: 1", scene).has_value());

    const auto unknownParent = deserializeScene(
        "StarCoreScene: 1\nEntities:\n  - Id: 5\n    Parent: 99\n", scene);
    REQUIRE_FALSE(unknownParent.has_value());
    REQUIRE(scene.entityCount() == 0);

    const auto badEnum = deserializeScene(
        "StarCoreScene: 1\nEntities:\n  - Id: 5\n    Stroke: {Pattern: Wavy}\n", scene);
    REQUIRE_FALSE(badEnum.has_value());
}
