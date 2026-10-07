#include "Horo/Prefab/PrefabTemplateCook.h"
#include "Horo/Prefab/PrefabTemplateProvider.h"
#include "PrefabTestUtils.h"
#include "assets/AssetCookOutputFixture.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Prefab {
    namespace {
        using namespace Assets::OutputTestSupport;

        PrefabLimitProfile Profile() {
            return PrefabLimitProfile::Create({}).Value();
        }

        std::vector<std::uint8_t> CookRoot(const Assets::AssetRegistrySnapshot &registry, const float translation) {
            PrefabDocumentData data{.projectVersion = Application::ParseHoroVersion("1.2.3").Value(),
                                    .assetId = Test::Asset(),
                                    .objects = {{.localId = {0}, .name = "Root"}}};
            data.objects[0].localTransform.translation.x = translation;
            auto document = PrefabDocument::Create(std::move(data), Profile());
            REQUIRE(document.HasValue());
            const auto canonical = document.Value().SerializeCanonical().Value();
            const auto digest = ComputeSha256(std::as_bytes(std::span(canonical.data(), canonical.size())));
            const PrefabSourceRevision revision{document.Value().Data().projectVersion, digest};
            auto sources = BuildPrefabSourceResolverSnapshot(registry, {{std::move(document).Value(), revision}}, Profile());
            REQUIRE(sources.HasValue());
            auto cooked = CookPrefabTemplate(sources.Value(), registry, Test::Asset(), {}, Target("headless-null"), Profile());
            REQUIRE(cooked.HasValue());
            std::vector<std::uint8_t> payload;
            for (const auto byte : cooked.Value().Bytes())
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            auto envelope = Assets::EncodeCookedArtifact({.id = Test::Asset(),
                                                          .type = Type("core.prefab"),
                                                          .target = Target("headless-null"),
                                                          .sourceDigest = digest,
                                                          .payloadDigest = DigestOf(payload),
                                                          .payload = std::move(payload)});
            REQUIRE(envelope.HasValue());
            return std::move(envelope).Value();
        }

        Assets::AssetCookManifestEntry RootEntry(const std::vector<std::uint8_t> &bytes) {
            return {.assetId = Test::Asset(),
                    .assetType = Type("core.prefab"),
                    .artifactFile = Test::Asset().ToString() + ".cooked",
                    .artifactHash = DigestOf(bytes)};
        }

        void ActivateEmptyScene(Runtime::RuntimeSceneService &scenes) {
            REQUIRE(scenes.Startup({}).HasValue());
            Runtime::SceneDefinitionBuilder builder{{7}, {1}};
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            REQUIRE(scenes.QueuePreparation(std::move(definition).Value()).HasValue());
            const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, {}};
            REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        }

        Result<PrefabTemplateLease> LoadTemplate(PrefabTemplateProvider &provider, const Sha256Digest &digest) {
            auto loading = provider.LoadAsync({Test::Asset(), digest, Target("headless-null")});
            REQUIRE(loading.HasValue());
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            while (loading.Value().State() == PrefabTemplateLoadState::Loading && std::chrono::steady_clock::now() < deadline) {
                REQUIRE(provider.Advance().HasValue());
                std::this_thread::yield();
            }
            return provider.TakeResult(loading.Value());
        }
    }  // namespace

    TEST_CASE("Published source cooked template loads through the filesystem generation provider", "[prefab][template-cook][native]") {
        TempDir directory;
        Assets::AssetRegistry registry;
        REQUIRE(registry
                    .Publish({{Test::Asset(), Type("core.prefab"), ProjectPath::Parse("assets/root.prefab").Value(),
                               ProjectPath::Parse("assets/root.prefab.horo").Value()}})
                    .status == Assets::AssetRegistryBuildStatus::Complete);
        const std::array payloads{CookRoot(registry.Snapshot(), 3)};
        const std::vector entries{RootEntry(payloads.front())};
        auto published = PublishFixture(directory.path, Target("headless-null"), entries, payloads);
        REQUIRE(published.HasValue());
        auto current = Assets::ResolveCurrentCookGeneration(directory.path);
        REQUIRE(current.HasValue());
        CHECK(current.Value().manifestDigest == published.Value().manifestDigest);
        Assets::FilesystemAssetProvider bytes(current.Value().generationRoot);
        JobSystem jobs{{1, 8}};
        Assets::AssetLoadService loads(jobs, bytes);
        Runtime::RuntimeSceneService scenes(registry, loads);
        ActivateEmptyScene(scenes);
        PrefabTemplateProvider provider(registry, loads, scenes, Profile());
        REQUIRE(provider.Startup({}).HasValue());
        auto lease = LoadTemplate(provider, entries.front().artifactHash);
        REQUIRE(lease.HasValue());
        CHECK(lease.Value().Template()->Data().entities.front().localTransform.translation.x == 3);

        const std::array replacementPayloads{CookRoot(registry.Snapshot(), 9)};
        const std::vector replacementEntries{RootEntry(replacementPayloads.front())};
        NativeDurableFileSystem files;
        auto lock = files.TryAcquireExclusive(directory.path / ".cook-writer.lock", "cancelled template generation");
        REQUIRE(lock.HasValue());
        const Assets::AssetCookPublicationPolicy policy{.files = &files, .beforeCommit = [] {
            return Result<void>::Failure(MakeError(PrefabErrors::Cancelled));
        }, .operationId = "12345678-1234-4234-8234-123456789abc", .writerLease = &lock.Value()};
        CHECK(Assets::PublishCookGeneration(directory.path, Target("headless-null"), replacementEntries, replacementPayloads, {}, policy)
                  .HasError());
        auto retained = Assets::ResolveCurrentCookGeneration(directory.path);
        REQUIRE(retained.HasValue());
        CHECK(retained.Value().manifestDigest == published.Value().manifestDigest);
        CHECK(lease.Value().Template()->Data().entities.front().localTransform.translation.x == 3);
        provider.Shutdown();
        CHECK(lease.Value().Template()->Data().entities.front().localTransform.translation.x == 3);
    }
}  // namespace Horo::Prefab
