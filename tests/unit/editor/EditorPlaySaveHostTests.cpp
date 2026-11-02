#include "EditorPlaySaveHost.h"
#include "Horo/Platform/SecureRandom.h"
#include "SaveFilesystemTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Application::Internal;
    using namespace Horo::Runtime::SaveFilesystemTest;

    /** @brief Preserves original production diagnostics when actual Scene/storage/session setup fails. */
    template <class T> T Value(Result<T> value) {
        INFO((value.HasError() ? value.ErrorValue().message : "Accepted actual operation."));
        REQUIRE(value.HasValue());
        return std::move(value).Value();
    }

    /** @brief Builds real camera state, not a synthetic archive or a fixture-only production serializer. */
    RuntimeSceneDefinition Definition() {
        SceneDefinitionBuilder builder({41}, {3});
        RuntimeEntityDefinition camera{.object = {1}};
        camera.components.camera = CameraComponent{};
        builder.Add(std::move(camera));
        return Value(std::move(builder).Build());
    }

    /** @brief Supplies actual isolated Test root capability; production host selection is tested separately. */
    PlaySaveStorageConfig Storage(const std::filesystem::path &path) {
        const NullProcessService environment;
        const auto product = Value(ProductStorageId::Parse("25302693-3fac-4b5c-b421-8be71f1f5738"));
        return {Value(ResolveProductSaveRoot({product, SaveRootPlatform::Test, path}, environment)),
                {product, Value(EnvironmentStorageId::Parse("693beca1-fb45-4a42-a9e0-c74692cce349")),
                 UserProfileOwner{Value(LocalUserStorageId::Parse("12c47eec-ce81-4525-a484-392302a7e905")),
                                  Value(GameProfileId::Parse("da3a7593-99f8-420c-9280-d285a907a19d"))}},
                Value(SaveProjectId::Parse("164d8cc0-5ef5-4b30-be21-8b435790e393")),
                Value(SaveWorldId::Parse("28b26764-6874-4479-b1a0-07184276013a")),
                Value(SaveBaseSceneId::Parse("9baa4860-f12a-478a-897f-56202aa4f88d")),
                Platform::CreateNativeSecureRandomSource(),
                "test-actual-runtime",
                "test-actual-runtime"};
    }

    /** @brief Captures the actual initial Scene using the production core adapter and canonical snapshot builder. */
    RuntimeSaveSnapshot CaptureForPolicy(RuntimeSceneService &scenes, const PlaySaveStorageSchema &schema) {
        CanonicalStateParticipantRegistry registry;
        auto adapter = std::make_shared<PlaySaveSceneCapture>(scenes, schema.baseline, schema.identities);
        REQUIRE(registry.Register(PlaySaveSceneDescriptor(), std::move(adapter)).HasValue());
        const auto participants = Value(registry.Snapshot());
        const auto scene = *scenes.ActiveScene();
        auto builder = Value(RuntimeSaveCaptureBuilder::Create({Test::Id<CapturedStateId>(51),
                                                                {1},
                                                                scene.RuntimeId().value,
                                                                scene.StructuralRevision(),
                                                                participants.Generation()},
                                                               participants));
        REQUIRE(builder.CaptureParticipants().HasValue());
        return Value(builder.Seal());
    }

    /** @brief Supplies valid fixture publication metadata while preserving the real isolated product/scope identities. */
    SaveArchiveHeader PolicyHeader(const PlaySaveStorageConfig &config, WallClock &clock) {
        const auto &owner = std::get<UserProfileOwner>(config.sandbox.owner);
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(clock.UtcNow().time_since_epoch()).count();
        REQUIRE(now > 0);
        return {.slot = Test::Id<SaveGameSlotId>(52),
                .slotGeneration = Test::Id<SlotGenerationId>(53),
                .productCompatibility = Value(ProductSaveCompatibilityVersion::Create(1)),
                .product = config.sandbox.product,
                .environment = config.sandbox.environment,
                .user = owner.user,
                .profile = owner.profile,
                .project = config.project,
                .world = config.world,
                .baseScene = config.baseScene,
                .capturedAtUnixMilliseconds = static_cast<std::uint64_t>(now),
                .engineVersion = config.engineVersion,
                .projectBuildId = config.projectBuildId};
    }

    /** @brief Owns a genuine scheduler, Scene, canonical participants and ordinary filesystem sandbox. */
    struct Fixture final {
        TemporaryDirectory directory;
        SteadyClock clock;
        SystemWallClock wall;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 16}};
        OperationStore operations{8, 16};
        CancellationSource lifetime;
        CancellationToken frameToken{lifetime.Token()};
        Gameplay::BehaviorRegistry behaviors;
        RuntimeSceneService scenes{SceneRuntimeId{701}};
        std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner;
        std::shared_ptr<PlaySaveStorageSchema> schema{std::make_shared<PlaySaveStorageSchema>(PlaySaveStorageSchema{Definition(), {}, {}})};
        std::filesystem::path blockedRoot;
        std::unique_ptr<EditorPlaySaveHost> host;
        std::uint64_t frame{};

        explicit Fixture(bool failOpen = false, bool sceneOwnedGameplay = false) {
            schema->identities = {{Value(PersistentEntityId::Parse("7d00b1e9-ec8b-407b-a132-d349f5ccaf76")),
                                   {1},
                                   AuthoredPersistentEntityProvenance{{41}, {1}},
                                   PersistentEntityDisposition::Live}};
            if (sceneOwnedGameplay) {
                REQUIRE(behaviors.Freeze().HasValue());
                behaviorOwner = Value(Gameplay::BehaviorRuntimeSceneOwner::Create(scenes, behaviors));
                REQUIRE(scenes.AddActivationParticipant(behaviorOwner->MakeActivationParticipant()).HasValue());
                schema->lifecycle = Gameplay::BehaviorLifecycleParticipant();
            }
            REQUIRE(scenes.Startup(frameToken).HasValue());
            REQUIRE(scenes.QueuePreparation(schema->baseline).HasValue());
            REQUIRE(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = ++frame, .cancellation = frameToken})
                        .HasValue());
            REQUIRE(scenes.ActiveScene());
            if (behaviorOwner)
                REQUIRE(behaviorOwner->ActivatePublished().HasValue());
            auto config = Storage(directory.Path());
            if (failOpen) {
                blockedRoot = config.root.CanonicalPath();
                REQUIRE(std::filesystem::remove(blockedRoot));
                std::ofstream blocker(blockedRoot);
                REQUIRE(blocker.good());
                blocker << "Fixture-owned file blocks the real namespace root.";
            }
            host = Value(EditorPlaySaveHost::Create(scenes, {jobs, operations, clock, wall, std::move(config)}, schema, {}, behaviorOwner));
            if (!failOpen)
                Ready();
        }

        ~Fixture() {
            host->BeginRetirement();
            jobs.StopAccepting();
            jobs.Shutdown(ShutdownPolicy::Cancel);
            for (std::size_t phase = 0; phase < MaximumSaveParticipantCount + 8 && !host->IsSettled(); ++phase)
                Pump();
            REQUIRE(host->IsSettled());
            host.reset();
            scenes.Shutdown();
        }

        /** @brief Advances only actual owner Scene/session phases; never invents operation completion. */
        void Pump() {
            const FrameContext context{.frameNumber = ++frame, .cancellation = frameToken};
            const auto scene = scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, context);
            INFO((scene.HasError() ? scene.ErrorValue().message : "Scene phase accepted."));
            REQUIRE(host->CommitAtSafePoint(context).HasValue());
        }

        /** @brief Uses a bounded explicit fixture wait over the actual immutable owner projection. */
        void Ready() {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                Pump();
                const auto snapshot = host->Snapshot();
                if (snapshot && !snapshot->operationPending && snapshot->availability == Editor::PlaySaveAvailability::Ready)
                    return;
                if (snapshot && snapshot->error) {
                    INFO(snapshot->error->domain.Value());
                    INFO(snapshot->error->code.Value());
                    FAIL(snapshot->error->message);
                }
                std::this_thread::yield();
            }
            FAIL("Actual Play save owner did not settle before fixture deadline");
        }

        /** @brief Mutates the real Scene under the same actual capture barrier used by Play callbacks. */
        void Move(float x) {
            REQUIRE(host->BeginMutation().HasValue());
            SceneCommandBuffer commands;
            Math::Transform transform;
            transform.translation.x = x;
            commands.SetLocalTransform(*scenes.ActiveScene()->Find({1}), transform);
            REQUIRE(scenes.QueueStructuralCommands(std::move(commands)).HasValue());
            REQUIRE(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = ++frame, .cancellation = frameToken})
                        .HasValue());
            REQUIRE(host->EndMutation().HasValue());
        }

        /** @brief Retrieves only the real current lifecycle projection; no directory scan or synthetic slot row is used. */
        SaveManagerPage Page() const {
            return Value(host->Snapshot()->manager->Page({.maximumRows = 16}));
        }
    };
}  // namespace

TEST_CASE("Play sandbox saves current real Scene and loads it through the sole "
          "runtime Scene bundle",
          "[editor][play][save][host]") {
    Fixture f;
    const auto authoringBytes = Value(SceneCook::EncodeCookedSceneDefinition(f.schema->baseline));
    f.Move(7);
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew, .displayName = "Actual camera"}).HasValue());
    f.Ready();
    const auto page = f.Page();
    REQUIRE(page.rows.size() == 1);
    CHECK(page.rows[0].displayName == "Actual camera");
    const auto command = Value(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, page.rows[0].slot));
    const auto originalRuntime = f.scenes.ActiveScene()->RuntimeId();
    f.Move(21);
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = command}).HasValue());
    f.Ready();
    CHECK(f.scenes.ActiveScene()->RuntimeId() != originalRuntime);
    const auto entity = Value(f.scenes.ActiveScene()->Get(*f.scenes.ActiveScene()->Find({1})));
    REQUIRE(entity.localTransform);
    CHECK(entity.localTransform->translation.x == 7);
    CHECK(Value(SceneCook::EncodeCookedSceneDefinition(f.schema->baseline)) == authoringBytes);
    CHECK(f.schema->baseline.Revision() == SceneDefinitionRevision{3});
}

TEST_CASE("Play sandbox rejects stale slot consent and does not replace "
          "authoring or publish an extra slot",
          "[editor][play][save][host]") {
    Fixture f;
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasValue());
    f.Ready();
    const auto page = f.Page();
    REQUIRE(page.rows.size() == 1);
    const auto old = Value(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, page.rows[0].slot));
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::Refresh}).HasValue());
    f.Ready();
    CHECK(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = old}).HasError());
    CHECK(f.Page().rows.size() == 1);
    CHECK(f.scenes.ActiveScene()->RuntimeId() == SceneRuntimeId{701});
}

TEST_CASE("Play retirement retains actual accepted worker until scheduler "
          "cancellation has genuinely joined",
          "[editor][play][save][host]") {
    Fixture f;
    const auto release = std::make_shared<std::atomic_bool>(false);
    auto blocker = Value(f.jobs.SubmitResult({}, [release](const CancellationToken &token) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!release->load() && !token.IsCancellationRequested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::yield();
        return Result<void>::Success();
    }));
    const auto started = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (blocker.Snapshot()->state == JobState::Queued && std::chrono::steady_clock::now() < started)
        std::this_thread::yield();
    REQUIRE(blocker.Snapshot()->state == JobState::Running);
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasValue());
    f.Pump();  // Actual capture submits the save worker behind the running genuine
               // blocker.
    f.host->BeginRetirement();
    CHECK(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasError());
    CHECK_FALSE(f.host->IsSettled());  // Cleanup has not been attempted, not a
                                       // claim about worker scheduling speed.
    release->store(true);
    f.jobs.StopAccepting();
    f.jobs.Shutdown(ShutdownPolicy::Cancel);
    f.Pump();
    CHECK(f.host->IsSettled());
    const auto operation = f.operations.SnapshotIfChanged(0);
    REQUIRE(operation);
    REQUIRE_FALSE(operation->operations.empty());
    CHECK(operation->operations.back().state == OperationState::Cancelled);
}

TEST_CASE("Play archive policy admits real writer output and counts header "
          "manifest separately from record chunks",
          "[editor][play][save][policy]") {
    Fixture f;
    const auto captured = CaptureForPolicy(f.scenes, *f.schema);
    const auto header = PolicyHeader(Storage(f.directory.Path()), f.wall);
    const auto written = Value(BuildPlaySaveArchive(captured, header));
    const auto admitted = Value(SaveArchiveReader{PlaySaveArchiveLimits()}.Read(written.Archive().bytes));
    REQUIRE(admitted.Directory().Entries().size() == 1);
    const auto bytes = Value(admitted.SelectChunk(PlaySaveSceneRecord()));
    REQUIRE(bytes);
    const auto decoded = Value(DecodePlaySaveScene(*bytes, f.schema->baseline, f.schema->identities));
    CHECK(decoded.definition.Entities().size() == f.schema->baseline.Entities().size());
    CHECK(admitted.Header().world == header.world);
    const auto policy = PlaySaveArchiveLimits();
    CHECK(policy.maximumStoredPayloadBytes <= policy.maximumArchiveBytes);
    CHECK(policy.metadata.maximumNestingDepth <= policy.maximumNestingDepth);
    CHECK(policy.metadata.maximumChunksPerParticipant <= policy.metadata.maximumTotalChunks);
    CHECK(policy.metadata.maximumTotalChunks + 2 <= policy.maximumEntries);
    CHECK(policy.chunks.maximumEntries == policy.maximumEntries);
    CHECK(policy.chunks.maximumPayloadBytes <= policy.maximumStoredPayloadBytes);
    CHECK(policy.chunks.maximumStoredChunkBytes <= policy.maximumStoredPayloadBytes);
    CHECK(policy.chunks.maximumDecodedChunkBytes <= policy.maximumDecodedBytes);
    CHECK(policy.chunks.maximumExpansionRatio <= policy.maximumExpansionRatio);
    auto exact = PlaySaveArchiveLimits();
    exact.maximumEntries = 3;
    exact.metadata.maximumTotalChunks = 1;
    exact.metadata.maximumChunksPerParticipant = 1;
    exact.chunks.maximumEntries = 3;
    CHECK(SaveArchiveReader{exact}.Read(written.Archive().bytes).HasValue());
    exact.maximumEntries = 2;
    exact.chunks.maximumEntries = 2;
    CHECK(SaveArchiveReader{exact}.Read(written.Archive().bytes).HasError());
}

TEST_CASE("Play binding suspension preserves the same sandbox and real slots "
          "across an exact inventory rebind",
          "[editor][play][save][rebind]") {
    Fixture f;
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasValue());
    f.Ready();
    const auto before = f.Page();
    REQUIRE(before.rows.size() == 1);
    const auto consent = Value(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, before.rows[0].slot));
    CHECK(f.host->RebindGameplay({}).HasError());
    CHECK(f.Page().rows[0].slot == before.rows[0].slot);
    CHECK(f.host->SuspendForModuleReload());
    CHECK_FALSE(f.host->HasRuntimeBinding());
    CHECK(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasError());
    CHECK(f.Page().rows[0].slot == before.rows[0].slot);
    REQUIRE(f.host->RebindGameplay({}).HasValue());
    REQUIRE(f.host->HasRuntimeBinding());
    CHECK(f.Page().rows[0].slot == before.rows[0].slot);
    const auto stale = f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = consent});
    REQUIRE(stale.HasError());
    CHECK(stale.ErrorValue().domain.Value() == SaveErrors::ManagerProjectionStale.domain.Value());
    CHECK(stale.ErrorValue().code.Value() == SaveErrors::ManagerProjectionStale.code.Value());
    const auto currentConsent = Value(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, before.rows[0].slot));
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = currentConsent}).HasValue());
    f.Ready();
    CHECK(f.Page().rows[0].slot == before.rows[0].slot);
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasValue());
    f.Ready();
    CHECK(f.Page().rows.size() == 2);
}

TEST_CASE("Explicit Play Refresh retries the same real root after asynchronous "
          "Open failed",
          "[editor][play][save][recovery]") {
    Fixture f{true};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (f.host->Snapshot()->operationPending && std::chrono::steady_clock::now() < deadline) {
        f.Pump();
        std::this_thread::yield();
    }
    const auto failed = f.host->Snapshot();
    REQUIRE_FALSE(failed->operationPending);
    REQUIRE(failed->error);
    CHECK(failed->availability == Editor::PlaySaveAvailability::Failed);
    CHECK_FALSE(failed->manager);
    CHECK(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasError());
    REQUIRE(std::filesystem::remove(f.blockedRoot));
    REQUIRE(std::filesystem::create_directory(f.blockedRoot));
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::Refresh}).HasValue());
    f.Ready();
    CHECK_FALSE(f.host->Snapshot()->error);
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasValue());
    f.Ready();
    CHECK(f.Page().rows.size() == 1);
}

TEST_CASE("Core capture metadata does not independently retain executable live "
          "bindings",
          "[editor][play][save][leases]") {
    Fixture f;
    auto live = std::make_shared<const std::vector<Editor::PlaySaveGameplayBinding>>();
    const std::weak_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> witness = live;
    auto capture = std::make_shared<PlaySaveSceneCapture>(f.scenes, f.schema->baseline, f.schema->identities, live);
    live.reset();
    CHECK(witness.expired());
    CanonicalStateParticipantRegistry registry;
    REQUIRE(registry.Register(PlaySaveSceneDescriptor(), capture).HasValue());
    const auto participants = Value(registry.Snapshot());
    CHECK(participants.Find(PlaySaveSceneParticipant()));
    CHECK(witness.expired());
}

TEST_CASE("Play host lifecycle adapter restores the actual Scene-owned runtime before repeated durable capture",
          "[editor][play][save][lifecycle]") {
    Fixture f{false, true};
    const auto before = f.scenes.ActiveScene()->RuntimeId();
    REQUIRE(f.behaviorOwner->Active());
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasValue());
    f.Ready();
    const auto rows = f.Page();
    REQUIRE(rows.rows.size() == 1);
    const auto load = Value(f.host->Snapshot()->manager->Command(SaveManagerCommandKind::Load, rows.rows[0].slot));
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = load}).HasValue());
    f.Ready();
    CHECK(f.scenes.ActiveScene()->RuntimeId() != before);
    REQUIRE(f.behaviorOwner->Active());
    REQUIRE(f.host->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}).HasValue());
    f.Ready();
    CHECK(f.Page().rows.size() == 2);
}
