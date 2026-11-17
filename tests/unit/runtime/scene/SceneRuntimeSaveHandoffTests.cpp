#include "../save/SaveGameplayPersistenceTestUtils.h"
#include "Horo/Runtime/Save/RuntimeSaveSession.h"
#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

#include <chrono>
#include <thread>

namespace Horo::Runtime {
    namespace {
        using namespace Test::GameplayPersistence;

        /** @brief Actual project state behind the production gameplay persistence adapter. */
        struct SavedGame final {
            std::uint32_t coins{42}, checkpoint{17};
            unsigned publications{};
            std::uint64_t candidateScene{};
        };

        /** @brief Prepared inactive semantic state publishes only inside the existing aggregate restore gate. */
        class GameCandidate final : public IPreparedGameplayPersistenceState {
        public:
            GameCandidate(SavedGame &active, const std::uint32_t coins, const std::uint32_t checkpoint)
                : active_(active), coins_(coins), checkpoint_(checkpoint) {}

            Result<void> FixupRuntimeReferences(const SaveRestoreReferenceView &references) override {
                candidateScene_ = references.generation.candidateScene;
                return Result<void>::Success();
            }

            void Publish() noexcept override {
                active_.coins = coins_;
                active_.checkpoint = checkpoint_;
                active_.candidateScene = candidateScene_;
                ++active_.publications;
            }

        private:
            SavedGame &active_;
            std::uint32_t coins_, checkpoint_;
            std::uint64_t candidateScene_{};
        };

        /** @brief Real project payload encoding; four ASCII signature bytes and two explicit little-endian semantic integers. */
        class GameSource final : public IGameplayPersistenceSource {
        public:
            SavedGame active;
            bool failPrepare{};

            Result<std::vector<std::byte>> CaptureRuntimeState(std::uint64_t) const override {
                std::vector<std::byte> bytes{std::byte{'G'}, std::byte{'S'}, std::byte{'V'}, std::byte{'1'}};
                for (const auto value : {active.coins, active.checkpoint})
                    for (unsigned shift = 0; shift < 32; shift += 8)
                        bytes.push_back(static_cast<std::byte>((value >> shift) & 255U));
                return Result<std::vector<std::byte>>::Success(std::move(bytes));
            }

            Result<std::unique_ptr<IPreparedGameplayPersistenceState>> PrepareRuntimeState(
                const std::span<const std::byte> bytes) override {
                using Outcome = Result<std::unique_ptr<IPreparedGameplayPersistenceState>>;
                if (failPrepare || bytes.size() != 12 || bytes[0] != std::byte{'G'} || bytes[1] != std::byte{'S'} ||
                    bytes[2] != std::byte{'V'} || bytes[3] != std::byte{'1'})
                    return Outcome::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
                return Outcome::Success(std::make_unique<GameCandidate>(active, Read(bytes, 4), Read(bytes, 8)));
            }

        private:
            static std::uint32_t Read(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
                std::uint32_t value{};
                for (unsigned index = 0; index < 4; ++index)
                    value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8);
                return value;
            }
        };

        /** @brief Stages the production adapter's real captured canonical records, not an invented archive placeholder. */
        class StoredSource final : public IRuntimeSaveRestoreSource {
        public:
            StoredSource(std::shared_ptr<GameplayPersistenceAdapter> adapter, RuntimeSaveSnapshot snapshot)
                : adapter_(std::move(adapter)), snapshot_(std::move(snapshot)) {}

            Result<RuntimeSaveRestoreStaging> Stage(const StagedRestoreContext &, const SaveParticipantRegistrySnapshot &,
                                                    const CancellationToken &) override {
                RuntimeSaveRestoreStaging result;
                for (const auto &captured : snapshot_.Records()) {
                    const auto &record = captured.Record();
                    auto receipt = adapter_->StageRestore(record.schemaVersion, record.record, captured.Segment(0));
                    if (receipt.HasError())
                        return Result<RuntimeSaveRestoreStaging>::Failure(receipt.ErrorValue());
                    result.participants.push_back(std::move(receipt).Value());
                }
                return Result<RuntimeSaveRestoreStaging>::Success(std::move(result));
            }

        private:
            std::shared_ptr<GameplayPersistenceAdapter> adapter_;
            RuntimeSaveSnapshot snapshot_;
        };

        struct SceneControl final {
            bool ready{}, retired{};
        };

        /** @brief Owns exactly the session-issued packet until the existing Scene transaction takes its original producer. */
        class PacketSource final : public ISceneRestoreOwnerSource {
        public:
            PacketSource(RuntimeSavePreparedRestore packet, std::shared_ptr<SceneControl> control)
                : packet_(std::move(packet)), sourceLease_(packet_->sourceLease), control_(std::move(control)) {}

            ~PacketSource() override {
                Cancel();
            }

            Result<std::optional<SceneRestorePreparedOwners>> TakeReady(RuntimeSceneView) override {
                using Outcome = Result<std::optional<SceneRestorePreparedOwners>>;
                if (!packet_)
                    return Outcome::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                if (packet_->operation.ObserveCancellation() == SaveCancellationObservation::Cancelled)
                    return Outcome::Failure(
                        packet_->operation.Handle().Snapshot()->terminalError.value_or(MakeError(SaveErrors::OperationCancelled)));
                if (!control_->ready)
                    return Outcome::Success(std::nullopt);
                SceneRestorePreparedOwners result{std::move(packet_->operation), std::move(packet_->staging.participants)};
                packet_.reset();
                return Outcome::Success(std::move(result));
            }

            void Cancel() noexcept override {
                if (packet_)
                    (void)packet_->operation.RequestShutdownCancellation();
                packet_.reset();
                control_->retired = true;
            }

        private:
            std::optional<RuntimeSavePreparedRestore> packet_;
            std::shared_ptr<IRuntimeSaveRestoreSource> sourceLease_;
            std::shared_ptr<SceneControl> control_;
        };

        class GenerationAuthority final : public ISceneRestoreGenerationAuthority {
        public:
            StagedRestoreActivationEvidence current;

            StagedRestoreActivationEvidence Current() const noexcept override {
                return current;
            }
        };

        RuntimeSceneDefinition SceneDefinition(const std::uint64_t revision) {
            SceneDefinitionBuilder builder{SceneDefinitionId{1481}, SceneDefinitionRevision{revision}};
            RuntimeEntityDefinition entity;
            entity.object = SceneObjectId{1};
            builder.Add(std::move(entity));
            return std::move(builder).Build().Value();
        }

        /** @brief Test host bridge routes genuine session-owned staging through the production Scene aggregate gate. */
        class SceneHandoff final : public IRuntimeSaveRestoreHandoff {
        public:
            SceneHandoff(RuntimeSceneService &scenes, std::shared_ptr<GenerationAuthority> authority)
                : scenes_(scenes), authority_(std::move(authority)) {}

            Result<void> Accept(RuntimeSavePreparedRestore &packet) override {
                ++accepts;
                if (packet.staging.references)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreAdapterContractInvalid));
                auto request = Request(packet);
                handle_ = packet.operation.Handle();
                auto source = std::make_unique<PacketSource>(std::move(packet), control);
                ownsCandidate_ = true;
                auto bundle = SceneRestoreBundle::Create(std::move(request), std::move(source), authority_);
                if (bundle.HasError())
                    return Result<void>::Failure(bundle.ErrorValue());
                return scenes_.QueuePreparationWithRestore(SceneDefinition(2), std::move(bundle).Value());
            }

            void Cancel() noexcept override {
                ++cancellations;
                (void)handle_.RequestCancellation();
            }

            bool IsSettled() const noexcept override {
                const auto operation = handle_.Snapshot();
                return !ownsCandidate_ || control->retired || (operation && operation->state == SaveOperationState::Completed);
            }

            std::shared_ptr<SceneControl> control = std::make_shared<SceneControl>();
            unsigned accepts{}, cancellations{};

        private:
            static SceneRestoreBundleRequest Request(const RuntimeSavePreparedRestore &packet) {
                SceneRestoreBundleRequest request;
                request.context = packet.context;
                request.participants = packet.participants;
                request.world = Test::Id<SaveWorldId>(1);
                request.definition = SceneDefinitionId{1481};
                request.revision = {2};
                request.entities = {{.identity = Test::Id<PersistentEntityId>(1),
                                     .generation = {1},
                                     .provenance = AuthoredPersistentEntityProvenance{SceneDefinitionId{1481}, SceneObjectId{1}},
                                     .disposition = PersistentEntityDisposition::Live}};
                return request;
            }

            RuntimeSceneService &scenes_;
            std::shared_ptr<GenerationAuthority> authority_;
            SaveOperationHandle handle_;
            bool ownsCandidate_{};
        };

        /** @brief Test barrier reads actual monotonic host time; Load never fabricates capture readiness. */
        class MonotonicClock final : public Clock {
        public:
            Duration MonotonicNow() const override {
                return Duration::FromNanoseconds(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            }
        };

        struct BridgeFixture final {
            MonotonicClock clock;
            JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
            RuntimeSceneService scenes;
            CancellationSource cancellation;
            std::shared_ptr<GameSource> game = std::make_shared<GameSource>();
            std::shared_ptr<GameplayPersistenceAdapter> adapter;
            SaveParticipantRegistrySnapshot participants;
            RuntimeSaveSnapshot snapshot;
            std::unique_ptr<SaveCaptureBarrier> barrier;
            std::unique_ptr<RuntimeSaveSession> session;
            std::shared_ptr<GenerationAuthority> authority = std::make_shared<GenerationAuthority>();
            std::shared_ptr<SceneHandoff> handoff;
            SaveRuntimeGeneration generation;

            BridgeFixture() {
                adapter = GameplayPersistenceAdapter::Create(Descriptor(GameplayPersistenceOwner::Service), game, game).Value();
                CanonicalStateParticipantRegistry registry;
                REQUIRE(registry.Register(adapter->Descriptor().participant, adapter).HasValue());
                participants = registry.Snapshot().Value();
                REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
                REQUIRE(scenes.QueuePreparation(SceneDefinition(1)).HasValue());
                PumpScene();
                auto provenance = Provenance(participants);
                provenance.sceneIncarnation = scenes.ActiveScene()->RuntimeId().value;
                provenance.sceneRevision = scenes.ActiveScene()->StructuralRevision();
                auto capture = RuntimeSaveCaptureBuilder::Create(provenance, participants).Value();
                REQUIRE(capture.CaptureParticipants().HasValue());
                snapshot = capture.Seal().Value();
                generation = {17, scenes.ActiveScene()->RuntimeId().value, participants.Generation()};
                authority->current = {generation.registry, generation.runtime, generation.scene};
                barrier = SaveCaptureBarrier::Create(1481, clock, 4).Value();
                session = RuntimeSaveSession::Create(jobs, *barrier, participants, generation).Value();
                handoff = std::make_shared<SceneHandoff>(scenes, authority);
                game->active.coins = 9;
                game->active.checkpoint = 3;
            }

            ~BridgeFixture() {
                session.reset();
                scenes.Shutdown();
                jobs.Shutdown(ShutdownPolicy::Cancel);
            }

            RuntimeSaveRequest Load() {
                return session
                    ->Load({.operation = 1481, .kind = SaveOperationKind::Load, .maximumCompletionCallbacks = 4},
                           std::make_shared<StoredSource>(adapter, snapshot), handoff)
                    .Value();
            }

            void PumpScene() {
                const FrameContext frame{1, {}, 0.0, 0, {}, false, cancellation.Token()};
                REQUIRE(scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
            }

            void Deliver() {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (handoff->accepts == 0 && std::chrono::steady_clock::now() < deadline) {
                    REQUIRE(session->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, generation).HasValue());
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                REQUIRE(handoff->accepts == 1);
            }
        };

        TEST_CASE("Session load transfers one real staged operation through Scene candidate and atomic world publication",
                  "[save][session][scene][handoff][integration]") {
            BridgeFixture fixture;
            const auto old = fixture.scenes.ActiveScene()->RuntimeId();
            const auto operation = fixture.Load();
            fixture.Deliver();
            fixture.PumpScene();
            CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
            CHECK(fixture.game->active.coins == 9);
            CHECK_FALSE(operation.Operation().Snapshot()->IsTerminal());
            CHECK_FALSE(fixture.session->IsSettled());
            fixture.handoff->control->ready = true;
            fixture.PumpScene();
            REQUIRE_FALSE(fixture.scenes.TakeOperationError());
            CHECK(fixture.scenes.ActiveScene()->RuntimeId() != old);
            CHECK(fixture.game->active.coins == 42);
            CHECK(fixture.game->active.checkpoint == 17);
            CHECK(fixture.game->active.publications == 1);
            CHECK(fixture.game->active.candidateScene == fixture.scenes.ActiveScene()->RuntimeId().value);
            CHECK(operation.Operation().Snapshot()->state == SaveOperationState::Completed);
            // Publication and terminal completion already finished; commit-window TooLate has its own operation-gate regression.
            CHECK(operation.RequestCancellation() == SaveCancellationRequestResult::AlreadyTerminal);
            CHECK(fixture.session->IsSettled());
            CHECK(fixture.session->Acknowledge().HasValue());
        }

        TEST_CASE("Session aggregate failure and retirement preserve the active Scene and wait for real candidate rollback",
                  "[save][session][scene][handoff][failure][integration]") {
            for (const bool retire : {false, true}) {
                BridgeFixture fixture;
                const auto old = fixture.scenes.ActiveScene()->RuntimeId();
                const auto operation = fixture.Load();
                fixture.Deliver();
                if (retire) {
                    REQUIRE(fixture.session->Retire().HasValue());
                    CHECK_FALSE(fixture.session->IsSettled());
                    CHECK(fixture.session->Acknowledge().HasError());
                } else {
                    fixture.game->failPrepare = true;
                }
                fixture.handoff->control->ready = true;
                fixture.PumpScene();
                CHECK(fixture.scenes.ActiveScene()->RuntimeId() == old);
                CHECK(fixture.game->active.coins == 9);
                CHECK(fixture.game->active.publications == 0);
                CHECK(operation.Operation().Snapshot()->IsTerminal());
                CHECK(fixture.session->IsSettled());
                CHECK(fixture.session->Acknowledge().HasValue());
            }
        }
    }  // namespace
}  // namespace Horo::Runtime
