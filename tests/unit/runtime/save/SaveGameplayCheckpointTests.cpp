#include "Horo/Runtime/Save/SaveGameplayCheckpoint.h"
#include "SaveTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <stdexcept>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace Test;

        SaveParticipantId Owner() {
            return SaveParticipantId::Parse("project.checkpoint.v1").Value();
        }

        class CaptureAdapter final : public ICanonicalStateAdapter {
        public:
            Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &, ICanonicalCaptureSink &sink) const override {
                const std::array bytes{std::byte{42}};
                auto result = sink.WriteCopied(Id<SaveRecordId>(1), bytes);
                if (result.HasError())
                    return Result<CanonicalCaptureDisposition>::Failure(result.ErrorValue());
                return Result<CanonicalCaptureDisposition>::Success(CanonicalCaptureDisposition::Captured);
            }
        };

        SaveParticipantRegistrySnapshot Registry(const std::uint32_t schema = 1) {
            CanonicalStateParticipantRegistry registry;
            const CanonicalStateParticipantDescriptor descriptor{.participant = Owner(),
                                                                 .schemaVersion = V<ParticipantSchemaVersion>(schema),
                                                                 .scope = SaveParticipantScope::RuntimeScene,
                                                                 .roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore,
                                                                 .required = true,
                                                                 .limits = {.maximumPayloadBytes = 64,
                                                                            .maximumRecordCount = 1,
                                                                            .maximumNestingDepth = 2},
                                                                 .ownedRecords = {Id<SaveRecordId>(1)}};
            REQUIRE(registry.Register(descriptor, std::make_shared<CaptureAdapter>()).HasValue());
            return registry.Snapshot().Value();
        }

        GameplayCheckpointMetadata Metadata() {
            return {.checkpoint = Id<SaveCheckpointId>(1),
                    .baseline = {.project = Id<SaveProjectId>(1),
                                 .world = Id<SaveWorldId>(2),
                                 .baseScene = Id<SaveBaseSceneId>(3),
                                 .compatibility = V<ProductSaveCompatibilityVersion>(1),
                                 .baseline = {.value = Digest(1)}},
                    .spawnAnchor = Id<SaveRecordId>(2),
                    .restartContext = Id<SaveRecordId>(3),
                    .payload = {.participant = Owner(), .record = Id<SaveRecordId>(1), .schema = V<ParticipantSchemaVersion>(1)},
                    .display = {.displayName = "Checkpoint"}};
        }

        GameplayCheckpoint Capture(const SaveParticipantRegistrySnapshot &registry) {
            return GameplayCheckpoint::Capture(Metadata(),
                                               {.capturedState = Id<CapturedStateId>(1),
                                                .epoch = {1},
                                                .sceneIncarnation = 3,
                                                .sceneRevision = 1,
                                                .registryGeneration = registry.Generation()},
                                               2, registry)
                .Value();
        }

        struct State final {
            int live{9};
            int rollbacks{};
            bool fail{};
        };

        class Prepared final : public ICanonicalRestorePreparedState {};

        class Receipt final : public IStagedRestoreParticipant {
        public:
            explicit Receipt(State &state) : state_(state) {}

            const StagedRestoreParticipantRequirement &Requirement() const noexcept override {
                return requirement_;
            }

            Result<void> Decode(const StagedRestoreContext &) override {
                return Result<void>::Success();
            }

            Result<void> Validate(const StagedRestoreContext &) override {
                return state_.fail ? Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid)) : Result<void>::Success();
            }

            Result<void> Instantiate(const StagedRestoreContext &) override {
                return Result<void>::Success();
            }

            const ICanonicalRestorePreparedState *PreparedState() const noexcept override {
                return &prepared_;
            }

            Result<void> ApplyState(const ICanonicalRestoreDependencyLookup &) override {
                return Result<void>::Success();
            }

            Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &) override {
                return Result<void>::Success();
            }

            void PublishPrepared() noexcept override {
                state_.live = 42;
                published_ = true;
            }

            void RollbackPrepared() noexcept override {
                if (!published_ && !rolledBack_) {
                    ++state_.rollbacks;
                    rolledBack_ = true;
                }
            }

        private:
            StagedRestoreParticipantRequirement requirement_{Owner(), V<ParticipantSchemaVersion>(1), SaveParticipantScope::RuntimeScene};
            State &state_;
            Prepared prepared_;
            bool published_{};
            bool rolledBack_{};
        };

        class Source final : public IGameplayCheckpointRestoreSource {
        public:
            State state;
            int calls{};
            bool reject{};
            bool throws{};
            bool durable{};
            GameplayCheckpointController *clearSelection{};

            Result<GameplayCheckpointRestoreStaging> Stage(const GameplayCheckpoint &checkpoint, const StagedRestoreContext &,
                                                           const SaveParticipantRegistrySnapshot &) override {
                ++calls;
                if (clearSelection)
                    REQUIRE(clearSelection->Clear().HasValue());
                if (throws)
                    throw std::runtime_error("staging failure");
                durable = checkpoint.Publication() != nullptr;
                if (!durable) {
                    REQUIRE(checkpoint.Snapshot()->Records().size() == 1);
                    REQUIRE(checkpoint.Snapshot()->Records()[0].Segment(0)[0] == std::byte{42});
                }
                if (reject)
                    return Result<GameplayCheckpointRestoreStaging>::Failure(MakeError(SaveErrors::CommandStale));
                std::vector<std::unique_ptr<IStagedRestoreParticipant>> receipts;
                receipts.push_back(std::make_unique<Receipt>(state));
                return Result<GameplayCheckpointRestoreStaging>::Success({std::move(receipts), {}});
            }
        };

        Result<StagedRestoreTransaction> Restart(GameplayCheckpointController &controller, const SaveParticipantRegistrySnapshot &registry,
                                                 Source &source, const std::uint64_t scene = 3, const std::uint64_t session = 2,
                                                 const bool cancel = false) {
            auto operation =
                CreateSaveOperation({.operation = 1452, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 1}).Value();
            if (cancel)
                (void)operation.Handle().RequestCancellation();
            return controller.Restart(Metadata().baseline,
                                      {.operation = 1452,
                                       .registryGeneration = registry.Generation(),
                                       .sessionGeneration = session,
                                       .sceneIncarnation = scene,
                                       .maximumParticipants = 8},
                                      std::move(operation), registry, source);
        }
    }  // namespace

    TEST_CASE("Gameplay checkpoint captures immutable state with display-independent identity", "[save][checkpoint]") {
        const auto registry = Registry();
        auto checkpoint = Capture(registry);
        CHECK(checkpoint.Lifetime() == GameplayCheckpointLifetime::Transient);
        CHECK(checkpoint.Publication() == nullptr);
        REQUIRE(checkpoint.Snapshot());
        CHECK(checkpoint.Snapshot()->PayloadByteLength() == 1);
        auto metadata = Metadata();
        metadata.display.displayName = "Kontrol noktası";
        auto alternate = GameplayCheckpoint::Capture(metadata, checkpoint.Snapshot()->Provenance(), 2, registry);
        REQUIRE(alternate.HasValue());
        CHECK(alternate.Value().Metadata().checkpoint == checkpoint.Metadata().checkpoint);
        metadata.display.displayName = std::string(300, 'a');
        auto invalidDisplay = GameplayCheckpoint::Capture(metadata, checkpoint.Snapshot()->Provenance(), 2, registry);
        REQUIRE(invalidDisplay.HasValue());
        CHECK(invalidDisplay.Value().Metadata().display.displayName.empty());
        metadata.payload.schema = V<ParticipantSchemaVersion>(2);
        CHECK(GameplayCheckpoint::Capture(metadata, checkpoint.Snapshot()->Provenance(), 2, registry).HasError());
    }

    TEST_CASE("Gameplay checkpoint durable promotion requires matching committed publication", "[save][checkpoint]") {
        auto checkpoint = Capture(Registry());
        auto publication = Entry(1, 2).publication;
        CHECK(checkpoint.CommitDurable(publication).HasError());
        publication.kind = SaveSlotKind::Checkpoint;
        publication.checkpoint = checkpoint.Metadata().checkpoint;
        auto durable = checkpoint.CommitDurable(publication);
        REQUIRE(durable.HasValue());
        CHECK(durable.Value().Lifetime() == GameplayCheckpointLifetime::Durable);
        CHECK(durable.Value().Snapshot() == nullptr);
        CHECK(durable.Value().Publication()->generation == publication.generation);
        CHECK(durable.Value().CommitDurable(publication).HasError());
        publication.baseScene = Id<SaveBaseSceneId>(4);
        CHECK(GameplayCheckpoint::OpenDurable(Metadata(), publication).HasError());
    }

    TEST_CASE("Gameplay checkpoint restart prepares shared transaction without publishing", "[save][checkpoint]") {
        const auto registry = Registry();
        GameplayCheckpointController controller;
        REQUIRE(controller.Activate(Capture(registry), Metadata().baseline, registry).HasValue());
        Source source;
        source.clearSelection = &controller;
        auto result = Restart(controller, registry, source);
        REQUIRE(result.HasValue());
        CHECK(result.Value().State() == StagedRestoreTransactionState::ReadyToActivate);
        CHECK(source.state.live == 9);
        REQUIRE(result.Value().Activate({registry.Generation(), 2, 3}).HasValue());
        CHECK(source.state.live == 42);
        CHECK(source.state.rollbacks == 0);
        REQUIRE(controller.Clear().HasValue());
        CHECK(controller.Active() == nullptr);
    }

    TEST_CASE("Gameplay checkpoint stale and incompatible selection preserves previous checkpoint", "[save][checkpoint]") {
        const auto registry = Registry();
        GameplayCheckpointController controller;
        REQUIRE(controller.Activate(Capture(registry), Metadata().baseline, registry).HasValue());
        auto baseline = Metadata().baseline;
        baseline.world = Id<SaveWorldId>(4);
        CHECK(controller.Activate(Capture(registry), baseline, registry).HasError());
        REQUIRE(controller.Active());
        CHECK(controller.Active()->Metadata().baseline.world == Metadata().baseline.world);
        Source source;
        CHECK(Restart(controller, registry, source, 4).HasError());
        CHECK(Restart(controller, registry, source, 3, 4).HasError());
        CHECK(Restart(controller, registry, source, 3, 2, true).HasError());
        CHECK(source.calls == 0);
        CHECK(controller.Activate(Capture(registry), Metadata().baseline, Registry(2)).HasError());
        bool wrongThreadRejected{};
        std::thread thread([&] {
            wrongThreadRejected = controller.Clear().HasError() && controller.Active() == nullptr;
        });
        thread.join();
        CHECK(wrongThreadRejected);
    }

    TEST_CASE("Gameplay checkpoint failure and stale activation retain live state", "[save][checkpoint]") {
        const auto registry = Registry();
        GameplayCheckpointController controller;
        REQUIRE(controller.Activate(Capture(registry), Metadata().baseline, registry).HasValue());
        Source source;
        SECTION("participant validation failure") {
            source.state.fail = true;
            CHECK(Restart(controller, registry, source).HasError());
            CHECK(source.state.rollbacks == 1);
        }
        SECTION("source rejects stale durable generation") {
            auto publication = Entry(1, 2).publication;
            publication.kind = SaveSlotKind::Checkpoint;
            publication.checkpoint = Metadata().checkpoint;
            REQUIRE(controller.Activate(Capture(registry).CommitDurable(publication).Value(), Metadata().baseline, registry).HasValue());
            source.reject = true;
            CHECK(Restart(controller, registry, source).HasError());
            CHECK(source.durable);
        }
        SECTION("source throws") {
            source.throws = true;
            CHECK(Restart(controller, registry, source).HasError());
        }
        SECTION("generation changed after preparation") {
            auto result = Restart(controller, registry, source);
            REQUIRE(result.HasValue());
            CHECK(result.Value().Activate({registry.Generation(), 4, 3}).HasError());
            CHECK(source.state.rollbacks == 1);
        }
        CHECK(source.state.live == 9);
    }
}  // namespace Horo::Runtime
