#include "EditorPlaySaveHostRestore.h"

#include <type_traits>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;

        /** @brief Owns the original packet only after actual Scene queue admission, never a borrowed callback reference. */
        struct PacketControl final {
            std::shared_ptr<PlaySaveSceneHandoffState> state;
            std::optional<RuntimeSavePreparedRestore> packet;
            SceneDefinitionId definition;
            SceneDefinitionRevision revision;
            std::shared_ptr<PlaySaveRestoreSource> source;
            std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> behaviorOwner;
            SceneRestoreBundle *bundle{}; /**< Borrowed only by the source actually owned
                                             inside this bundle. */
            SceneRuntimeId candidate;
        };

        /** @brief Preserves canonical cancellation evidence without assuming an optional snapshot exists. */
        Error CancelledPacketError(const SaveOperationHandle &operation) {
            const auto observed = operation.Snapshot();
            if (observed && observed->terminalError)
                return *observed->terminalError;
            return MakeError(SaveErrors::OperationCancelled);
        }

        /** @brief Supplies the sole original producer and receipts to SceneRestoreBundle exactly once. */
        class PacketSource final : public ISceneRestoreOwnerSource {
        public:
            explicit PacketSource(std::shared_ptr<PacketControl> control) : control_(std::move(control)) {}

            /** @copydoc ISceneRestoreOwnerSource::TakeReady */
            Result<std::optional<SceneRestorePreparedOwners>> TakeReady(RuntimeSceneView candidate) override {
                using Outcome = Result<std::optional<SceneRestorePreparedOwners>>;
                if (!control_->packet)
                    return Outcome::Success({});
                auto &packet = *control_->packet;
                if (!candidate.IsCurrent() || candidate.DefinitionId() != control_->definition ||
                    candidate.DefinitionRevision() != control_->revision || !packet.operation.Handle().IsValid())
                    return Outcome::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                if (packet.operation.ObserveCancellation() == SaveCancellationObservation::Cancelled)
                    return Outcome::Failure(CancelledPacketError(packet.operation.Handle()));
                auto owners = PrepareActualOwners(candidate, packet.participants);
                if (owners.HasError())
                    return Outcome::Failure(owners.ErrorValue());
                auto staged = std::move(owners).Value();
                SceneRestorePreparedOwners ready{std::move(packet.operation), std::move(staged.participants)};
                // The real bundle already owns registry/context/entities; receipts own
                // their actual admitted native/source leases.
                control_->packet.reset();
                control_->source.reset();  // Actual receipts now own all admitted native
                                           // preparation/source leases.
                return Outcome::Success(std::optional{std::move(ready)});
            }

            /** @copydoc ISceneRestoreOwnerSource::Cancel */
            void Cancel() noexcept override {
                control_->state->retired = true;
                if (!control_->packet || !control_->packet->operation.Handle().IsValid())
                    return;
                try {
                    static_cast<void>(control_->packet->operation.RequestShutdownCancellation());
                    static_cast<void>(control_->packet->operation.ObserveCancellation());
                } catch (...) {
                    // The canonical producer destructor publishes abandonment if a terminal
                    // observer itself violates its boundary.
                    control_->packet.reset();
                }
            }

        private:
            /** @brief Creates only actual candidate-bound receipts using this bundle's
             * one identity map. */
            Result<RuntimeSaveRestoreStaging> PrepareActualOwners(RuntimeSceneView candidate,
                                                                  const SaveParticipantRegistrySnapshot &participants) {
                using Outcome = Result<RuntimeSaveRestoreStaging>;
                const auto *identities = control_->bundle ? control_->bundle->Identities() : nullptr;
                if (!identities || !control_->source || candidate.RuntimeId() != control_->candidate)
                    return Outcome::Failure(MakeError(SaveErrors::RestoreContextInvalid));
                auto gameplay = control_->behaviorOwner ? control_->behaviorOwner->Prepared(candidate.RuntimeId()) : nullptr;
                if (control_->behaviorOwner && !gameplay)
                    return Outcome::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                return control_->source->StageOwners(participants, std::move(gameplay), *identities);
            }

            std::shared_ptr<PacketControl> control_;
        };

        /** @brief Observes actual aggregate publication/rollback while keeping SceneRestoreBundle as the only commit gate. */
        class ObservedRestore final : public SceneAggregateRestore, public ISceneRestoreTransientRetirement {
        public:
            ObservedRestore(std::unique_ptr<SceneRestoreBundle> bundle, std::shared_ptr<PacketControl> control)
                : bundle_(std::move(bundle)), state_(control->state), control_(std::move(control)) {}

            ~ObservedRestore() override {
                Rollback();
            }

            /** @copydoc SceneAggregateRestore::PrepareScene */
            Result<void> PrepareScene(RuntimeScene &scene) override {
                const auto prepared = bundle_->PrepareScene(scene);
                if (prepared.HasError())
                    return prepared;
                control_->candidate = scene.View().RuntimeId();
                if (!control_->behaviorOwner)
                    return Result<void>::Success();
                // Candidate-specific native clients are explicitly withheld by this
                // profile, never borrowed from the active world.
                const auto gameplay = control_->behaviorOwner->PrepareRestoreCandidate(scene, {});
                return gameplay.HasError() ? Result<void>::Failure(gameplay.ErrorValue()) : Result<void>::Success();
            }

            /** @copydoc SceneAggregateRestore::PrepareOwners */
            Result<bool> PrepareOwners() override {
                return bundle_->PrepareOwners();
            }

            /** @copydoc SceneAggregateRestore::ValidatePublication */
            Result<void> ValidatePublication(const RuntimeSceneView *active) const override {
                return bundle_->ValidatePublication(active);
            }

            /** @copydoc SceneAggregateRestore::Commit */
            Result<void> Commit(IStagedRestoreAggregatePublication &transfer) override {
                const auto committed = bundle_->Commit(transfer);
                if (committed.HasValue())
                    state_->published = true;
                return committed;
            }

            /** @copydoc ISceneRestoreTransientRetirement::RetirePublishedTransient */
            Result<bool> RetirePublishedTransient() override {
                return bundle_->RetirePublishedTransient();
            }

            /** @copydoc SceneAggregateRestore::Rollback */
            void Rollback() noexcept override {
                bundle_->Rollback();
                if (control_->behaviorOwner)
                    control_->behaviorOwner->CancelPrepared(control_->candidate);
                state_->retired = true;
            }

        private:
            std::unique_ptr<SceneRestoreBundle> bundle_;
            std::shared_ptr<PlaySaveSceneHandoffState> state_;
            std::shared_ptr<PacketControl> control_;
        };

        /** @brief Rechecks actual live host generations rather than echoing saved/source metadata into publication authority. */
        bool CurrentAuthority(const ISceneRestoreGenerationAuthority &authority, const StagedRestoreContext &context) noexcept {
            const auto current = authority.Current();
            return current.registryGeneration == context.registryGeneration && current.sessionGeneration == context.sessionGeneration &&
                   current.sceneIncarnation == context.sceneIncarnation;
        }

        /** @brief Rejects foreign packets and unrelated resolvers before changing any Scene or producer ownership. */
        Result<void> AdmitHandoff(const std::shared_ptr<PlaySaveRestoreSource> &source,
                                  const std::shared_ptr<const ISceneRestoreGenerationAuthority> &authority,
                                  const std::shared_ptr<PlaySaveSceneHandoffState> &state, const RuntimeSavePreparedRestore &prepared) {
            if (!source || !authority || !state || state->transferred || prepared.sourceLease.get() != source.get())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
            if (prepared.staging.references)
                return Result<void>::Failure(
                    MakeError(SaveErrors::CompositionUnsupported, "This Scene restore cannot discard an unrelated reference resolver."));
            if (!CurrentAuthority(*authority, prepared.context))
                return Result<void>::Failure(MakeError(SaveErrors::RestoreActivationStale));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PlaySaveSceneHandoff::PlaySaveSceneHandoff */
    PlaySaveSceneHandoff::PlaySaveSceneHandoff(Runtime::RuntimeSceneService &scenes, std::shared_ptr<PlaySaveRestoreSource> source,
                                               std::shared_ptr<const Runtime::ISceneRestoreGenerationAuthority> authority,
                                               std::shared_ptr<PlaySaveSceneHandoffState> state, Runtime::SaveWorldId world)
        : scenes_(scenes), source_(std::move(source)), authority_(std::move(authority)), state_(std::move(state)), world_(world) {}

    /** @copydoc PlaySaveSceneHandoff::Accept */
    Result<void> PlaySaveSceneHandoff::Accept(Runtime::RuntimeSavePreparedRestore &prepared) {
        if (const auto admitted = AdmitHandoff(source_, authority_, state_, prepared); admitted.HasError())
            return admitted;
        const auto decoded = source_->PreparedScene();
        if (!decoded)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        auto control = std::make_shared<PacketControl>(PacketControl{state_, {}, decoded->definition.Id(), decoded->definition.Revision()});
        control->source = source_;
        control->behaviorOwner = source_->BehaviorOwner();
        Runtime::SceneRestoreBundleRequest request{.context = prepared.context,
                                                   .world = world_,
                                                   .definition = decoded->definition.Id(),
                                                   .revision = decoded->definition.Revision(),
                                                   .participants = prepared.participants,
                                                   .entities = decoded->identities,
                                                   .maximumEntities = 16'384,
                                                   .maximumComponentBytes = PlaySaveMaximumPayloadBytes};
        auto bundle = Runtime::SceneRestoreBundle::Create(std::move(request), std::make_unique<PacketSource>(control), authority_);
        if (bundle.HasError())
            return Result<void>::Failure(bundle.ErrorValue());
        control->bundle = bundle.Value().get();
        auto observed = std::make_unique<ObservedRestore>(std::move(bundle).Value(), control);
        if (const auto queued = scenes_.QueuePreparationWithRestore(decoded->definition, std::move(observed)); queued.HasError())
            return queued;
        if (state_->retired)
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        static_assert(std::is_nothrow_move_constructible_v<Runtime::RuntimeSavePreparedRestore>);
        // QueuePreparation performs candidate preparation only; owners/TakeReady run
        // at the next exclusive Scene safe point.
        control->packet.emplace(std::move(prepared));
        state_->operation = control->packet->operation.Handle();
        state_->transferred = true;
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveSceneHandoff::Cancel */
    void PlaySaveSceneHandoff::Cancel() noexcept {
        if (state_ && state_->operation.IsValid())
            static_cast<void>(state_->operation.RequestCancellation());
    }

    /** @copydoc PlaySaveSceneHandoff::IsSettled */
    bool PlaySaveSceneHandoff::IsSettled() const noexcept {
        return !state_ || !state_->transferred || state_->published || state_->retired;
    }
}  // namespace Horo::Application::Internal
