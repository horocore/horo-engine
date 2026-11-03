#include "EditorPlaySaveFailurePublication.h"
#include "EditorPlaySaveHostRestore.h"
#include "EditorPlaySavePrefabServices.h"

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
            std::shared_ptr<PlaySavePrefabServices> prefabs;
            std::shared_ptr<Prefab::PrefabRestoreEntities> dynamic;
            std::shared_ptr<const PlaySaveSceneState> decoded;
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

            Result<void> ValidateRestorePartitions(std::span<const SceneRestoreGroupPartition> partitions) const override {
                return bundle_->ValidateRestorePartitions(partitions);
            }

            SceneRestoreStructuralLimits StructuralLimits() const noexcept override {
                return bundle_->StructuralLimits();
            }

            /** @copydoc SceneAggregateRestore::PrepareScene */
            Result<void> PrepareScene(RuntimeScene &scene) override {
                auto prepared = bundle_->PrepareScene(scene);
                if (prepared.HasError())
                    return RecordFailure(std::move(prepared));
                return RecordFailure(PrepareGameplay(scene));
            }

            Result<void> PrepareScene(RuntimeScene &scene, SceneRestoreCandidateCommands &commands) override {
                auto prepared = bundle_->PrepareScene(scene, commands);
                if (prepared.HasError())
                    return RecordFailure(std::move(prepared));
                return RecordFailure(PrepareGameplay(scene));
            }

            Result<void> PrepareGameplay(RuntimeScene &scene) {
                control_->candidate = scene.View().RuntimeId();
                std::shared_ptr<const Gameplay::GameplayPrefabContext> prefabs;
                if (control_->dynamic && control_->prefabs) {
                    auto services = control_->prefabs->PrepareRestored(scene, *control_->decoded->prefabs, *control_->dynamic,
                                                                       control_->prefabs->Tick());
                    if (services.HasError())
                        return Result<void>::Failure(services.ErrorValue());
                    prefabs = std::move(services).Value();
                }
                if (!control_->behaviorOwner)
                    return Result<void>::Success();
                const auto gameplay = control_->behaviorOwner->PrepareRestoreCandidate(scene, {}, std::move(prefabs));
                return gameplay.HasError() ? Result<void>::Failure(gameplay.ErrorValue()) : Result<void>::Success();
            }

            /** @copydoc SceneAggregateRestore::PrepareOwners */
            Result<bool> PrepareOwners() override {
                return RecordFailure(bundle_->PrepareOwners());
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
                if (!state_->published && control_->prefabs)
                    control_->prefabs->CancelPrepared(control_->candidate);
                state_->retired = true;
            }

        private:
            /** @brief Preserve the actual failure before rollback cancellation only while this control still owns the original producer. */
            template <typename T> Result<T> RecordFailure(Result<T> result) {
                if (control_->packet)
                    return PreservePlaySaveFailure(control_->packet->operation, std::move(result));
                return result;
            }

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

    struct PlaySaveSceneHandoff::Pending final {
        std::shared_ptr<PacketControl> control;
        std::vector<std::optional<Prefab::PrefabTemplateLoadHandle>> loads;
        std::vector<Prefab::PrefabTemplateLease> leases;
        CancellationSource cancellation;
        std::size_t next{};
        bool draining{};
        Error allocationFailure{MakeError(SaveErrors::RestoreAllocationFailed)};
        Error callbackFailure{MakeError(SaveErrors::RestoreAdapterContractInvalid)};
    };

    /** @copydoc PlaySaveSceneHandoff::PlaySaveSceneHandoff */
    PlaySaveSceneHandoff::PlaySaveSceneHandoff(Runtime::RuntimeSceneService &scenes, std::shared_ptr<PlaySaveRestoreSource> source,
                                               std::shared_ptr<const Runtime::ISceneRestoreGenerationAuthority> authority,
                                               std::shared_ptr<PlaySaveSceneHandoffState> state, Runtime::SaveWorldId world,
                                               std::shared_ptr<PlaySavePrefabServices> prefabs)
        : scenes_(scenes), source_(std::move(source)), authority_(std::move(authority)), state_(std::move(state)), world_(world),
          prefabs_(std::move(prefabs)) {}

    PlaySaveSceneHandoff::~PlaySaveSceneHandoff() = default;

    /** @copydoc PlaySaveSceneHandoff::Accept */
    Result<void> PlaySaveSceneHandoff::Accept(Runtime::RuntimeSavePreparedRestore &prepared) {
        if (const auto admitted = AdmitHandoff(source_, authority_, state_, prepared); admitted.HasError())
            return admitted;
        const auto decoded = source_->PreparedScene();
        if (!decoded || (decoded->prefabs && !prefabs_))
            return Result<void>::Failure(MakeError(Runtime::SaveErrors::RestoreTransitionInvalid));
        auto pending = std::make_unique<Pending>();
        pending->control =
            std::make_shared<PacketControl>(PacketControl{state_, {}, decoded->definition.Id(), decoded->definition.Revision()});
        pending->control->source = source_;
        pending->control->behaviorOwner = source_->BehaviorOwner();
        pending->control->prefabs = prefabs_;
        pending->control->decoded = decoded;
        const auto count = decoded->prefabs ? decoded->prefabs->instances.size() : 0;
        pending->loads.resize(count);
        pending->leases.resize(count);
        static_assert(std::is_nothrow_move_constructible_v<Runtime::RuntimeSavePreparedRestore>);
        pending->control->packet.emplace(std::move(prepared));
        state_->operation = pending->control->packet->operation.Handle();
        state_->transferred = true;
        pending_ = std::move(pending);
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveSceneHandoff::RejectPending */
    void PlaySaveSceneHandoff::RejectPending(Error error) {
        if (!pending_ || pending_->draining)
            return;
        pending_->draining = true;
        pending_->cancellation.RequestCancellation();
        for (const auto &load : pending_->loads)
            if (load)
                load->RequestCancel();
        if (pending_->control->packet)
            static_cast<void>(pending_->control->packet->operation.Fail(std::move(error), SaveOperationCommitOutcome::NotCommitted));
    }

    /** @copydoc PlaySaveSceneHandoff::QueueReady */
    Result<void> PlaySaveSceneHandoff::QueueReady() {
        auto &pending = *pending_;
        auto control = pending.control;
        auto &packet = *control->packet;
        const auto &decoded = *control->decoded;
        if (!CurrentAuthority(*authority_, packet.context))
            return Result<void>::Failure(MakeError(SaveErrors::RestoreActivationStale));
        if (decoded.prefabs) {
            auto adapter = prefabs_->PrepareEntities(*decoded.prefabs, std::move(pending.leases), pending.cancellation.Token());
            if (adapter.HasError())
                return Result<void>::Failure(adapter.ErrorValue());
            control->dynamic = std::move(adapter).Value();
        }
        Runtime::SceneRestoreBundleRequest request{.context = packet.context,
                                                   .world = world_,
                                                   .definition = decoded.definition.Id(),
                                                   .revision = decoded.definition.Revision(),
                                                   .participants = packet.participants,
                                                   .entities = decoded.identities,
                                                   .maximumEntities = 16'384,
                                                   .maximumComponentBytes = PlaySaveMaximumPayloadBytes,
                                                   .dynamicEntities = control->dynamic};
        if (decoded.prefabs)
            for (const auto &instance : decoded.prefabs->instances)
                for (const auto &member : instance.members)
                    request.entities.push_back(member.record);
        auto bundle = Runtime::SceneRestoreBundle::Create(std::move(request), std::make_unique<PacketSource>(control), authority_);
        if (bundle.HasError())
            return Result<void>::Failure(bundle.ErrorValue());
        control->bundle = bundle.Value().get();
        auto observed = std::make_unique<ObservedRestore>(std::move(bundle).Value(), control);
        return scenes_.QueuePreparationWithRestore(decoded.definition, std::move(observed));
    }

    /** @copydoc PlaySaveSceneHandoff::Advance */
    Result<void> PlaySaveSceneHandoff::Advance() {
        try {
            return AdvanceInternal();
        } catch (const std::bad_alloc &) {
            if (pending_ && !pending_->draining)
                RejectPending(std::move(pending_->allocationFailure));
        } catch (...) {
            if (pending_ && !pending_->draining)
                RejectPending(std::move(pending_->callbackFailure));
        }
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveSceneHandoff::AdvanceInternal */
    Result<void> PlaySaveSceneHandoff::AdvanceInternal() {
        if (!pending_)
            return Result<void>::Success();
        auto &pending = *pending_;
        ObservePending();
        std::size_t active = ConsumeLoads();
        if (pending.draining) {
            if (!active) {
                state_->retired = true;
                pending_.reset();
            }
            return Result<void>::Success();
        }
        SubmitLoads(active);
        if (pending.draining)
            return Result<void>::Success();
        if (pending.next != pending.loads.size() || active)
            return Result<void>::Success();
        auto queued = QueueReady();
        if (queued.HasError())
            RejectPending(queued.ErrorValue());
        else
            pending_.reset();  // Scene-owned PacketSource now retains the sole original packet and all source/lease roots.
        return Result<void>::Success();
    }

    /** @copydoc PlaySaveSceneHandoff::ObservePending */
    void PlaySaveSceneHandoff::ObservePending() {
        auto &pending = *pending_;
        if (!pending.draining && pending.control->packet->operation.ObserveCancellation() == SaveCancellationObservation::Cancelled)
            RejectPending(CancelledPacketError(state_->operation));
        if (prefabs_)
            if (auto advanced = prefabs_->Provider().Advance(); advanced.HasError())
                RejectPending(advanced.ErrorValue());
    }

    /** @copydoc PlaySaveSceneHandoff::ConsumeLoads */
    std::size_t PlaySaveSceneHandoff::ConsumeLoads() {
        auto &pending = *pending_;
        std::size_t active{};
        for (std::size_t index = 0; index < pending.loads.size(); ++index) {
            auto &load = pending.loads[index];
            if (!load)
                continue;
            if (load->State() == Prefab::PrefabTemplateLoadState::Loading) {
                ++active;
                continue;
            }
            if (!pending.draining) {
                auto lease = prefabs_->Provider().TakeResult(*load);
                if (lease.HasError())
                    RejectPending(lease.ErrorValue());
                else
                    pending.leases[index] = std::move(lease).Value();
            }
            load.reset();
        }
        return active;
    }

    /** @copydoc PlaySaveSceneHandoff::SubmitLoads */
    void PlaySaveSceneHandoff::SubmitLoads(std::size_t &active) {
        auto &pending = *pending_;
        while (pending.next < pending.loads.size() && active < 8) {
            const auto index = pending.next++;
            const auto &recipe = pending.control->decoded->prefabs->instances[index];
            if (recipe.disposition == PersistentEntityDisposition::Tombstone)
                continue;
            auto loaded = prefabs_->Provider().LoadAsync(recipe.source, pending.cancellation.Token());
            if (loaded.HasError()) {
                RejectPending(loaded.ErrorValue());
                return;
            }
            pending.loads[index].emplace(std::move(loaded).Value());
            ++active;
        }
    }

    /** @copydoc PlaySaveSceneHandoff::Cancel */
    void PlaySaveSceneHandoff::Cancel() noexcept {
        if (state_ && state_->operation.IsValid())
            static_cast<void>(state_->operation.RequestCancellation());
        if (pending_) {
            pending_->cancellation.RequestCancellation();
            for (const auto &load : pending_->loads)
                if (load)
                    load->RequestCancel();
        }
    }

    /** @copydoc PlaySaveSceneHandoff::IsSettled */
    bool PlaySaveSceneHandoff::IsSettled() const noexcept {
        return !pending_ && (!state_ || !state_->transferred || state_->published || state_->retired);
    }
}  // namespace Horo::Application::Internal
