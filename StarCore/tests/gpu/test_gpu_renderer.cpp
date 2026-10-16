// GPU renderer vs the CPU reference. The GPU path tracer runs the same
// estimator with the same random streams, so the images agree almost bit for
// bit (on an RTX 4060 and an Intel Arc iGPU, 8x8 block means differ by at most
// 1/64). Other GPUs may round differently and let paths diverge, so the
// tolerances below are statistical; primary visibility (entity ids) must agree
// almost everywhere.
//
// STARCORE_VULKAN_DEVICE=Intel (any part of a device name) picks another GPU.

#include <algorithm>
#include <memory>
#include <utility>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <glm/common.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <starcore/asset/mesh.hpp>
#include <starcore/gpu/gpu_renderer.hpp>
#include <starcore/math/color.hpp>
#include <starcore/render/path_tracer.hpp>
#include <starcore/scene/entity.hpp>
#include <starcore/scene/layers.hpp>
#include <starcore/vulkan/context.hpp>

using namespace starcore;

namespace
{

/** @brief One headless context for all tests (nullptr without a usable GPU). */
auto gpuContext() -> vulkan::Context*
{
    static const std::unique_ptr<vulkan::Context> context = [] {
        auto created = vulkan::Context::create(
            {.applicationName = "StarCoreGpuTests", .enableValidation = true});
        return created ? std::move(*created) : nullptr;
    }();
    return context.get();
}

/** @brief A context of the same GPU with ray queries left disabled. */
auto contextWithoutRayQueries() -> vulkan::Context*
{
    static const std::unique_ptr<vulkan::Context> context = [] {
        auto created = vulkan::Context::create({.applicationName = "StarCoreGpuTests",
                                                .enableValidation = true,
                                                .enableRayQuery = false});
        return created ? std::move(*created) : nullptr;
    }();
    return context.get();
}

/**
 * @brief Skips the test without a GPU renderer (or ray queries, if asked);
 *        fails it on new validation errors.
 */
class GpuTest
{
public:
    explicit GpuTest(bool rayTracing = true, vulkan::Context* context = gpuContext())
        : m_context(context)
    {
        if (m_context == nullptr || !m_context->deviceInfo().pushDescriptors) {
            m_context = nullptr;
            SKIP("no Vulkan device for the GPU renderer");
        }
        if (rayTracing && !m_context->deviceInfo().rayQuery) {
            m_context = nullptr;
            SKIP("no Vulkan device with ray query support");
        }
        m_errors = m_context->validationErrors();
    }
    ~GpuTest()
    {
        if (m_context != nullptr) {
            CHECK(m_context->validationErrors() == m_errors);
        }
    }
    GpuTest(const GpuTest&) = delete;
    auto operator=(const GpuTest&) -> GpuTest& = delete;

    [[nodiscard]] auto context() const -> vulkan::Context&
    {
        return *m_context;
    }
    [[nodiscard]] auto makeRenderer(std::uint32_t width, std::uint32_t height) const
        -> std::unique_ptr<GpuRenderer>
    {
        auto tracer = GpuRenderer::create(*m_context);
        REQUIRE(tracer.has_value());
        (*tracer)->resize(width, height);
        return std::move(*tracer);
    }

private:
    vulkan::Context* m_context;
    std::uint32_t m_errors = 0;
};

auto makeCamera(std::uint32_t width, std::uint32_t height) -> Camera
{
    Camera camera;  // At (0, 0, 10) looking down -Z.
    camera.setViewport(width, height);
    return camera;
}

auto channel(std::uint32_t pixel, int index) -> int
{
    return static_cast<int>((pixel >> (8 * index)) & 0xFFU);
}

/** @brief Mean of each RGB channel over a block (in 8-bit units). */
auto blockMean(const ColorImage& image, std::uint32_t x0, std::uint32_t y0, std::uint32_t size)
    -> Vec3
{
    Vec3 sum{0.0F};
    for (std::uint32_t y = y0; y < y0 + size; ++y) {
        for (std::uint32_t x = x0; x < x0 + size; ++x) {
            const std::uint32_t pixel = image.at(x, y);
            sum += Vec3(channel(pixel, 0), channel(pixel, 1), channel(pixel, 2));
        }
    }
    return sum / static_cast<float>(size * size);
}

/** @brief Mean absolute difference of the RGB channels (in 8-bit units). */
auto meanDifference(const ColorImage& a, const ColorImage& b) -> float
{
    REQUIRE(a.size() == b.size());
    double sum = 0.0;
    for (std::size_t i = 0; i < a.pixels().size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            sum += std::abs(channel(a.pixels()[i], c) - channel(b.pixels()[i], c));
        }
    }
    return static_cast<float>(sum / (3.0 * static_cast<double>(a.pixels().size())));
}

/** @brief Mean linear value of all channels (how much light an image shows). */
auto meanLinear(const ColorImage& image) -> double
{
    double sum = 0.0;
    for (const std::uint32_t pixel : image.pixels()) {
        for (int c = 0; c < 3; ++c) {
            sum += srgbToLinear(static_cast<float>(channel(pixel, c)) / 255.0F);
        }
    }
    return sum / (3.0 * static_cast<double>(image.pixels().size()));
}

/** @brief Worst and mean differences of 8x8 block means (in 8-bit units). */
struct BlockDifference
{
    float worst = 0.0F;
    float mean = 0.0F;  // Of the image means, per channel, worst channel.
};

auto blockDifference(const ColorImage& a, const ColorImage& b) -> BlockDifference
{
    constexpr std::uint32_t kBlock = 8;
    REQUIRE(a.size() == b.size());
    Vec3 totalA{0.0F};
    Vec3 totalB{0.0F};
    BlockDifference result;
    float blocks = 0.0F;
    for (std::uint32_t y = 0; y + kBlock <= a.height(); y += kBlock) {
        for (std::uint32_t x = 0; x + kBlock <= a.width(); x += kBlock) {
            const Vec3 meanA = blockMean(a, x, y, kBlock);
            const Vec3 meanB = blockMean(b, x, y, kBlock);
            totalA += meanA;
            totalB += meanB;
            const Vec3 difference = glm::abs(meanA - meanB);
            result.worst = std::max({result.worst, difference.x, difference.y, difference.z});
            blocks += 1.0F;
        }
    }
    const Vec3 meanDifference = glm::abs(totalA - totalB) / blocks;
    result.mean = std::max({meanDifference.x, meanDifference.y, meanDifference.z});
    return result;
}

/** @brief Fraction of pixels whose ids differ. */
auto idMismatch(const IdImage& a, const IdImage& b) -> double
{
    REQUIRE(a.size() == b.size());
    std::size_t different = 0;
    for (std::size_t i = 0; i < a.pixels().size(); ++i) {
        different += a.pixels()[i] != b.pixels()[i] ? 1 : 0;
    }
    return static_cast<double>(different) / static_cast<double>(a.pixels().size());
}

/**
 * @brief A small room: diffuse floor, a mirror ball, a glass ball, a rough
 *        gold cube (mesh), a window pane, two sphere lights and a dim sky.
 */
struct TestScene
{
    Scene scene;
    Entity floor;
    Entity mirror;
    Entity glass;
    Entity cube;
    Entity pane;
    Entity hidden;

    TestScene()
    {
        scene.environment().skyColor = {0.05F, 0.07F, 0.1F};

        floor = scene.createEntity("Floor");
        floor.get<TransformComponent>().translation = {0.0F, -1.5F, 0.0F};
        floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
        floor.add<MaterialComponent>(MaterialComponent{.albedo = {0.7F, 0.7F, 0.7F}});

        mirror = scene.createEntity("Mirror");
        mirror.get<TransformComponent>().translation = {-2.2F, 0.0F, 0.0F};
        mirror.add<SphereComponent>(1.0F);
        mirror.add<MaterialComponent>(
            MaterialComponent{.albedo = {0.9F, 0.9F, 0.9F}, .roughness = 0.05F, .metallic = 1.0F});

        glass = scene.createEntity("Glass");
        glass.get<TransformComponent>().translation = {0.3F, -0.5F, 1.5F};
        glass.add<SphereComponent>(0.8F);
        glass.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .ior = 1.5F});

        cube = scene.createEntity("Cube");
        cube.get<TransformComponent>().translation = {2.2F, -0.5F, -0.5F};
        cube.get<TransformComponent>().rotation = Quat(Vec3(0.3F, 0.6F, 0.0F));
        cube.add<MeshComponent>(MeshComponent{.mesh = primitives::box(Vec3(0.8F)).build()});
        cube.add<MaterialComponent>(
            MaterialComponent{.albedo = {1.0F, 0.78F, 0.34F}, .roughness = 0.4F, .metallic = 1.0F});

        pane = scene.createEntity("Pane");
        pane.get<TransformComponent>().translation = {-1.5F, 0.5F, 4.0F};
        pane.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(1.2F)).build()});
        pane.add<MaterialComponent>(MaterialComponent{.albedo = {0.8F, 0.9F, 1.0F},
                                                      .roughness = 0.0F,
                                                      .transmission = 1.0F,
                                                      .thinWalled = true});

        for (const auto& [x, power] : {std::pair{-3.0F, 30.0F}, std::pair{3.5F, 20.0F}}) {
            Entity light = scene.createEntity("Light");
            light.get<TransformComponent>().translation = {x, 4.0F, 2.0F};
            light.add<SphereComponent>(0.6F);
            light.add<MaterialComponent>(MaterialComponent{
                .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = power});
        }

        // On layer 3, which the tests hide through the instance mask.
        hidden = scene.createEntity("Hidden");
        hidden.get<TransformComponent>().translation = {0.0F, 1.5F, 2.0F};
        hidden.add<SphereComponent>(0.7F);
        scene.setLayer(hidden, 3);
    }
};

}  // namespace

TEST_CASE("GPU: emissive sphere on a sky", "[gpu]")
{
    const GpuTest gpu;
    Scene scene;
    scene.environment().skyColor = {0.0F, 0.0F, 1.0F};
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(2.0F);
    ball.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = {1.0F, 0.0F, 0.0F}, .emissionPower = 1.0F});

    auto tracer = gpu.makeRenderer(64, 48);
    tracer->render(buildRenderScene(scene), makeCamera(64, 48), PathTracerSettings{});
    const ColorImage image = tracer->readImage();
    const IdImage ids = tracer->readEntityIds();
    REQUIRE(image.at(32, 24) == packRgba8({1.0F, 0.0F, 0.0F, 1.0F}));
    REQUIRE(image.at(0, 0) == packRgba8({0.0F, 0.0F, 1.0F, 1.0F}));
    REQUIRE(ids.at(32, 24) == entt::to_integral(ball.handle()));
    REQUIRE(ids.at(0, 0) == kNoEntityId);
    REQUIRE(tracer->accumulatedFrames() == 1);
}

TEST_CASE("GPU: white furnace", "[gpu]")
{
    const GpuTest gpu;
    Scene scene;
    scene.environment().skyColor = Vec3(0.5F);
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(3.0F);
    ball.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(1.0F)});

    auto tracer = gpu.makeRenderer(32, 32);
    const RenderScene renderScene = buildRenderScene(scene);
    const PathTracerSettings settings{.maxBounces = 16, .srgbOutput = false};
    for (int i = 0; i < 4; ++i) {
        tracer->render(renderScene, makeCamera(32, 32), settings);
    }
    const ColorImage image = tracer->readImage();
    const IdImage ids = tracer->readEntityIds();
    REQUIRE(ids.at(16, 16) == entt::to_integral(ball.handle()));
    for (const std::uint32_t pixel : image.pixels()) {
        REQUIRE(channel(pixel, 0) == 128);
        REQUIRE(channel(pixel, 1) == 128);
        REQUIRE(channel(pixel, 2) == 128);
    }
}

TEST_CASE("GPU: primary visibility matches the CPU", "[gpu]")
{
    const GpuTest gpu;
    TestScene test;
    const RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 160;
    constexpr std::uint32_t kHeight = 120;
    const Camera camera = makeCamera(kWidth, kHeight);

    for (const std::uint32_t mask : {kAllInstances, kAllInstances & ~layerBit(3)}) {
        const PathTracerSettings settings{.mode = RenderMode::Preview, .instanceMask = mask};
        CpuPathTracer cpu;
        cpu.resize(kWidth, kHeight);
        cpu.render(renderScene, camera, settings);
        auto tracer = gpu.makeRenderer(kWidth, kHeight);
        tracer->render(renderScene, camera, settings);
        const IdImage ids = tracer->readEntityIds();

        // Only pixels on silhouettes may differ (jitter is the same, rounding is not).
        CHECK(idMismatch(ids, cpu.entityIds()) < 0.005);
        const bool hiddenVisible = std::ranges::contains(
            ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.hidden.handle())));
        CHECK(hiddenVisible == (mask == kAllInstances));
        // The pane is looked through, the cube behind... is not.
        CHECK_FALSE(std::ranges::contains(
            ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.pane.handle()))));
        CHECK(std::ranges::contains(
            ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.cube.handle()))));
    }
}

TEST_CASE("GPU: path tracing converges to the CPU image", "[gpu]")
{
    const GpuTest gpu;
    TestScene test;
    RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 48;
    constexpr std::uint32_t kBlock = 8;
    const Camera camera = makeCamera(kWidth, kHeight);

    auto compare = [&](const PathTracerSettings& settings, std::uint32_t frames,
                       float blockTolerance, float meanTolerance) {
        renderScene.updateTlas();
        CpuPathTracer cpu;
        cpu.resize(kWidth, kHeight);
        auto tracer = gpu.makeRenderer(kWidth, kHeight);
        for (std::uint32_t i = 0; i < frames; ++i) {
            cpu.render(renderScene, camera, settings);
            tracer->render(renderScene, camera, settings);
        }
        REQUIRE(tracer->accumulatedFrames() == frames);
        const ColorImage gpuImage = tracer->readImage();
        Vec3 cpuTotal{0.0F};
        Vec3 gpuTotal{0.0F};
        float worst = 0.0F;
        for (std::uint32_t y = 0; y < kHeight; y += kBlock) {
            for (std::uint32_t x = 0; x < kWidth; x += kBlock) {
                const Vec3 a = blockMean(cpu.image(), x, y, kBlock);
                const Vec3 b = blockMean(gpuImage, x, y, kBlock);
                cpuTotal += a;
                gpuTotal += b;
                const Vec3 difference = glm::abs(a - b);
                worst = std::max({worst, difference.x, difference.y, difference.z});
            }
        }
        const float blocks = static_cast<float>((kWidth / kBlock) * (kHeight / kBlock));
        INFO("CPU mean " << cpuTotal.x / blocks << ", " << cpuTotal.y / blocks << ", "
                         << cpuTotal.z / blocks << "; GPU mean " << gpuTotal.x / blocks << ", "
                         << gpuTotal.y / blocks << ", " << gpuTotal.z / blocks
                         << "; worst block difference " << worst);
        CHECK(worst < blockTolerance);
        const Vec3 meanDifference = glm::abs(cpuTotal - gpuTotal) / blocks;
        CHECK(std::max({meanDifference.x, meanDifference.y, meanDifference.z}) < meanTolerance);
    };

    SECTION("path traced with light sampling")
    {
        compare(PathTracerSettings{.samplesPerPixel = 4, .seed = 7}, 64, 12.0F, 2.0F);
    }
    SECTION("path traced without light sampling, clamped")
    {
        compare(PathTracerSettings{.samplesPerPixel = 4,
                                   .nextEventEstimation = false,
                                   .toneMapper = ToneMapper::Aces,
                                   .fireflyClamp = 4.0F},
                64, 14.0F, 2.5F);
    }
    SECTION("preview (deterministic apart from the jitter)")
    {
        compare(PathTracerSettings{.mode = RenderMode::Preview}, 16, 3.0F, 0.6F);
    }
    SECTION("glowing meshes sampled as lights")
    {
        // A glowing panel over the scene and a small glowing box beside the cube.
        Entity panel = test.scene.createEntity("Panel");
        panel.get<TransformComponent>().translation = {0.0F, 2.5F, 0.5F};
        panel.get<TransformComponent>().rotation = Quat(Vec3(0.5F * kPi, 0.0F, 0.0F));
        panel.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(2.0F, 1.0F)).build()});
        panel.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(0.0F), .emissionColor = {1.0F, 0.6F, 0.3F}, .emissionPower = 6.0F});
        Entity box = test.scene.createEntity("Box");
        box.get<TransformComponent>().translation = {1.0F, -1.2F, 0.8F};
        box.add<MeshComponent>(MeshComponent{.mesh = primitives::box(Vec3(0.2F)).build()});
        box.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(0.5F), .emissionColor = {0.3F, 0.6F, 1.0F}, .emissionPower = 4.0F});
        renderScene = buildRenderScene(test.scene);
        REQUIRE(renderScene.lights.size() == 4);
        compare(PathTracerSettings{.samplesPerPixel = 4, .seed = 7}, 64, 12.0F, 2.0F);
    }
}

TEST_CASE("GPU: accumulation restarts and scenes update", "[gpu]")
{
    const GpuTest gpu;
    TestScene test;
    constexpr std::uint32_t kSize = 48;
    Camera camera = makeCamera(kSize, kSize);
    auto tracer = gpu.makeRenderer(kSize, kSize);
    const PathTracerSettings settings;

    // Before the first frame: black, no entities.
    const ColorImage blank = tracer->readImage();
    REQUIRE(blank.at(5, 5) == packRgba8({0.0F, 0.0F, 0.0F, 1.0F}));
    const IdImage noIds = tracer->readEntityIds();
    REQUIRE(noIds.at(5, 5) == kNoEntityId);

    RenderScene renderScene = buildRenderScene(test.scene);
    tracer->render(renderScene, camera, settings);
    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->accumulatedFrames() == 2);

    // More samples per frame keep accumulating (frames are weighted by samples).
    PathTracerSettings more = settings;
    more.samplesPerPixel = 4;
    tracer->render(renderScene, camera, more);
    REQUIRE(tracer->accumulatedFrames() == 3);
    tracer->waitIdle();
    if (gpu.context().deviceInfo().timestampPeriod > 0.0F) {
        REQUIRE(tracer->gpuTimeMs() > 0.0F);
        // A generous budget asks for more samples, at most twice the last frame's.
        REQUIRE(tracer->samplesForBudget(1000.0F) == 8);
        REQUIRE(tracer->samplesForBudget(1e-6F) == 1);
    }

    // Moving the camera restarts.
    camera.setPosition({0.0F, 0.0F, 9.0F});
    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->accumulatedFrames() == 1);

    // Moving an entity restarts and is visible on the GPU.
    const auto mirrorId = static_cast<std::uint32_t>(entt::to_integral(test.mirror.handle()));
    auto mirrorPixels = [&] {
        const IdImage ids = tracer->readEntityIds();
        return std::ranges::count(ids.pixels(), mirrorId);
    };
    tracer->render(renderScene, camera, settings);
    const auto before = mirrorPixels();
    REQUIRE(before > 0);
    test.mirror.get<TransformComponent>().translation = {-30.0F, 0.0F, 0.0F};
    test.scene.markDirty();
    renderScene = buildRenderScene(test.scene);
    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->accumulatedFrames() == 1);
    REQUIRE(mirrorPixels() == 0);

    // Removing every mesh and adding new ones (BLAS cache churn) keeps working.
    for (int i = 0; i < 12; ++i) {
        test.cube.get<MeshComponent>().mesh =
            primitives::sphere(0.5F + 0.05F * static_cast<float>(i), 12, 8).build();
        test.scene.markDirty();
        tracer->render(buildRenderScene(test.scene), camera, settings);
    }
    const auto cubeId = static_cast<std::uint32_t>(entt::to_integral(test.cube.handle()));
    const IdImage ids = tracer->readEntityIds();
    REQUIRE(std::ranges::contains(ids.pixels(), cubeId));

    // An empty scene shows the sky.
    Scene empty;
    empty.environment().skyColor = {1.0F, 0.0F, 0.0F};
    tracer->render(buildRenderScene(empty), camera, settings);
    const ColorImage sky = tracer->readImage();
    REQUIRE(sky.at(10, 10) == packRgba8({1.0F, 0.0F, 0.0F, 1.0F}));

    // Resizing keeps rendering.
    tracer->resize(20, 10);
    tracer->render(renderScene, makeCamera(20, 10), settings);
    REQUIRE(tracer->readImage().size() == UVec2(20, 10));
}

TEST_CASE("GPU: scene updates upload only what changed", "[gpu]")
{
    const GpuTest gpu;
    TestScene test;
    constexpr std::uint32_t kSize = 64;
    const Camera camera = makeCamera(kSize, kSize);
    const PathTracerSettings settings{.mode = RenderMode::Preview};
    RenderSceneBuilder builder;
    const RenderScene& renderScene = builder.update(test.scene);
    const auto instanceCount = static_cast<std::uint32_t>(renderScene.instances.size());
    auto tracer = gpu.makeRenderer(kSize, kSize);

    // The same image as a renderer given the whole scene anew.
    auto requireUpToDate = [&] {
        auto fresh = gpu.makeRenderer(kSize, kSize);
        fresh->render(buildRenderScene(test.scene), camera, settings);
        CHECK(idMismatch(tracer->readEntityIds(), fresh->readEntityIds()) == 0.0);
        CHECK(meanDifference(tracer->readImage(), fresh->readImage()) == 0.0F);
    };

    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->uploadedInstances() == instanceCount);
    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->uploadedInstances() == 0);

    // Moving one entity uploads its instance.
    test.mirror.patch<TransformComponent>([](auto& t) { t.translation.y += 0.5F; });
    builder.update(test.scene);
    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->uploadedInstances() == 1);
    requireUpToDate();

    // New, changed and removed entities.
    Entity ball = test.scene.createEntity("Ball");
    ball.get<TransformComponent>().translation = {0.0F, 2.0F, 0.0F};
    ball.add<SphereComponent>(0.5F);
    builder.update(test.scene);
    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->uploadedInstances() == 1);
    test.cube.patch<MaterialComponent>([](auto& m) { m.albedo = {0.2F, 0.4F, 1.0F}; });
    test.scene.destroyEntity(test.glass);
    builder.update(test.scene);
    tracer->render(renderScene, camera, settings);
    REQUIRE(tracer->uploadedInstances() == 2);  // The cube and the instance moved into the gap.
    requireUpToDate();

    // Another render scene is uploaded whole.
    tracer->render(buildRenderScene(test.scene), camera, settings);
    REQUIRE(tracer->uploadedInstances() == instanceCount);
}

// Hybrid pipeline ------------------------------------------------------------------------------------

namespace
{

auto hybridOptions(bool spatial = true, TraceResolution resolution = TraceResolution::Full)
    -> GpuRenderOptions
{
    GpuRenderOptions options{.pipeline = GpuPipeline::Hybrid, .traceResolution = resolution};
    options.denoiser.spatial = spatial;
    return options;
}

/** @brief Options of a pipeline upscaled at a quality. */
auto upscaled(GpuRenderOptions options, Upscaler upscaler, UpscalerQuality quality)
    -> GpuRenderOptions
{
    options.upscaling = {.upscaler = upscaler, .quality = quality};
    return options;
}

}  // namespace

TEST_CASE("GPU hybrid: emissive sphere, sky and white furnace", "[gpu][hybrid]")
{
    const GpuTest gpu;
    // Pixels not traced (half resolution) borrow what their neighbors found.
    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    INFO("trace resolution " << enumToString(resolution));
    for (const Vec3 sky : {Vec3(0.0F), Vec3(0.0F, 0.0F, 1.0F)}) {
        Scene scene;
        scene.environment().skyColor = sky;
        Entity ball = scene.createEntity("Ball");
        ball.add<SphereComponent>(2.0F);
        ball.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(0.0F), .emissionColor = {1.0F, 0.0F, 0.0F}, .emissionPower = 1.0F});
        auto tracer = gpu.makeRenderer(64, 48);
        tracer->render(buildRenderScene(scene), makeCamera(64, 48), {},
                       hybridOptions(true, resolution));
        const ColorImage image = tracer->readImage();
        const IdImage ids = tracer->readEntityIds();
        REQUIRE(image.at(0, 0) == packRgba8(Vec4(sky, 1.0F)));
        REQUIRE(ids.at(32, 24) == entt::to_integral(ball.handle()));
        REQUIRE(ids.at(0, 0) == kNoEntityId);
        if (sky == Vec3(0.0F)) {
            // Under a blue sky the ball reflects some of it (Fresnel), filtered over the ball.
            REQUIRE(image.at(32, 24) == packRgba8({1.0F, 0.0F, 0.0F, 1.0F}));
        }
    }
    {
        // Every path from the white ball reaches the sky: 0.5 per sample, filtered or
        // not (up to rounding: the filter works on half floats, GPUs round them
        // differently).
        Scene scene;
        scene.environment().skyColor = Vec3(0.5F);
        Entity ball = scene.createEntity("Ball");
        ball.add<SphereComponent>(3.0F);
        ball.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(1.0F)});
        auto tracer = gpu.makeRenderer(32, 32);
        const RenderScene renderScene = buildRenderScene(scene);
        const PathTracerSettings settings{.maxBounces = 16, .srgbOutput = false};
        for (int i = 0; i < 4; ++i) {
            tracer->render(renderScene, makeCamera(32, 32), settings,
                           hybridOptions(true, resolution));
        }
        const ColorImage image = tracer->readImage();
        for (const std::uint32_t pixel : image.pixels()) {
            for (int c = 0; c < 3; ++c) {
                REQUIRE(std::abs(channel(pixel, c) - 128) <= 1);
            }
        }
    }
}

TEST_CASE("GPU hybrid: rasterized visibility matches the path tracer", "[gpu][hybrid]")
{
    const GpuTest gpu;
    TestScene test;
    const RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 160;
    constexpr std::uint32_t kHeight = 120;
    const Camera camera = makeCamera(kWidth, kHeight);

    for (const std::uint32_t mask : {kAllInstances, kAllInstances & ~layerBit(3)}) {
        const PathTracerSettings settings{.instanceMask = mask};
        auto reference = gpu.makeRenderer(kWidth, kHeight);
        reference->render(renderScene, camera, settings);
        auto hybrid = gpu.makeRenderer(kWidth, kHeight);
        hybrid->render(renderScene, camera, settings, hybridOptions());
        const IdImage ids = hybrid->readEntityIds();

        // The two sample each pixel at a different position: silhouettes may differ.
        CHECK(idMismatch(ids, reference->readEntityIds()) < 0.03);
        const bool hiddenVisible = std::ranges::contains(
            ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.hidden.handle())));
        CHECK(hiddenVisible == (mask == kAllInstances));
        // The pane is in the glass layer, looked through.
        CHECK_FALSE(std::ranges::contains(
            ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.pane.handle()))));
        CHECK(std::ranges::contains(
            ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.cube.handle()))));
    }
}

TEST_CASE("GPU hybrid: a still view converges to the path tracer's image", "[gpu][hybrid]")
{
    const GpuTest gpu;
    TestScene test;
    const RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    const Camera camera = makeCamera(kWidth, kHeight);
    const PathTracerSettings settings{.samplesPerPixel = 8, .fireflyClamp = 8.0F};

    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 128; ++i) {
        reference->render(renderScene, camera, settings);
    }
    const ColorImage expected = reference->readImage();

    // Half resolution: each pixel's own samples, a quarter of them (the
    // neighbors' only count while its history is young).
    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    for (const bool spatial : {false, true}) {
        INFO("spatial filter: " << spatial << ", trace resolution " << enumToString(resolution));
        auto hybrid = gpu.makeRenderer(kWidth, kHeight);
        for (int i = 0; i < 128; ++i) {
            hybrid->render(renderScene, camera, settings, hybridOptions(spatial, resolution));
        }
        REQUIRE(hybrid->accumulatedFrames() == 128);
        const BlockDifference difference = blockDifference(hybrid->readImage(), expected);
        INFO("worst block " << difference.worst << ", mean " << difference.mean);
        // Anti-aliasing jitter and the (fading) filter differ a little on edges.
        CHECK(difference.worst < 10.0F);
        CHECK(difference.mean < 1.0F);
    }
}

TEST_CASE("GPU hybrid: samples are reused while moving and filtered", "[gpu][hybrid]")
{
    const GpuTest gpu;
    TestScene test;
    RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    const Camera camera = makeCamera(kWidth, kHeight);
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};
    Camera moved = camera;
    moved.setPosition(camera.position() + Vec3(0.08F, 0.04F, -0.2F));

    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 128; ++i) {
        reference->render(renderScene, moved,
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    const ColorImage expected = reference->readImage();

    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    // 32 still frames, then 4 frames moving to `moved`; or one frame there.
    auto render = [&](bool spatial, bool temporal) {
        GpuRenderOptions options = hybridOptions(spatial, resolution);
        options.denoiser.temporal = temporal;
        auto tracer = gpu.makeRenderer(kWidth, kHeight);
        if (temporal) {
            for (int i = 0; i < 32; ++i) {
                tracer->render(renderScene, camera, settings, options);
            }
            for (int i = 1; i <= 4; ++i) {
                Camera step = camera;
                step.setPosition(camera.position() +
                                 (moved.position() - camera.position()) * (0.25F * i));
                tracer->render(renderScene, step, settings, options);
            }
            REQUIRE(tracer->accumulatedFrames() == 36);  // Reprojected, not restarted.
        } else {
            tracer->render(renderScene, moved, settings, options);
        }
        return meanDifference(tracer->readImage(), expected);
    };
    const float reused = render(false, true);
    const float fresh = render(false, false);
    const float filtered = render(true, false);
    INFO("trace resolution " << enumToString(resolution) << ", mean error: reprojected "
                             << reused << ", one frame " << fresh << ", filtered " << filtered);
    // Full 0.47 and 0.58, Half 0.68 and 0.73: one frame is not that noisy any more,
    // the spheres' light unshadowed is in closed form (the direct layer); it was 0.39
    // and 0.51, 0.58 and 0.67 (the errors themselves were 2.28, 5.83 and 2.97 and
    // are 2.27, 4.87 and 2.80; Half, 2.88, 5.00 and 3.33 and are 2.81, 4.13 and 3.02).
    CHECK(reused < 0.75F * fresh);
    CHECK(filtered < 0.8F * fresh);
}

TEST_CASE("GPU hybrid: more samples per frame make the history smoother while moving",
          "[gpu][hybrid]")
{
    // The history cap counts frames: frames of more samples leave less noise (a cap of
    // samples would only make the history shorter). Of the primary surfaces' lighting.
    const GpuTest gpu;
    TestScene test;
    const RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    constexpr int kFrames = 24;  // Past the cap.
    auto cameraAt = [&](int frame) {
        Camera camera = makeCamera(kWidth, kHeight);
        camera.setPosition(camera.position() + Vec3(0.01F * static_cast<float>(frame), 0.0F, 0.0F));
        return camera;
    };
    GpuRenderOptions options = hybridOptions(false);  // The history's own noise.
    options.denoiser.historyFrames = 8;
    options.view = HybridView::Lighting;

    auto render = [&](std::uint32_t samples, std::uint32_t seed) {
        auto hybrid = gpu.makeRenderer(kWidth, kHeight);
        const PathTracerSettings settings{
            .samplesPerPixel = samples, .seed = seed, .fireflyClamp = 8.0F};
        for (int i = 0; i < kFrames; ++i) {
            hybrid->render(renderScene, cameraAt(i), settings, options);
        }
        REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
        return hybrid->readImage();
    };
    // Two runs of other random numbers differ by their noise only.
    auto noise = [&](std::uint32_t samples) {
        return meanDifference(render(samples, 1), render(samples, 2));
    };
    const float one = noise(1);
    const float four = noise(4);
    INFO("noise: 1 sample per frame " << one << ", 4 samples " << four);
    CHECK(four < 0.8F * one);  // 0.7 (with a cap of 8 samples: 0.96)
}

TEST_CASE("GPU hybrid: reflections leave no trail while moving", "[gpu][hybrid]")
{
    // A mirror floor and a window pane reflecting bright balls, the camera
    // sliding sideways: the reflections move unlike the surfaces reflecting
    // them. Reprojected with the surfaces' motion, their history would trail
    // behind them.
    const GpuTest gpu;
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity floor = scene.createEntity("Floor");
    floor.get<TransformComponent>().translation = {0.0F, -1.0F, 0.0F};
    floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(40.0F)).build()});
    floor.add<MaterialComponent>(
        MaterialComponent{.albedo = Vec3(0.9F), .roughness = 0.0F, .metallic = 1.0F});
    Entity pane = scene.createEntity("Pane");
    pane.get<TransformComponent>().translation = {0.6F, 0.8F, 5.0F};
    pane.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(2.4F, 1.4F)).build()});
    pane.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    // One in front, reflected by the floor; one behind the camera, by the pane (which
    // reflects a few percent).
    for (const auto& [position, power] :
         {std::pair{Vec3(0.0F, 0.3F, 0.0F), 4.0F}, std::pair{Vec3(0.6F, 0.8F, 13.0F), 40.0F}}) {
        Entity light = scene.createEntity("Light");
        light.get<TransformComponent>().translation = position;
        light.add<SphereComponent>(0.6F);
        light.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = power});
    }
    const RenderScene renderScene = buildRenderScene(scene);
    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 96;
    constexpr int kFrames = 24;
    auto cameraAt = [&](float x) {
        Camera camera = makeCamera(kWidth, kHeight);
        camera.setPosition({x, 1.2F, 9.0F});
        camera.setRotation(-90.0F, -10.0F);
        return camera;
    };
    const PathTracerSettings settings{.samplesPerPixel = 2};

    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < kFrames; ++i) {
        hybrid->render(renderScene, cameraAt(-0.8F + 1.6F * static_cast<float>(i) / (kFrames - 1)),
                       settings, hybridOptions());
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        reference->render(renderScene, cameraAt(0.8F), settings);
    }
    const ColorImage image = hybrid->readImage();
    const ColorImage expected = reference->readImage();

    // A trail: bright pixels with nothing bright near them in the reference (edges
    // may differ, anti-aliased by fewer samples).
    auto brightness = [](const ColorImage& from, std::uint32_t x, std::uint32_t y) {
        const std::uint32_t pixel = from.at(x, y);
        return std::max({channel(pixel, 0), channel(pixel, 1), channel(pixel, 2)});
    };
    std::uint32_t trail = 0;
    std::uint32_t lit = 0;
    for (std::uint32_t y = 2; y + 2 < kHeight; ++y) {
        for (std::uint32_t x = 2; x + 2 < kWidth; ++x) {
            int nearby = 0;
            for (std::uint32_t v = y - 2; v <= y + 2; ++v) {
                for (std::uint32_t u = x - 2; u <= x + 2; ++u) {
                    nearby = std::max(nearby, brightness(expected, u, v));
                }
            }
            lit += brightness(expected, x, y) > 96 ? 1 : 0;
            trail += brightness(image, x, y) > 96 && nearby < 48 ? 1 : 0;
        }
    }
    INFO(trail << " pixels of trails, " << lit << " lit");
    REQUIRE(lit > 200);  // Both balls and their reflections.
    CHECK(trail == 0);
}

TEST_CASE("GPU hybrid: reflected light that moves leaves no trail", "[gpu][hybrid]")
{
    // A light sweeping past a wall behind the camera, which a window pane in front
    // reflects (a canopy reflecting the sunlit patch of a cockpit as the ship turns),
    // and past a glossy metal ball seen through the pane (a highlight sliding along
    // it). Nothing moves on screen and the reflections' guides see the same surfaces:
    // only the temporal gradients of the reflections tell that their light moved on.
    const GpuTest gpu;
    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    INFO("trace resolution " << enumToString(resolution));
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity pane = scene.createEntity("Pane");
    pane.get<TransformComponent>().translation = {0.0F, 1.0F, 5.0F};
    pane.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(8.0F, 6.0F)).build()});
    pane.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    Entity wall = scene.createEntity("Wall");
    wall.get<TransformComponent>().translation = {0.0F, 1.0F, 13.0F};
    wall.get<TransformComponent>().rotation = Quat(Vec3(0.0F, kPi, 0.0F));
    wall.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(16.0F, 10.0F)).build()});
    wall.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.8F)});
    Entity ball = scene.createEntity("Ball");
    ball.get<TransformComponent>().translation = {0.0F, 1.0F, 2.0F};
    ball.add<SphereComponent>(1.0F);
    ball.add<MaterialComponent>(
        MaterialComponent{.albedo = Vec3(0.9F), .roughness = 0.3F, .metallic = 1.0F});
    Entity light = scene.createEntity("Light");
    light.add<SphereComponent>(0.15F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 400.0F});
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 96;
    constexpr int kFrames = 24;
    auto lightAt = [&](int frame) {
        light.get<TransformComponent>().translation = {
            -2.5F + 5.0F * static_cast<float>(frame) / (kFrames - 1), 1.0F, 12.0F};
        scene.markDirty();
        return &builder.update(scene);
    };
    Camera camera = makeCamera(kWidth, kHeight);
    camera.setPosition({0.0F, 1.0F, 9.0F});
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};

    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < kFrames; ++i) {
        hybrid->render(*lightAt(i), camera, settings, hybridOptions(true, resolution));
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        reference->render(builder.update(scene), camera,
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    const ColorImage image = hybrid->readImage();
    const ColorImage expected = reference->readImage();

    // A trail: bright pixels with nothing bright near them in the reference.
    auto brightness = [](const ColorImage& from, std::uint32_t x, std::uint32_t y) {
        const std::uint32_t pixel = from.at(x, y);
        return std::max({channel(pixel, 0), channel(pixel, 1), channel(pixel, 2)});
    };
    std::uint32_t trail = 0;
    std::uint32_t lit = 0;
    for (std::uint32_t y = 2; y + 2 < kHeight; ++y) {
        for (std::uint32_t x = 2; x + 2 < kWidth; ++x) {
            int nearby = 0;
            for (std::uint32_t v = y - 2; v <= y + 2; ++v) {
                for (std::uint32_t u = x - 2; u <= x + 2; ++u) {
                    nearby = std::max(nearby, brightness(expected, u, v));
                }
            }
            lit += brightness(expected, x, y) > 64 ? 1 : 0;
            trail += brightness(image, x, y) > 64 && nearby < 32 ? 1 : 0;
        }
    }
    INFO(trail << " pixels of trails, " << lit << " lit");
    REQUIRE(lit > 100);  // The reflected patch and the highlight.
    CHECK(trail < 10);
}

TEST_CASE("GPU hybrid: reflected light that keeps changing does not lag", "[gpu][hybrid]")
{
    // A big light far away swinging around (the sun as a ship turns): a wall behind
    // the camera, which a window pane in front reflects, gets dimmer by a few percent
    // every frame as its light falls ever more obliquely. Nothing moves on screen: the
    // reflection keeps a glow of the light it had unless its history follows the
    // gradients (in TraceResolution::Half, whose pixels wait for samples of their own
    // up to three frames, most of all).
    const GpuTest gpu;
    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    INFO("trace resolution " << enumToString(resolution));
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity pane = scene.createEntity("Pane");
    pane.get<TransformComponent>().translation = {0.0F, 1.0F, 5.0F};
    pane.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(8.0F, 6.0F)).build()});
    pane.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    Entity wall = scene.createEntity("Wall");
    wall.get<TransformComponent>().translation = {0.0F, 1.0F, 13.0F};
    wall.get<TransformComponent>().rotation = Quat(Vec3(0.0F, kPi, 0.0F));
    wall.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(16.0F, 10.0F)).build()});
    wall.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.8F)});
    Entity light = scene.createEntity("Light");
    light.add<SphereComponent>(4.0F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 100.0F});
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 96;
    constexpr int kFrames = 30;
    auto lightAt = [&](int frame) {
        const float angle = glm::radians(45.0F + 1.4F * static_cast<float>(frame));
        light.get<TransformComponent>().translation = {40.0F * std::sin(angle), 1.0F,
                                                       13.0F - 40.0F * std::cos(angle)};
        scene.markDirty();
        return &builder.update(scene);
    };
    Camera camera = makeCamera(kWidth, kHeight);
    camera.setPosition({0.0F, 1.0F, 9.0F});
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};
    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < kFrames; ++i) {
        hybrid->render(*lightAt(i), camera, settings, hybridOptions(true, resolution));
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        reference->render(builder.update(scene), camera,
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    // (Half the history at most dropped per frame, and pixels not traced showing
    // theirs: 1.19 and 1.39 times as bright.)
    const double bright = meanLinear(hybrid->readImage()) / meanLinear(reference->readImage());
    INFO("the reflection " << bright << " times as bright as it should be");
    CHECK(bright == Catch::Approx(1.0).epsilon(0.05));
}

TEST_CASE("GPU hybrid: a reflection whose light keeps changing is filtered", "[gpu][hybrid]")
{
    // As above: the reflection's gradients cut its history short every frame, so the
    // spatial filter has to make up for it, as for the lighting. (Filtered 3 times where
    // the lighting is 5 times, TraceResolution::Half: 1.91, now 1.25; Full, each pixel
    // traced every frame, 0.92 and 0.88.)
    const GpuTest gpu;
    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    INFO("trace resolution " << enumToString(resolution));
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity pane = scene.createEntity("Pane");
    pane.get<TransformComponent>().translation = {0.0F, 1.0F, 5.0F};
    pane.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(8.0F, 6.0F)).build()});
    pane.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    Entity wall = scene.createEntity("Wall");
    wall.get<TransformComponent>().translation = {0.0F, 1.0F, 13.0F};
    wall.get<TransformComponent>().rotation = Quat(Vec3(0.0F, kPi, 0.0F));
    wall.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(16.0F, 10.0F)).build()});
    wall.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.8F)});
    Entity light = scene.createEntity("Light");
    light.add<SphereComponent>(4.0F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 100.0F});
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 96;
    constexpr int kFrames = 30;
    auto lightAt = [&](int frame) {
        const float angle = glm::radians(45.0F + 1.4F * static_cast<float>(frame));
        light.get<TransformComponent>().translation = {40.0F * std::sin(angle), 1.0F,
                                                       13.0F - 40.0F * std::cos(angle)};
        scene.markDirty();
        return &builder.update(scene);
    };
    Camera camera = makeCamera(kWidth, kHeight);
    camera.setPosition({0.0F, 1.0F, 9.0F});
    GpuRenderOptions options = hybridOptions(true, resolution);
    options.view = HybridView::Glass;
    auto render = [&](std::uint32_t seed) {
        const PathTracerSettings settings{.samplesPerPixel = 1, .seed = seed, .fireflyClamp = 8.0F};
        auto hybrid = gpu.makeRenderer(kWidth, kHeight);
        for (int i = 0; i < kFrames; ++i) {
            hybrid->render(*lightAt(i), camera, settings, options);
        }
        REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
        return hybrid->readImage();
    };
    // Two runs of other random numbers differ by their noise only.
    const ColorImage first = render(1);
    REQUIRE(meanLinear(first) > 1e-3);  // The reflection is there.
    const float noise = meanDifference(first, render(2));
    INFO("noise " << noise);
    CHECK(noise < (resolution == TraceResolution::Full ? 1.0F : 1.5F));
}
TEST_CASE("GPU hybrid: a highlight sliding along a glossy surface follows it", "[gpu][hybrid]")
{
    // A glossy metal floor and a big light far away swinging around (the sun along a
    // cockpit's frame as the ship turns): the highlight slides along the floor every
    // frame. The highlight's closed form tells where it moved (direct.comp).
    const GpuTest gpu;
    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    INFO("trace resolution " << enumToString(resolution));
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity floor = scene.createEntity("Floor");
    floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
    floor.add<MaterialComponent>(
        MaterialComponent{.albedo = Vec3(0.9F), .roughness = 0.12F, .metallic = 1.0F});
    Entity light = scene.createEntity("Light");
    light.add<SphereComponent>(3.0F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 20.0F});
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 96;
    constexpr int kFrames = 30;
    auto lightAt = [&](int frame) {
        const float angle = glm::radians(-30.0F + 2.0F * static_cast<float>(frame));
        light.get<TransformComponent>().translation = {40.0F * std::sin(angle), 12.0F,
                                                       -40.0F * std::cos(angle)};
        scene.markDirty();
        return &builder.update(scene);
    };
    Camera camera = makeCamera(kWidth, kHeight);
    camera.setPosition({0.0F, 2.0F, 6.0F});
    camera.lookAt({0.0F, 0.0F, -2.0F});
    const PathTracerSettings settings{.samplesPerPixel = 1, .fireflyClamp = 8.0F};
    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < kFrames; ++i) {
        hybrid->render(*lightAt(i), camera, settings, hybridOptions(true, resolution));
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.

    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        reference->render(builder.update(scene), camera,
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    const ColorImage image = hybrid->readImage();
    const ColorImage expected = reference->readImage();
    REQUIRE(meanLinear(expected) > 1e-3);  // The highlight is there.
    // How far off, linear, relative to the light there is.
    double difference = 0.0;
    for (std::size_t i = 0; i < image.pixels().size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            difference +=
                std::abs(srgbToLinear(static_cast<float>(channel(image.pixels()[i], c)) / 255.0F) -
                         srgbToLinear(static_cast<float>(channel(expected.pixels()[i], c)) / 255.0F));
        }
    }
    const double off = difference / (3.0 * static_cast<double>(image.pixels().size())) /
                       meanLinear(expected);
    INFO("off by " << off << " of the light");
    // 0.28, Half 0.32; by the samples' gradients alone 0.48 and 0.45, half as bright as
    // it should be: the reflection is sharp enough that its samples find the light by
    // sampling the lobe, past the vertex their gradients compare.
    CHECK(off < 0.4);
}
TEST_CASE("GPU hybrid: the light of a sphere light follows it at once", "[gpu][hybrid]")
{
    // A floor that a lamp lights (an emissive panel over it, a cockpit's screen) and a
    // big light far away swinging around it, a little every frame (the sun, as the
    // ship turns). What the floor gets from the sphere unshadowed is in closed form
    // every frame; only how much of it arrives accumulates. Accumulated with the
    // lamp's, its change was a small part of all the light, which the history kept.
    const GpuTest gpu;
    const TraceResolution resolution = GENERATE(TraceResolution::Full, TraceResolution::Half);
    INFO("trace resolution " << enumToString(resolution));
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity floor = scene.createEntity("Floor");
    floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
    floor.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.7F)});
    Entity lamp = scene.createEntity("Lamp");
    lamp.get<TransformComponent>().translation = {0.0F, 2.0F, 0.0F};
    lamp.get<TransformComponent>().rotation = Quat(Vec3(0.5F * kPi, 0.0F, 0.0F));
    lamp.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(2.0F)).build()});
    lamp.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = 4.0F});
    Entity light = scene.createEntity("Light");
    light.add<SphereComponent>(1.0F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 20.0F});
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    constexpr int kFrames = 30;
    auto lightAt = [&](int frame) {
        const float angle = glm::radians(-60.0F + 4.0F * static_cast<float>(frame));
        light.get<TransformComponent>().translation = {6.0F * std::sin(angle), 4.0F,
                                                       6.0F * std::cos(angle)};
        scene.markDirty();
        return &builder.update(scene);
    };
    Camera camera = makeCamera(kWidth, kHeight);
    camera.setPosition({0.0F, 5.0F, 5.0F});
    camera.lookAt({0.0F, 0.0F, 0.0F});
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};
    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < kFrames; ++i) {
        hybrid->render(*lightAt(i), camera, settings, hybridOptions(true, resolution));
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        reference->render(builder.update(scene), camera,
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    const ColorImage image = hybrid->readImage();
    const ColorImage expected = reference->readImage();
    // How far off, linear, relative to the light there is.
    double difference = 0.0;
    for (std::size_t i = 0; i < image.pixels().size(); ++i) {
        for (int c = 0; c < 3; ++c) {
            difference +=
                std::abs(srgbToLinear(static_cast<float>(channel(image.pixels()[i], c)) / 255.0F) -
                         srgbToLinear(static_cast<float>(channel(expected.pixels()[i], c)) / 255.0F));
        }
    }
    const double off = difference / (3.0 * static_cast<double>(image.pixels().size())) /
                       meanLinear(expected);
    INFO("off by " << off << " of the light");
    // Full 0.030, Half 0.035 (an Intel Arc: 0.042); with the lamp's in the history, 0.037 and
    // 0.046.
    CHECK(off < (resolution == TraceResolution::Full ? 0.034 : 0.044));
}

TEST_CASE("GPU hybrid: moving shadows leave no trail", "[gpu][hybrid]")
{
    // A ball moving under a light, seen from above: its shadow sweeps a floor that
    // does not move, whose history would keep the shadow where it was.
    const GpuTest gpu;
    Scene scene;
    scene.environment().skyColor = Vec3(0.01F);
    Entity floor = scene.createEntity("Floor");
    floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
    floor.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.7F)});
    Entity light = scene.createEntity("Light");
    light.get<TransformComponent>().translation = {0.0F, 4.0F, 0.0F};
    light.add<SphereComponent>(0.3F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 100.0F});
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(0.5F);
    ball.add<MaterialComponent>(MaterialComponent{.albedo = {0.8F, 0.3F, 0.2F}});
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    constexpr int kFrames = 24;
    Camera camera = makeCamera(kWidth, kHeight);
    camera.setPosition({0.0F, 6.0F, 3.0F});
    camera.lookAt({0.0F, 0.0F, 0.0F});
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};
    // The shadow moves by about a tenth of its width per frame.
    auto ballAt = [&](int frame) {
        ball.get<TransformComponent>().translation = {
            -0.75F + 1.5F * static_cast<float>(frame) / (kFrames - 1), 1.5F, 0.0F};
        scene.markDirty();
        return &builder.update(scene);
    };

    // Shadows traced and accumulated, or looked up in shadow maps every frame.
    const DirectShadows shadows = GENERATE(DirectShadows::Traced, DirectShadows::ShadowMaps);
    INFO(enumToString(shadows));
    GpuRenderOptions options = hybridOptions();
    options.directShadows = shadows;
    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < kFrames; ++i) {
        hybrid->render(*ballAt(i), camera, settings, options);
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        reference->render(builder.update(scene), camera,
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    const ColorImage image = hybrid->readImage();
    const ColorImage expected = reference->readImage();
    const IdImage ids = reference->readEntityIds();

    // On the floor: how dark the shadow is where it is, and how much of it is left
    // where it was (lit pixels still dark).
    auto brightness = [](const ColorImage& from, std::size_t i) {
        const std::uint32_t pixel = from.pixels()[i];
        return std::max({channel(pixel, 0), channel(pixel, 1), channel(pixel, 2)});
    };
    double shadowSum = 0.0;
    double expectedShadowSum = 0.0;
    std::size_t trail = 0;
    for (std::size_t i = 0; i < ids.pixels().size(); ++i) {
        if (ids.pixels()[i] != static_cast<std::uint32_t>(entt::to_integral(floor.handle()))) {
            continue;
        }
        const int value = brightness(image, i);
        const int truth = brightness(expected, i);
        if (truth < 64) {
            shadowSum += value;
            expectedShadowSum += truth;
        } else if (value < truth / 2) {
            ++trail;
        }
    }
    REQUIRE(expectedShadowSum > 0.0);
    const double shadow = shadowSum / expectedShadowSum;
    INFO("shadow " << shadow << " times as bright as it should be, " << trail
                   << " pixels of trails");
    CHECK(shadow < 1.5);
    CHECK(trail < 20);
}

namespace
{

/** @brief A ball on a floor under a sphere light, seen from above (soft shadows). */
struct ShadowedFloor
{
    Scene scene;
    Entity floor;
    Entity light;

    ShadowedFloor()
    {
        scene.environment().skyColor = Vec3(0.01F);
        floor = scene.createEntity("Floor");
        floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(20.0F)).build()});
        floor.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.7F)});
        light = scene.createEntity("Light");
        light.get<TransformComponent>().translation = {0.0F, 4.0F, 0.0F};
        light.add<SphereComponent>(0.5F);
        light.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 60.0F});
        Entity ball = scene.createEntity("Ball");
        ball.get<TransformComponent>().translation = {0.0F, 1.0F, 0.0F};
        ball.add<SphereComponent>(0.5F);
        ball.add<MaterialComponent>(MaterialComponent{.albedo = {0.8F, 0.3F, 0.2F}});
    }

    [[nodiscard]] static auto camera(std::uint32_t width, std::uint32_t height) -> Camera
    {
        Camera camera = makeCamera(width, height);
        camera.setPosition({0.0F, 6.0F, 3.0F});
        camera.lookAt({0.0F, 0.0F, 0.0F});
        return camera;
    }
};

auto shadowOptions(DirectShadows shadows) -> GpuRenderOptions
{
    GpuRenderOptions options = hybridOptions();
    options.directShadows = shadows;
    options.view = HybridView::Direct;
    return options;
}

}  // namespace

TEST_CASE("GPU hybrid: shadow-mapped direct light has no noise", "[gpu][hybrid]")
{
    const GpuTest gpu;
    ShadowedFloor test;
    const RenderScene renderScene = buildRenderScene(test.scene);
    const Camera camera = ShadowedFloor::camera(96, 72);
    // One frame each, of two random streams: the direct light (soft shadows included).
    auto frame = [&](DirectShadows shadows, std::uint32_t seed) {
        auto renderer = gpu.makeRenderer(96, 72);
        renderer->render(renderScene, camera, PathTracerSettings{.seed = seed},
                         shadowOptions(shadows));
        return renderer->readImage();
    };
    const float traced = meanDifference(frame(DirectShadows::Traced, 1),
                                        frame(DirectShadows::Traced, 2));
    const float mapped = meanDifference(frame(DirectShadows::ShadowMaps, 1),
                                        frame(DirectShadows::ShadowMaps, 2));
    INFO("seed 1 against seed 2: traced " << traced << ", shadow maps " << mapped);
    CHECK(traced > 1.0F);
    CHECK(mapped == 0.0F);
}

TEST_CASE("GPU hybrid: shadow-mapped direct light follows a moving light at once",
          "[gpu][hybrid]")
{
    const GpuTest gpu;
    ShadowedFloor test;
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    const Camera camera = ShadowedFloor::camera(kWidth, kHeight);
    // The light swings across, the ball's shadow sweeping the floor about its own width
    // every few frames.
    auto lightAt = [&](int frame) {
        test.light.patch<TransformComponent>([&](auto& t) {
            t.translation = {-3.0F + 0.25F * static_cast<float>(frame), 4.0F, 0.0F};
        });
        return &builder.update(test.scene);
    };
    constexpr int kFrames = 24;
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};
    // Anti-aliased by the hybrid pipeline, or by an upscaler (each frame's direct light).
    const Upscaler upscaler = GENERATE(Upscaler::Off, Upscaler::Taau);
    INFO("upscaler " << enumToString(upscaler));
    auto options = [&](DirectShadows shadows) {
        GpuRenderOptions final = upscaled(shadowOptions(shadows), upscaler, UpscalerQuality::Native);
        final.view = HybridView::Final;
        return final;
    };
    auto sweep = [&](DirectShadows shadows) {
        auto renderer = gpu.makeRenderer(kWidth, kHeight);
        for (int i = 0; i < kFrames; ++i) {
            renderer->render(*lightAt(i), camera, settings, options(shadows));
        }
        return renderer->readImage();
    };
    // Each against itself where the light ended, still for long (no history to lag).
    auto converged = [&](DirectShadows shadows) {
        auto renderer = gpu.makeRenderer(kWidth, kHeight);
        for (int i = 0; i < 128; ++i) {
            renderer->render(*lightAt(kFrames - 1), camera, settings, options(shadows));
        }
        return renderer->readImage();
    };
    const ColorImage tracedEnd = converged(DirectShadows::Traced);
    const ColorImage mappedEnd = converged(DirectShadows::ShadowMaps);
    // How far each is behind, over the image (what lags besides is the same in both: the
    // bounced light, the highlights on the ball). Measured on an RTX 4060: traced 0.71,
    // shadow maps 0.08 (with TAAU 0.57, 0.14); the two still differ by 1.5 (soft shadows
    // approximated).
    const BlockDifference tracedLag = blockDifference(sweep(DirectShadows::Traced), tracedEnd);
    const BlockDifference mappedLag =
        blockDifference(sweep(DirectShadows::ShadowMaps), mappedEnd);
    INFO("behind, mean: traced " << tracedLag.mean << ", shadow maps " << mappedLag.mean
                                 << "; the two still differ by "
                                 << blockDifference(mappedEnd, tracedEnd).mean);
    CHECK(mappedLag.mean < 0.5F * tracedLag.mean);
    CHECK(mappedLag.mean < 0.3F);
}

TEST_CASE("GPU hybrid: what moves with the camera keeps its history", "[gpu][hybrid]")
{
    // A deck under a lamp, both moving with the camera (like a cockpit), past still
    // pillars that light it a little by reflection. Its lighting hardly changes, but
    // every path of it that bounces off the pillars comes out different: the temporal
    // gradients must not take that for a change of its lighting.
    const GpuTest gpu;
    Scene scene;
    scene.environment().skyColor = Vec3(0.05F);
    Entity deck = scene.createEntity("Deck");
    deck.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(6.0F, 5.0F)).build()});
    deck.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.7F)});
    Entity lamp = scene.createEntity("Lamp");
    lamp.add<SphereComponent>(0.2F);
    lamp.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = 40.0F});
    for (int i = 0; i < 7; ++i) {
        Entity pillar = scene.createEntity("Pillar");
        pillar.get<TransformComponent>().translation = {-4.5F + 1.5F * static_cast<float>(i),
                                                        0.5F, 2.6F};
        pillar.add<MeshComponent>(
            MeshComponent{.mesh = primitives::box(Vec3(0.25F, 1.5F, 0.25F)).build()});
        pillar.add<MaterialComponent>(MaterialComponent{
            .albedo = i % 2 == 0 ? Vec3(0.9F, 0.2F, 0.1F) : Vec3(0.1F, 0.8F, 0.3F)});
    }
    RenderSceneBuilder builder;
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    constexpr int kFrames = 40;  // Past the history cap.
    auto at = [&](float x) {
        deck.get<TransformComponent>().translation = {x, -1.0F, 5.5F};
        lamp.get<TransformComponent>().translation = {x, 1.5F, 5.5F};
        scene.markDirty();
        Camera camera = makeCamera(kWidth, kHeight);
        camera.setPosition({x, 1.0F, 10.0F});
        camera.setRotation(-90.0F, -25.0F);
        return camera;
    };
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};
    GpuRenderOptions options = hybridOptions();
    options.denoiser.historyFrames = 32;
    options.view = HybridView::History;

    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    Camera camera;
    for (int i = 0; i < kFrames; ++i) {
        camera = at(-1.0F + 2.0F * static_cast<float>(i) / (kFrames - 1));
        hybrid->render(builder.update(scene), camera, settings, options);
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
    auto reference = gpu.makeRenderer(kWidth, kHeight);
    reference->render(builder.update(scene), camera, settings);
    const IdImage ids = reference->readEntityIds();
    const ColorImage history = hybrid->readImage();

    // White: the history cap (edge pixels keep less: in some frames their jittered
    // samples miss the deck).
    std::size_t deckPixels = 0;
    double kept = 0.0;
    for (std::size_t i = 0; i < ids.pixels().size(); ++i) {
        if (ids.pixels()[i] == static_cast<std::uint32_t>(entt::to_integral(deck.handle()))) {
            kept += channel(history.pixels()[i], 0) / 255.0;
            ++deckPixels;
        }
    }
    REQUIRE(deckPixels > 1000);
    kept /= static_cast<double>(deckPixels);
    INFO("history kept: " << kept << " of the cap on average");
    CHECK(kept > 0.95);
}

TEST_CASE("GPU hybrid: reflections that are rarely bright keep their light while moving",
          "[gpu][hybrid]")
{
    // A pane reflecting another one behind the camera, turned by 45 degrees: that one
    // lets most of the reflected rays through to a black sky, and reflects a few onto a
    // bright panel. So the first pane's reflection is a few bright samples among dark
    // ones (like a cockpit canopy's). This frame's samples around a pixel mostly miss
    // the bright ones: clamped to them, its history would lose most of its light.
    const GpuTest gpu;
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    const MaterialComponent glass{
        .albedo = Vec3(1.0F), .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true};
    auto quad = [&](const char* name, Vec3 position, float yaw, Vec2 size,
                    const MaterialComponent& material) {
        Entity entity = scene.createEntity(name);
        entity.get<TransformComponent>().translation = position;
        entity.get<TransformComponent>().rotation = Quat(Vec3(0.0F, yaw, 0.0F));
        entity.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(size).build()});
        entity.add<MaterialComponent>(material);
    };
    quad("Pane", {0.0F, 1.0F, 5.0F}, 0.0F, {3.2F, 2.0F}, glass);
    quad("Back pane", {0.0F, 1.0F, 13.0F}, -0.75F * kPi, {10.0F, 8.0F}, glass);
    // Far to the side: the first pane does not reflect it directly.
    quad("Panel", {-10.0F, 1.0F, 13.0F}, 0.5F * kPi, {4.0F, 4.0F},
         MaterialComponent{
             .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = 20.0F});
    const RenderScene renderScene = buildRenderScene(scene);
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    constexpr int kFrames = 24;
    auto cameraAt = [&](float x) {
        Camera camera = makeCamera(kWidth, kHeight);
        camera.setPosition({x, 1.0F, 9.0F});
        return camera;
    };
    const PathTracerSettings settings{.samplesPerPixel = 2, .fireflyClamp = 8.0F};

    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < kFrames; ++i) {
        hybrid->render(renderScene,
                       cameraAt(-0.8F + 1.6F * static_cast<float>(i) / (kFrames - 1)), settings,
                       hybridOptions());
    }
    REQUIRE(hybrid->accumulatedFrames() == kFrames);  // Reprojected, not restarted.
    auto reference = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        reference->render(renderScene, cameraAt(0.8F),
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    const double light = meanLinear(hybrid->readImage());
    const double expected = meanLinear(reference->readImage());
    REQUIRE(expected > 1e-4);  // The reflection is there.
    INFO("the reflection has " << light / expected << " of its light");
    CHECK(light > 0.6 * expected);
}

TEST_CASE("GPU hybrid: what restarts the accumulation", "[gpu][hybrid]")
{
    const GpuTest gpu;
    TestScene test;
    RenderSceneBuilder builder;
    const RenderScene& renderScene = builder.update(test.scene);
    constexpr std::uint32_t kWidth = 48;
    constexpr std::uint32_t kHeight = 36;
    Camera camera = makeCamera(kWidth, kHeight);
    const PathTracerSettings settings;
    auto hybrid = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 3; ++i) {
        hybrid->render(renderScene, camera, settings, hybridOptions());
    }
    REQUIRE(hybrid->accumulatedFrames() == 3);

    // Moving the camera or an entity reprojects; display settings change nothing.
    camera.setPosition(camera.position() + Vec3(0.1F, 0.0F, 0.0F));
    hybrid->render(renderScene, camera, settings, hybridOptions());
    test.mirror.get<TransformComponent>().translation += Vec3(0.05F, 0.0F, 0.0F);
    test.scene.markDirty();
    builder.update(test.scene);
    hybrid->render(renderScene, camera, settings, hybridOptions());
    PathTracerSettings display = settings;
    display.exposure = 2.0F;
    display.toneMapper = ToneMapper::Aces;
    display.samplesPerPixel = 3;
    hybrid->render(renderScene, camera, display, hybridOptions());
    REQUIRE(hybrid->accumulatedFrames() == 6);

    // New content, estimator settings or the other pipeline start over.
    test.mirror.get<MaterialComponent>().albedo = {0.9F, 0.2F, 0.2F};
    test.scene.markDirty();
    builder.update(test.scene);
    hybrid->render(renderScene, camera, settings, hybridOptions());
    REQUIRE(hybrid->accumulatedFrames() == 1);
    hybrid->render(renderScene, camera, settings, hybridOptions());
    PathTracerSettings bounces = settings;
    bounces.maxBounces = 2;
    hybrid->render(renderScene, camera, bounces, hybridOptions());
    REQUIRE(hybrid->accumulatedFrames() == 1);
    hybrid->render(renderScene, camera, bounces);
    REQUIRE(hybrid->accumulatedFrames() == 1);
    hybrid->render(renderScene, camera, bounces, hybridOptions());
    REQUIRE(hybrid->accumulatedFrames() == 1);
    hybrid->render(renderScene, camera, bounces, hybridOptions());
    hybrid->render(renderScene, camera, bounces, hybridOptions(true, TraceResolution::Half));
    REQUIRE(hybrid->accumulatedFrames() == 1);
    hybrid->render(renderScene, camera, bounces, hybridOptions());
    REQUIRE(hybrid->accumulatedFrames() == 1);

    // Debug views, resizing, an empty scene.
    constexpr std::size_t kViews = EnumNames<HybridView>::kNames.size();
    for (std::size_t view = 0; view < kViews; ++view) {
        GpuRenderOptions options = hybridOptions();
        options.view = static_cast<HybridView>(view);
        hybrid->render(renderScene, camera, bounces, options);
    }
    REQUIRE(hybrid->accumulatedFrames() == 1 + kViews);
    // The same content rebuilt is new content (another render scene).
    hybrid->render(buildRenderScene(test.scene), camera, bounces, hybridOptions());
    REQUIRE(hybrid->accumulatedFrames() == 1);
    hybrid->resize(40, 30);
    Scene empty;
    empty.environment().skyColor = {1.0F, 0.0F, 0.0F};
    hybrid->render(buildRenderScene(empty), makeCamera(40, 30), settings, hybridOptions());
    const ColorImage sky = hybrid->readImage();
    REQUIRE(sky.at(10, 10) == packRgba8({1.0F, 0.0F, 0.0F, 1.0F}));
}

// Raster pipeline ------------------------------------------------------------------------------------

namespace
{

const GpuRenderOptions kRaster{.pipeline = GpuPipeline::Raster};

/**
 * @brief Spheres, a cube and a window pane in front, lit by two sphere lights
 *        that nothing occludes: neither the preview's shadow rays nor the
 *        raster's shadow maps find anything in the way.
 */
auto unshadowedScene(Scene& scene) -> Entity
{
    scene.environment().skyColor = {0.05F, 0.07F, 0.1F};
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(1.0F);
    ball.add<MaterialComponent>(MaterialComponent{.albedo = {0.8F, 0.3F, 0.2F}});
    Entity cube = scene.createEntity("Cube");
    cube.get<TransformComponent>().translation = {2.5F, 0.0F, 0.0F};
    cube.get<TransformComponent>().rotation = Quat(Vec3(0.3F, 0.6F, 0.0F));
    cube.add<MeshComponent>(MeshComponent{.mesh = primitives::box(Vec3(0.6F)).build()});
    cube.add<MaterialComponent>(MaterialComponent{.albedo = {0.3F, 0.8F, 0.4F}});
    Entity pane = scene.createEntity("Pane");
    pane.get<TransformComponent>().translation = {0.0F, 0.0F, 3.0F};
    pane.add<MeshComponent>(MeshComponent{.mesh = primitives::quad(Vec2(1.2F)).build()});
    pane.add<MaterialComponent>(MaterialComponent{
        .albedo = {0.8F, 0.9F, 1.0F}, .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    for (const auto& [x, power] : {std::pair{-3.0F, 30.0F}, std::pair{3.0F, 20.0F}}) {
        Entity light = scene.createEntity("Light");
        light.get<TransformComponent>().translation = {x, 4.0F, 4.0F};
        light.add<SphereComponent>(0.5F);
        light.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(1.0F), .emissionColor = Vec3(1.0F), .emissionPower = power});
    }
    return ball;
}

}  // namespace

TEST_CASE("GPU raster: the CPU preview where nothing casts a shadow", "[gpu][raster]")
{
    const GpuTest gpu(false);
    Scene scene;
    const Entity ball = unshadowedScene(scene);
    const RenderScene renderScene = buildRenderScene(scene);
    constexpr std::uint32_t kWidth = 96;
    constexpr std::uint32_t kHeight = 72;
    const Camera camera = makeCamera(kWidth, kHeight);
    const PathTracerSettings settings{.mode = RenderMode::Preview};

    CpuPathTracer cpu;
    cpu.resize(kWidth, kHeight);
    auto raster = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 32; ++i) {
        cpu.render(renderScene, camera, settings);
        raster->render(renderScene, camera, settings, kRaster);
    }
    REQUIRE(raster->accumulatedFrames() == 32);
    const BlockDifference difference = blockDifference(raster->readImage(), cpu.image());
    INFO("worst block " << difference.worst << ", mean " << difference.mean);
    // Both anti-alias, with different sample positions: edges differ a little
    // (measured: 0.4 levels at worst on an RTX 4060).
    CHECK(difference.worst < 2.0F);
    CHECK(difference.mean < 0.25F);
    // Ids of the first frame (pixel centers): the pane is looked through.
    const IdImage ids = raster->readEntityIds();
    CHECK(idMismatch(ids, cpu.entityIds()) < 0.02);
    CHECK(ids.at(kWidth / 2, kHeight / 2) == entt::to_integral(ball.handle()));
}

TEST_CASE("GPU raster: what restarts the accumulation", "[gpu][raster]")
{
    const GpuTest gpu(false);
    TestScene test;
    RenderSceneBuilder builder;
    const RenderScene& renderScene = builder.update(test.scene);
    Camera camera = makeCamera(48, 36);
    const PathTracerSettings settings;
    auto raster = gpu.makeRenderer(48, 36);
    for (int i = 0; i < 4; ++i) {
        raster->render(renderScene, camera, settings, kRaster);
    }
    REQUIRE(raster->accumulatedFrames() == 4);
    if (gpu.context().deviceInfo().timestampPeriod > 0.0F) {
        raster->waitIdle();
        REQUIRE(raster->gpuTimeMs() > 0.0F);
        // Nothing is traced: two timestamps one after the other (0.013 ms apart at times,
        // after the hybrid tests' work).
        REQUIRE(raster->traceTimeMs() < 0.1F);
    }

    // Display settings and things nothing traces keep it, anything seen restarts it.
    PathTracerSettings display = settings;
    display.exposure = 2.0F;
    display.toneMapper = ToneMapper::Aces;
    display.maxBounces = 1;
    raster->render(renderScene, camera, display, kRaster);
    REQUIRE(raster->accumulatedFrames() == 5);
    test.mirror.patch<TransformComponent>([](auto& t) { t.translation.x += 0.1F; });
    builder.update(test.scene);
    raster->render(renderScene, camera, settings, kRaster);
    REQUIRE(raster->accumulatedFrames() == 1);
    camera.setPosition(camera.position() + Vec3(0.0F, 0.1F, 0.0F));
    raster->render(renderScene, camera, settings, kRaster);
    REQUIRE(raster->accumulatedFrames() == 1);
    PathTracerSettings masked = settings;
    masked.instanceMask = kAllInstances & ~layerBit(3);
    raster->render(renderScene, camera, masked, kRaster);
    REQUIRE(raster->accumulatedFrames() == 1);
    const IdImage ids = raster->readEntityIds();
    CHECK_FALSE(std::ranges::contains(
        ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.hidden.handle()))));
    CHECK_FALSE(std::ranges::contains(
        ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.pane.handle()))));
    raster->render(renderScene, camera, masked, kRaster);
    REQUIRE(raster->accumulatedFrames() == 2);
    GpuRenderOptions withoutShadows = kRaster;
    withoutShadows.shadows.enabled = false;
    raster->render(renderScene, camera, masked, withoutShadows);
    REQUIRE(raster->accumulatedFrames() == 1);

    // Switching pipelines both ways.
    raster->render(renderScene, camera, settings, {.pipeline = GpuPipeline::Hybrid});
    raster->render(renderScene, camera, settings, kRaster);
    REQUIRE(raster->accumulatedFrames() == 1);
}

TEST_CASE("GPU raster: runs without ray queries", "[gpu][raster]")
{
    const GpuTest gpu(false, contextWithoutRayQueries());
    TestScene test;
    const RenderScene renderScene = buildRenderScene(test.scene);
    const Camera camera = makeCamera(64, 48);
    auto raster = gpu.makeRenderer(64, 48);
    REQUIRE_FALSE(raster->rayTracing());
    for (int i = 0; i < 8; ++i) {
        raster->render(renderScene, camera, {}, kRaster);
    }
    const ColorImage image = raster->readImage();
    const IdImage ids = raster->readEntityIds();

    // The same as on the device with ray queries, if there is one.
    vulkan::Context* withRayQueries = gpuContext();
    if (withRayQueries != nullptr && withRayQueries->deviceInfo().rayQuery) {
        const GpuTest reference;
        auto expected = reference.makeRenderer(64, 48);
        for (int i = 0; i < 8; ++i) {
            expected->render(renderScene, camera, {}, kRaster);
        }
        CHECK(meanDifference(image, expected->readImage()) == 0.0F);
        CHECK(idMismatch(ids, expected->readEntityIds()) == 0.0);
    }
    CHECK(std::ranges::contains(
        ids.pixels(), static_cast<std::uint32_t>(entt::to_integral(test.cube.handle()))));
}

namespace
{

/**
 * @brief A floor with a floating ball, a table (one mesh: its top and a mat
 *        under it, which it shadows) and a sphere light; diffuse surfaces
 *        without a coat (IOR 1): the path tracer lights them as the raster.
 */
struct ShadowScene
{
    Scene scene;
    Entity floor;
    Entity table;

    ShadowScene(Vec3 lightPosition, float lightRadius, float lightPower)
    {
        floor = scene.createEntity("Floor");
        floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(30.0F)).build()});
        floor.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.6F), .ior = 1.0F});

        Entity ball = scene.createEntity("Ball");
        ball.get<TransformComponent>().translation = {0.5F, 1.6F, 0.0F};
        ball.add<SphereComponent>(1.0F);
        ball.add<MaterialComponent>(
            MaterialComponent{.albedo = {0.8F, 0.3F, 0.2F}, .ior = 1.0F});

        table = scene.createEntity("Table");
        MeshBuilder mesh;
        mesh.append(primitives::plane(Vec2(2.0F)),
                    glm::translate(Mat4(1.0F), Vec3(-2.5F, 1.2F, 0.0F)));
        mesh.append(primitives::plane(Vec2(4.0F)),
                    glm::translate(Mat4(1.0F), Vec3(-2.5F, 0.02F, 0.0F)));
        table.add<MeshComponent>(MeshComponent{.mesh = mesh.build()});
        table.add<MaterialComponent>(
            MaterialComponent{.albedo = {0.7F, 0.7F, 0.5F}, .ior = 1.0F});

        Entity light = scene.createEntity("Light");
        light.get<TransformComponent>().translation = lightPosition;
        light.add<SphereComponent>(lightRadius);
        light.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = lightPower});
    }
};

/** @brief Linear luminance of each pixel of an 8-bit sRGB image. */
auto linearLuminance(const ColorImage& image) -> std::vector<float>
{
    std::vector<float> result;
    result.reserve(image.pixels().size());
    for (const std::uint32_t pixel : image.pixels()) {
        Vec3 color;
        for (int c = 0; c < 3; ++c) {
            color[c] = srgbToLinear(static_cast<float>(channel(pixel, c)) / 255.0F);
        }
        result.push_back(glm::dot(color, Vec3(0.2126F, 0.7152F, 0.0722F)));
    }
    return result;
}

}  // namespace

TEST_CASE("GPU raster: shadow maps shade like the path tracer's shadow rays", "[gpu][raster]")
{
    // How much of the light reaches each pixel of the floor and the table:
    // (raster - raster without the light) / (raster without shadows - the
    // same), and the path tracer's direct light (one segment, nothing else
    // lights the scene) over that same unshadowed light.
    const GpuTest gpu;
    struct Light
    {
        const char* name;
        Vec3 position;
        float radius;
        float power;
        float distance;  // ShadowSettings::distance
    };
    const auto light = GENERATE(Light{"a light near the camera: six views around it",
                                      {2.0F, 6.0F, 1.0F}, 0.4F, 100.0F, 64.0F},
                                Light{"a far light: three views nested around the camera",
                                      {-60.0F, 80.0F, 40.0F}, 6.0F, 150.0F, 16.0F});
    INFO(light.name);
    constexpr std::uint32_t kWidth = 128;
    constexpr std::uint32_t kHeight = 96;
    Camera camera = makeCamera(kWidth, kHeight);
    camera.setPosition({0.0F, 9.0F, 9.0F});
    camera.lookAt({0.0F, 0.0F, 0.0F});

    const ShadowScene lit(light.position, light.radius, light.power);
    const ShadowScene unlit(light.position, light.radius, 0.0F);
    GpuRenderOptions shadows = kRaster;
    shadows.shadows.distance = light.distance;
    GpuRenderOptions noShadows = shadows;
    noShadows.shadows.enabled = false;
    auto renderer = gpu.makeRenderer(kWidth, kHeight);
    auto render = [&](const Scene& scene, const GpuRenderOptions& options, int frames) {
        const RenderScene renderScene = buildRenderScene(scene);
        const PathTracerSettings settings{.maxBounces = 1};
        for (int i = 0; i < frames; ++i) {
            renderer->render(renderScene, camera, settings, options);
        }
        return renderer->readImage();
    };
    const std::vector<float> raster = linearLuminance(render(lit.scene, shadows, 16));
    const IdImage ids = renderer->readEntityIds();
    const std::vector<float> unshadowed = linearLuminance(render(lit.scene, noShadows, 4));
    const std::vector<float> ambient = linearLuminance(render(unlit.scene, noShadows, 4));
    const std::vector<float> traced =
        linearLuminance(render(lit.scene, {.pipeline = GpuPipeline::PathTracing}, 256));

    const std::array receivers{static_cast<std::uint32_t>(entt::to_integral(lit.floor.handle())),
                               static_cast<std::uint32_t>(entt::to_integral(lit.table.handle()))};
    std::vector<std::size_t> pixels;
    std::vector<float> rasterVisible(raster.size(), 0.0F);
    std::vector<float> tracedVisible(raster.size(), 0.0F);
    double litTraced = 0.0;
    double litLight = 0.0;
    for (std::size_t i = 0; i < raster.size(); ++i) {
        const float lightOnly = unshadowed[i] - ambient[i];
        if (!std::ranges::contains(receivers, ids.pixels()[i]) || lightOnly < 0.05F) {
            continue;
        }
        pixels.push_back(i);
        rasterVisible[i] = (raster[i] - ambient[i]) / lightOnly;
        tracedVisible[i] = traced[i] / lightOnly;
        if (rasterVisible[i] > 0.995F) {
            litTraced += traced[i];
            litLight += lightOnly;
        }
    }
    REQUIRE(pixels.size() > kWidth * kHeight / 3);
    REQUIRE(litLight > 0.0);
    // Where nothing is in the way, the path tracer's light is the raster's.
    const auto scale = static_cast<float>(litTraced / litLight);
    CHECK(scale == Catch::Approx(1.0).margin(0.03));
    double difference = 0.0;
    std::size_t large = 0;
    for (const std::size_t i : pixels) {
        const float d = std::abs(rasterVisible[i] - tracedVisible[i] / scale);
        difference += d;
        large += d > 0.25F ? 1 : 0;
    }
    const double meanDifference = difference / static_cast<double>(pixels.size());
    const double largeFraction = static_cast<double>(large) / static_cast<double>(pixels.size());
    INFO("mean visibility difference " << meanDifference << ", over 0.25: " << largeFraction);
    // Measured on an RTX 4060: 0.0096 and 0.0073, none over 0.25 (penumbras
    // are an estimate: their width from the occluders' mean distance).
    CHECK(meanDifference < 0.02);
    CHECK(largeFraction < 0.005);

    // The table's top shadows its own mat.
    auto visibleAt = [&](Vec3 world) {
        const Vec2 pixel = camera.worldToScreen(world).value();
        return rasterVisible[static_cast<std::size_t>(pixel.y) * kWidth +
                             static_cast<std::size_t>(pixel.x)];
    };
    CHECK(visibleAt({-2.5F, 0.02F, 0.0F}) < 0.1F);
    CHECK(visibleAt({5.0F, 0.0F, 4.0F}) == Catch::Approx(1.0F).margin(0.02F));
}

TEST_CASE("GPU raster: see-through panes and the light itself cast no shadow", "[gpu][raster]")
{
    const GpuTest gpu(false);
    Scene scene;
    Entity floor = scene.createEntity("Floor");
    floor.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(30.0F)).build()});
    Entity pane = scene.createEntity("Pane");
    pane.get<TransformComponent>().translation = {0.0F, 3.0F, 0.0F};
    pane.add<MeshComponent>(MeshComponent{.mesh = primitives::plane(Vec2(3.0F)).build()});
    pane.add<MaterialComponent>(MaterialComponent{
        .albedo = {0.8F, 0.9F, 1.0F}, .roughness = 0.0F, .transmission = 1.0F, .thinWalled = true});
    Entity light = scene.createEntity("Light");
    light.get<TransformComponent>().translation = {0.0F, 5.0F, 0.0F};
    light.add<SphereComponent>(0.5F);
    light.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = 40.0F});
    const RenderScene renderScene = buildRenderScene(scene);
    Camera camera = makeCamera(64, 48);
    camera.setPosition({0.0F, 8.0F, 8.0F});
    camera.lookAt({0.0F, 0.0F, 0.0F});

    auto renderer = gpu.makeRenderer(64, 48);
    GpuRenderOptions noShadows = kRaster;
    noShadows.shadows.enabled = false;
    auto render = [&](const GpuRenderOptions& options) {
        for (int i = 0; i < 4; ++i) {
            renderer->render(renderScene, camera, {}, options);
        }
        return renderer->readImage();
    };
    const ColorImage shadowed = render(kRaster);
    CHECK(meanDifference(shadowed, render(noShadows)) < 0.05F);
}

// Effects --------------------------------------------------------------------------------------------

namespace
{

/**
 * @brief Sum of the squared differences of horizontally adjacent pixels (a
 *        step blurred over n pixels keeps its absolute sum but has 1/n of this).
 */
auto horizontalContrast(const ColorImage& image) -> double
{
    double sum = 0.0;
    for (std::uint32_t y = 0; y < image.height(); ++y) {
        for (std::uint32_t x = 0; x + 1 < image.width(); ++x) {
            for (int c = 0; c < 3; ++c) {
                const int step = channel(image.at(x + 1, y), c) - channel(image.at(x, y), c);
                sum += step * step;
            }
        }
    }
    return sum;
}

auto countPixels(const ColorImage& image, int channelIndex) -> std::size_t
{
    return static_cast<std::size_t>(std::ranges::count_if(
        image.pixels(), [&](std::uint32_t pixel) { return channel(pixel, channelIndex) > 0; }));
}

}  // namespace

TEST_CASE("GPU effects: bloom spreads bright light around it", "[gpu][effects]")
{
    const GpuTest gpu(false);
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity sun = scene.createEntity("Sun");
    sun.add<SphereComponent>(1.0F);  // About 12 pixels in radius.
    sun.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = 20.0F});
    const RenderScene renderScene = buildRenderScene(scene);
    const Camera camera = makeCamera(128, 96);
    GpuRenderOptions glowing = kRaster;
    glowing.effects.bloom = true;
    glowing.effects.bloomStrength = 0.2F;
    auto render = [&](const GpuRenderOptions& options) {
        auto renderer = gpu.makeRenderer(128, 96);
        renderer->render(renderScene, camera, {}, options);
        return renderer->readImage();
    };
    const ColorImage sharp = render(kRaster);
    const ColorImage glow = render(glowing);
    REQUIRE(channel(sharp.at(84, 48), 0) == 0);  // Outside the sphere.
    CHECK(channel(glow.at(84, 48), 0) > 0);
    CHECK(channel(glow.at(84, 48), 0) > channel(glow.at(124, 48), 0));  // Fading away.
    CHECK(channel(glow.at(64, 48), 0) == 255);
}

TEST_CASE("GPU effects: motion blur smears what moves, not what rests", "[gpu][effects]")
{
    const GpuTest gpu(false);
    Scene scene;
    unshadowedScene(scene);
    const RenderScene renderScene = buildRenderScene(scene);
    const Camera first = makeCamera(128, 96);
    Camera moved = first;
    moved.setPosition(first.position() + Vec3(1.0F, 0.0F, 0.0F));  // About 12 pixels at the ball.
    GpuRenderOptions blurred = kRaster;
    blurred.effects.motionBlur = true;
    blurred.effects.shutter = 1.0F;
    auto twoFrames = [&](const GpuRenderOptions& options, const Camera& second) {
        auto renderer = gpu.makeRenderer(128, 96);
        renderer->render(renderScene, first, {}, options);
        renderer->render(renderScene, second, {}, options);
        return renderer->readImage();
    };
    CHECK(meanDifference(twoFrames(kRaster, first), twoFrames(blurred, first)) == 0.0F);
    const double sharp = horizontalContrast(twoFrames(kRaster, moved));
    const double smeared = horizontalContrast(twoFrames(blurred, moved));
    INFO("horizontal contrast: sharp " << sharp << ", blurred " << smeared);
    CHECK(smeared < 0.5 * sharp);
}

TEST_CASE("GPU effects: particles glow, hide behind surfaces and streak", "[gpu][effects]")
{
    const GpuTest gpu(false);
    Scene scene;  // A black ball on a black sky.
    scene.environment().skyColor = Vec3(0.0F);
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(1.0F);
    ball.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.0F)});
    const RenderScene renderScene = buildRenderScene(scene);
    const Camera camera = makeCamera(128, 96);
    auto particle = [](Vec3 position, Vec3 color, Vec3 moved = Vec3(0.0F)) {
        return RenderParticle{.position = position,
                              .size = 0.05F,
                              .previousPosition = position - moved,
                              .color = Vec4(color, 1.0F)};
    };
    auto render = [&](std::span<const RenderParticle> particles, const GpuRenderOptions& options) {
        auto renderer = gpu.makeRenderer(128, 96);
        renderer->render(renderScene, camera, {}, options, particles);
        return renderer->readImage();
    };

    const RenderParticle still[] = {
        particle({0.0F, 0.0F, 3.0F}, {50.0F, 0.0F, 0.0F}),   // In front of the ball.
        particle({0.0F, 0.0F, -3.0F}, {0.0F, 50.0F, 0.0F}),  // Behind it.
        particle({3.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 50.0F}),   // Beside it.
    };
    const ColorImage image = render(still, kRaster);
    const Vec2 front = *camera.worldToScreen({0.0F, 0.0F, 3.0F});
    CHECK(channel(image.at(static_cast<std::uint32_t>(front.x), static_cast<std::uint32_t>(front.y)),
                  0) == 255);
    CHECK(countPixels(image, 1) == 0);
    const Vec2 side = *camera.worldToScreen({3.0F, 1.0F, 0.0F});
    CHECK(channel(image.at(static_cast<std::uint32_t>(side.x), static_cast<std::uint32_t>(side.y)),
                  2) > 200);
    // Not part of the accumulated image: a frame without them has none.
    CHECK(countPixels(render({}, kRaster), 0) == 0);

    // A particle that moved a unit since the last frame streaks (with motion blur).
    GpuRenderOptions blurred = kRaster;
    blurred.effects.motionBlur = true;
    blurred.effects.shutter = 1.0F;
    const RenderParticle moving[] = {particle({0.0F, 0.0F, 3.0F}, {50.0F, 0.0F, 0.0F},
                                              {1.0F, 0.0F, 0.0F})};
    const std::size_t dot = countPixels(render(std::span(still).first(1), blurred), 0);
    const std::size_t streak = countPixels(render(moving, blurred), 0);
    INFO("lit pixels: still " << dot << ", moving " << streak);
    CHECK(streak > 3 * dot);
}

namespace
{

/** @brief A renderer, or SKIP if the upscaler cannot run here (the vendors' need their GPUs). */
auto upscalingRenderer(const GpuTest& gpu, Upscaler upscaler, std::uint32_t width,
                       std::uint32_t height) -> std::unique_ptr<GpuRenderer>
{
    auto renderer = gpu.makeRenderer(width, height);
    if (!renderer->upscalerAvailable(upscaler)) {
        SKIP(enumToString(upscaler) << " is not available: " << renderer->upscalerError(upscaler));
    }
    return renderer;
}

}  // namespace

TEST_CASE("GPU upscaling: fewer pixels rendered, the output's reconstructed", "[gpu][upscaling]")
{
    const GpuTest gpu(false);
    Scene scene;
    const Entity ball = unshadowedScene(scene);
    const RenderScene renderScene = buildRenderScene(scene);
    constexpr std::uint32_t kWidth = 192;
    constexpr std::uint32_t kHeight = 144;
    const Camera camera = makeCamera(kWidth, kHeight);
    // The output's pixels rendered and anti-aliased.
    auto native = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        native->render(renderScene, camera, {}, kRaster);
    }
    const ColorImage expected = native->readImage();

    const Upscaler upscaler = GENERATE(Upscaler::Taau, Upscaler::Fsr, Upscaler::Dlss);
    INFO(enumToString(upscaler));
    auto renderer = upscalingRenderer(gpu, upscaler, kWidth, kHeight);
    const GpuRenderOptions options = upscaled(kRaster, upscaler, UpscalerQuality::Performance);
    for (int i = 0; i < 64; ++i) {
        renderer->render(renderScene, camera, {}, options);
    }
    REQUIRE(renderer->activeUpscaler() == upscaler);
    REQUIRE(renderer->renderWidth() == kWidth / 2);
    REQUIRE(renderer->renderHeight() == kHeight / 2);
    const ColorImage image = renderer->readImage();
    REQUIRE(image.size() == expected.size());
    const BlockDifference difference = blockDifference(image, expected);
    INFO("worst block " << difference.worst << ", mean " << difference.mean);
    // A quarter of the samples per frame: edges are reconstructed from the frames
    // (measured on an RTX 4060: worst blocks 2.7 TAAU, 1.7 FSR, 1.4 DLSS).
    CHECK(difference.worst < 6.0F);
    CHECK(difference.mean < 0.5F);
    // Entity ids at the output's size (each from the rendered pixel it is in).
    const IdImage ids = renderer->readEntityIds();
    REQUIRE(ids.size() == expected.size());
    CHECK(idMismatch(ids, native->readEntityIds()) < 0.05);
    CHECK(ids.at(kWidth / 2, kHeight / 2) == entt::to_integral(ball.handle()));

    // Off again: the output's size.
    renderer->render(renderScene, camera, {}, kRaster);
    CHECK(renderer->activeUpscaler() == Upscaler::Off);
    CHECK(renderer->renderWidth() == kWidth);
}

TEST_CASE("GPU upscaling: falls back where the upscaler cannot run", "[gpu][upscaling]")
{
    const GpuTest gpu(false);
    Scene scene;
    unshadowedScene(scene);
    const RenderScene renderScene = buildRenderScene(scene);
    auto renderer = gpu.makeRenderer(40, 18);
    const GpuRenderOptions options = upscaled(kRaster, Upscaler::Dlss, UpscalerQuality::Quality);
    // DLSS refuses images this small (a window being laid out): FSR or TAAU instead.
    renderer->render(renderScene, makeCamera(40, 18), {}, options);
    CHECK(renderer->activeUpscaler() != Upscaler::Off);
    CHECK(renderer->renderWidth() == 27);
    if (renderer->upscalerAvailable(Upscaler::Dlss)) {
        CHECK(renderer->activeUpscaler() != Upscaler::Dlss);
        renderer->resize(160, 120);
        renderer->render(renderScene, makeCamera(160, 120), {}, options);
        CHECK(renderer->activeUpscaler() == Upscaler::Dlss);
    }
    // TAAU runs everywhere.
    CHECK(renderer->upscalerAvailable(Upscaler::Taau));
    CHECK(renderer->upscalerError(Upscaler::Taau).empty());
}

TEST_CASE("GPU upscaling: the hybrid pipeline's image", "[gpu][upscaling][hybrid]")
{
    const GpuTest gpu;
    TestScene test;
    const RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 144;
    constexpr std::uint32_t kHeight = 108;
    const Camera camera = makeCamera(kWidth, kHeight);
    const PathTracerSettings settings{.samplesPerPixel = 4, .fireflyClamp = 8.0F};
    const GpuRenderOptions options = hybridOptions(true, TraceResolution::Half);
    auto native = gpu.makeRenderer(kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        native->render(renderScene, camera, settings, options);
    }
    const ColorImage expected = native->readImage();

    const Upscaler upscaler = GENERATE(Upscaler::Taau, Upscaler::Fsr, Upscaler::Dlss);
    INFO(enumToString(upscaler));
    auto renderer = upscalingRenderer(gpu, upscaler, kWidth, kHeight);
    for (int i = 0; i < 64; ++i) {
        renderer->render(renderScene, camera, settings,
                         upscaled(options, upscaler, UpscalerQuality::Quality));
    }
    REQUIRE(renderer->renderWidth() == 96);
    const BlockDifference difference = blockDifference(renderer->readImage(), expected);
    INFO("worst block " << difference.worst << ", mean " << difference.mean);
    CHECK(difference.worst < 16.0F);
    CHECK(difference.mean < 1.0F);
}

TEST_CASE("GPU upscaling: what moves leaves no trail", "[gpu][upscaling]")
{
    const GpuTest gpu(false);
    // A bright ball sweeping across a dark sky, a pixel or two per frame.
    Scene scene;
    scene.environment().skyColor = Vec3(0.0F);
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(0.8F);
    ball.add<MaterialComponent>(MaterialComponent{
        .albedo = Vec3(0.0F), .emissionColor = Vec3(1.0F), .emissionPower = 1.0F});
    constexpr std::uint32_t kWidth = 160;
    constexpr std::uint32_t kHeight = 96;
    const Camera camera = makeCamera(kWidth, kHeight);
    auto sweep = [&](const GpuRenderOptions& options, GpuRenderer& renderer) {
        RenderSceneBuilder builder;
        ball.patch<TransformComponent>([](auto& t) { t.translation = {-3.0F, 0.0F, 0.0F}; });
        for (int i = 0; i < 40; ++i) {
            ball.patch<TransformComponent>([](auto& t) { t.translation.x += 0.15F; });
            renderer.render(builder.update(scene), camera, {}, options);
        }
        return renderer.readImage();
    };
    // Where it ends, on its own (no history).
    auto native = gpu.makeRenderer(kWidth, kHeight);
    const ColorImage expected = sweep(kRaster, *native);

    const Upscaler upscaler = GENERATE(Upscaler::Taau, Upscaler::Fsr, Upscaler::Dlss);
    INFO(enumToString(upscaler));
    auto renderer = upscalingRenderer(gpu, upscaler, kWidth, kHeight);
    const ColorImage image =
        sweep(upscaled(kRaster, upscaler, UpscalerQuality::Performance), *renderer);
    // Its edges are reconstructed from fewer pixels; behind it, where it was a few frames
    // ago, no light is left.
    const BlockDifference difference = blockDifference(image, expected);
    INFO("worst block " << difference.worst << ", mean " << difference.mean);
    CHECK(difference.mean < 1.0F);
    const float ballX = ball.get<TransformComponent>().translation.x;
    for (const float back : {1.3F, 1.8F, 2.6F, 3.5F}) {
        const Vec2 behind = *camera.worldToScreen({ballX - back, 0.0F, 0.0F});
        const Vec3 trail = blockMean(image, static_cast<std::uint32_t>(behind.x) - 3,
                                     static_cast<std::uint32_t>(behind.y) - 3, 6);
        INFO(back << " behind the ball's center: " << trail.x);
        CHECK(trail.x < 4.0F);
    }
}

TEST_CASE("GPU upscaling: particles hide behind the rendered surfaces", "[gpu][upscaling]")
{
    const GpuTest gpu(false);
    Scene scene;  // A black ball on a black sky.
    scene.environment().skyColor = Vec3(0.0F);
    Entity ball = scene.createEntity("Ball");
    ball.add<SphereComponent>(1.0F);
    ball.add<MaterialComponent>(MaterialComponent{.albedo = Vec3(0.0F)});
    const RenderScene renderScene = buildRenderScene(scene);
    const Camera camera = makeCamera(128, 96);
    const RenderParticle particles[] = {
        {.position = {0.0F, 0.0F, 3.0F},  // In front of the ball.
         .size = 0.05F,
         .previousPosition = {0.0F, 0.0F, 3.0F},
         .color = {50.0F, 0.0F, 0.0F, 1.0F}},
        {.position = {0.0F, 0.0F, -3.0F},  // Behind it.
         .size = 0.05F,
         .previousPosition = {0.0F, 0.0F, -3.0F},
         .color = {0.0F, 50.0F, 0.0F, 1.0F}},
    };
    auto renderer = gpu.makeRenderer(128, 96);
    renderer->render(renderScene, camera, {},
                     upscaled(kRaster, Upscaler::Taau, UpscalerQuality::Performance), particles);
    REQUIRE(renderer->renderWidth() == 64);
    const ColorImage image = renderer->readImage();
    const Vec2 front = *camera.worldToScreen({0.0F, 0.0F, 3.0F});
    CHECK(channel(image.at(static_cast<std::uint32_t>(front.x), static_cast<std::uint32_t>(front.y)),
                  0) == 255);
    CHECK(countPixels(image, 1) == 0);
}
