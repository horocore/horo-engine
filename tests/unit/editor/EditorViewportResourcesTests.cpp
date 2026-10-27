#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraph.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/EditorViewportResources.h"
#include "runtime/renderer/frontend/RenderFrontendErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Render;

    constexpr int MaxResourcePasses = 10;

    class ImageResolverFixture final {
    public:
        ImageResolverFixture() noexcept {
            active_ = this;
        }

        ~ImageResolverFixture() {
            active_ = nullptr;
        }

        ImageResolverFixture(const ImageResolverFixture &) = delete;
        ImageResolverFixture &operator=(const ImageResolverFixture &) = delete;

        [[nodiscard]] static Result<std::uintptr_t> Resolve(const RenderFrontend &, const RenderTextureViewHandle view) {
            REQUIRE(active_ != nullptr);
            active_->lastView = view;
            if (active_->reject) {
                return Result<std::uintptr_t>::Failure(
                    MakeError(RendererErrors::ViewportGeometryCreationFailed, "Injected viewport image resolution failure."));
            }
            return Result<std::uintptr_t>::Success(view.slot);
        }

        RenderTextureViewHandle lastView;
        bool reject{false};

    private:
        static thread_local ImageResolverFixture *active_;
    };

    thread_local ImageResolverFixture *ImageResolverFixture::active_ = nullptr;

    [[nodiscard]] std::unique_ptr<RenderFrontend> CreateFrontend(const RenderResourceUploadLimits &uploadLimits = {}) {
        RenderBackendRegistry registry;
        REQUIRE(RegisterNullRenderBackend(registry).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"null"}, RenderBackendConfig{}, uploadLimits);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    [[nodiscard]] EditorViewportResources CreateResources(RenderFrontend &frontend) {
        return EditorViewportResources{
            frontend,
            {.depthFormat = RenderTextureFormat::Depth32Float,
             .depthAspect = RenderTextureAspect::Depth,
             .resolveImage = &ImageResolverFixture::Resolve},
        };
    }

    void SettleResources(EditorViewportResources &resources, RenderFrontend &frontend, const RenderSceneView &scene,
                         const EditorViewportExtent extent) {
        for (int step = 0; step < MaxResourcePasses && (!resources.IsReady() || resources.AllocatedExtent() != extent); ++step) {
            REQUIRE(resources.Prepare(scene, extent).HasValue());
            REQUIRE(frontend.ProcessResourceRequests().HasValue());
        }
        REQUIRE(resources.IsReady());
        REQUIRE(resources.AllocatedExtent() == extent);
    }

    struct MeshSceneFixture {
        std::array<MeshVertex, 3> vertices{{
            {.position = {-1.0F, -1.0F, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}, .uv = {0.0F, 0.0F}},
            {.position = {1.0F, -1.0F, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}, .uv = {1.0F, 0.0F}},
            {.position = {0.0F, 1.0F, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}, .uv = {0.5F, 1.0F}},
        }};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::array<RenderMeshResourceView, 1> resources;

        [[nodiscard]] RenderSceneView Scene(const std::uint32_t generation) {
            resources[0] = {
                .handle = {.id = MeshResourceId{41}, .generation = generation},
                .vertices = vertices,
                .indices = indices,
                .localBounds = {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}},
            };
            return {.meshResources = resources};
        }
    };
}  // namespace

TEST_CASE("Cold mesh preparation admits exact ready buffers and mesh before graph imports", "[unit][headless][renderer][resource]") {
    auto frontend = CreateFrontend();
    SECTION("default canonical drain limits") {
        REQUIRE(frontend->UploadSnapshot().acceptingRequests);
    }
    SECTION("one request per canonical drain") {
        frontend = CreateFrontend({.maximumRequestsPerDrain = 1});
    }
    auto resources = CreateResources(*frontend);
    MeshSceneFixture fixture;
    const auto scene = fixture.Scene(1);
    REQUIRE(resources.PrepareMeshesCold(scene.meshResources).HasValue());
    const auto mesh = resources.FindMesh(41);
    REQUIRE(mesh);
    REQUIRE(mesh->mesh.IsValid());
    const auto meshState = frontend->ResourceState(mesh->mesh);
    const auto vertexState = frontend->ResourceState(mesh->descriptor.vertexBuffer);
    const auto indexState = frontend->ResourceState(mesh->descriptor.indexBuffer);
    REQUIRE(meshState.HasValue());
    REQUIRE(vertexState.HasValue());
    REQUIRE(indexState.HasValue());
    CHECK(meshState.Value() == RenderResourceState::Ready);
    CHECK(vertexState.Value() == RenderResourceState::Ready);
    CHECK(indexState.Value() == RenderResourceState::Ready);
    CHECK(mesh->descriptor.vertexCount == fixture.vertices.size());
    CHECK(mesh->descriptor.indexCount == fixture.indices.size());
    auto created = RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 2, .maxUsages = 2, .maxDependencies = 1});
    REQUIRE(created.HasValue());
    auto builder = std::move(created).Value();
    const auto vertices = builder.ImportBuffer(mesh->descriptor.vertexBuffer, RenderGraphResourceClass::Persistent);
    const auto indices = builder.ImportBuffer(mesh->descriptor.indexBuffer, RenderGraphResourceClass::Persistent);
    const auto pass = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
    REQUIRE(vertices.HasValue());
    REQUIRE(indices.HasValue());
    REQUIRE(pass.HasValue());
    const auto vertexUse = builder.AddUsage({pass.Value(), vertices.Value(), RenderGraphAccess::Read, RenderGraphUsageKind::VertexBuffer});
    const auto indexUse = builder.AddUsage({pass.Value(), indices.Value(), RenderGraphAccess::Read, RenderGraphUsageKind::IndexBuffer});
    REQUIRE(vertexUse.HasValue());
    REQUIRE(indexUse.HasValue());
    REQUIRE(builder.Finalize().HasValue());
    const auto replacementScene = fixture.Scene(2);
    REQUIRE(resources.PrepareMeshesCold(replacementScene.meshResources).HasValue());
    const auto replacement = resources.FindMesh(41);
    REQUIRE(replacement);
    CHECK(replacement->mesh != mesh->mesh);
    CHECK(replacement->descriptor.vertexBuffer != mesh->descriptor.vertexBuffer);
    resources.Shutdown();
}

TEST_CASE("Cold mesh preparation preserves canonical partial rollback and shutdown safety", "[unit][headless][renderer][resource]") {
    auto frontend = CreateFrontend({.maximumRequestsPerDrain = 1, .maximumPendingRequests = 1});
    auto resources = CreateResources(*frontend);
    MeshSceneFixture fixture;
    const auto scene = fixture.Scene(1);
    const auto prepared = resources.PrepareMeshesCold(scene.meshResources);
    REQUIRE(prepared.HasError());
    CHECK(prepared.ErrorValue().code.Value() == FrontendErrors::ResourceUploadCapacityExceeded.code.Value());
    CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(frontend->UploadSnapshot().cancelledRequestCount == 1);
    resources.Shutdown();
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(frontend->UploadSnapshot().cancelledRequestCount == 1);
}

TEST_CASE("Headless viewport resources replace resize generations atomically", "[unit][headless][renderer][resource]") {
    ImageResolverFixture imageResolver;
    std::unique_ptr<RenderFrontend> frontend = CreateFrontend();
    EditorViewportResources resources = CreateResources(*frontend);
    const RenderSceneView scene{};

    SettleResources(resources, *frontend, scene, {320, 180});
    REQUIRE(resources.IsReady());
    REQUIRE(resources.AllocatedExtent() == EditorViewportExtent{320, 180});
    const RenderTargetHandle original = resources.Target();
    const std::uintptr_t originalImage = resources.ImageIdentity();

    const auto pendingResize = resources.Prepare(scene, {640, 360});
    REQUIRE(pendingResize.HasValue());
    REQUIRE(pendingResize.Value() == std::optional<RenderTargetHandle>{original});
    REQUIRE(resources.Target() == original);
    REQUIRE(resources.ImageIdentity() == originalImage);
    REQUIRE(frontend->ProcessResourceRequests().HasValue());

    SettleResources(resources, *frontend, scene, {640, 360});
    REQUIRE(resources.IsReady());
    REQUIRE(resources.AllocatedExtent() == EditorViewportExtent{640, 360});
    REQUIRE(resources.Target() != original);
    const auto retired = frontend->ResourceState(original);
    REQUIRE(retired.HasError());
    REQUIRE(retired.ErrorValue().code.Value() == "render.frontend.resource.stale");

    const RenderTargetHandle replacement = resources.Target();
    const auto steady = resources.Prepare(scene, {640, 360});
    REQUIRE(steady.HasValue());
    REQUIRE(steady.Value() == std::optional<RenderTargetHandle>{replacement});
    const auto processed = frontend->ProcessResourceRequests();
    REQUIRE(processed.HasValue());
    REQUIRE(processed.Value() == 0);
    REQUIRE(resources.Target() == replacement);
}

TEST_CASE("Headless viewport resources preserve active meshes until replacements are ready", "[unit][headless][renderer][resource]") {
    ImageResolverFixture imageResolver;
    std::unique_ptr<RenderFrontend> frontend = CreateFrontend();
    EditorViewportResources resources = CreateResources(*frontend);
    MeshSceneFixture fixture;

    RenderSceneView scene = fixture.Scene(1);
    SettleResources(resources, *frontend, scene, {320, 180});
    const auto original = resources.FindMesh(41);
    REQUIRE(original.has_value());

    scene = fixture.Scene(2);
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());
    const auto firstProcessed = frontend->ProcessResourceRequests();
    REQUIRE(firstProcessed.HasValue());
    REQUIRE(firstProcessed.Value() > 0);
    const auto firstPending = resources.FindMesh(41);
    REQUIRE(firstPending.has_value());
    REQUIRE(firstPending->mesh == original->mesh);
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());
    const auto secondProcessed = frontend->ProcessResourceRequests();
    REQUIRE(secondProcessed.HasValue());
    REQUIRE(secondProcessed.Value() > 0);
    const auto secondPending = resources.FindMesh(41);
    REQUIRE(secondPending.has_value());
    REQUIRE(secondPending->mesh == original->mesh);
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());

    const auto replacement = resources.FindMesh(41);
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->mesh != original->mesh);
    REQUIRE(frontend->ResourceState(original->mesh).HasError());

    REQUIRE(resources.Prepare(RenderSceneView{}, {320, 180}).HasValue());
    REQUIRE_FALSE(resources.FindMesh(41).has_value());
    REQUIRE(frontend->ResourceState(replacement->mesh).HasError());
}

TEST_CASE("Headless viewport resources recover image resolution and shut down repeatedly", "[unit][headless][renderer][resource]") {
    ImageResolverFixture imageResolver;
    imageResolver.reject = true;
    std::unique_ptr<RenderFrontend> frontend = CreateFrontend();
    EditorViewportResources resources = CreateResources(*frontend);
    const RenderSceneView scene{};

    std::optional<Error> resolutionError;
    for (int step = 0; step < MaxResourcePasses && !resolutionError.has_value(); ++step) {
        const auto prepared = resources.Prepare(scene, {320, 180});
        if (prepared.HasError()) {
            resolutionError = prepared.ErrorValue();
            break;
        }
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
    }
    REQUIRE(resolutionError.has_value());
    REQUIRE(resolutionError->code.Value() == RendererErrors::ViewportGeometryCreationFailed.code.Value());
    REQUIRE(imageResolver.lastView.IsValid());
    REQUIRE_FALSE(resources.Target().IsValid());

    imageResolver.reject = false;
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());
    REQUIRE(resources.IsReady());
    const RenderTargetHandle target = resources.Target();
    const RenderTargetHandle shadowTarget = resources.ShadowTarget();

    resources.Shutdown();
    resources.Shutdown();
    REQUIRE(frontend->ResourceState(target).HasError());
    REQUIRE(frontend->ResourceState(shadowTarget).HasError());
    REQUIRE_FALSE(resources.IsReady());
}
