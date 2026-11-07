#include "Horo/Assets/AssetProvider.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Scene/SceneRestoreBundle.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime {
    namespace {
        template <typename Identity> Identity StableSealId(std::uint8_t value) {
            SaveIdentityDetail::Bytes bytes{};
            bytes.back() = value;
            return Identity::FromBytes(bytes).Value();
        }

        enum class SealProbeMode {
            KeepUncommitted,
            ReplacePublic,
            ReplayPrevious,
            CommitIssued,
            PublishIssued
        };

        struct SealProbeState final {
            SceneCommandBuffer batch;
            SceneCommandBuffer retained;
            SceneRuntimeId firstCandidate;
            SceneRuntimeId nextCandidate;
            bool created{}, rejected{}, committed{}, published{}, rolledBack{};
        };

        /** @brief Exercises the actual service capability using public buffer copy/move APIs; no native feature acceptance is claimed. */
        class SealAggregate final : public SceneAggregateRestore {
        public:
            SealAggregate(std::shared_ptr<SealProbeState> state, SealProbeMode mode, Assets::AssetRegistryRevision revision)
                : state_(std::move(state)), mode_(mode), revision_(revision) {}

            Result<void> ValidateRestorePartitions(std::span<const SceneRestoreGroupPartition> partitions) const override {
                const PersistentEntityRecord record{StableSealId<PersistentEntityId>(3),
                                                    {1},
                                                    SpawnedPersistentEntityProvenance{StableSealId<SaveAssetId>(4),
                                                                                      StableSealId<SavePrefabInstanceId>(2)},
                                                    PersistentEntityDisposition::Live};
                return ValidateSceneRestorePartitions(StableSealId<SaveWorldId>(1), std::span{&record, 1}, partitions, 1);
            }

            Result<void> PrepareScene(RuntimeScene &) override {
                return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            }

            Result<void> PrepareScene(RuntimeScene &scene, SceneRestoreCandidateCommands &capability) override {
                state_->nextCandidate = scene.View().RuntimeId();
                state_->batch = {};
                RuntimeEntityGroupEntry entry;
                entry.restorePartition = 0;
                SceneRestoreGroupPartition partition{StableSealId<SaveWorldId>(1),
                                                     StableSealId<SavePrefabInstanceId>(2),
                                                     0,
                                                     {{0, StableSealId<PersistentEntityId>(3), 1}}};
                auto created = capability.CreateGroup(state_->batch, {std::move(entry)}, {}, {}, {std::move(partition)});
                if (created.HasError())
                    return Result<void>::Failure(created.ErrorValue());
                state_->created = true;
                return Probe(scene, capability);
            }

            Result<bool> PrepareOwners() override {
                return Result<bool>::Success(true);
            }

            Result<void> ValidatePublication(const RuntimeSceneView *) const override {
                return Result<void>::Success();
            }

            Result<void> Commit(IStagedRestoreAggregatePublication &transfer) override {
                transfer.PublishPrepared();
                state_->published = true;
                return Result<void>::Success();
            }

            void Rollback() noexcept override {
                if (!state_->published)
                    state_->rolledBack = true;
            }

        private:
            Result<void> Probe(RuntimeScene &scene, SceneRestoreCandidateCommands &capability) {
                if (mode_ == SealProbeMode::KeepUncommitted) {
                    state_->firstCandidate = scene.View().RuntimeId();
                    state_->retained = state_->batch;
                    return RejectCandidate();
                }
                if (mode_ == SealProbeMode::ReplacePublic) {
                    state_->batch = {};
                    auto publicGroup =
                        state_->batch.CreateGroup({RuntimeEntityGroupEntry{}}, {}, {scene.View().RuntimeId(), revision_, {}});
                    if (publicGroup.HasError())
                        return Result<void>::Failure(publicGroup.ErrorValue());
                }
                if (mode_ == SealProbeMode::ReplayPrevious)
                    state_->batch = state_->retained;
                auto committed = capability.Commit(state_->batch);
                state_->rejected = committed.HasError();
                state_->committed = committed.HasValue();
                if (mode_ != SealProbeMode::PublishIssued)
                    return RejectCandidate();
                if (committed.HasError())
                    return Result<void>::Failure(committed.ErrorValue());
                state_->retained = state_->batch;
                return Result<void>::Success();
            }

            static Result<void> RejectCandidate() {
                return Result<void>::Failure(
                    MakeError(SaveErrors::RestoreParticipantInvalid, "Seal probe abandons its private candidate."));
            }

            std::shared_ptr<SealProbeState> state_;
            SealProbeMode mode_;
            Assets::AssetRegistryRevision revision_;
        };

        struct SealFixture final {
            Assets::AssetRegistry registry;
            Assets::MemoryAssetProvider bytes;
            JobSystem jobs{{1, 4}};
            Assets::AssetLoadService loads{jobs, bytes};
            RuntimeSceneService scenes{registry, loads};
            CancellationSource cancellation;

            SealFixture() {
                REQUIRE(registry.Publish(std::vector<Assets::AssetRecord>{}).status == Assets::AssetRegistryBuildStatus::Complete);
                REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
                Activate();
            }

            ~SealFixture() {
                scenes.Shutdown();
                loads.Shutdown();
                jobs.Shutdown(ShutdownPolicy::Cancel);
            }

            RuntimeSceneDefinition Definition() {
                SceneDefinitionBuilder builder{{7}, {1}};
                return std::move(builder).Build().Value();
            }

            void Commit() {
                REQUIRE(
                    scenes.OnPhase(RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 1, .cancellation = cancellation.Token()})
                        .HasValue());
            }

            void Activate() {
                REQUIRE(scenes.QueuePreparation(Definition()).HasValue());
                Commit();
            }

            Result<void> Queue(std::shared_ptr<SealProbeState> state, SealProbeMode mode) {
                return scenes.QueuePreparationWithRestore(Definition(), std::make_unique<SealAggregate>(std::move(state), mode,
                                                                                                        registry.Snapshot().Revision()));
            }
        };

        void CheckOrdinaryReplayRejected(SealFixture &fixture, const SceneCommandBuffer &retained) {
            const auto before = fixture.scenes.ActiveScene()->StructuralRevision();
            SceneCommandBuffer copied = retained;
            SceneCommandBuffer moved = std::move(copied);
            auto queued = fixture.scenes.QueueStructuralCommands(std::move(moved));
            std::optional<Error> error;
            if (queued.HasError())
                error = queued.ErrorValue();
            else {
                fixture.Commit();
                error = fixture.scenes.TakeOperationError();
            }
            REQUIRE(error);
            CHECK(error->code.Value() == "scene.runtime.invalid_candidate");
            auto direct = RuntimeScene::Create(fixture.Definition(), fixture.scenes.ActiveScene()->RuntimeId());
            REQUIRE(direct.HasValue());
            auto rejected = direct.Value()->Commit(retained);
            REQUIRE(rejected.HasError());
            CHECK(rejected.ErrorValue().code.Value() == "scene.runtime.invalid_candidate");
            CHECK(direct.Value()->View().SlotCount() == 0);
            CHECK(fixture.scenes.ActiveScene()->StructuralRevision() == before);
            CHECK_FALSE(fixture.scenes.TakeStructuralCommitResult());
        }
    }  // namespace

    TEST_CASE("Private restore rejects public group replacement at the same buffer address", "[runtime][scene][restore][seal]") {
        SealFixture fixture;
        const auto original = fixture.scenes.ActiveScene()->RuntimeId();
        auto state = std::make_shared<SealProbeState>();
        REQUIRE(fixture.Queue(state, SealProbeMode::ReplacePublic).HasError());
        REQUIRE(state->created);
        CHECK(state->rejected);
        CHECK_FALSE(state->committed);
        CHECK(state->rolledBack);
        CHECK(fixture.scenes.ActiveScene()->RuntimeId() == original);
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
    }

    TEST_CASE("Private retained group cannot replay across failed candidates reusing the same runtime identity",
              "[runtime][scene][restore][seal]") {
        SealFixture fixture;
        auto state = std::make_shared<SealProbeState>();
        REQUIRE(fixture.Queue(state, SealProbeMode::KeepUncommitted).HasError());
        REQUIRE(state->created);
        REQUIRE(fixture.Queue(state, SealProbeMode::ReplayPrevious).HasError());
        CHECK(state->firstCandidate == state->nextCandidate);
        CHECK(state->rejected);
        CHECK_FALSE(state->committed);
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
    }

    TEST_CASE("Exact issued private group commits while ordinary copies cannot inherit its admission", "[runtime][scene][restore][seal]") {
        SealFixture fixture;
        auto state = std::make_shared<SealProbeState>();
        REQUIRE(fixture.Queue(state, SealProbeMode::CommitIssued).HasError());
        CHECK(state->committed);
        CHECK_FALSE(state->rejected);
        CHECK(state->rolledBack);
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == 0);
    }

    TEST_CASE("Retained private groups are rejected by ordinary paths after failure or publication",
              "[runtime][scene][restore][seal][replay]") {
        SealFixture fixture;
        auto state = std::make_shared<SealProbeState>();
        SECTION("failed private attempt then matching ordinary runtime") {
            REQUIRE(fixture.Queue(state, SealProbeMode::KeepUncommitted).HasError());
            fixture.Activate();
            REQUIRE(fixture.scenes.ActiveScene()->RuntimeId() == state->firstCandidate);
        }
        SECTION("private aggregate actually published") {
            REQUIRE(fixture.Queue(state, SealProbeMode::PublishIssued).HasValue());
            fixture.Commit();
            REQUIRE(state->published);
            REQUIRE(state->committed);
        }
        const auto count = fixture.scenes.ActiveScene()->SlotCount();
        CheckOrdinaryReplayRejected(fixture, state->retained);
        CHECK(fixture.scenes.ActiveScene()->SlotCount() == count);
    }
}  // namespace Horo::Runtime
