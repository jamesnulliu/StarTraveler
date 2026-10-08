// GPU path tracer vs the CPU reference. The GPU runs the same estimator with
// the same random streams, so the images agree almost bit for bit (on an RTX
// 4060 and an Intel Arc iGPU, 8x8 block means differ by at most 1/64). Other
// GPUs may round differently and let paths diverge, so the tolerances below
// are statistical; primary visibility (entity ids) must agree almost everywhere.
//
// STARCORE_VULKAN_DEVICE=Intel (any part of a device name) picks another GPU.

#include <algorithm>
#include <memory>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <glm/common.hpp>

#include <starcore/asset/mesh.hpp>
#include <starcore/gpu/gpu_path_tracer.hpp>
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

/** @brief Skips the test without ray queries; fails it on new validation errors. */
class GpuTest
{
public:
    GpuTest() : m_context(gpuContext())
    {
        if (m_context == nullptr || !m_context->deviceInfo().rayQuery) {
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
    [[nodiscard]] auto makeTracer(std::uint32_t width, std::uint32_t height) const
        -> std::unique_ptr<GpuPathTracer>
    {
        auto tracer = GpuPathTracer::create(*m_context);
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

    auto tracer = gpu.makeTracer(64, 48);
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

    auto tracer = gpu.makeTracer(32, 32);
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
        auto tracer = gpu.makeTracer(kWidth, kHeight);
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
    const RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 64;
    constexpr std::uint32_t kHeight = 48;
    constexpr std::uint32_t kBlock = 8;
    const Camera camera = makeCamera(kWidth, kHeight);

    auto compare = [&](const PathTracerSettings& settings, std::uint32_t frames,
                       float blockTolerance, float meanTolerance) {
        CpuPathTracer cpu;
        cpu.resize(kWidth, kHeight);
        auto tracer = gpu.makeTracer(kWidth, kHeight);
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
}

TEST_CASE("GPU: accumulation restarts and scenes update", "[gpu]")
{
    const GpuTest gpu;
    TestScene test;
    constexpr std::uint32_t kSize = 48;
    Camera camera = makeCamera(kSize, kSize);
    auto tracer = gpu.makeTracer(kSize, kSize);
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

// Hybrid pipeline ------------------------------------------------------------------------------------

namespace
{

auto hybridOptions(bool spatial = true) -> GpuRenderOptions
{
    GpuRenderOptions options{.pipeline = GpuPipeline::Hybrid};
    options.denoiser.spatial = spatial;
    return options;
}

}  // namespace

TEST_CASE("GPU hybrid: emissive sphere, sky and white furnace", "[gpu][hybrid]")
{
    const GpuTest gpu;
    for (const Vec3 sky : {Vec3(0.0F), Vec3(0.0F, 0.0F, 1.0F)}) {
        Scene scene;
        scene.environment().skyColor = sky;
        Entity ball = scene.createEntity("Ball");
        ball.add<SphereComponent>(2.0F);
        ball.add<MaterialComponent>(MaterialComponent{
            .albedo = Vec3(0.0F), .emissionColor = {1.0F, 0.0F, 0.0F}, .emissionPower = 1.0F});
        auto tracer = gpu.makeTracer(64, 48);
        tracer->render(buildRenderScene(scene), makeCamera(64, 48), {}, hybridOptions());
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
        auto tracer = gpu.makeTracer(32, 32);
        const RenderScene renderScene = buildRenderScene(scene);
        const PathTracerSettings settings{.maxBounces = 16, .srgbOutput = false};
        for (int i = 0; i < 4; ++i) {
            tracer->render(renderScene, makeCamera(32, 32), settings, hybridOptions());
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
        auto reference = gpu.makeTracer(kWidth, kHeight);
        reference->render(renderScene, camera, settings);
        auto hybrid = gpu.makeTracer(kWidth, kHeight);
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

    auto reference = gpu.makeTracer(kWidth, kHeight);
    for (int i = 0; i < 128; ++i) {
        reference->render(renderScene, camera, settings);
    }
    const ColorImage expected = reference->readImage();

    for (const bool spatial : {false, true}) {
        INFO("spatial filter: " << spatial);
        auto hybrid = gpu.makeTracer(kWidth, kHeight);
        for (int i = 0; i < 128; ++i) {
            hybrid->render(renderScene, camera, settings, hybridOptions(spatial));
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

    auto reference = gpu.makeTracer(kWidth, kHeight);
    for (int i = 0; i < 128; ++i) {
        reference->render(renderScene, moved,
                          PathTracerSettings{.samplesPerPixel = 8, .fireflyClamp = 8.0F});
    }
    const ColorImage expected = reference->readImage();

    // 32 still frames, then 4 frames moving to `moved`; or one frame there.
    auto render = [&](bool spatial, bool temporal) {
        GpuRenderOptions options = hybridOptions(spatial);
        options.denoiser.temporal = temporal;
        auto tracer = gpu.makeTracer(kWidth, kHeight);
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
    INFO("mean error: reprojected " << reused << ", one frame " << fresh << ", filtered "
                                    << filtered);
    CHECK(reused < 0.5F * fresh);
    CHECK(filtered < 0.6F * fresh);
}

TEST_CASE("GPU hybrid: what restarts the accumulation", "[gpu][hybrid]")
{
    const GpuTest gpu;
    TestScene test;
    RenderScene renderScene = buildRenderScene(test.scene);
    constexpr std::uint32_t kWidth = 48;
    constexpr std::uint32_t kHeight = 36;
    Camera camera = makeCamera(kWidth, kHeight);
    const PathTracerSettings settings;
    auto hybrid = gpu.makeTracer(kWidth, kHeight);
    for (int i = 0; i < 3; ++i) {
        hybrid->render(renderScene, camera, settings, hybridOptions());
    }
    REQUIRE(hybrid->accumulatedFrames() == 3);

    // Moving the camera or an entity reprojects; display settings change nothing.
    camera.setPosition(camera.position() + Vec3(0.1F, 0.0F, 0.0F));
    hybrid->render(renderScene, camera, settings, hybridOptions());
    test.mirror.get<TransformComponent>().translation += Vec3(0.05F, 0.0F, 0.0F);
    test.scene.markDirty();
    renderScene = buildRenderScene(test.scene);
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
    renderScene = buildRenderScene(test.scene);
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

    // Debug views, resizing, an empty scene.
    for (std::size_t view = 0; view < EnumNames<HybridView>::kNames.size(); ++view) {
        GpuRenderOptions options = hybridOptions();
        options.view = static_cast<HybridView>(view);
        hybrid->render(renderScene, camera, bounces, options);
    }
    REQUIRE(hybrid->accumulatedFrames() == 9);
    hybrid->resize(40, 30);
    Scene empty;
    empty.environment().skyColor = {1.0F, 0.0F, 0.0F};
    hybrid->render(buildRenderScene(empty), makeCamera(40, 30), settings, hybridOptions());
    const ColorImage sky = hybrid->readImage();
    REQUIRE(sky.at(10, 10) == packRgba8({1.0F, 0.0F, 0.0F, 1.0F}));
}
