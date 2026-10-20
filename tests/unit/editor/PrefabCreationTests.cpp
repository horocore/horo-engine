#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "PrefabCreationTestSupport.h"
#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabCreation.h"

#include <array>
#include <catch2/generators/catch_generators.hpp>
#include <fstream>
#include <new>
#include <span>
#include <stdexcept>

namespace Horo::Editor {
    namespace {
        using namespace PrefabHierarchyTestSupport;
        using namespace PrefabCreationTestSupport;

        /** @brief Exception families that must all retain publication ownership through rollback. */
        enum class PublicationException {
            Provider,
            Allocation,
            NonStandard
        };

        /** @brief Provider failure deliberately outside the standard exception hierarchy. */
        struct ForeignProviderFailure final {};

        /** @brief Probes actual cooperating writer admission both during rollback and after transaction lease release. */
        void CheckWriterAdmission(Creation &fixture, const std::array<std::filesystem::path, 3> &sources, const bool available) {
            const auto project = fixture.mutations.TryAcquire({fixture.root, ProjectMutationOwner::Save, "tests.prefab.rollback_probe"});
            CHECK(project.HasValue() == available);
            for (const auto &source : sources) {
                const auto path = Assets::AssetSourcePublicationLockPath(fixture.root, source);
                REQUIRE(path.HasValue());
                const auto writer = fixture.files.native.TryAcquireExclusive(path.Value(), "tests.prefab.rollback_probe");
                CHECK(writer.HasValue() == available);
            }
        }
    }  // namespace

    TEST_CASE("Prefab creation commits portable source sidecar placement and one canonical undo step", "[prefab][creation][native]") {
        Creation fixture;
        const auto baseline = fixture.document.Snapshot();
        const auto result = fixture.Run();
        REQUIRE(result.committed);
        CHECK_FALSE(result.error);
        CHECK(fixture.document.IsDirty());
        REQUIRE(fixture.document.Objects().size() == 1);
        REQUIRE(fixture.document.PrefabInstances().size() == 1);
        const auto &placement = fixture.document.PrefabInstances().front();
        CHECK(placement.sourcePrefab.Asset() == fixture.asset);
        CHECK(placement.parent == SceneObjectId{10});
        CHECK(placement.rootTransform == baseline.objects[1].localTransform);
        const auto limits = Prefab::PrefabLimitProfile::Create({}).Value();
        const auto source = Prefab::PrefabDocument::Parse(fixture.Read(fixture.destination), limits);
        REQUIRE(source.HasValue());
        REQUIRE(source.Value().Data().objects.size() == 2);
        CHECK(source.Value().Data().objects.front().localTransform == Math::Transform{});
        CHECK(source.Value().Data().objects[1].parentLocalId == Prefab::LocalObjectId{0});
        CHECK(source.Value().Data().objects[1].localTransform == baseline.objects[2].localTransform);
        REQUIRE(source.Value().Data().objects[1].components.size() == 1);
        Runtime::RuntimeComponentSet components;
        std::optional<Runtime::PrimitiveMeshDescriptor> primitive;
        REQUIRE(SceneSource::ProjectPrefabBuiltinComponent(source.Value().Data().objects[1].components.front(), components, primitive)
                    .HasValue());
        CHECK(components.camera == baseline.objects[2].components.camera);
        const auto sidecar = Assets::DecodeAssetIdentitySidecar("assets/prefab ü 日本.prefab", fixture.Read(result.files[1].path));
        REQUIRE(sidecar.HasValue());
        CHECK(sidecar.Value().id == fixture.asset);
        CHECK(result.files[0].identity->identity.has_value());

        REQUIRE(fixture.scene.Replay(fixture.Context(), false).HasValue());
        CHECK(fixture.document.Objects().size() == baseline.objects.size());
        CHECK(fixture.document.PrefabInstances().empty());
        CHECK_FALSE(fixture.document.IsDirty());
        CHECK_FALSE(fixture.history.CanUndo());
        CHECK(std::filesystem::exists(fixture.destination));  // Undo never silently deletes a reusable registered source.
        REQUIRE(fixture.scene.Replay(fixture.Context(), true).HasValue());
        CHECK(fixture.document.PrefabInstances().size() == 1);
        CHECK(fixture.document.IsDirty());
    }

    TEST_CASE("Prefab exclusive publication failures preserve original dirty state history and authored subtree",
              "[prefab][creation][native]") {
        const auto failAt = GENERATE(1U, 2U);
        const bool dirty = GENERATE(false, true);
        Creation fixture;
        if (dirty) {
            SceneDocumentCommandExecutor commands{fixture.document, fixture.history};
            REQUIRE(commands.Execute(RenameSceneObjectCommand{{10}, "Authored edit"}).HasValue());
        }
        const auto before = fixture.document.Snapshot();
        const bool history = fixture.history.CanUndo();
        fixture.files.failAt = failAt;
        const auto result = fixture.Run();
        REQUIRE_FALSE(result.committed);
        REQUIRE(result.error);
        CHECK(fixture.document.State() == before.state);
        CHECK(fixture.document.Revision() == before.revision);
        CHECK(fixture.document.IsDirty() == dirty);
        CHECK(fixture.history.CanUndo() == history);
        CHECK(fixture.document.Objects().size() == 3);
        CHECK(fixture.document.PrefabInstances().empty());
        CHECK_FALSE(std::filesystem::exists(fixture.destination));
        if (failAt == 2)
            CHECK(result.files[0].removed);
    }

    enum class Rejection {
        Stale,
        Unauthorized,
        Cancelled,
        ExistingSource,
        ExistingSidecar,
        Protected,
        MissingDirectory
    };

    /** @brief Configure a genuine pre-publication conflict without changing expected rollback invariants. */
    void ConfigureRejection(Creation &fixture, SceneTransactionContext &context, CancellationSource &cancellation,
                            std::optional<std::filesystem::path> &existing, const Rejection rejection) {
        switch (rejection) {
            case Rejection::Stale:
                ++context.revision;
                break;
            case Rejection::Unauthorized:
                context.canEdit = false;
                break;
            case Rejection::Cancelled:
                context.cancellation = cancellation.Token();
                cancellation.RequestCancellation();
                break;
            case Rejection::ExistingSource:
                existing = fixture.destination;
                break;
            case Rejection::ExistingSidecar:
                existing = fixture.destination.string() + ".horo";
                break;
            case Rejection::Protected:
                fixture.destination = fixture.root / ".horo/fail.prefab";
                break;
            case Rejection::MissingDirectory:
                fixture.destination = fixture.root / "assets/missing/fail.prefab";
                break;
        }
    }

    TEST_CASE("Prefab preparation rejects stale unauthorized cancelled and destination conflicts before publication",
              "[prefab][creation][native]") {
        const auto rejection = GENERATE(Rejection::Stale, Rejection::Unauthorized, Rejection::Cancelled, Rejection::ExistingSource,
                                        Rejection::ExistingSidecar, Rejection::Protected, Rejection::MissingDirectory);
        Creation fixture;
        auto context = fixture.Context();
        CancellationSource cancellation;
        std::optional<std::filesystem::path> existing;
        ConfigureRejection(fixture, context, cancellation, existing, rejection);
        if (existing)
            std::ofstream(*existing) << "foreign";
        const auto before = fixture.document.Snapshot();
        const auto result = fixture.Run(context);
        CHECK_FALSE(result.committed);
        REQUIRE(result.error);
        CHECK(fixture.files.writes == 0);
        CHECK(fixture.document.State() == before.state);
        CHECK_FALSE(fixture.history.CanUndo());
        if (existing)
            CHECK(fixture.Read(*existing) == "foreign");
    }

    TEST_CASE("Prefab filename policy rejects quotes and backslashes before native publication", "[prefab][creation][native]") {
        const auto filename = GENERATE("bad\"name.prefab", "bad\\name.prefab");
        Creation fixture;
        fixture.destination = fixture.root / "assets" / filename;
        const auto before = fixture.document.Snapshot();
        const auto result = fixture.Run();
        fixture.CheckUncommitted(result);
        CHECK(result.error->code.Value() == Assets::AssetSourceErrors::InvalidPath.code.Value());
        CHECK(fixture.files.writes == 0);
        CHECK(fixture.document.State() == before.state);
        CHECK(fixture.document.Revision() == before.revision);
        CHECK_FALSE(fixture.history.CanUndo());
    }

    TEST_CASE("Prefab failure receipts conservatively retain foreign replacements and unavailable identity", "[prefab][creation][native]") {
        const bool sameBytes = GENERATE(false, true);
        Creation fixture;
        std::string foreignBytes;
        std::optional<FileEntryIdentity> foreignIdentity;
        fixture.files.beforeSecond = [&] {
            foreignBytes = sameBytes ? fixture.Read(fixture.destination) : std::string{"foreign"};
            const auto other = fixture.root / "foreign.tmp";
            std::ofstream(other, std::ios::binary) << foreignBytes;
            std::filesystem::rename(other, fixture.destination);
            const auto inspected = fixture.files.native.InspectEntry(fixture.destination);
            REQUIRE(inspected.HasValue());
            REQUIRE(inspected.Value());
            foreignIdentity = inspected.Value()->identity;
            REQUIRE(foreignIdentity);
        };
        fixture.files.failAt = 2;
        const auto result = fixture.Run();
        CHECK_FALSE(result.committed);
        REQUIRE(result.files[0].created);
        CHECK_FALSE(result.files[0].removed);
        REQUIRE(result.files[0].recoveryError);
        REQUIRE(std::filesystem::exists(fixture.destination));
        CHECK(fixture.Read(fixture.destination) == foreignBytes);
        const auto retained = fixture.files.native.InspectEntry(fixture.destination);
        REQUIRE(retained.HasValue());
        REQUIRE(retained.Value());
        CHECK(retained.Value()->identity == foreignIdentity);
        REQUIRE(result.files[0].identity);
        CHECK(result.files[0].identity->identity != foreignIdentity);
        CHECK(fixture.document.Objects().size() == 3);
        CHECK_FALSE(fixture.document.IsDirty());
        CHECK_FALSE(fixture.history.CanUndo());
    }

    TEST_CASE("Prefab cleanup failures and provider exceptions retain actionable receipts without Scene commits",
              "[prefab][creation][native]") {
        enum class Failure {
            Cleanup,
            MissingIdentity,
            CleanupAllocation,
            Exception,
            OwnerShutdown,
            RevisionConflict,
            Cancellation
        };
        const auto failure = GENERATE(Failure::Cleanup, Failure::MissingIdentity, Failure::CleanupAllocation, Failure::Exception,
                                      Failure::OwnerShutdown, Failure::RevisionConflict, Failure::Cancellation);
        Creation fixture;
        auto context = fixture.Context();
        CancellationSource cancellation;
        context.cancellation = cancellation.Token();
        if (failure == Failure::Cleanup) {
            fixture.files.failAt = 2;
            fixture.files.failCleanup = true;
        } else if (failure == Failure::MissingIdentity) {
            fixture.files.omitIdentity = true;
        } else if (failure == Failure::CleanupAllocation) {
            fixture.files.failAt = 2;
            fixture.files.throwInspectionAfterSecond = true;
        } else if (failure == Failure::Exception) {
            fixture.files.throwSecond = true;
        } else {
            fixture.files.beforeSecond = [&] {
                if (failure == Failure::OwnerShutdown)
                    fixture.scene.Shutdown();
                else if (failure == Failure::Cancellation)
                    cancellation.RequestCancellation();
                else {
                    SceneDocumentCommandExecutor commands{fixture.document, fixture.history};
                    REQUIRE(commands.Execute(RenameSceneObjectCommand{{10}, "Concurrent owner edit"}).HasValue());
                }
            };
        }
        const auto result = fixture.Run(context);
        fixture.CheckUncommitted(result);
        if (failure == Failure::Cleanup || failure == Failure::MissingIdentity || failure == Failure::CleanupAllocation) {
            CHECK(std::filesystem::exists(fixture.destination));
            REQUIRE(result.files[0].recoveryError);
        } else {
            CHECK_FALSE(std::filesystem::exists(fixture.destination));
        }
        CHECK(fixture.document.IsDirty() == (failure == Failure::RevisionConflict));
        CHECK(fixture.history.CanUndo() == (failure == Failure::RevisionConflict));
    }

    TEST_CASE("Prefab exception rollback retains project and all source writer leases until cleanup completes",
              "[prefab][creation][native][leases]") {
        const auto exception =
            GENERATE(PublicationException::Provider, PublicationException::Allocation, PublicationException::NonStandard);
        Creation fixture;
        const auto before = fixture.document.Snapshot();
        const auto fence = fixture.SaveSceneFence();
        const auto original = fixture.Read(fence.path);
        auto metadata = fixture.destination;
        metadata += ".horo";
        const std::array sources{fixture.destination, metadata, fence.path};
        std::size_t observedCleanup{};
        fixture.files.beforeSecond = [&] {
            if (exception == PublicationException::Allocation)
                throw std::bad_alloc{};
            if (exception == PublicationException::NonStandard)
                throw ForeignProviderFailure{};
            throw std::runtime_error{"provider write failure"};
        };
        fixture.files.beforeCleanup = [&](const std::filesystem::path &removed) {
            ++observedCleanup;
            CHECK(removed == fixture.destination);
            CheckWriterAdmission(fixture, sources, false);
        };
        const auto result = fixture.Run({}, fence);
        fixture.files.beforeSecond = {};
        fixture.files.beforeCleanup = {};
        fixture.CheckUncommitted(result);
        const auto expected = exception == PublicationException::Allocation ? Prefab::PrefabErrors::ExpansionCacheAllocationFailed
                                                                            : Assets::AssetSourceErrors::ReadFailed;
        CHECK(result.error->code.Value() == expected.code.Value());
        CHECK(observedCleanup == 1);
        CheckWriterAdmission(fixture, sources, true);
        CHECK_FALSE(std::filesystem::exists(fixture.destination));
        CHECK_FALSE(std::filesystem::exists(metadata));
        CHECK(fixture.Read(fence.path) == original);
        CHECK(fixture.document.State() == before.state);
        CHECK(fixture.document.Revision() == before.revision);
        CHECK_FALSE(fixture.document.IsDirty());
        CHECK_FALSE(fixture.history.CanUndo());
    }

    TEST_CASE("Prefab failed writes with a native creation receipt retain unverified sidecars for explicit reconciliation",
              "[prefab][creation][native]") {
        Creation fixture;
        fixture.files.failUncapturedSecond = true;
        fixture.files.partialSecond = GENERATE(false, true);
        const auto result = fixture.Run();
        CHECK_FALSE(result.committed);
        REQUIRE(result.error);
        REQUIRE(result.files[1].created);
        CHECK_FALSE(result.files[1].identity);
        CHECK_FALSE(result.files[1].removed);
        REQUIRE(result.files[1].recoveryError);
        CHECK(std::filesystem::exists(result.files[1].path));
        CHECK(result.files[0].removed);
        CHECK_FALSE(std::filesystem::exists(fixture.destination));
        CHECK_FALSE(fixture.document.IsDirty());
        CHECK_FALSE(fixture.history.CanUndo());
        CHECK(fixture.document.Objects().size() == 3);
        CHECK(fixture.document.PrefabInstances().empty());
    }

    TEST_CASE("Prefab source admission bounds complete subtrees and rejects unsupported and protected data before writes",
              "[prefab][creation]") {
        enum class Case {
            AtLimit,
            OverLimit,
            UnsupportedSchema,
            Locked,
            Nested
        };
        const auto test = GENERATE(Case::AtLimit, Case::OverLimit, Case::UnsupportedSchema, Case::Locked, Case::Nested);
        Creation fixture;
        auto snapshot = fixture.scene.Snapshot().Value();
        if (test == Case::AtLimit || test == Case::OverLimit) {
            snapshot.objects = {{.id = {20}, .name = "Root"}};
            const std::size_t size = Prefab::PrefabHardLimits::SourceObjectCount + (test == Case::OverLimit ? 1U : 0U);
            for (std::size_t index = 1; index != size; ++index)
                snapshot.objects.push_back({.id = {100 + index}, .parent = SceneObjectId{20}, .name = "Child"});
        } else if (test == Case::UnsupportedSchema) {
            snapshot.objects.back().components.audioListener.emplace();
        } else if (test == Case::Locked) {
            snapshot.objects.back().editorState.locked = true;
        } else {
            snapshot.prefabInstances.push_back(
                {.instanceId = Prefab::PrefabInstanceId::Create(1).Value(), .sourcePrefab = PrefabAsset(), .parent = SceneObjectId{20}});
        }
        std::vector<PrefabCreationBoundary> boundaries;
        const auto result = PreparePrefabSelectionSource(snapshot, {20}, fixture.asset, ScenePrefabVersion(), fixture.registry.Snapshot(),
                                                         nullptr, boundaries);
        CHECK(result.HasValue() == (test == Case::AtLimit));
        CHECK(fixture.files.writes == 0);
    }

    TEST_CASE("Prefab source capture follows canonical rejection of malformed authoring without losing the accepted root",
              "[prefab][creation][native]") {
        Creation fixture;
        const auto revision = fixture.document.Revision();
        const auto state = fixture.document.State();
        const auto epoch = fixture.document.ReplacementEpoch();
        std::vector<SceneObjectSnapshot> malformed{fixture.document.Objects().begin(), fixture.document.Objects().end()};
        malformed.back().components.camera->nearPlane = -1;
        const auto rejected = fixture.document.LoadSaved(malformed);
        REQUIRE(rejected.HasError());
        CHECK(fixture.document.Revision() == revision);
        CHECK(fixture.document.State() == state);
        CHECK(fixture.document.ReplacementEpoch() == epoch);
        CHECK_FALSE(fixture.document.IsDirty());
        CHECK_FALSE(fixture.history.CanUndo());
        CHECK(fixture.files.writes == 0);
        CHECK(fixture.document.Objects().back().components.camera->nearPlane == 0.1F);
        const auto created = fixture.Run();
        CHECK(created.committed);
        CHECK(fixture.document.PrefabInstances().size() == 1);
    }

    TEST_CASE("Prefab transaction preserves foreign Scene source edits before and during pair publication", "[prefab][creation][native]") {
        const bool duringPublication = GENERATE(false, true);
        Creation fixture;
        const auto scenePath = fixture.root / "assets/Scene.horo";
        const auto original = SceneSource::EncodeSceneSource({fixture.document.Objects(), fixture.document.PrefabInstances()});
        std::ofstream(scenePath, std::ios::binary) << original;
        const PrefabCreationSourceFence fence{scenePath,
                                              {true, original.size(),
                                               ComputeSha256(std::as_bytes(std::span{original.data(), original.size()}))}};
        const auto edit = [&] {
            std::ofstream(scenePath, std::ios::binary | std::ios::trunc) << "foreign Scene edit";
        };
        if (duringPublication)
            fixture.files.beforeSecond = edit;
        else
            edit();
        const auto baseline = fixture.document.State();
        const auto result = CreatePrefabFromSelection(fixture.scene,
                                                      {fixture.Context(),
                                                       {20},
                                                       fixture.root,
                                                       fixture.destination,
                                                       fixture.asset,
                                                       ScenePrefabVersion(),
                                                       fixture.registry.Snapshot(),
                                                       nullptr,
                                                       fence},
                                                      fixture.mutations, fixture.files);
        CHECK_FALSE(result.committed);
        REQUIRE(result.error);
        CHECK(fixture.Read(scenePath) == "foreign Scene edit");
        CHECK(fixture.document.State() == baseline);
        CHECK_FALSE(fixture.document.IsDirty());
        CHECK_FALSE(fixture.history.CanUndo());
        CHECK(fixture.document.PrefabInstances().empty());
        CHECK_FALSE(std::filesystem::exists(fixture.destination));
        CHECK(fixture.files.writes == (duringPublication ? 2 : 0));
        if (duringPublication) {
            CHECK(result.files[0].removed);
            CHECK(result.files[1].removed);
        }
    }

    TEST_CASE("Prefab creation follows the shared asset root spelling for legacy projects", "[prefab][creation][native]") {
        Creation fixture;
        std::filesystem::rename(fixture.root / "assets", fixture.root / "Assets");
        fixture.destination = fixture.root / "Assets/Legacy.prefab";
        const auto result = fixture.Run();
        REQUIRE(result.committed);
        CHECK(std::filesystem::exists(fixture.destination));
        CHECK(fixture.document.PrefabInstances().size() == 1);
    }

    TEST_CASE("Prefab boundary audit reports every explicit source target and blocks opaque uncertainty before source creation",
              "[prefab][creation]") {
        Creation fixture;
        auto objects = fixture.document.Snapshot().objects;
        Runtime::PhysicsConstraintComponent constraint{.id = {1},
                                                       .constraint = {1},
                                                       .first = {.body = {{10}, {1}}},
                                                       .second = Runtime::PhysicsConstraintBodyEndpoint{.body = {{30}, {1}}},
                                                       .enabled = false};
        objects[1].components.physicsConstraints.push_back(constraint);
        objects[0].components.characterController = Runtime::CharacterControllerComponent{.id = {1}};
        objects[0].components.characterController->collision.excludedBody = Runtime::PhysicsBodyReference{{20}, {1}};
        objects[0].components.characterController->enabled = false;
        objects[2].components.gameplayComponents.push_back({.typeId = Gameplay::ComponentTypeId::Parse("game.test.opaque").Value(),
                                                            .schemaVersion = 1,
                                                            .encoding = Gameplay::ComponentPayloadEncoding::CanonicalJson,
                                                            .payload = {static_cast<std::byte>('{'), static_cast<std::byte>('}')}});
        REQUIRE(fixture.document.LoadSaved(std::move(objects)).HasValue());
        auto bound = SceneDocumentTransactions::Bind(fixture.document, fixture.history, {PrefabAsset(8).Asset(), 2});
        REQUIRE(bound.HasValue());
        fixture.scene = std::move(bound).Value();
        const auto captured = fixture.scene.Snapshot();
        REQUIRE(captured.HasValue());
        const auto &snapshot = captured.Value();
        std::vector<PrefabCreationBoundary> boundaries;
        const auto prepared = PreparePrefabSelectionSource(snapshot, {20}, fixture.asset, ScenePrefabVersion(), fixture.registry.Snapshot(),
                                                           nullptr, boundaries);
        CHECK(prepared.HasError());
        REQUIRE(boundaries.size() == 3);
        CHECK(std::ranges::any_of(boundaries, [](const auto &site) {
            return site.source == SceneObjectId{20} && site.target == SceneObjectId{10};
        }));
        CHECK(std::ranges::any_of(boundaries, [](const auto &site) {
            return site.source == SceneObjectId{10} && site.target == SceneObjectId{20} &&
                   site.property == "characterController.collision.excludedBody";
        }));
        CHECK(fixture.files.writes == 0);
    }

}  // namespace Horo::Editor
