#include "Horo/Assets/AssetPreviewService.h"
#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/ContentBrowserModel.h"
#include "editor/screens/workspace/prefab/PrefabAssetPreview.h"

#include <atomic>
#include <thread>

namespace Horo::Editor {
    using namespace PrefabHierarchyTestSupport;

    namespace {
        Prefab::RawComponentPayload BoxPayload() {
            return {.instance = Prefab::PrefabComponentInstanceId::Create(1).Value(),
                    .component = {.typeId = Gameplay::ComponentTypeId::Parse("game.horo.primitive_mesh").Value(),
                                  .schemaVersion = 1,
                                  .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                  .payload = Bytes(R"({"id":"primitive.mesh.box","version":1,"parameters":{"size":[1,2,3]}})")}};
        }

        Prefab::EffectivePrefabCandidate BoxCandidate(const Math::Transform transform = {}) {
            const auto asset = PrefabAsset().Asset();
            auto resolver =
                ScenePrefabResolver(asset, {{.localId = {}, .name = "Box", .localTransform = transform, .components = {BoxPayload()}}});
            return resolver.Resolve(asset, Prefab::PrefabInstanceId::Create(1).Value(), ScenePrefabLimits()).Value();
        }

        Assets::AssetPreviewRequest Request(Project &project, std::shared_ptr<const Assets::IAssetPreviewProvider> provider) {
            return {.contributionId = "core.prefab.thumbnail",
                    .moduleId = "test",
                    .moduleVersion = "1",
                    .providerVersion = "1",
                    .absoluteAssetPath = project.source,
                    .assetType = Assets::AssetTypeId::Parse("core.prefab").Value(),
                    .provider = std::move(provider)};
        }
    }  // namespace

    TEST_CASE("Prefab cards classify exact typed registered identities independently from filenames", "[editor][prefab][preview]") {
        ContentBrowserEntry entry{.displayName = "not-a-template.txt",
                                  .assetType = "core.prefab",
                                  .registered = true,
                                  .registeredType = Assets::AssetTypeId::Parse("core.prefab").Value()};
        CHECK(entry.IsPrefab());
        entry.registeredType = Assets::AssetTypeId::Parse("project.prefab_backup").Value();
        CHECK_FALSE(entry.IsPrefab());
        entry.registeredType = Assets::AssetTypeId::Parse("core.mesh").Value();
        entry.displayName = "prefab.prefab";
        CHECK_FALSE(entry.IsPrefab());
        entry.registeredType = Assets::AssetTypeId::Parse("core.prefab").Value();
        entry.registered = false;
        CHECK_FALSE(entry.IsPrefab());
        entry.registered = true;
        entry.kind = ContentBrowserEntryKind::Directory;
        CHECK_FALSE(entry.IsPrefab());
        entry.kind = ContentBrowserEntryKind::Asset;
        entry.registeredType.reset();
        CHECK_FALSE(entry.IsPrefab());
    }

    TEST_CASE("Prefab directory search uses real typed registry records and retains unregistered broken files",
              "[editor][prefab][preview][native]") {
        Project project;
        auto directory = BuildContentBrowserDirectory(project.directory.path, project.source.parent_path(), project.registry.Snapshot());
        REQUIRE(directory.readable);
        REQUIRE(directory.entries.size() == 1);
        CHECK(directory.entries.front().IsPrefab());
        CHECK(ProjectContentBrowserEntries(directory, {.name = "HIERARCHY", .assetType = "core.prefab"}).size() == 1);
        CHECK(ProjectContentBrowserEntries(directory, {.name = "HIERARCHY", .assetType = "core.mesh"}).empty());
        auto broken = BuildContentBrowserDirectory(project.directory.path, project.source.parent_path(), {});
        REQUIRE(broken.entries.size() == 1);
        CHECK_FALSE(broken.entries.front().IsPrefab());
        CHECK_FALSE(broken.entries.front().registered);
        CHECK(broken.entries.front().absolutePath == directory.entries.front().absolutePath);
    }

    TEST_CASE("Prefab thumbnails render actual geometry deterministically", "[editor][prefab][preview]") {
        const auto candidate = BoxCandidate();
        auto first = RenderPrefabAssetPreview(candidate, 128, 128);
        auto second = RenderPrefabAssetPreview(candidate, 128, 128);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(first.Value().IsValid());
        CHECK(first.Value().pixels == second.Value().pixels);
        CHECK(std::ranges::any_of(first.Value().pixels, [](auto byte) {
            return byte != 0;
        }));
        Math::Transform stretched;
        stretched.scale = {3, 1, 1};
        auto changed = RenderPrefabAssetPreview(BoxCandidate(stretched), 128, 128);
        REQUIRE(changed.HasValue());
        CHECK(changed.Value().pixels != first.Value().pixels);
        CHECK(RenderPrefabAssetPreview(candidate, 16, 16).HasValue());
        CHECK(RenderPrefabAssetPreview(candidate, 256, 256).HasValue());
        CHECK(RenderPrefabAssetPreview(candidate, 0, 128).HasError());
        CHECK(RenderPrefabAssetPreview(candidate, 257, 128).HasError());
        CancellationSource stopped;
        stopped.RequestCancellation();
        auto cancelled = RenderPrefabAssetPreview(candidate, 128, 128, stopped.Token());
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code == Prefab::PrefabErrors::Cancelled.code);
    }

    TEST_CASE("Prefab thumbnails inherit parent transforms without activating a Scene", "[editor][prefab][preview]") {
        Math::Transform parent;
        parent.scale = {3, 1, 1};
        const auto asset = PrefabAsset().Asset();
        auto resolver = ScenePrefabResolver(asset, {{.localId = {}, .name = "Parent", .localTransform = parent},
                                                    {.localId = {1},
                                                     .parentLocalId = Prefab::LocalObjectId{0},
                                                     .name = "Child",
                                                     .components = {BoxPayload()}}});
        auto candidate = resolver.Resolve(asset, Prefab::PrefabInstanceId::Create(1).Value(), ScenePrefabLimits());
        REQUIRE(candidate.HasValue());
        auto inherited = RenderPrefabAssetPreview(candidate.Value(), 128, 128);
        auto direct = RenderPrefabAssetPreview(BoxCandidate(parent), 128, 128);
        REQUIRE(inherited.HasValue());
        REQUIRE(direct.HasValue());
        CHECK(inherited.Value().pixels == direct.Value().pixels);
    }

    TEST_CASE("Prefab provider reads real canonical geometry through the bounded host", "[editor][prefab][preview][native]") {
        Project project;
        auto document = ScenePrefabDocument(project.asset, {{.localId = {}, .name = "Box", .components = {BoxPayload()}}});
        auto source = document.SerializeCanonical();
        REQUIRE(source.HasValue());
        std::ofstream(project.source, std::ios::trunc) << source.Value();
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        Assets::AssetPreviewService service(jobs);
        auto provider = CreatePrefabAssetPreviewProvider(project.registry.Snapshot(), project.asset, project.Loader());
        auto admitted = service.Submit(Request(project, provider));
        REQUIRE(admitted.HasValue());
        REQUIRE(admitted.Value().Wait().HasValue());
        auto result = admitted.Value().TakeResult();
        REQUIRE(result.HasValue());
        CHECK(result.Value().image.IsValid());
        CHECK(std::ranges::any_of(result.Value().image.pixels, [](auto byte) {
            return byte != 0;
        }));
        service.Shutdown();
    }

    TEST_CASE("Prefab thumbnails preserve malformed primitive diagnostics", "[editor][prefab][preview]") {
        auto bad = BoxPayload();
        bad.component.payload = Bytes("{}");
        const auto asset = PrefabAsset().Asset();
        auto resolver = ScenePrefabResolver(asset, {{.localId = {}, .name = "Bad primitive", .components = {bad}}});
        auto candidate = resolver.Resolve(asset, Prefab::PrefabInstanceId::Create(1).Value(), ScenePrefabLimits());
        REQUIRE(candidate.HasValue());
        auto image = RenderPrefabAssetPreview(candidate.Value(), 128, 128);
        REQUIRE(image.HasError());
        CHECK(image.ErrorValue().code == Prefab::PrefabErrors::CorruptedPayload.code);
    }

    TEST_CASE("Prefab provider rejects source drift and malformed captured sources", "[editor][prefab][preview][native]") {
        Project project;
        const auto before = project.source;
        const std::string old = [&] {
            std::ifstream stream(before);
            return std::string(std::istreambuf_iterator<char>(stream), {});
        }();
        auto provider = CreatePrefabAssetPreviewProvider(project.registry.Snapshot(), project.asset, project.Loader());
        SECTION("root source changed after scheduler read") {
            project.WriteSource("New root");
        }
        SECTION("root source malformed") {
            std::ofstream(project.source, std::ios::trunc) << "{broken";
        }
        const Assets::AssetPreviewInput input{.editorPayload = std::span(reinterpret_cast<const std::uint8_t *>(old.data()), old.size()),
                                              .absoluteAssetPath = "unused diagnostic path",
                                              .assetType = project.record.type};
        CHECK(provider->GeneratePreview(input, {}).HasError());
    }

    TEST_CASE("Prefab thumbnail completion rejects edits in its captured closure", "[editor][prefab][preview][native]") {
        Project project;
        auto document = ScenePrefabDocument(project.asset, {{.localId = {}, .name = "Box", .components = {BoxPayload()}}});
        auto source = document.SerializeCanonical();
        REQUIRE(source.HasValue());
        std::ofstream(project.source, std::ios::trunc) << source.Value();
        int captures{};
        auto provider =
            CreatePrefabAssetPreviewProvider(project.registry.Snapshot(), project.asset,
                                             [&](const auto &registry, auto roots, const auto &limits, const auto &cancellation) {
            if (++captures == 2)
                project.WriteSource("Edited during rendering");
            return project.Capture(registry, roots, limits, cancellation);
        });
        const auto &bytes = source.Value();
        const Assets::AssetPreviewInput input{.editorPayload =
                                                  std::span(reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()),
                                              .absoluteAssetPath = "unused diagnostic path",
                                              .assetType = project.record.type};
        auto result = provider->GeneratePreview(input, {});
        REQUIRE(result.HasError());
        CHECK(captures == 2);
        CHECK(result.ErrorValue().code == Prefab::PrefabErrors::ResolutionStale.code);
    }

    TEST_CASE("Prefab source jobs join before borrowed project services retire", "[editor][prefab][preview][lifecycle]") {
        Project project;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        Assets::AssetPreviewService service(jobs, {.maximumOutstanding = 1});
        std::atomic<bool> entered{}, stopped{};
        auto provider =
            CreatePrefabAssetPreviewProvider(project.registry.Snapshot(), project.asset,
                                             [&](const auto &, auto, const auto &,
                                                 const CancellationToken &cancellation) -> Result<PreparedPrefabHierarchySources> {
            entered.store(true);
            while (!cancellation.IsCancellationRequested())
                std::this_thread::yield();
            stopped.store(true);
            return Result<PreparedPrefabHierarchySources>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
        });
        auto admitted = service.Submit(Request(project, provider));
        REQUIRE(admitted.HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!entered.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        REQUIRE(entered.load());
        auto overflow = service.Submit(Request(project, provider));
        REQUIRE(overflow.HasError());
        CHECK(overflow.ErrorValue().code.Value() == "asset.preview.queue_full");
        service.Shutdown();
        CHECK(stopped.load());
        CHECK(admitted.Value().State() == Assets::AssetPreviewState::Cancelled);
        CHECK(service.Submit(Request(project, provider)).HasError());
    }
}  // namespace Horo::Editor
