#pragma once
#include "EditorPlaySaveHost.h"
#include "EditorPlaySavePhysicsServices.h"
#include "Horo/Platform/SecureRandom.h"
#include "PhysicsSceneSavePrefabSupport.h"
#include "PhysicsSceneStateTestSupport.h"
#include "SaveFilesystemTestSupport.h"

#include <chrono>
#include <cstdio>
#include <thread>

namespace Horo::Physics::StateTest {
    namespace {
        using namespace Application::Internal;

        template <class T> T SaveIdentity(std::uint8_t suffix) {
            Runtime::SaveIdentityDetail::Bytes bytes{};
            bytes.back() = suffix;
            return Take(T::FromBytes(bytes));
        }

        PlaySaveStorageConfig SaveStorage(const std::filesystem::path &path) {
            const NullProcessService process;
            const auto product = SaveIdentity<Runtime::ProductStorageId>(11);
            return {Take(Runtime::ResolveProductSaveRoot({product, Runtime::SaveRootPlatform::Test, path}, process)),
                    {product, SaveIdentity<Runtime::EnvironmentStorageId>(12),
                     Runtime::UserProfileOwner{SaveIdentity<Runtime::LocalUserStorageId>(13), SaveIdentity<Runtime::GameProfileId>(14)}},
                    SaveIdentity<Runtime::SaveProjectId>(15),
                    SaveIdentity<Runtime::SaveWorldId>(16),
                    SaveIdentity<Runtime::SaveBaseSceneId>(17),
                    Platform::CreateNativeSecureRandomSource(),
                    "actual-physics-save-test",
                    "actual-physics-save-test"};
        }

        std::vector<Runtime::PersistentEntityRecord> SceneRecords(const Runtime::RuntimeSceneDefinition &definition) {
            std::vector<Runtime::PersistentEntityRecord> records;
            for (const auto &entity : definition.Entities()) {
                Runtime::SaveIdentityDetail::Bytes bytes{};
                bytes.front() = 0x91;
                bytes.back() = static_cast<std::uint8_t>(entity.object.value);
                records.push_back({Take(Runtime::PersistentEntityId::FromBytes(bytes)),
                                   {7},
                                   Runtime::AuthoredPersistentEntityProvenance{definition.Id(), entity.object},
                                   Runtime::PersistentEntityDisposition::Live});
            }
            return records;
        }

        /** @brief Actual Scene aggregate owns Physics, Save uses production storage/1481 handoff, and every worker really settles. */
        struct SceneSaveFixture final {
            Runtime::SaveFilesystemTest::TemporaryDirectory directory;
            // Keep instrumented capture work independent of host scheduling; Ready bounds real worker settlement.
            DeterministicClock clock;
            SystemWallClock wall;
            OperationStore operations{16, 32};
            std::unique_ptr<PhysicsRuntime> runtime;
            PhysicsSceneActivationAuthority authority;
            ScenePhysicsHost native;
            AssetSceneFixture assets{false, {47}};
            Runtime::RuntimeSceneDefinition definition;
            Assets::MemoryAssetProvider prefabArtifacts;
            Assets::AssetLoadService prefabLoads{assets.jobs, prefabArtifacts};
            std::unique_ptr<Prefab::PrefabTemplateProvider> prefabProvider;
            PhysicsSavePrefabProjector prefabProjector;
            std::shared_ptr<PlaySavePrefabServices> prefabs;
            std::shared_ptr<Gameplay::GameplayPrefabContext> prefabContext;
            Gameplay::BehaviorRegistry behaviors;
            Gameplay::ComponentRegistry components;
            std::vector<std::uint8_t> prefabBytes;
            std::shared_ptr<PlaySaveStorageSchema> schema;
            std::shared_ptr<PlaySavePhysicsServices> physics;
            std::unique_ptr<EditorPlaySaveHost> save;
            std::uint64_t frame{};
            bool closed{};

            explicit SceneSaveFixture(bool withPrefabs = false, Math::Vec3 colliderTranslation = {})
                : definition(ContinuationSceneDefinition(assets.material, assets.materialType, true)) {
                try {
                    Initialize(withPrefabs, colliderTranslation);
                } catch (...) {
                    CloseNoThrow();
                    throw;
                }
            }

            void Initialize(bool withPrefabs, Math::Vec3 colliderTranslation) {
                runtime = Take(PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical));
                Check(runtime->BindStateSnapshotBuild(ActualStaticBinding(), StateRuntimeBudget(32, 1024U * 1024U * 1024U)));
                RegisterNative();
                if (withPrefabs)
                    InstallPrefabs(colliderTranslation);
                Check(assets.service.Startup(assets.cancellation.Token()));
                if (prefabProvider)
                    Check(prefabProvider->Startup(assets.cancellation.Token()));
                (void)assets.Prepare(definition);
                for (unsigned index = 0; index < 5; ++index)
                    Check(native.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)));
                schema = std::make_shared<PlaySaveStorageSchema>(PlaySaveStorageSchema{.baseline = definition,
                                                                                       .identities = SceneRecords(definition),
                                                                                       .physics = Detail::PhysicsStateSaveDescriptor()});
                if (prefabs)
                    schema->prefabProfile = prefabProvider->LimitProfile();
                physics = Take(PlaySavePhysicsServices::Create(native, assets.service,
                                                               {.state = SceneStoragePolicy(),
                                                                .content = {64, 64, 2U * 1024U * 1024U},
                                                                .maximumWireBytes = 8U * 1024U * 1024U}));
                save =
                    Take(EditorPlaySaveHost::Create(assets.service, {assets.jobs, operations, clock, wall, SaveStorage(directory.Path())},
                                                    schema, {}, {}, prefabs, physics));
                Ready();
                if (prefabs)
                    prefabContext = Take(prefabs->Bind(behaviors, components, 1));
            }

            ~SceneSaveFixture() {
                CloseNoThrow();
            }

            void RegisterNative() {
                Check(assets.service.AddActivationParticipant(
                    Take(native.MakeParticipant(*runtime, authority, ContinuationSceneSettings()))));
                Check(assets.service.AddStructuralParticipant(Take(native.MakeStructuralParticipant())));
            }

            void InstallPrefabs(Math::Vec3 colliderTranslation) {
                const auto material = Record(assets.material, assets.materialType);
                const Assets::AssetRecord prefab{Prefab::Test::Asset(), Take(Assets::AssetTypeId::Parse("core.prefab")),
                                                 Take(ProjectPath::Parse("assets/physics-save.prefab")),
                                                 Take(ProjectPath::Parse("assets/physics-save.prefab.horo"))};
                REQUIRE(assets.registry.Publish({material, prefab}).status == Assets::AssetRegistryBuildStatus::Complete);
                const auto profile = Prefab::TestSupport::Profile();
                const auto materialPayload = CanonicalMaterialBytes(assets.material);
                const auto materialArtifact = Prefab::TestSupport::Envelope(assets.material, assets.materialType.Value(), materialPayload);
                prefabArtifacts.Insert(assets.material, materialArtifact);
                prefabBytes =
                    CookPhysicsSavePrefab(assets.registry.Snapshot(), profile, assets.material, materialArtifact, colliderTranslation);
                prefabArtifacts.Insert(Prefab::Test::Asset(), prefabBytes);
                prefabProvider = std::make_unique<Prefab::PrefabTemplateProvider>(assets.registry, prefabLoads, assets.service, profile);
                prefabs = std::make_shared<PlaySavePrefabServices>(*prefabProvider, assets.service, prefabProjector);
                Check(assets.service.AddActivationParticipant(prefabs->MakeParticipant()));
                Check(behaviors.Freeze());
                Check(components.Freeze());
            }

            Prefab::PrefabInstance SpawnBeforeCapture() {
                REQUIRE(prefabContext);
                Check(save->BeginMutation());
                Prefab::PrefabSpawnRequest request;
                request.source = {Prefab::Test::Asset(), ComputeSha256(std::as_bytes(std::span{prefabBytes})),
                                  Prefab::TestSupport::Target()};
                request.placement.translation = {7, 2, 0};
                auto operation = Take(prefabContext->Spawn(request));
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (operation.State() == Prefab::PrefabSpawnState::Pending && std::chrono::steady_clock::now() < deadline) {
                    Check(prefabs->Advance(1));
                    Check(assets.service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                                 {.frameNumber = ++frame, .cancellation = assets.cancellation.Token()}));
                    Check(prefabs->Advance(1));
                    std::this_thread::yield();
                }
                INFO((operation.Failure() ? operation.Failure()->message : std::string{"no prefab operation error"}));
                REQUIRE(operation.State() == Prefab::PrefabSpawnState::Committed);
                Check(save->EndMutation());
                return Take(operation.Spawned());
            }

            Result<void> PumpResult() {
                const Runtime::FrameContext context{.frameNumber = ++frame, .cancellation = assets.cancellation.Token()};
                if (prefabs) {
                    if (auto advanced = prefabs->Advance(1); advanced.HasError())
                        return advanced;
                }
                if (auto scene = assets.service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, context); scene.HasError())
                    return scene;
                return save->CommitAtSafePoint(context);
            }

            void Ready(std::string_view stage = "initial save readiness") {
                INFO(stage);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (std::chrono::steady_clock::now() < deadline) {
                    Check(PumpResult());
                    const auto snapshot = save->Snapshot();
                    if (snapshot && snapshot->error)
                        FAIL(snapshot->error->message);
                    if (snapshot && !snapshot->operationPending && snapshot->availability == Editor::PlaySaveAvailability::Ready)
                        return;
                    std::this_thread::yield();
                }
                FAIL("Actual Scene/Save workers did not settle within the fixture deadline");
            }

            std::vector<std::byte> Wire() {
                const auto scene = *assets.service.ActiveScene();
                const auto identities = prefabs ? Take(prefabs->Inventory()->Identities())
                                                : Take(CapturePlaySaveIdentities(scene, definition, schema->identities));
                const auto content = Take(PlaySavePhysicsContent::Capture(scene, {64, 64, 2U * 1024U * 1024U}));
                const auto target = Take(native.PublishedStateTarget());
                const auto capture = Take(target.PrepareCapture(identities, SceneStoragePolicy(), content->Fingerprint(), content));
                const auto snapshot = Take(capture->Capture());
                return {snapshot->Bytes().begin(), snapshot->Bytes().end()};
            }

            Runtime::SaveManagerCommand SaveThenSelect() {
                Check(save->Request({.kind = Editor::PlaySaveCommandKind::SaveNew, .displayName = "Canonical actual Physics"}));
                Ready();
                const auto page = Take(save->Snapshot()->manager->Page({.maximumRows = 16}));
                REQUIRE_FALSE(page.rows.empty());
                return Take(save->Snapshot()->manager->Command(Runtime::SaveManagerCommandKind::Load, page.rows.back().slot));
            }

            void AwaitRetirement() noexcept {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
                while (!save->IsSettled() && std::chrono::steady_clock::now() < deadline) {
                    auto result = PumpResult();
                    if (result.HasError())
                        std::fprintf(stderr, "Physics Save retirement: %s\n", result.ErrorValue().message.c_str());
                    std::this_thread::yield();
                }
                if (!save->IsSettled())
                    std::terminate();
            }

            void CloseNoThrow() noexcept {
                if (closed)
                    return;
                try {
                    CloseOwners();
                } catch (...) {
                    std::terminate();
                }
            }

            void CloseOwners() {
                if (save) {
                    save->BeginRetirement();
                    AwaitRetirement();
                    save.reset();
                }
                if (physics)
                    physics->CloseCapture();
                physics.reset();
                prefabContext.reset();
                assets.service.Shutdown();
                if (prefabProvider)
                    prefabProvider->Shutdown();
                prefabs.reset();
                prefabProvider.reset();
                prefabLoads.Shutdown();
                assets.loads.Shutdown();
                assets.jobs.Shutdown(ShutdownPolicy::Drain);
                if (runtime) {
                    auto retirement = Take(runtime->BeginRetirement());
                    runtime.reset();
                    for (unsigned attempt = 0; attempt < 64; ++attempt)
                        if (Take(retirement.Collect(64)).phase == PhysicsRuntimeRetirementPhase::Drained) {
                            closed = true;
                            return;
                        }
                    std::terminate();
                }
                closed = true;
            }
        };

    }  // namespace
}  // namespace Horo::Physics::StateTest
