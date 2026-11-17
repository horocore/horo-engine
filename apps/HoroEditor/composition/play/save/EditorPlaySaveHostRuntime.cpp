#include "EditorPlaySaveHostInternal.h"
#include "EditorPlaySaveIdentityIssuing.h"
#include "EditorPlaySavePrefabCapture.h"

#include <algorithm>
#include <array>
#include <chrono>

namespace Horo::Application::Internal {
    namespace {
        using namespace Runtime;

        /** @brief Worker-owned writer; capture bytes, scope and display are detached before actual publication. */
        class PlaySaveWriter final : public IRuntimeSaveWriter {
        public:
            PlaySaveWriter(std::shared_ptr<PlaySaveStorage> storage, std::string display, std::uint64_t utcMilliseconds,
                           std::uint64_t playNanoseconds)
                : storage_(std::move(storage)), display_(std::move(display)), utc_(utcMilliseconds), play_(playNanoseconds) {}

            /** @copydoc IRuntimeSaveWriter::Prepare */
            Result<void> Prepare(const RuntimeSaveSnapshot &snapshot, const CancellationToken &cancellation) override {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(SaveErrors::OperationCancelled));
                auto target = storage_->NewSlot();
                if (target.HasError())
                    return Result<void>::Failure(target.ErrorValue());
                target_.emplace(std::move(target).Value());
                const auto header = storage_->Header(*target_, utc_, play_);
                if (header.HasError())
                    return Result<void>::Failure(header.ErrorValue());
                auto archive = BuildPlaySaveArchive(snapshot, header.Value());
                if (archive.HasError())
                    return Result<void>::Failure(archive.ErrorValue());
                archive_.emplace(std::move(archive).Value());
                return Result<void>::Success();
            }

            /** @copydoc IRuntimeSaveWriter::Commit */
            Result<void> Commit() override {
                if (!target_ || !archive_)
                    return Result<void>::Failure(MakeError(SaveErrors::RestoreTransitionInvalid));
                const auto published = storage_->Commit(*target_, *archive_, {.displayName = display_});
                return published.HasError() ? Result<void>::Failure(published.ErrorValue()) : Result<void>::Success();
            }

        private:
            std::shared_ptr<PlaySaveStorage> storage_;
            std::string display_;
            std::uint64_t utc_{};
            std::uint64_t play_{};
            std::optional<SaveSlotLifecycleTarget> target_;
            std::optional<FinalizedSaveArchive> archive_;
        };

        /** @brief Maps only actual canonical terminal state into the existing process diagnostics store. */
        OperationState PresentationState(SaveOperationState state) noexcept {
            switch (state) {
                case SaveOperationState::Completed:
                    return OperationState::Succeeded;
                case SaveOperationState::Failed:
                    return OperationState::Failed;
                case SaveOperationState::Cancelled:
                    return OperationState::Cancelled;
                default:
                    return OperationState::Running;
            }
        }

        /** @brief Rebinds the actual installed native source to the same current inventory inside the real capture barrier. */
        class GameplayCapture final : public ICanonicalStateAdapter {
        public:
            using Identities = std::function<Result<PersistentEntityIdentityMap>()>;

            GameplayCapture(std::shared_ptr<const Editor::PlaySaveGameplayBinding> binding,
                            std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner, Identities identities)
                : binding_(std::move(binding)), owner_(std::move(owner)), identities_(std::move(identities)) {}

            Result<CanonicalCaptureDisposition> Capture(const CanonicalCaptureContext &context,
                                                        ICanonicalCaptureSink &sink) const override {
                using Outcome = Result<CanonicalCaptureDisposition>;
                const auto *type = std::get_if<Gameplay::BehaviorTypeId>(&binding_->declaration.ownerIdentity);
                if (!type || !owner_)
                    return Outcome::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
                auto identities = identities_();
                if (identities.HasError())
                    return Outcome::Failure(identities.ErrorValue());
                auto target = owner_->BindingForActive(*type, identities.Value());
                if (target.HasError())
                    return Outcome::Failure(target.ErrorValue());
                auto installed = binding_->adapter->BindRestoreCandidate(std::move(target).Value());
                if (installed.HasError())
                    return Outcome::Failure(installed.ErrorValue());
                if (!binding_->installation.SameGeneration(installed.Value()))
                    return Outcome::Failure(MakeError(SaveErrors::CaptureRegistryStale));
                auto actual = installed.Value().AcquireAdapter();
                if (!actual)
                    return Outcome::Failure(MakeError(SaveErrors::ParticipantAdapterMissing));
                return actual->Capture(context, sink);
            }

        private:
            /** @brief Aliases the exact immutable host binding owner, preserving its unique installation and native pin. */
            std::shared_ptr<const Editor::PlaySaveGameplayBinding> binding_;
            std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner_;
            Identities identities_;
        };

        /** @brief Registers every installed owner under its frozen descriptor without inventing sources or authoritative IDs. */
        Result<void> RegisterGameplay(CanonicalStateParticipantRegistry &registry,
                                      const std::shared_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> &bindings,
                                      std::shared_ptr<Gameplay::BehaviorRuntimeSceneOwner> owner, GameplayCapture::Identities identities) {
            if (!bindings)
                return Result<void>::Success();
            for (const auto &binding : *bindings) {
                auto adapter = binding.installation.AcquireAdapter();
                if (!adapter)
                    return Result<void>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
                if (std::holds_alternative<Gameplay::BehaviorTypeId>(binding.declaration.ownerIdentity))
                    adapter = std::make_shared<GameplayCapture>(std::shared_ptr<const Editor::PlaySaveGameplayBinding>{bindings, &binding},
                                                                owner, identities);
                const auto added = registry.Register(binding.declaration.descriptor.participant, std::move(adapter));
                if (added.HasError())
                    return Result<void>::Failure(added.ErrorValue());
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EditorPlaySaveHost::State::RegisterCoreCapture */
    Result<void> EditorPlaySaveHost::State::RegisterCoreCapture() {
        if (const auto core = registry.Register(PlaySaveSceneDescriptor(),
                                                std::make_shared<PlaySaveSceneCapture>(scenes, schema->baseline, schema->identities,
                                                                                       liveBindings, prefabs, physics));
            core.HasError())
            return Result<void>::Failure(core.ErrorValue());
        if (prefabs) {
            const auto added = registry.Register(PlaySavePrefabDescriptor(), std::make_shared<PlaySavePrefabCaptureAdapter>(prefabs));
            if (added.HasError())
                return Result<void>::Failure(added.ErrorValue());
        }
        return RegisterPhysicsCapture();
    }

    /** @copydoc EditorPlaySaveHost::State::Compose */
    Result<void> EditorPlaySaveHost::State::Compose() {
        if (auto core = RegisterCoreCapture(); core.HasError())
            return core;
        if (const auto native = RegisterGameplay(registry, liveBindings, behaviorOwner,
                                                 [scene = &scenes, values = schema, prefabs = prefabs] {
            return CurrentPlaySaveIdentities(*scene, *values, prefabs.get());
        });
            native.HasError())
            return native;
        if (const auto registered = RegisterBehaviorLifecycle(); registered.HasError())
            return registered;
        auto frozen = registry.Snapshot();
        if (frozen.HasError())
            return Result<void>::Failure(frozen.ErrorValue());
        participants = std::move(frozen).Value();
        const auto active = scenes.ActiveScene();
        authority = std::make_shared<PlaySaveGeneration>(scenes, active->RuntimeId().value, participants.Generation());
        auto created = SaveCaptureBarrier::Create(active->RuntimeId().value, environment.clock, MaximumSaveParticipantCount);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        barrier = std::move(created).Value();
        const std::array names{"horo.editor.play.fixed", "horo.editor.play.jobs", "horo.editor.play.mutation",
                               "horo.editor.play.subsystem"};
        const std::array domains{SaveBarrierDomain::FixedSimulation, SaveBarrierDomain::Jobs, SaveBarrierDomain::SceneMutation,
                                 SaveBarrierDomain::Subsystem};
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto registered = barrier->Register(SaveParticipantId::Parse(names[index]).Value(), domains[index]);
            if (registered.HasError())
                return Result<void>::Failure(registered.ErrorValue());
            barrierOwners.push_back(registered.Value());
        }
        return ComposeCaptureOwners();
    }

    /** @brief Registers actual capture participants in the same finite barrier before any request is possible. */
    Result<void> EditorPlaySaveHost::State::ComposeCaptureOwners() {
        for (const auto &binding : participants.CaptureBindings()) {
            const auto registered = barrier->Register(binding.Descriptor().participant, SaveBarrierDomain::Subsystem);
            if (registered.HasError())
                return Result<void>::Failure(registered.ErrorValue());
            barrierOwners.push_back(registered.Value());
        }
        mutations.reserve(barrierOwners.size());
        for (const auto owner : barrierOwners)
            if (const auto ready = barrier->PublishReadiness(owner, {epoch}); ready.HasError())
                return ready;
        return ComposeRuntime();
    }

    /** @copydoc EditorPlaySaveHost::State::ComposeRuntime */
    Result<void> EditorPlaySaveHost::State::ComposeRuntime() {
        const RuntimeSaveCaptureLimits limits{.maximumParticipants = MaximumSaveParticipantCount,
                                              .maximumRecords = MaximumSaveParticipantCount,
                                              .maximumSegments = MaximumSaveParticipantCount,
                                              .maximumPayloadBytes = PlaySaveMaximumPayloadBytes,
                                              .maximumCopiedRecordBytes = PlaySaveMaximumPayloadBytes};
        auto created = RuntimeSaveSession::Create(environment.jobs, *barrier, participants, authority->Generation(), limits);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        runtime = std::move(created).Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorPlaySaveHost::State::StartRuntime */
    Result<void> EditorPlaySaveHost::State::StartRuntime(const Editor::PlaySaveCommand &command) {
        const auto identity = IssuePlaySaveIdentity<CapturedStateId>(*environment.storage.entropy);
        if (identity.HasError())
            return Result<void>::Failure(identity.ErrorValue());
        captured = identity.Value();
        requestCancellation = CancellationSource{cancellation.Token()};
        operationRevision = 0;
        const auto operationId = environment.operations.Begin(
            {.title = command.kind == Editor::PlaySaveCommandKind::Load ? "Load Play sandbox" : "Save Play sandbox",
             .cancellable = true,
             .requestCancel = [cancel = requestCancellation]() {
            cancel.RequestCancellation();
        }});
        if (!operationId.has_value())
            return Result<void>::Failure(MakeError(SaveErrors::OperationInProgress));
        const auto loaded = TryAdmitRuntime(command, *operationId);
        if (loaded.HasError())
            (void)environment.operations.Update(*operationId, {.state = OperationState::Failed, .error = loaded.ErrorValue()});
        dirty = true;
        return loaded;
    }

    /** @brief Keeps coarse diagnostics terminal if allocation fails before the canonical runtime request is admitted. */
    Result<void> EditorPlaySaveHost::State::TryAdmitRuntime(const Editor::PlaySaveCommand &command, OperationId id) {
        try {
            return AdmitRuntime(command, id);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(SaveErrors::StorageAllocationFailed));
        }
    }

    /** @brief Admits the original RuntimeSaveSession operation without allocating any second producer or commit gate. */
    Result<void> EditorPlaySaveHost::State::AdmitRuntime(const Editor::PlaySaveCommand &command, OperationId id) {
        const auto kind = command.kind == Editor::PlaySaveCommandKind::Load ? SaveOperationKind::Load : SaveOperationKind::Save;
        const SaveOperationDescriptor descriptor{.operation = id,
                                                 .kind = kind,
                                                 .maximumCompletionCallbacks = 1,
                                                 .parentCancellation = requestCancellation.Token()};
        if (kind == SaveOperationKind::Load) {
            if (!command.selection)
                return Result<void>::Failure(MakeError(SaveErrors::CompositionUnsupported));
            auto source = std::make_shared<PlaySaveRestoreSource>(storage, schema, *command.selection, liveBindings, behaviorOwner,
                                                                  lifecycle, physics);
            auto preparedHandoff =
                std::make_shared<PlaySaveSceneHandoff>(scenes, source, authority, std::make_shared<PlaySaveSceneHandoffState>(),
                                                       environment.storage.world, prefabs);
            auto admitted = runtime->Load(descriptor, std::move(source), preparedHandoff);
            if (admitted.HasError())
                return Result<void>::Failure(admitted.ErrorValue());
            handoff = std::move(preparedHandoff);
            request.emplace(std::move(admitted).Value());
            loading = true;
            return Result<void>::Success();
        }
        if (behaviorOwner) {
            const auto known = behaviorOwner->ValidateKnownNoServiceRestoreRequirements();
            if (known.HasError())
                return Result<void>::Failure(known.ErrorValue());
        }
        const auto milliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(environment.wallClock.UtcNow().time_since_epoch()).count();
        const auto elapsed = (environment.clock.MonotonicNow() - started).ToNanoseconds();
        if (milliseconds < 0 || elapsed < 0)
            return Result<void>::Failure(MakeError(SaveErrors::LifecycleUnavailable));
        auto writer = std::make_shared<PlaySaveWriter>(storage, command.displayName, static_cast<std::uint64_t>(milliseconds),
                                                       static_cast<std::uint64_t>(elapsed));
        auto admitted = runtime->Save(descriptor, std::move(writer), CapturePreparation());
        if (admitted.HasError())
            return Result<void>::Failure(admitted.ErrorValue());
        request.emplace(std::move(admitted).Value());
        loading = false;
        return Result<void>::Success();
    }

    /** @brief Preserves the canonical terminal failure instead of presenting a failed operation as an idle ready sandbox. */
    void EditorPlaySaveHost::State::ObserveRuntime(const SaveOperationSnapshot &current) {
        operation = current;
        operationRevision = current.revision;
        dirty = true;
        if (current.terminalError)
            error = current.terminalError;
        (void)environment.operations.Update(current.operation, {.state = PresentationState(current.state), .error = current.terminalError});
    }

    /** @copydoc EditorPlaySaveHost::State::PumpRuntime */
    Result<void> EditorPlaySaveHost::State::PumpRuntime() {
        if (!request)
            return Result<void>::Success();
        if (handoff)
            if (auto advanced = handoff->Advance(); advanced.HasError())
                return advanced;
        const auto current = request->Operation().Snapshot();
        if (current && current->revision != operationRevision) {
            ObserveRuntime(*current);
        }
        if (!current || !current->IsTerminal() || !runtime->IsSettled())
            return runtime->CommitAtSafePoint(RuntimePhase::CommitDeferredLifecycleChanges, authority->Generation(), Provenance());
        if (const auto acknowledged = runtime->Acknowledge(); acknowledged.HasError())
            return acknowledged;
        request.reset();
        handoff.reset();
        loading = false;
        return ResumeAfterRequest();
    }

    /** @copydoc EditorPlaySaveHost::State::ResumeAfterRequest */
    Result<void> EditorPlaySaveHost::State::ResumeAfterRequest() {
        if (retiring || suspending)
            return Result<void>::Success();
        if (const auto composed = ComposeRuntime(); composed.HasError())
            return composed;
        return StartStorage(PlaySaveStorageAction::Refresh);
    }
}  // namespace Horo::Application::Internal
