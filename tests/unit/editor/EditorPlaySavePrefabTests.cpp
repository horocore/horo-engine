#include "../runtime/prefab/PrefabTemplateProviderTestSupport.h"
#include "EditorPlaySaveHost.h"
#include "EditorPlaySavePrefabServices.h"
#include "GameplayBehaviorRestoreFixture.h"
#include "GameplayRetirementTestSupport.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Platform/SecureRandom.h"
#include "Horo/Prefab/PrefabCookedOverrides.h"
#include "SaveFilesystemTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Prefab;
    using namespace Horo::Application::Internal;
    using namespace Horo::Runtime::SaveFilesystemTest;

    template <typename T> T Take(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Actual operation accepted"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    void Accept(Result<void> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "Actual operation accepted"));
        REQUIRE(result.HasValue());
    }

    struct Projector final : PrefabComponentProjector {
        Result<bool> Project(const RawComponentPayload &, RuntimeComponentSet &, std::optional<PrimitiveMeshDescriptor> &) const override {
            return Result<bool>::Success(false);
        }
    };

    /** @brief Same thin Scene structural dispatch used by Play; actual Gameplay runtime owns lifecycle/candidates. */
    class GameplayStructural final : public SceneStructuralParticipant {
    public:
        explicit GameplayStructural(std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner) : owner_(std::move(owner)) {}

        SceneStructuralOwner Owner() const noexcept override {
            return SceneStructuralOwner::Gameplay;
        }

        Result<std::unique_ptr<SceneStructuralCandidate>> Prepare(const RuntimeSceneView &scene, std::span<const RuntimeEntityView> created,
                                                                  std::span<const EntityRef> destroyed) override {
            auto *runtime = owner_->Active();
            if (!runtime)
                return Result<std::unique_ptr<SceneStructuralCandidate>>::Failure(
                    MakeError(Gameplay::GameplayErrors::InvalidBehaviorComponent));
            return runtime->MakeStructuralParticipant()->Prepare(scene, created, destroyed);
        }

    private:
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner_;
    };

    std::vector<Editor::PlaySaveGameplayBinding> NativeBindings(const Gameplay::LoadedGameModule &module) {
        std::vector<Editor::PlaySaveGameplayBinding> bindings;
        for (const auto &declaration : Take(module.PersistenceDeclarations())) {
            auto installed = Take(module.AcquireInstalledPersistence(declaration.descriptor.participant.participant));
            auto adapter = std::dynamic_pointer_cast<const GameplayPersistenceAdapter>(installed.AcquireAdapter());
            REQUIRE(adapter);
            bindings.push_back({declaration, std::move(installed), std::move(adapter)});
        }
        return bindings;
    }

    RuntimeSceneDefinition Baseline() {
        SceneDefinitionBuilder builder{{41}, {3}};
        RuntimeEntityDefinition camera{.object = {1}};
        camera.components.camera = CameraComponent{};
        builder.Add(std::move(camera));
        return Take(std::move(builder).Build());
    }

    PlaySaveStorageConfig Storage(const std::filesystem::path &path) {
        const NullProcessService process;
        const auto product = Runtime::Test::Id<ProductStorageId>(11);
        return {Take(ResolveProductSaveRoot({product, SaveRootPlatform::Test, path}, process)),
                {product, Runtime::Test::Id<EnvironmentStorageId>(12),
                 UserProfileOwner{Runtime::Test::Id<LocalUserStorageId>(13), Runtime::Test::Id<GameProfileId>(14)}},
                Runtime::Test::Id<SaveProjectId>(15),
                Runtime::Test::Id<SaveWorldId>(1),
                Runtime::Test::Id<SaveBaseSceneId>(16),
                Platform::CreateNativeSecureRandomSource(),
                "prefab-runtime-test",
                "prefab-runtime-test"};
    }

    /** @brief Actual scheduler/cooker/provider/Scene/SaveSession/filesystem lifecycle, without a fixture-only restore producer. */
    struct Fixture final {
        std::unique_ptr<Tests::NativeGameplayFailureControl> probe;
        Gameplay::GameModuleHost loader;
        std::unique_ptr<Gameplay::LoadedGameModule> module;
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner;
        bool native{};
        TemporaryDirectory directory;
        // Functional lifecycle checks use controlled capture time; Until still bounds real worker settlement.
        DeterministicClock clock;
        SystemWallClock wall;
        JobSystem jobs{{2, 32}};
        OperationStore operations{8, 16};
        Prefab::TestSupport::GatedProvider bytes;
        Assets::AssetLoadService loads{jobs, bytes};
        Assets::AssetRegistry registry;
        RuntimeSceneService scenes{registry, loads};
        PrefabLimitProfile profile{Prefab::TestSupport::Profile()};
        PrefabTemplateProvider provider{registry, loads, scenes, profile};
        Projector projector;
        std::shared_ptr<PlaySavePrefabServices> services{std::make_shared<PlaySavePrefabServices>(provider, scenes, projector)};
        Gameplay::BehaviorRegistry behaviors;
        Gameplay::ComponentRegistry components;
        CancellationSource cancellation;
        CancellationToken token{cancellation.Token()};
        std::shared_ptr<PlaySaveStorageSchema> schema;
        std::unique_ptr<EditorPlaySaveHost> host;
        std::shared_ptr<Gameplay::GameplayPrefabContext> context;
        std::vector<std::uint8_t> cooked;
        std::uint64_t frame{};

        explicit Fixture(bool nativeGameplay = false) : native(nativeGameplay) {
            if (native) {
                probe = std::make_unique<Tests::NativeGameplayFailureControl>(0);
                module = Take(loader.Load(HORO_TEST_GAME_MODULE_PATH, Tests::NativeRetirementExpectation())).owner;
            }
            const auto asset = Prefab::Test::Asset();
            const auto type = Assets::AssetTypeId::Parse("core.prefab").Value();
            Assets::AssetRecord record{asset, type, ProjectPath::Parse("assets/runtime.prefab").Value(),
                                       ProjectPath::Parse("assets/runtime.prefab.horo").Value()};
            REQUIRE(registry.Publish({record}).status == Assets::AssetRegistryBuildStatus::Complete);
            cooked = Cook();
            bytes.Memory().Insert(asset, cooked);
            Accept(behaviors.Freeze());
            Accept(components.Freeze());
            Accept(scenes.AddActivationParticipant(services->MakeParticipant()));
            if (native) {
                behaviorOwner = Take(Gameplay::BehaviorRuntimeSceneOwner::Create(scenes, module->Registry()));
                Accept(scenes.AddActivationParticipant(behaviorOwner->MakeActivationParticipant()));
                Accept(scenes.AddStructuralParticipant(std::make_unique<GameplayStructural>(behaviorOwner)));
            }
            Accept(scenes.Startup(token));
            Accept(provider.Startup(token));
            Accept(scenes.QueuePreparation(Baseline()));
            Accept(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, Frame()));
            context = Take(services->Bind(native ? module->Registry() : behaviors, native ? module->Components() : components, 1));
            if (behaviorOwner)
                Accept(behaviorOwner->ActivatePublished({}, context));
            auto config = Storage(directory.Path());
            std::vector<PersistentEntityRecord> authored{{Runtime::Test::Id<PersistentEntityId>(1),
                                                          {1},
                                                          AuthoredPersistentEntityProvenance{{41}, {1}},
                                                          PersistentEntityDisposition::Live}};
            auto bindings = native ? NativeBindings(*module) : std::vector<Editor::PlaySaveGameplayBinding>{};
            std::vector<Gameplay::GameplayPersistenceDeclaration> declarations;
            for (const auto &binding : bindings)
                declarations.push_back(binding.declaration);
            schema = std::make_shared<PlaySaveStorageSchema>(
                PlaySaveStorageSchema{Baseline(), authored, declarations,
                                      native ? std::optional{Gameplay::BehaviorLifecycleParticipant()} : std::nullopt, profile});
            auto live = std::make_shared<const std::vector<Editor::PlaySaveGameplayBinding>>(std::move(bindings));
            host = Take(EditorPlaySaveHost::Create(scenes, {jobs, operations, clock, wall, std::move(config)}, schema, std::move(live),
                                                   behaviorOwner, services));
            Ready();
        }

        ~Fixture() {
            bytes.Release();
            if (host) {
                host->BeginRetirement();
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!host->IsSettled() && std::chrono::steady_clock::now() < deadline) {
                    Pump();
                    std::this_thread::yield();
                }
                REQUIRE(host->IsSettled());
                host.reset();
            }
            scenes.Shutdown();
            provider.Shutdown();
            loads.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Cancel);
        }

        std::vector<std::uint8_t> Cook() {
            PrefabObjectNode root{.localId = {0}, .name = "runtime-root"};
            if (native)
                root.behaviors.emplace_back(Gameplay::BehaviorInstanceId{1},
                                            Take(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover")));
            PrefabObjectNode child{.localId = {7}, .parentLocalId = LocalObjectId{0}, .name = "runtime-child"};
            child.localTransform.translation.y = 4;
            auto document = Take(PrefabDocument::Create({.projectVersion = Application::ParseHoroVersion("1.2.3").Value(),
                                                         .assetId = Prefab::Test::Asset(),
                                                         .objects = {root, child}},
                                                        profile));
            const auto source = Take(document.SerializeCanonical());
            const PrefabSourceRevision revision{document.Data().projectVersion,
                                                ComputeSha256(std::as_bytes(std::span{source.data(), source.size()}))};
            auto resolver = Take(BuildPrefabSourceResolverSnapshot(registry.Snapshot(), {{document, revision}}, profile));
            auto value =
                Take(CookPrefabTemplate(resolver, registry.Snapshot(), Prefab::Test::Asset(), {}, Prefab::TestSupport::Target(), profile));
            REQUIRE(value.Data().rootSourceRevision == revision);
            std::vector<std::uint8_t> payload;
            for (auto byte : value.Bytes())
                payload.push_back(std::to_integer<std::uint8_t>(byte));
            return Prefab::TestSupport::Envelope(Prefab::Test::Asset(), "core.prefab", std::move(payload));
        }

        FrameContext Frame() {
            return {.frameNumber = ++frame, .cancellation = token};
        }

        void Pump() {
            const auto frame = Frame();
            Accept(services->Advance(1));
            Accept(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, frame));
            Accept(services->Advance(1));
            Accept(host->CommitAtSafePoint(frame));
        }

        template <typename Predicate> void Until(Predicate predicate) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!predicate() && std::chrono::steady_clock::now() < deadline) {
                Pump();
                std::this_thread::yield();
            }
            REQUIRE(predicate());
        }

        void Ready(std::string_view phase = "initial host admission") {
            INFO("Host phase: " << phase << "; current Scene runtime: " << scenes.ActiveScene()->RuntimeId().value);
            Until([&] {
                const auto snapshot = host->Snapshot();
                if (snapshot && snapshot->error) {
                    INFO(snapshot->error->code.Value() << ": " << snapshot->error->message);
                    FAIL("Actual Save host failed");
                }
                return snapshot && !snapshot->operationPending && snapshot->availability == Editor::PlaySaveAvailability::Ready;
            });
        }

        void StepGameplay() {
            if (!behaviorOwner)
                return;
            Accept(host->BeginMutation());
            Accept(behaviorOwner->Active()->FixedUpdate({}, {0.01}));
            Accept(host->EndMutation());
        }

        PrefabInstance Spawn(std::optional<PrefabValueOverrideSet> overrides = {}) {
            PrefabSpawnRequest request;
            request.source = {Prefab::Test::Asset(), ComputeSha256(std::as_bytes(std::span{cooked})), Prefab::TestSupport::Target()};
            request.placement.translation.x = 7;
            request.parent = scenes.ActiveScene()->Find({1});
            request.overrides = std::move(overrides);
            auto operation = Take(context->Spawn(request));
            Until([&] {
                return operation.State() != PrefabSpawnState::Pending;
            });
            auto instance = Take(operation.Spawned());
            StepGameplay();
            return instance;
        }

        void Despawn(const PrefabInstance &instance) {
            auto operation = Take(context->Despawn(instance, 1));
            Until([&] {
                return operation.State() != PrefabSpawnState::Pending;
            });
            REQUIRE(operation.State() == PrefabSpawnState::Committed);
        }

        SaveManagerPage Save() {
            INFO("Actual SaveNew request");
            Accept(host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}));
            Ready("SaveNew capture and asynchronous publication");
            return Take(host->Snapshot()->manager->Page({.maximumRows = 16}));
        }
    };
}  // namespace

TEST_CASE("Real Play saves mutated cooked hierarchy then Load restores stable identities and fresh Despawn capability",
          "[editor][play][save][prefab][end-to-end]") {
    Fixture f;
    const auto originalBytes = Take(SceneCook::EncodeCookedSceneDefinition(f.schema->baseline));
    const auto oldContext = f.context;
    const auto original = f.Spawn();
    auto captured = Take(f.services->Inventory()->Capture());
    REQUIRE(captured.prefabs.instances.size() == 1);
    REQUIRE(original.PersistentId());
    const auto identity = *original.PersistentId();
    CHECK(identity == captured.prefabs.instances[0].identity);
    const auto child = Take(captured.identities.Resolve(captured.prefabs.instances[0].members[1].record.identity, {1}));
    Accept(f.host->BeginMutation());
    SceneCommandBuffer edits;
    Math::Transform changed;
    changed.translation = {3, 9, 11};
    edits.SetLocalTransform(child, changed);
    Accept(f.scenes.QueueStructuralCommands(std::move(edits)));
    Accept(f.scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, f.Frame()));
    Accept(f.host->EndMutation());
    const auto page = f.Save();
    REQUIRE(page.rows.size() == 1);
    const auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, page.rows[0].slot));
    const auto savedState = Take(f.services->Inventory()->Capture());
    const auto savedRuntime = f.scenes.ActiveScene()->RuntimeId();
    f.Despawn(original);
    CHECK(f.scenes.ActiveScene()->Get(original.Root()).HasError());
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    REQUIRE(f.scenes.ActiveScene()->RuntimeId() != savedRuntime);
    f.context = Take(f.services->Bind(f.behaviors, f.components, 1));
    CHECK(oldContext->FindPersistent(identity).HasError());
    CHECK(f.context->Despawn(original, 1).HasError());
    const auto restored = Take(f.context->FindPersistent(identity));
    const auto current = Take(f.services->Inventory()->Capture());
    REQUIRE(current.prefabs.instances.size() == 1);
    CHECK(current.prefabs.instances[0].identity == identity);
    CHECK(current.prefabs.instances[0].members[1].record.identity == savedState.prefabs.instances[0].members[1].record.identity);
    const auto restoredChild = Take(current.identities.Resolve(current.prefabs.instances[0].members[1].record.identity, {1}));
    CHECK(*Take(f.scenes.ActiveScene()->Get(restoredChild)).localTransform == changed);
    CHECK(f.Save().rows.size() == 2);
    f.Despawn(restored);
    CHECK(f.context->FindPersistent(identity).HasError());
    const auto tombstones = Take(f.services->Inventory()->Capture());
    CHECK(tombstones.prefabs.instances[0].disposition == PersistentEntityDisposition::Tombstone);
    CHECK(f.Save().rows.size() == 3);
    CHECK(Take(SceneCook::EncodeCookedSceneDefinition(f.schema->baseline)) == originalBytes);
}

namespace {
    /** @brief Reads/writes actual native fixture state through the existing scoped Gameplay visitor, never component memory. */
    class NativeFields final : public Gameplay::IBehaviorPersistenceVisitor {
    public:
        bool write{};
        std::uint64_t value{42};
        std::vector<Tests::GameplayBehaviorDurableFields> fields;

        Result<void> Visit(std::span<const Gameplay::BehaviorRestoreInstance> instances) override {
            for (const auto &instance : instances) {
                auto *actual = dynamic_cast<Tests::IGameplayBehaviorDurableFixture *>(instance.implementation);
                REQUIRE(actual);
                if (write)
                    actual->SetDurableFields({value, {}, {}});
                fields.push_back(actual->DurableFields());
            }
            return Result<void>::Success();
        }
    };

    std::vector<Tests::GameplayBehaviorDurableFields> VisitNative(Fixture &fixture, bool write, std::uint64_t value) {
        NativeFields visitor;
        visitor.write = write;
        visitor.value = value;
        const auto identities = Take(fixture.services->Inventory()->Identities());
        auto target =
            Take(fixture.behaviorOwner->BindingForActive(Take(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover")), identities));
        if (write)
            Accept(fixture.host->BeginMutation());
        Accept(target.Visit(visitor));
        if (write)
            Accept(fixture.host->EndMutation());
        return visitor.fields;
    }
}  // namespace

TEST_CASE("Real native dynamic prefab Save Load preserves owned state and lifecycle without replaying ordinary creation",
          "[editor][play][save][prefab][native][end-to-end]") {
    Fixture f{true};
    const auto instance = f.Spawn();
    const auto initial = Take(f.services->Inventory()->Capture());
    REQUIRE(initial.prefabs.instances.size() == 1);
    const auto identity = initial.prefabs.instances[0].identity;
    REQUIRE(VisitNative(f, true, 73).size() == 1);
    const auto page = f.Save();
    const auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, page.rows[0].slot));
    f.Despawn(instance);
    const auto creates = f.probe->control.behaviorCreated;
    const auto enabled = f.probe->control.behaviorEnabled;
    const auto started = f.probe->control.behaviorStarted;
    const auto prehooks = f.probe->control.behaviorPrehooks;
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    f.context = Take(f.services->Bind(f.module->Registry(), f.module->Components(), 1));
    f.StepGameplay();
    REQUIRE(started > 0);
    CHECK(f.probe->control.behaviorCreated == creates);
    CHECK(f.probe->control.behaviorEnabled == enabled);
    CHECK(f.probe->control.behaviorStarted == started);
    CHECK(f.probe->control.behaviorPrehooks > prehooks);
    const auto fields = VisitNative(f, false, 0);
    REQUIRE(fields.size() == 1);
    CHECK(fields[0].value == 73);
    const auto restored = Take(f.context->FindPersistent(identity));
    const auto restoredBehavior = Take(f.scenes.ActiveScene()->Get(restored.Root())).components->behaviors.front().instanceId.value;
    const auto future = f.Spawn();
    const auto futureBehavior = Take(f.scenes.ActiveScene()->Get(future.Root())).components->behaviors.front().instanceId.value;
    CHECK(futureBehavior > restoredBehavior);
    f.Despawn(future);
    CHECK(f.Save().rows.size() == 2);
    f.Despawn(restored);
    CHECK(f.context->FindPersistent(identity).HasError());
    CHECK(f.Save().rows.size() == 3);
}

TEST_CASE("Real native dynamic prefab restore faults preserve the last live Scene and permit an actual retry",
          "[editor][play][save][prefab][native][rollback]") {
    Fixture f{true};
    const auto instance = f.Spawn();
    REQUIRE(VisitNative(f, true, 57).size() == 1);
    const auto page = f.Save();
    const auto slot = page.rows[0].slot;
    REQUIRE(VisitNative(f, true, 91).size() == 1);
    const auto currentScene = f.scenes.ActiveScene()->RuntimeId();
    const auto identity = Take(f.services->Inventory()->Capture()).prefabs.instances[0].identity;
    SECTION("typed prehook failure") {
        f.probe->control.restoreFailure = 6;
    }
    SECTION("standard prehook exception") {
        f.probe->control.restoreFailure = 19;
    }
    SECTION("native allocation exception") {
        f.probe->control.restoreFailure = 21;
    }
    SECTION("nonstandard prehook exception") {
        f.probe->control.restoreFailure = 22;
    }
    auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, slot));
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Until([&] {
        return !f.host->Snapshot()->operationPending && f.host->Snapshot()->error.has_value();
    });
    CHECK(f.scenes.ActiveScene()->RuntimeId() == currentScene);
    CHECK(Take(f.context->FindPersistent(identity)).Root() == instance.Root());
    const auto unchanged = VisitNative(f, false, 0);
    REQUIRE(unchanged.size() == 1);
    CHECK(unchanged[0].value == 91);
    f.probe->control.restoreFailure = 0;
    selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, slot));
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    const auto restored = VisitNative(f, false, 0);
    REQUIRE(restored.size() == 1);
    CHECK(restored[0].value == 57);
}

TEST_CASE("Real dynamic prefab Load cancellation and Stop retain original packet and actual gated worker until settlement",
          "[editor][play][save][prefab][cancel][retirement]") {
    Fixture f;
    const auto instance = f.Spawn();
    const auto page = f.Save();
    const auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, page.rows[0].slot));
    const auto currentScene = f.scenes.ActiveScene()->RuntimeId();
    f.provider.Evict(Prefab::Test::Asset());
    f.bytes.Block();
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Until([&] {
        return f.bytes.Entered();
    });
    REQUIRE(f.host->Snapshot()->operationPending);
    bool retirement{};
    SECTION("caller cancellation") {
        Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Cancel}));
    }
    SECTION("Stop retirement") {
        f.host->BeginRetirement();
        retirement = true;
    }
    for (unsigned phase = 0; phase < 4; ++phase)
        f.Pump();
    CHECK_FALSE(f.host->IsSettled());
    CHECK(f.scenes.ActiveScene()->RuntimeId() == currentScene);
    CHECK(f.scenes.ActiveScene()->Get(instance.Root()).HasValue());
    f.bytes.Release();
    if (retirement)
        f.Until([&] {
            return f.host->IsSettled();
        });
    else
        f.Until([&] {
            return !f.host->Snapshot()->operationPending;
        });
    CHECK(f.scenes.ActiveScene()->RuntimeId() == currentScene);
    CHECK(f.scenes.ActiveScene()->Get(instance.Root()).HasValue());
}

TEST_CASE("Real prefab Load rejects missing or changed installed content and preserves last good durable instance",
          "[editor][play][save][prefab][compatibility][rollback]") {
    Fixture f;
    const auto instance = f.Spawn();
    const auto captured = Take(f.services->Inventory()->Capture());
    REQUIRE(instance.PersistentId());
    const auto identity = *instance.PersistentId();
    CHECK(identity == captured.prefabs.instances[0].identity);
    const auto page = f.Save();
    const auto slot = page.rows[0].slot;
    const auto before = f.scenes.ActiveScene()->RuntimeId();
    f.provider.Evict(Prefab::Test::Asset());
    SECTION("installed artifact removed") {
        f.bytes.Memory().Remove(Prefab::Test::Asset());
    }
    SECTION("installed artifact digest changed") {
        auto corrupt = f.cooked;
        corrupt.back() ^= 1;
        f.bytes.Memory().Insert(Prefab::Test::Asset(), std::move(corrupt));
    }
    auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, slot));
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Until([&] {
        return !f.host->Snapshot()->operationPending && f.host->Snapshot()->error.has_value();
    });
    CHECK(f.scenes.ActiveScene()->RuntimeId() == before);
    CHECK(Take(f.context->FindPersistent(identity)).Root() == instance.Root());
    REQUIRE(f.host->Snapshot()->error);
    CHECK_FALSE(f.host->Snapshot()->error->code.Value().empty());
    f.bytes.Memory().Insert(Prefab::Test::Asset(), f.cooked);
    selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, slot));
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    CHECK(f.scenes.ActiveScene()->RuntimeId() != before);
}

TEST_CASE("Real prefab tombstone Load preserves deletion even after the old cooked artifact disappears",
          "[editor][play][save][prefab][tombstone]") {
    Fixture f;
    const auto instance = f.Spawn();
    const auto identity = Take(f.services->Inventory()->Capture()).prefabs.instances[0].identity;
    f.Despawn(instance);
    const auto page = f.Save();
    const auto slot = page.rows[0].slot;
    f.provider.Evict(Prefab::Test::Asset());
    f.bytes.Memory().Remove(Prefab::Test::Asset());
    const auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, slot));
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    f.context = Take(f.services->Bind(f.behaviors, f.components, 1));
    CHECK(f.context->FindPersistent(identity).HasError());
    const auto captured = Take(f.services->Inventory()->Capture());
    REQUIRE(captured.prefabs.instances.size() == 1);
    CHECK(captured.prefabs.instances[0].identity == identity);
    CHECK(captured.prefabs.instances[0].disposition == PersistentEntityDisposition::Tombstone);
    CHECK(f.Save().rows.size() == 2);
}

TEST_CASE("Real multi-instance Load restores independent Despawn authority and resaves only the surviving occurrence",
          "[editor][play][save][prefab][partitions][end-to-end]") {
    Fixture f;
    const auto first = f.Spawn();
    const auto second = f.Spawn();
    const auto captured = Take(f.services->Inventory()->Capture());
    REQUIRE(captured.prefabs.instances.size() == 2);
    const auto firstIdentity = captured.prefabs.instances[0].identity;
    const auto secondIdentity = captured.prefabs.instances[1].identity;
    const auto page = f.Save();
    const auto originalSlot = page.rows[0].slot;
    auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, originalSlot));
    f.Despawn(first);
    f.Despawn(second);
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    f.context = Take(f.services->Bind(f.behaviors, f.components, 1));
    const auto currentFirst = Take(f.context->FindPersistent(firstIdentity));
    const auto currentSecond = Take(f.context->FindPersistent(secondIdentity));
    f.Despawn(currentFirst);
    CHECK(f.scenes.ActiveScene()->Get(currentSecond.Root()).HasValue());
    CHECK(f.context->FindPersistent(firstIdentity).HasError());
    const auto surviving = f.Save();
    REQUIRE(surviving.rows.size() == 2);
    const auto savedSurvivor = std::ranges::find_if(surviving.rows, [&](const auto &row) {
        return row.slot != originalSlot;
    });
    REQUIRE(savedSurvivor != surviving.rows.end());
    selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, savedSurvivor->slot));
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    f.context = Take(f.services->Bind(f.behaviors, f.components, 1));
    CHECK(f.context->FindPersistent(firstIdentity).HasError());
    const auto remaining = Take(f.context->FindPersistent(secondIdentity));
    f.Despawn(remaining);
    CHECK(f.context->FindPersistent(secondIdentity).HasError());
    CHECK(f.Save().rows.size() == 3);
}

TEST_CASE("Real persistent prefab lookup authorizes stable admitted creator and never accepts a request-supplied owner",
          "[editor][play][save][prefab][creator]") {
    Fixture f;
    const auto instance = f.Spawn();
    const auto identity = Take(f.services->Inventory()->Capture()).prefabs.instances[0].identity;
    PrefabSpawnService other{f.provider, f.scenes, &f.components, &f.projector, f.services->Inventory()};
    Accept(other.Advance(1));
    auto foreign =
        Take(other.Acquire({"editor.other", f.scenes.ActiveScene()->RuntimeId(), true, true, std::vector<Gameplay::BehaviorTypeId>{}}));
    CHECK(foreign->FindPersistent(identity).HasError());
    CHECK(Take(f.context->FindPersistent(identity)).Root() == instance.Root());
    foreign->Revoke();
    CHECK(foreign->FindPersistent(identity).HasError());
}

TEST_CASE("Real cooked typed root and child intent restores with one root placement owner and the same canonical delta core",
          "[editor][play][save][prefab][typed-overrides]") {
    Fixture f;
    const PrefabTemplateLoadRequest source{Prefab::Test::Asset(), ComputeSha256(std::as_bytes(std::span{f.cooked})),
                                           Prefab::TestSupport::Target()};
    auto loading = Take(f.provider.LoadAsync(source));
    f.Until([&] {
        return loading.State() != PrefabTemplateLoadState::Loading;
    });
    const auto lease = Take(f.provider.TakeResult(loading));
    const auto objects = Take(PrepareCookedPrefabOverrideBaseline(*lease.Template(), f.profile));
    const PrefabOverrideBaseline baseline{source.asset, *lease.Template()->Data().rootSourceRevision, objects};
    Math::Transform root, child;
    root.translation = {42, 8, 3};
    child.translation = {11, 19, 23};
    const auto rootIntent =
        Take(PreparePrefabValueEdit(baseline, {}, {PrefabValueEditKind::Assign, objects[0].key.object, root}, nullptr, f.profile));
    const auto both = Take(PreparePrefabValueEdit(baseline, rootIntent.rebased, {PrefabValueEditKind::Assign, objects[1].key.object, child},
                                                  nullptr, f.profile));
    const auto instance = f.Spawn(both.rebased);
    REQUIRE(instance.PersistentId());
    const auto identity = *instance.PersistentId();
    const auto captured = Take(f.services->Inventory()->Capture());
    REQUIRE(captured.prefabs.instances.size() == 1);
    const auto &recipe = captured.prefabs.instances[0];
    CHECK(recipe.placement == root);
    CHECK(std::ranges::none_of(recipe.overrides.records, [&](const auto &record) {
        const auto *address = std::get_if<PrefabObjectAddress>(&record.target);
        return address && *address == objects[0].key.object;
    }));
    const auto page = f.Save();
    const auto selection = Take(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, page.rows[0].slot));
    f.Despawn(instance);
    Accept(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
    f.Ready("Load private reconstruction and owner restore");
    f.context = Take(f.services->Bind(f.behaviors, f.components, 1));
    const auto restored = Take(f.context->FindPersistent(identity));
    CHECK(*Take(f.scenes.ActiveScene()->Get(restored.Root())).localTransform == root);
    const auto current = Take(f.services->Inventory()->Capture());
    const auto childRef = Take(current.identities.Resolve(recipe.members[1].record.identity, {1}));
    CHECK(*Take(f.scenes.ActiveScene()->Get(childRef)).localTransform == child);
}

namespace {
    Gameplay::BehaviorPersistenceTarget ActiveNativeTarget(Fixture &fixture, const PersistentEntityIdentityMap &identities) {
        return Take(fixture.behaviorOwner->BindingForActive(Take(Gameplay::BehaviorTypeId::Parse("game.tests.dynamic_mover")), identities));
    }

    /** @brief Uses another fully validated identity map to prove equal-count durable remaps are distinct binding authorities. */
    PersistentEntityIdentityMap RemapNativeIdentity(const PersistentEntityIdentityMap &original, EntityRef root, bool generationOnly) {
        std::vector<PersistentEntityRecord> records{original.Records().begin(), original.Records().end()};
        std::vector<PersistentEntityRuntimeBinding> bindings;
        for (auto &record : records) {
            if (record.disposition != PersistentEntityDisposition::Live)
                continue;
            const auto runtime = Take(original.Resolve(record.identity, record.generation));
            if (runtime == root) {
                if (generationOnly)
                    ++record.generation.value;
                else
                    record.identity = Runtime::Test::Id<PersistentEntityId>(99);
            }
            bindings.push_back({record.identity, record.generation, runtime});
        }
        return Take(PersistentEntityIdentityMap::Create(root.runtime, records, bindings));
    }
}  // namespace

TEST_CASE("Actual native source binding reuses exact snapshots and refreshes after dynamic attachment changes",
          "[editor][play][save][prefab][native][binding-inventory]") {
    Fixture fixture{true};
    const auto emptyMap = Take(fixture.services->Inventory()->Identities());
    auto empty = ActiveNativeTarget(fixture, emptyMap);
    auto emptyAgain = ActiveNativeTarget(fixture, emptyMap);
    CHECK(empty.SameOwner(emptyAgain));
    CHECK(fixture.Save().rows.size() == 1);
    const auto instance = fixture.Spawn();
    const auto populatedMap = Take(fixture.services->Inventory()->Identities());
    auto populated = ActiveNativeTarget(fixture, populatedMap);
    auto populatedAgain = ActiveNativeTarget(fixture, populatedMap);
    CHECK_FALSE(empty.SameOwner(populated));
    CHECK(populated.SameOwner(populatedAgain));
    CHECK(fixture.Save().rows.size() == 2);
    CHECK(fixture.Save().rows.size() == 3);
    fixture.Despawn(instance);
    const auto removedMap = Take(fixture.services->Inventory()->Identities());
    auto removed = ActiveNativeTarget(fixture, removedMap);
    CHECK_FALSE(populated.SameOwner(removed));
    CHECK(empty.SameOwner(removed));
    CHECK(fixture.Save().rows.size() == 4);
}

TEST_CASE("Actual native binding identity includes durable mapping and incarnation at the same attachment count",
          "[editor][play][save][prefab][native][binding-inventory][remap]") {
    Fixture fixture{true};
    const auto instance = fixture.Spawn();
    const auto original = Take(fixture.services->Inventory()->Identities());
    auto before = ActiveNativeTarget(fixture, original);
    bool generationOnly{};
    SECTION("changed durable identity") {}
    SECTION("changed durable incarnation") {
        generationOnly = true;
    }
    const auto remapped = RemapNativeIdentity(original, instance.Root(), generationOnly);
    auto after = ActiveNativeTarget(fixture, remapped);
    CHECK_FALSE(before.SameOwner(after));
    CHECK(before.SameOwner(ActiveNativeTarget(fixture, original)));
    CHECK(fixture.Save().rows.size() == 1);
}
