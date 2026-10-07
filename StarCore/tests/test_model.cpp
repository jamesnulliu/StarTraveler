#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <glm/gtc/quaternion.hpp>

#include <starcore/asset/model.hpp>
#include <starcore/math/random.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/scene/serializer.hpp>

using namespace starcore;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{

/** @brief A fresh, empty directory under the system temp directory. */
auto makeTempDirectory(const char* name) -> std::filesystem::path
{
    const auto directory = std::filesystem::temp_directory_path() / "starcore-tests" /
                           std::format("{}-{}", name, Rng(static_cast<std::uint32_t>(
                                                              std::hash<std::string>{}(name)))
                                                          .nextUint());
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    return directory;
}

auto makeTestModel() -> Model
{
    Model model;
    const auto hull = model.addMaterial(
        "Hull", MaterialComponent{.albedo = {0.4F, 0.45F, 0.5F}, .roughness = 0.35F,
                                  .metallic = 1.0F});
    const auto glass = model.addMaterial(
        "Glass", MaterialComponent{.albedo = Vec3(1.0F), .roughness = 0.0F,
                                   .transmission = 1.0F, .ior = 1.45F, .thinWalled = true});
    const auto light = model.addMaterial(
        "Light", MaterialComponent{.emissionColor = {2.0F, 1.0F, 0.5F}, .emissionPower = 3.0F});

    const auto box = model.addMesh("Box", primitives::box({1, 0.25F, 0.5F}).build(), hull);
    const auto canopy = model.addMesh(
        "Canopy", primitives::sphere(1.0F, 24, 12, {0.0F, kPi / 2}).build(), glass);
    const auto lamp = model.addMesh("Lamp", primitives::quad({0.2F, 0.1F}).build(), light);

    const auto root = model.addNode("Cockpit");
    const auto dashboard =
        model.addNode("Dashboard",
                      TransformComponent{.translation = {0, -0.5F, -1},
                                         .rotation = glm::angleAxis(0.3F, Vec3{1, 0, 0})},
                      box, root);
    model.addNode("Lamp", TransformComponent{.translation = {0, 0.3F, 0.4F}}, lamp, dashboard);
    model.addNode("Canopy", TransformComponent{.scale = {1.2F, 0.8F, 1.5F}}, canopy, root);
    return model;
}

void requireSameMesh(const MeshData& a, const MeshData& b)
{
    REQUIRE(a.positions == b.positions);
    REQUIRE(a.indices == b.indices);
    REQUIRE(a.normals.size() == b.normals.size());
    for (std::size_t i = 0; i < a.normals.size(); ++i) {
        REQUIRE(glm::distance(a.normals[i], b.normals[i]) < 1e-6F);
    }
}

}  // namespace

TEST_CASE("Models round-trip through binary glTF", "[asset][model]")
{
    const auto directory = makeTempDirectory("gltf-roundtrip");
    const Model original = makeTestModel();
    const auto path = directory / "cockpit.glb";
    REQUIRE(saveModel(original, path).has_value());

    const auto loaded = loadModel(path);
    REQUIRE(loaded.has_value());
    const Model& model = *loaded;

    REQUIRE(model.materials.size() == 3);
    const MaterialComponent& hull = model.materials[0].material;
    REQUIRE(model.materials[0].name == "Hull");
    REQUIRE(hull.metallic == 1.0F);
    REQUIRE_THAT(hull.roughness, WithinAbs(0.35, 1e-6));
    const MaterialComponent& glass = model.materials[1].material;
    REQUIRE(glass.transmission == 1.0F);
    REQUIRE(glass.thinWalled);
    REQUIRE_THAT(glass.ior, WithinAbs(1.45, 1e-6));
    // Emission > 1 survives (stored as a [0, 1] factor + emissive strength).
    const MaterialComponent& light = model.materials[2].material;
    REQUIRE(glm::distance(light.emission(), Vec3{6.0F, 3.0F, 1.5F}) < 1e-5F);

    REQUIRE(model.meshes.size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        requireSameMesh(*original.meshes[i].mesh, *model.meshes[i].mesh);
        REQUIRE(model.meshes[i].material == original.meshes[i].material);
    }
    REQUIRE(model.meshes[0].name == original.meshes[0].name);

    REQUIRE(model.nodes.size() == 4);
    REQUIRE(model.roots == std::vector<std::uint32_t>{0});
    REQUIRE(model.nodes[0].children == std::vector<std::uint32_t>{1, 3});
    REQUIRE_FALSE(model.nodes[0].mesh.has_value());
    REQUIRE(model.nodes[1].name == "Dashboard");
    REQUIRE(model.nodes[1].mesh == 0U);
    REQUIRE(model.nodes[1].children == std::vector<std::uint32_t>{2});
    REQUIRE(model.nodes[1].transform.translation == Vec3{0, -0.5F, -1});
    REQUIRE(glm::all(glm::epsilonEqual(model.nodes[1].transform.rotation,
                                       original.nodes[1].transform.rotation, 1e-6F)));
    REQUIRE(model.nodes[2].mesh == 2U);
    REQUIRE(model.nodes[3].mesh == 1U);
    REQUIRE(model.nodes[3].transform.scale == Vec3{1.2F, 0.8F, 1.5F});
}

TEST_CASE("Model loading and saving report errors", "[asset][model]")
{
    const auto directory = makeTempDirectory("gltf-errors");
    REQUIRE_FALSE(loadModel(directory / "missing.glb").has_value());
    REQUIRE_FALSE(saveModel(makeTestModel(), directory / "cockpit.gltf").has_value());

    std::ofstream(directory / "garbage.glb") << "definitely not glTF";
    REQUIRE_FALSE(loadModel(directory / "garbage.glb").has_value());

    Model cyclic = makeTestModel();
    cyclic.nodes[1].children.push_back(0);
    REQUIRE_FALSE(cyclic.validate().has_value());
    Model badMesh = makeTestModel();
    badMesh.nodes[2].mesh = 42;
    REQUIRE_FALSE(badMesh.validate().has_value());
}

TEST_CASE("Instantiated models are entity trees referencing the model", "[asset][model]")
{
    const Model model = makeTestModel();
    Scene scene;
    Entity ship = scene.createEntity("Ship");
    ship.get<TransformComponent>().translation = {10, 0, 0};

    const auto roots = instantiateModel(scene, model, "assets/models/cockpit.glb", ship);
    REQUIRE(roots.has_value());
    REQUIRE(roots->size() == 1);
    Entity cockpit = roots->front();
    REQUIRE(cockpit.name() == "Cockpit");
    REQUIRE(scene.parentOf(cockpit) == ship);
    // The ship plus one entity per node.
    REQUIRE(scene.entityCount() == 1 + 4);

    REQUIRE_FALSE(cockpit.has<MeshComponent>());
    Entity box = scene.findEntityByName("Dashboard");
    Entity lamp = scene.findEntityByName("Lamp");
    REQUIRE(scene.parentOf(box) == cockpit);
    REQUIRE(scene.parentOf(lamp) == box);
    REQUIRE(lamp.get<MaterialComponent>().emissionPower == 3.0F);
    REQUIRE(box.get<MeshComponent>().mesh == model.meshes[0].mesh);  // Shared, not copied.
    REQUIRE(box.get<MaterialComponent>().metallic == 1.0F);
    REQUIRE(box.get<MeshComponent>().source.meshIndex == 0);
    REQUIRE_THAT(box.get<MeshComponent>().source.path,
                 ContainsSubstring("assets/models/cockpit.glb"));
    REQUIRE(std::filesystem::path(box.get<MeshComponent>().source.path).is_absolute());

    Entity canopy = scene.findEntityByName("Canopy");
    REQUIRE(canopy.get<MaterialComponent>().transmission == 1.0F);
    // Local transforms come from the model; world = ship * model.
    REQUIRE(canopy.get<TransformComponent>().scale == Vec3{1.2F, 0.8F, 1.5F});
    REQUIRE_THAT(Vec3(canopy.worldTransform()[3]).x, WithinAbs(10.0, 1e-5));
}

TEST_CASE("Scene files reference model meshes by relative path", "[asset][model][serializer]")
{
    const auto directory = makeTempDirectory("scene-references");
    const auto modelPath = directory / "models" / "cockpit.glb";
    REQUIRE(saveModel(makeTestModel(), modelPath).has_value());

    AssetLibrary assets;
    const auto model = assets.loadModel(modelPath);
    REQUIRE(model.has_value());
    REQUIRE(assets.loadModel(modelPath).value() == *model);  // Cached.

    Scene scene;
    REQUIRE(instantiateModel(scene, **model, modelPath.string(), Entity{}).has_value());
    Entity inline_ = scene.createEntity("Inline");
    inline_.add<MeshComponent>(MeshComponent{.mesh = primitives::box(Vec3(1.0F)).build()});

    const auto scenePath = directory / "scenes" / "test.yaml";
    REQUIRE(saveScene(scene, scenePath).has_value());
    std::stringstream text;
    text << std::ifstream(scenePath).rdbuf();
    REQUIRE_THAT(text.str(), ContainsSubstring("Model: ../models/cockpit.glb"));
    REQUIRE_THAT(text.str(), ContainsSubstring("Positions:"));  // The inline mesh.

    Scene loaded;
    REQUIRE(loadScene(scenePath, loaded, &assets).has_value());
    REQUIRE(loaded.entityCount() == scene.entityCount());
    REQUIRE(loaded.findEntityByName("Dashboard").get<MeshComponent>().mesh ==
            (*model)->meshes[0].mesh);
    REQUIRE(loaded.findEntityByName("Inline").get<MeshComponent>().mesh->triangleCount() == 12);
    REQUIRE(serializeScene(loaded, scenePath.parent_path()) ==
            serializeScene(scene, scenePath.parent_path()));

    // Without a library the model is loaded again (a temporary cache).
    Scene again;
    REQUIRE(loadScene(scenePath, again).has_value());
    REQUIRE(again.findEntityByName("Canopy").get<MeshComponent>().mesh->triangleCount() ==
            (*model)->meshes[1].mesh->triangleCount());

    // A missing model is an error and leaves the scene empty.
    std::filesystem::remove(modelPath);
    Scene broken;
    const auto result = loadScene(scenePath, broken);
    REQUIRE_FALSE(result.has_value());
    REQUIRE_THAT(result.error(), ContainsSubstring("cockpit.glb"));
    REQUIRE(broken.entityCount() == 0);
}
