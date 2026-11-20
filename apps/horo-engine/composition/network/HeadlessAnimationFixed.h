#pragma once

#include "HeadlessReplicaAnimationInputs.h"

/** @file HeadlessAnimationFixed.h
 * @brief Target-private fixed participant borrowing the exact generation-owned world records.
 */
#include "HeadlessNetworkWorldOwners.h"

#include <type_traits>

namespace Horo::Application::Internal::HeadlessNetworkDetail {
    class HeadlessFixedParticipant final : public Runtime::RuntimeLifecycleParticipant {
        /** @brief Restricts construction to the factory that validates this owner's one-shot dispatch binding. */
        class ConstructionKey final {
            friend class HeadlessFixedParticipant;
            ConstructionKey() noexcept = default;
        };

        static_assert(!std::is_aggregate_v<ConstructionKey>);

    public:
        static Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>> Create(std::shared_ptr<WorldOwners> owners,
                                                                                    Runtime::RuntimeDispatchSource source) {
            using Return = Result<std::unique_ptr<Runtime::RuntimeLifecycleParticipant>>;
            if (std::this_thread::get_id() != owners->owner || owners->fixedBound)
                return Return::Failure(MakeError(Animation::AnimationErrors::GraphAdmissionRejected));
            if (source.BindingStatus() != Runtime::RuntimeDispatchStatus::Valid)
                return Return::Failure(MakeError(Animation::AnimationErrors::GraphAdmissionRejected));
            auto participant = std::make_unique<HeadlessFixedParticipant>(ConstructionKey{}, owners, std::move(source));
            owners->fixedBound = true;
            owners->fixed = participant.get();
            return Return::Success(std::move(participant));
        }

        /** @brief Retains the exact factory-validated generation and dispatch source.
         * @param owners Existing generation owners retained through the participant's lifecycle.
         * @param source Validated dispatch source moved only after allocation succeeds.
         * @details The private key prevents construction outside the validating one-shot factory.
         */
        HeadlessFixedParticipant(const ConstructionKey &, std::shared_ptr<WorldOwners> owners, Runtime::RuntimeDispatchSource source)
            : owners_(std::move(owners)), source_(std::move(source)) {}

        HeadlessFixedParticipant(const HeadlessFixedParticipant &) = delete;
        HeadlessFixedParticipant &operator=(const HeadlessFixedParticipant &) = delete;
        HeadlessFixedParticipant(HeadlessFixedParticipant &&) = delete;
        HeadlessFixedParticipant &operator=(HeadlessFixedParticipant &&) = delete;

        ~HeadlessFixedParticipant() override {
            if (!OnOwner() || attempting_)
                std::terminate();
            Shutdown();
            owners_->fixed = nullptr;
        }

        const Runtime::RuntimeDispatchSource &Source() const noexcept {
            return source_;
        }

        Result<void> Startup(const CancellationToken &token) override {
            if (closed_ || started_ || token.IsCancellationRequested())
                return Rejected();
            if (source_.BindingStatus() != Runtime::RuntimeDispatchStatus::Valid)
                return Rejected();
            started_ = true;
            return Result<void>::Success();
        }

        Result<void> OnPhase(Runtime::RuntimePhase phase, const Runtime::FrameContext &frame) override {
            if (!OnOwner())
                return Rejected();
            DrainResolved();
            if (phase == Runtime::RuntimePhase::CommitDeferredLifecycleChanges && owners_->configured &&
                (owners_->configured->initialSceneState ||
                 (owners_->configured->mountedWorld && owners_->configured->mountedWorld->liveCell)))
                sceneDispatch_ = frame.dispatchEvidence;
            return closed_ ? Rejected() : Result<void>::Success();
        }

        /** @brief Borrows current structural evidence for the paired generation-owned Scene services. */
        const Runtime::RuntimeDispatchEvidence &SceneDispatch() const noexcept {
            return sceneDispatch_;
        }

        Result<void> OnFixedUpdate(const Runtime::FixedStepContext &context) override {
            if (!Valid(context) || closed_ || !started_ || attempting_)
                return Rejected();
            if (auto captured = CapturePins(); captured.HasError())
                return captured;
            // Catch-up ticks may be consecutive: discard only duplicate references, never the lifetime pins here.
            evaluated_ = {};
            count_ = 0;
            gameplay_ = {};
            gameplayCount_ = 0;
            evidence_ = context.dispatchEvidence;
            attempting_ = true;
            return Result<void>::Success();
        }

        Result<void> Evaluate(const std::shared_ptr<WorldRecord> &record, const Runtime::FixedStepContext &context) {
            if (!Valid(context) || !Same(context) || closed_)
                return Rejected();
            if (!attempting_ || !record || record->closeRequested || !record->animation)
                return Rejected();
            if (!CanEvaluate(record))
                return Rejected();
            evaluated_[count_++] = record;  // Before the fallible call: partial actor evaluation also needs rollback.
            return record->animation->OnFixedUpdate(context);
        }

        /** @brief Arms only the current opaque fixed dispatch for the configured standalone World. */
        void ArmDecisionReplay(WorldRecord &record, const Runtime::FixedStepContext &context) {
            if (auto *replay = DecisionReplay(); replay && replay->Selection().tick == context.simulationTick && attempting_ && !closed_ &&
                                                 Valid(context) && Same(context) && std::ranges::any_of(pins_, [&record](const auto &pin) {
                return pin.get() == &record;
            }))
                replay->Arm(record, context);
        }

        /** @brief Tracks the actual attempted gameplay owner before its fallible callback; unrelated pinned Worlds stay untouched. */
        Result<void> TrackGameplay(const std::shared_ptr<WorldRecord> &record, const Runtime::FixedStepContext &context) {
            if (!Valid(context) || !Same(context) || !attempting_ || closed_)
                return Rejected();
            if (!record || record->closeRequested || !record->activeGameplay || !CanTrackGameplay(record))
                return Rejected();
            gameplay_[gameplayCount_++] = record;
            return Result<void>::Success();
        }

        Result<void> PrepareFixedCommit(const Runtime::FixedStepContext &context) override {
            if (!Valid(context) || !Same(context) || !attempting_ || closed_)
                return Rejected();
            for (std::size_t index = 0; index < count_; ++index) {
                const auto &record = evaluated_[index];
                if (record->closeRequested)
                    return Rejected();
                auto prepared = record->animation->PrepareFixedCommit(context);
                if (prepared.HasError())
                    return prepared;
            }
            if (auto prepared = PrepareGameplay(context); prepared.HasError())
                return prepared;
            if (auto *replay = DecisionReplay()) {
                for (const auto &record : pins_)
                    if (record)
                        replay->Prepare(*record, context);
            }
            return Result<void>::Success();
        }

        void CommitFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (!Valid(context) || !Same(context) || !attempting_)
                std::terminate();
            Runtime::RuntimeDispatchFacts facts;
            if (context.dispatchEvidence.Read(source_, Runtime::RuntimePhase::FixedUpdate, facts) != Runtime::RuntimeDispatchStatus::Valid)
                std::terminate();
            if (facts.committedTick != facts.fixedTick || facts.committedAttempt != facts.fixedAttempt)
                std::terminate();
            for (std::size_t index = 0; index < count_; ++index)
                evaluated_[index]->animation->CommitFixedUpdate(context);
            CommitGameplay(context);
            if (auto *replay = DecisionReplay())
                replay->Commit(context);
            Resolve();  // Keep every strong pin until a safe phase/Shutdown, never cold teardown in nofail publication.
        }

        void AbortFixedUpdate(const Runtime::FixedStepContext &context) noexcept override {
            if (!OnOwner() || !attempting_ || !Same(context))
                return;
            Runtime::RuntimeDispatchFacts facts;
            if (const auto status = context.dispatchEvidence.Read(source_, Runtime::RuntimePhase::FixedUpdate, facts);
                status != Runtime::RuntimeDispatchStatus::Valid && status != Runtime::RuntimeDispatchStatus::Retired)
                return;
            AbortGameplay(context);
            if (auto *replay = DecisionReplay())
                replay->Abort(context);
            for (std::size_t index = count_; index > 0; --index)
                evaluated_[index - 1]->animation->AbortFixedUpdate(context);
            Resolve();
        }

        void Shutdown() noexcept override {
            if (!OnOwner())
                return;
            closed_ = true;
            if (attempting_)
                return;
            DrainResolved();
            for (const auto &known : owners_->known)
                if (auto record = known.lock())
                    CloseRecord(*record);
        }

    private:
        /** @brief Reads only the optional configured diagnostic mailbox. */
        HeadlessDecisionReplay *DecisionReplay() const noexcept {
            return owners_->configured ? owners_->configured->decisionReplay.get() : nullptr;
        }

        static Result<void> Rejected() {
            return Result<void>::Failure(MakeError(Animation::AnimationErrors::GraphAdmissionRejected));
        }

        bool OnOwner() const noexcept {
            return std::this_thread::get_id() == owners_->owner;
        }

        bool Same(const Runtime::FixedStepContext &context) const noexcept {
            return evidence_.RefersToSameDispatch(context.dispatchEvidence);
        }

        bool Valid(const Runtime::FixedStepContext &context) const noexcept {
            if (!OnOwner())
                return false;
            Runtime::RuntimeDispatchFacts facts;
            if (context.dispatchEvidence.Read(source_, Runtime::RuntimePhase::FixedUpdate, facts) != Runtime::RuntimeDispatchStatus::Valid)
                return false;
            return facts.fixedTick == context.simulationTick && facts.fixedAttempt == context.attemptNumber &&
                   facts.frame == context.frameNumber && facts.fixedDuration == context.fixedDelta;
        }

        /** @brief Uses only actual attempted gameplay owners; animation presence never gates source commitment. */
        Result<void> PrepareGameplay(const Runtime::FixedStepContext &context) {
            for (std::size_t index = 0; index < gameplayCount_; ++index) {
                const auto &record = gameplay_[index];
                if (record->closeRequested || !record->activeGameplay)
                    return Rejected();
                if (auto prepared = record->activeGameplay->PrepareFixedCommit(context); prepared.HasError())
                    return prepared;
            }
            return Result<void>::Success();
        }

        void CommitGameplay(const Runtime::FixedStepContext &context) noexcept {
            for (std::size_t index = 0; index < gameplayCount_; ++index) {
                const auto &record = gameplay_[index];
                record->activeGameplay->CommitFixedUpdate(context);
                if (record->publicTransforms && !record->sceneCancellation.IsCancellationRequested()) {
                    if (const auto scene = record->scenes->ActiveScene())
                        static_cast<void>(record->publicTransforms->ObserveCommittedTick(*scene, context.dispatchEvidence));
                }
            }
        }

        void AbortGameplay(const Runtime::FixedStepContext &context) noexcept {
            for (std::size_t index = gameplayCount_; index > 0; --index) {
                const auto &record = gameplay_[index - 1];
                if (record->activeGameplay)
                    record->activeGameplay->AbortFixedUpdate(context);
            }
        }

        bool CanTrackGameplay(const std::shared_ptr<WorldRecord> &record) const noexcept {
            if (std::ranges::find(pins_, record) == pins_.end() || gameplayCount_ == gameplay_.size())
                return false;
            const auto end = gameplay_.begin() + gameplayCount_;
            return std::ranges::find(gameplay_.begin(), end, record) == end;
        }

        Result<void> CapturePins() {
            for (std::size_t index = 0; index < pins_.size(); ++index) {
                auto record = owners_->known[index].lock();
                if (pinsHeld_) {
                    if (record != pins_[index])
                        return Rejected();
                } else {
                    pins_[index] = std::move(record);
                    if (pins_[index])
                        pins_[index]->fixedReserved = true;
                }
            }
            pinsHeld_ = true;
            return Result<void>::Success();
        }

        bool CanEvaluate(const std::shared_ptr<WorldRecord> &record) const noexcept {
            if (std::ranges::find(pins_, record) == pins_.end() || count_ == evaluated_.size())
                return false;
            return std::ranges::find(evaluated_.begin(), evaluated_.begin() + count_, record) == evaluated_.begin() + count_;
        }

        void Resolve() noexcept {
            attempting_ = false;
            evidence_ = {};
            // Record reservation lasts until safe DrainResolved, including the gap after this decision.
        }

        void DrainResolved() noexcept {
            if (attempting_)
                return;
            for (const auto &pin : pins_) {
                if (pin) {
                    pin->fixedReserved = false;
                    if (pin->closeRequested)
                        CloseRecord(*pin);
                }
            }
            evaluated_ = {};
            count_ = 0;
            gameplay_ = {};
            gameplayCount_ = 0;
            pins_ = {};
            pinsHeld_ = false;
        }

        std::shared_ptr<WorldOwners> owners_;
        Runtime::RuntimeDispatchSource source_;
        Runtime::RuntimeDispatchEvidence evidence_;
        Runtime::RuntimeDispatchEvidence sceneDispatch_;
        std::array<std::shared_ptr<WorldRecord>, 4> pins_;
        std::array<std::shared_ptr<WorldRecord>, 4> evaluated_;
        std::size_t count_{};
        std::array<std::shared_ptr<WorldRecord>, 4> gameplay_;
        std::size_t gameplayCount_{};
        bool pinsHeld_{};
        bool started_{};
        bool attempting_{};
        bool closed_{};
    };

    /** @brief Binds the owner-issued animation identity to this configured Scene before startup. */
    inline Result<void> PrepareConfiguredAnimation(WorldOwners &owners, WorldRecord &record) {
        const auto &input = *owners.configured;
        if (!input.animationIdentities)
            return Result<void>::Success();
        if (!owners.fixed)
            return Result<void>::Failure(MakeError(Animation::AnimationErrors::GraphAdmissionRejected));
        const auto issued = input.animationIdentities->Issue();
        if (!issued.HasValue())
            return Result<void>::Failure(MakeError(Animation::AnimationErrors::GenerationExhausted));
        Runtime::SceneAnimationRuntimeConfig config;
        config.expectedDispatch = owners.fixed->Source();
        config.runtimeOwner = issued.identity;
        config.target = input.animationTarget;
        config.ownerCancellation = record.sceneCancellation;
        if (record.request.world == Network::NetworkModeWorldKind::Client) {
            config.evaluationDomain = Animation::AnimationEvaluationDomain::Presentation;
            if (!input.replicaAnimationBindings.empty())
                record.replicaAnimation = std::make_shared<ReplicaAnimationInputs>(input.replicaAnimationBindings);
            config.inputs = record.replicaAnimation ? record.replicaAnimation : input.replicaAnimationInputs;
        } else {
            config.inputs = input.animationInputs;
        }
        auto adapter = Runtime::SceneAnimationRuntime::Create(config);
        if (adapter.HasError())
            return Result<void>::Failure(std::move(adapter).ErrorValue());
        record.animation = std::move(adapter).Value();
        if (auto added = record.scenes->AddActivationParticipant(record.animation->MakeActivationParticipant()); added.HasError())
            return added;
        if (auto added = record.scenes->AddStructuralParticipant(record.animation->MakeStructuralParticipant()); added.HasError())
            return added;
        return record.animation->Startup(record.sceneCancellation);
    }

}  // namespace Horo::Application::Internal::HeadlessNetworkDetail
