// Prepare reproducible pilot-view scenes with the game's actual ship and cabin light.
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

#include <CLI/CLI.hpp>
#include <glm/gtc/quaternion.hpp>
#include <starcore/asset/model.hpp>
#include <starcore/render/camera.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/scene/serializer.hpp>

#include "gameplay/ship.hpp"

using namespace starcore;

namespace
{
auto vectorJson(Vec3 value) -> std::string
{
    return std::format("[{},{},{}]", value.x, value.y, value.z);
}

auto cameraJson(const Camera& camera) -> std::string
{
    return std::format("{{\"position\":{},\"look_at\":{},\"yaw\":{},"
                       "\"pitch\":{},\"fov\":{}}}",
        vectorJson(camera.position()), vectorJson(camera.position() + camera.forward()),
        camera.yaw(), camera.pitch(), camera.verticalFov());
}
}  // namespace

auto main(int argc, char** argv) -> int
{
    CLI::App app{"Prepare initial/end evaluation scenes with the game's pilot-view ship"};
    std::filesystem::path scenePath, modelPath, output;
    std::vector<float> position{0.0F, 0.0F, 0.0F}, move{0.0F, 0.0F, 0.0F};
    std::vector<float> freeCamera;
    float viewYaw = -90.0F, viewPitch = 0.0F, fov = 70.0F;
    float yaw = -90.0F, pitch = 0.0F, turn = 0.0F;
    std::uint32_t frames = 81;
    app.add_option("--scene", scenePath)->required()->check(CLI::ExistingFile);
    app.add_option("--model", modelPath)->required()->check(CLI::ExistingFile);
    app.add_option("--output", output)->required();
    app.add_option("--position", position)->expected(3);
    app.add_option("--yaw", yaw);
    app.add_option("--pitch", pitch)->check(CLI::Range(-89.0F, 89.0F));
    app.add_option("--move", move)->expected(3);
    app.add_option("--turn", turn, "Yaw degrees per update, as --turn-entity Ship");
    app.add_option("--frames", frames)->check(CLI::Range(2U, 100000U));
    app.add_option("--free-camera", freeCamera)->expected(3);
    app.add_option("--view-yaw", viewYaw);
    app.add_option("--view-pitch", viewPitch)->check(CLI::Range(-89.0F, 89.0F));
    app.add_option("--fov", fov)->check(CLI::Range(1.0F, 179.0F));
    CLI11_PARSE(app, argc, argv);
    try {
        scenePath = std::filesystem::absolute(scenePath).lexically_normal();
        modelPath = std::filesystem::absolute(modelPath).lexically_normal();
        output = std::filesystem::absolute(output).lexically_normal();
        std::filesystem::create_directories(output);
        Scene scene;
        AssetLibrary assets;
        if (auto loaded = loadScene(scenePath, scene, &assets); !loaded) {
            throw std::runtime_error(loaded.error());
        }
        if (scene.findEntityByName("Ship")) {
            throw std::runtime_error("Input scene already contains Ship");
        }
        startraveler::ShipSystem ships;
        startraveler::configureGameLayers(scene.layers());
        auto spawned = ships.spawn(scene, assets, modelPath,
            Vec3(position[0], position[1], position[2]), yaw, pitch);
        if (!spawned) {
            throw std::runtime_error(spawned.error());
        }
        const Entity ship = *spawned;
        ship.remove<TransientComponent>();  // Persist this otherwise transient game entity tree.
        Camera camera;
        ships.attachCamera(camera, ship);
        if (freeCamera.size() == 3) {
            camera.setPosition(Vec3(freeCamera[0], freeCamera[1], freeCamera[2]));
            camera.setRotation(viewYaw, viewPitch);
            camera.setVerticalFov(fov);
        }
        const std::string initialCamera = cameraJson(camera);
        const auto initial = output / "initial.yaml";
        const auto end = output / "end.yaml";
        const bool overwritesScene =
            scenePath.filename() == initial.filename() || scenePath.filename() == end.filename();
        if (std::filesystem::equivalent(scenePath.parent_path(), output) && overwritesScene) {
            throw std::runtime_error("Output would overwrite the source scene");
        }
        if (auto saved = saveScene(scene, initial); !saved) {
            throw std::runtime_error(saved.error());
        }
        const Vec3 delta(move[0], move[1], move[2]);
        // Match render.cpp's update count and order. The first frame never moves.
        for (std::uint32_t i = 1; i < frames; ++i) {
            camera.setPosition(camera.position() + delta);
            ship.patch<TransformComponent>([&](TransformComponent& t) { t.translation += delta; });
            if (turn != 0.0F) {
                const Quat rotation = glm::angleAxis(glm::radians(-turn), Vec3(0.0F, 1.0F, 0.0F));
                Vec3 pivot(0.0F);
                ship.patch<TransformComponent>([&](TransformComponent& t) {
                    pivot = t.translation;
                    t.rotation = rotation * t.rotation;
                });
                camera.setPosition(pivot + rotation * (camera.position() - pivot));
                camera.setRotation(camera.yaw() + turn, camera.pitch());
            }
        }
        if (auto saved = saveScene(scene, end); !saved) {
            throw std::runtime_error(saved.error());
        }
        std::ofstream metadata(output / "poses.json");
        metadata << std::format("{{\"motion_updates\":{},\"initial\":{},\"end\":{}}}\n",
            frames - 1, initialCamera, cameraJson(camera));
        metadata.close();
        if (!metadata) {
            throw std::runtime_error("Could not write poses.json");
        }
        std::cout << "Prepared " << frames - 1 << " updates in " << output << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
