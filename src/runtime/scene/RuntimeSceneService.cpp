#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"
#include "RuntimeScenePublicationInternal.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

namespace Horo::Runtime {
    /** @copydoc ScenePublicationReceipt::Snapshot */
    Result<ScenePublicationSnapshot> ScenePublicationReceipt::Snapshot() const {
        if (!state_ || state_->owner != std::this_thread::get_id())
            return Result<ScenePublicationSnapshot>::Failure(MakeError(SceneErrors::StaleView));
        return Result<ScenePublicationSnapshot>::Success(state_->snapshot);
    }

    /** @copydoc RuntimeSceneService::RetirePublicationReceipt */
    void RuntimeSceneService::RetirePublicationReceipt(const ScenePublicationStatus status) noexcept {
        if (publicationReceipt_ && publicationReceipt_->snapshot.status == ScenePublicationStatus::Pending)
            publicationReceipt_->snapshot.status = status;
        publicationReceipt_.reset();
    }

    namespace {
        /** @brief New animation-bearing Scenes require one actual composed activation authority before any publication. */
        Result<void> ValidateActivationOwners(const RuntimeSceneDefinition &definition,
                                              std::span<const std::unique_ptr<SceneActivationParticipant>> participants) {
            const bool required = std::ranges::any_of(definition.Entities(), [](const auto &entity) {
                return entity.components.animation.has_value();
            });
            if (!required)
                return Result<void>::Success();
            const auto owners = std::ranges::count_if(participants, [](const auto &participant) {
                return participant->RequiredOwner() == SceneActivationOwner::Animation;
            });
            if (owners != 1)
                return Result<void>::Failure(
                    MakeError(SceneErrors::AssetServicesUnavailable, "Animation requires exactly one canonical Scene activation owner."));
            return Result<void>::Success();
        }

        /** @brief Preserves the existing finite preparation-phase vocabulary. */
        Result<void> ValidateActivationPhases(std::span<const std::unique_ptr<SceneActivationParticipant>> participants) {
            for (const auto &participant : participants)
                if (participant->PreparationPhase() != SceneActivationPhase::InitialPlacement &&
                    participant->PreparationPhase() != SceneActivationPhase::DerivedResources)
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Unknown Scene activation preparation phase."));
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsTerminal(const Horo::Assets::AssetLoadState state) noexcept {
            using enum Horo::Assets::AssetLoadState;
            return state == Succeeded || state == Failed || state == Cancelled;
        }

        [[nodiscard]] Error WithAssetContext(Error error, const SceneDefinitionId scene, const Horo::Assets::AssetId asset) {
            error.diagnostics.emplace_back(DiagnosticCode{"scene.asset.context"}, DiagnosticSeverity::Note,
                                           std::format("Scene {} requires asset {}.", scene.value, asset.ToString()),
                                           SourceLocation{asset.ToString(), 0, 0});
            return error;
        }

        /** @brief Reads the same actual registry revision for ordinary structural admission without adding another validator. */
        Assets::AssetRegistryRevision StructuralRegistryRevision(const Assets::AssetRegistry *registry) {
            return registry ? registry->Snapshot().Revision() : Assets::AssetRegistryRevision{};
        }

        [[nodiscard]] Result<void> ValidateRuntimeIdentity(const std::uint64_t nextRuntimeId) {
            if (nextRuntimeId == 0)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Runtime scene identity space is exhausted."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePreparation(const RuntimeSceneDefinition &definition, const RuntimeSceneConfig config,
                                                       const RuntimeSceneAssetLimits &limits, const std::uint64_t nextRuntimeId) {
            if (const auto identity = ValidateRuntimeIdentity(nextRuntimeId); identity.HasError())
                return identity;
            if (config.maximumGeneration == 0 || limits.maximumDependencies == 0 || limits.maximumConcurrentLoads == 0 ||
                limits.maximumResidentBytes == 0)
                return Result<void>::Failure(MakeError(SceneErrors::AssetLimitsInvalid));
            if (limits.maximumDependencyReferences == 0 || limits.maximumDependencyReferences > 1048576)
                return Result<void>::Failure(MakeError(SceneErrors::AssetLimitsInvalid));
            if (definition.AssetDependencies().size() > limits.maximumDependencies)
                return Result<void>::Failure(
                    MakeError(SceneErrors::AssetBudgetExceeded, "Runtime scene dependency count exceeds the configured limit."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CheckDependenciesExist(const RuntimeSceneDefinition &definition,
                                                          const Horo::Assets::AssetRegistrySnapshot &snapshot) {
            for (const SceneAssetDependency &dependency : definition.AssetDependencies()) {
                const Horo::Assets::AssetRecord *record = snapshot.Find(dependency.id);
                if (!record)
                    return Result<void>::Failure(WithAssetContext(MakeError(SceneErrors::AssetMissing), definition.Id(), dependency.id));
                if (record->type != dependency.expectedType)
                    return Result<void>::Failure(
                        WithAssetContext(MakeError(SceneErrors::AssetTypeMismatch), definition.Id(), dependency.id));
            }
            return Result<void>::Success();
        }

    }  // namespace

    struct RuntimeSceneService::Preparation {
        struct Entry {
            SceneAssetDependency dependency;
            std::shared_ptr<const std::vector<std::uint8_t>> payload;
            std::optional<Assets::AssetLoadHandle> load;
            std::size_t inspectedParticipants{};
        };

        Preparation(RuntimeSceneDefinition source, const RuntimeSceneConfig sceneConfig, Assets::AssetRegistrySnapshot registrySnapshot)
            : definition(std::move(source)), config(sceneConfig), snapshot(std::move(registrySnapshot)) {}

        RuntimeSceneDefinition definition;
        RuntimeSceneConfig config;
        Assets::AssetRegistrySnapshot snapshot;
        std::vector<Entry> entries;
        std::size_t activeLoads{};
        std::size_t residentBytes{};
        std::size_t discoveredReferences{};
    };

    /** @brief Keeps observer-triggered shutdown outside preparation, rollback, publication and retirement stacks. */
    struct RuntimeSceneService::LifecycleMutation final {
        explicit LifecycleMutation(RuntimeSceneService &service) noexcept
            : service(service), outer(!std::exchange(service.mutatingLifecycle_, true)) {}

        ~LifecycleMutation() {
            if (!outer)
                return;
            service.mutatingLifecycle_ = false;
            if (std::exchange(service.shutdownDeferred_, false) && !service.shutdown_)
                service.Shutdown();
        }

        LifecycleMutation(const LifecycleMutation &) = delete;
        LifecycleMutation &operator=(const LifecycleMutation &) = delete;

        RuntimeSceneService &service;
        bool outer{};
    };

    /** @copydoc RuntimeSceneService::RetirePublishedRestorePreparation */
    Result<bool> RuntimeSceneService::RetirePublishedRestorePreparation() {
        if (std::this_thread::get_id() != restoreRetirementOwner_ || mutatingLifecycle_)
            return Result<bool>::Failure(MakeError(SceneErrors::OperationInProgress));
        const LifecycleMutation mutation{*this};
        auto result = RetirePublishedRestorePreparationInternal();
        return result;
    }

    RuntimeSceneService::RuntimeSceneService() = default;

    /** @copydoc RuntimeSceneService::RuntimeSceneService */
    RuntimeSceneService::RuntimeSceneService(const SceneRuntimeId initialRuntimeId) : nextRuntimeId_(initialRuntimeId.value) {}

    RuntimeSceneService::RuntimeSceneService(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads,
                                             const RuntimeSceneAssetLimits limits)
        : assetPreparationServices_{&registry, &loads, limits} {}

    /** @copydoc RuntimeSceneService::RuntimeSceneService */
    RuntimeSceneService::RuntimeSceneService(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads,
                                             const RuntimeSceneAssetLimits limits, const SceneRuntimeId initialRuntimeId)
        : nextRuntimeId_(initialRuntimeId.value), assetPreparationServices_{&registry, &loads, limits} {}

    RuntimeSceneService::~RuntimeSceneService() {
        Shutdown();
    }

    /** @copydoc RuntimeSceneService::AddActivationParticipant */
    Result<void> RuntimeSceneService::AddActivationParticipant(std::unique_ptr<SceneActivationParticipant> participant) {
        if (!participant)
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene activation participant is null."));
        if (started_ || shutdown_ || transition_ != TransitionKind::None || preparation_ || pending_.scene)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        if (participants_.size() >= MaximumSceneActivationParticipants)
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene activation participant capacity exceeded."));
        participants_.push_back(std::move(participant));
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::QueuePreparation */
    Result<void> RuntimeSceneService::QueuePreparation(RuntimeSceneDefinition definition, const RuntimeSceneConfig config) {
        auto queued = QueuePreparationWithPublicationCheck(std::move(definition), {}, config);
        return queued.HasError() ? Result<void>::Failure(queued.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::QueuePreparationWithPublicationCheck */
    Result<ScenePublicationReceipt> RuntimeSceneService::QueuePreparationWithPublicationCheck(
        RuntimeSceneDefinition definition, std::unique_ptr<ScenePublicationCheck> publicationCheck, const RuntimeSceneConfig config) {
        if (shutdown_)
            return Result<ScenePublicationReceipt>::Failure(MakeError(SceneErrors::ServiceShutdown));
        if (mutatingLifecycle_ || transition_ != TransitionKind::None || structuralCommands_ || preparation_)
            return Result<ScenePublicationReceipt>::Failure(MakeError(SceneErrors::OperationInProgress));
        if (const auto valid = ValidateRuntimeIdentity(nextRuntimeId_); valid.HasError())
            return Result<ScenePublicationReceipt>::Failure(valid.ErrorValue());
        const LifecycleMutation mutation{*this};
        if (publicationCheck) {
            if (const auto valid = publicationCheck->ValidatePublication(); valid.HasError())
                return Result<ScenePublicationReceipt>::Failure(valid.ErrorValue());
        }
        try {
            // Receipt storage is admitted before world preparation. Publication fills only bounded values, with no callbacks.
            auto receipt = publicationCheck ? std::make_shared<ScenePublicationDetail::State>() : nullptr;
            operationError_.reset();
            if (const auto admitted = BeginPreparation(std::move(definition), config); admitted.HasError())
                return Result<ScenePublicationReceipt>::Failure(admitted.ErrorValue());
            if (pending_.scene && publicationCheck) {
                const auto composition = publicationCheck->ValidatePreparedComposition(pending_.datasets);
                if (composition.HasError()) {
                    RejectPendingPublication(composition.ErrorValue());
                    return Result<ScenePublicationReceipt>::Failure(composition.ErrorValue());
                }
            }
            publicationCheck_ = std::move(publicationCheck);
            publicationReceipt_ = receipt;
            return Result<ScenePublicationReceipt>::Success(ScenePublicationReceipt{std::move(receipt)});
        } catch (const std::bad_alloc &) {
            return Result<ScenePublicationReceipt>::Failure(
                MakeError(SceneErrors::InvalidCandidate, "Scene publication receipt allocation failed."));
        }
    }

    /** @copydoc RuntimeSceneService::QueuePreparationWithRestore */
    Result<void> RuntimeSceneService::QueuePreparationWithRestore(RuntimeSceneDefinition definition,
                                                                  std::unique_ptr<SceneAggregateRestore> restore,
                                                                  const RuntimeSceneConfig config) {
        if (!restore)
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate));
        if (shutdown_)
            return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
        if (mutatingLifecycle_ || transition_ != TransitionKind::None || structuralCommands_ || preparation_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        if (const auto valid = ValidateRuntimeIdentity(nextRuntimeId_); valid.HasError())
            return valid;
        const LifecycleMutation mutation{*this};
        operationError_.reset();
        aggregateRestore_ = std::move(restore);
        const auto admitted = BeginPreparation(std::move(definition), config);
        if (admitted.HasError()) {
            aggregateRestore_->Rollback();
            aggregateRestore_.reset();
        }
        return admitted;
    }

    Result<void> RuntimeSceneService::ReusePreparationPayload(Preparation &prep, std::size_t index) const {
        auto &entry = prep.entries[index];
        if (active_.scene && active_.scene->assetRegistryRevision_ == prep.snapshot.Revision()) {
            const auto reusable = std::ranges::find(active_.scene->assets_, entry.dependency.id, [](const auto &asset) {
                return asset.dependency.id;
            });
            if (reusable != active_.scene->assets_.end() && reusable->dependency.expectedType == entry.dependency.expectedType)
                entry.payload = reusable->payload;
        }
        if (entry.payload) {
            if (entry.payload->size() > assetPreparationServices_.limits.maximumResidentBytes - prep.residentBytes)
                return Result<void>::Failure(
                    WithAssetContext(MakeError(SceneErrors::AssetBudgetExceeded), prep.definition.Id(), entry.dependency.id));
            prep.residentBytes += entry.payload->size();
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::PopulatePreparationEntries(Preparation &prep, const RuntimeSceneDefinition &definition) const {
        prep.entries.reserve(definition.AssetDependencies().size());
        for (const auto &dependency : definition.AssetDependencies()) {
            prep.entries.push_back(Preparation::Entry{dependency});
            if (auto reused = ReusePreparationPayload(prep, prep.entries.size() - 1); reused.HasError())
                return reused;
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::BeginPreparation(RuntimeSceneDefinition definition, const RuntimeSceneConfig config) {
        try {
            if (const auto validation = ValidatePreparation(definition, config, assetPreparationServices_.limits, nextRuntimeId_);
                validation.HasError())
                return validation;

            if (definition.AssetDependencies().empty()) {
                if (const auto staged = StageCandidate(definition, config, {}, {}); staged.HasError())
                    return staged;
                transition_ = TransitionKind::Activate;
                return Result<void>::Success();
            }
            if (!assetPreparationServices_.registry || !assetPreparationServices_.loads)
                return Result<void>::Failure(MakeError(SceneErrors::AssetServicesUnavailable));

            Assets::AssetRegistrySnapshot snapshot = assetPreparationServices_.registry->Snapshot();
            if (const auto checked = CheckDependenciesExist(definition, snapshot); checked.HasError())
                return checked;

            auto prep = std::make_unique<Preparation>(std::move(definition), config, std::move(snapshot));
            if (const auto populated = PopulatePreparationEntries(*prep, prep->definition); populated.HasError())
                return populated;

            preparation_ = std::move(prep);
            if (const Result<void> submitted = SubmitPreparationLoads(); submitted.HasError()) {
                const Error error = submitted.ErrorValue();
                CancelPreparation(false);
                return Result<void>::Failure(error);
            }
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            CancelPreparation(false);
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene preparation allocation failed."));
        }
    }

    /** @copydoc RuntimeSceneService::QueueUnload */
    Result<void> RuntimeSceneService::QueueUnload() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
        if (mutatingLifecycle_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        const LifecycleMutation mutation{*this};
        using enum TransitionKind;
        if (transition_ == Unload)
            return Result<void>::Success();
        if (structuralCommands_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        CancelPreparation(false);
        if (aggregateRestore_)
            aggregateRestore_->Rollback();
        aggregateRestore_.reset();
        ScenePublicationDetail::ShutdownCandidates(pending_.candidates);
        pending_.scene.reset();
        RetirePublicationReceipt(ScenePublicationStatus::Cancelled);
        publicationCheck_.reset();
        transition_ = None;
        if (!active_.scene)
            return Result<void>::Success();
        transition_ = Unload;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::QueueStructuralCommands */
    Result<void> RuntimeSceneService::QueueStructuralCommands(SceneCommandBuffer commands) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
        if (!active_.scene)
            return Result<void>::Failure(MakeError(SceneErrors::NoActiveScene));
        if (mutatingLifecycle_ || transition_ != TransitionKind::None || structuralCommands_ || preparation_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        const LifecycleMutation mutation{*this};
        const auto revision = StructuralRegistryRevision(assetPreparationServices_.registry);
        if (commands.HasRestoreGroups())
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Private restore groups cannot enter ordinary commit."));
        if (auto admission = commands.ValidateAdmission(active_.scene->View().RuntimeId(), revision); admission.HasError())
            return admission;
        if (!commands.Empty())
            structuralCommands_ = std::move(commands);
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::QueueTrackedStructuralCommands */
    Result<std::shared_ptr<const SceneStructuralReceipt>> RuntimeSceneService::QueueTrackedStructuralCommands(SceneCommandBuffer commands) {
        using Receipt = std::shared_ptr<const SceneStructuralReceipt>;
        if (commands.Empty())
            return Result<Receipt>::Failure(MakeError(SceneErrors::InvalidCandidate));
        auto receipt = std::make_shared<SceneStructuralReceipt>();
        if (auto queued = QueueStructuralCommands(std::move(commands)); queued.HasError())
            return Result<Receipt>::Failure(queued.ErrorValue());
        structuralReceipt_ = receipt;
        return Result<Receipt>::Success(std::move(receipt));
    }

    /** @copydoc RuntimeSceneService::CancelPendingBaseline */
    Result<void> RuntimeSceneService::CancelPendingBaseline(const SceneRuntimeId scene, const SceneDefinitionId baseline,
                                                            const SceneBaselineOwnership &ownership) {
        if (mutatingLifecycle_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        if (!active_.scene || active_.scene->runtimeId_ != scene || !structuralCommands_ || structuralCommands_->commands_.size() != 1)
            return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
        if (const bool matches = std::visit(
                [&]<typename T>(const T &command) {
            if constexpr (std::is_same_v<T, SceneCommandBuffer::AttachBaselineCommand>)
                return command.definition.Id() == baseline && command.ownership->MatchesOperation(ownership);
            else if constexpr (std::is_same_v<T, SceneCommandBuffer::DetachBaselineCommand>)
                return command.id == baseline && command.ownership->MatchesOperation(ownership);
            else
                return false;
        }, structuralCommands_->commands_.front());
            !matches)
            return Result<void>::Failure(MakeError(SceneErrors::BaselineStale));
        const LifecycleMutation mutation{*this};
        structuralCommands_.reset();
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::AddStructuralParticipant */
    Result<void> RuntimeSceneService::AddStructuralParticipant(std::unique_ptr<SceneStructuralParticipant> participant) {
        if (!participant || started_ || shutdown_)
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate));
        if (std::ranges::any_of(structuralParticipants_, [&](const auto &existing) {
            return existing->Owner() == participant->Owner();
        }))
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "A structural owner is already registered."));
        structuralParticipants_.push_back(std::move(participant));
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::ActiveScene */
    std::optional<RuntimeSceneView> RuntimeSceneService::ActiveScene() const noexcept {
        if (!active_.scene)
            return std::nullopt;
        return active_.scene->View();
    }

    /** @copydoc RuntimeSceneService::TakeStructuralCommitResult */
    std::optional<StructuralCommitResult> RuntimeSceneService::TakeStructuralCommitResult() {
        return std::exchange(structuralResult_, std::nullopt);
    }

    /** @copydoc RuntimeSceneService::TakeOperationError */
    std::optional<Error> RuntimeSceneService::TakeOperationError() {
        return std::exchange(operationError_, std::nullopt);
    }

    /** @copydoc RuntimeSceneService::Startup */
    Result<void> RuntimeSceneService::Startup(const CancellationToken &cancellation) {
        if (mutatingLifecycle_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene service startup was cancelled."));
        started_ = true;
        shutdown_ = false;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::OnPhase */
    Result<void> RuntimeSceneService::OnPhase(const RuntimePhase phase, const FrameContext &context) {
        if (phase == RuntimePhase::RenderExtraction) {
            vfxFrame_ = {};
            vfxFrame_ = active_.scene ? active_.scene->CaptureVfx(context.frameNumber) : Vfx::VfxExtractionOutcome{};
        }
        if (phase == RuntimePhase::CommitDeferredLifecycleChanges) {
            if (mutatingLifecycle_)
                return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
            const LifecycleMutation mutation{*this};
            if (preparation_ && publicationCheck_) {
                if (const auto valid = publicationCheck_->ValidatePublication(); valid.HasError()) {
                    operationError_ = valid.ErrorValue();
                    CancelPreparation(false);
                    RetirePublicationReceipt(ScenePublicationStatus::Rejected);
                    publicationCheck_.reset();
                    return Result<void>::Success();
                }
            }
            AdvancePreparation(context.cancellation);
            return CommitDeferredChanges(context.cancellation);
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::OnFixedUpdate */
    Result<void> RuntimeSceneService::OnFixedUpdate(const FixedStepContext &) {
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::Shutdown */
    void RuntimeSceneService::Shutdown() noexcept {
        if (mutatingLifecycle_) {
            shutdownDeferred_ = true;
            return;
        }
        const LifecycleMutation mutation{*this};
        CancelPreparation(true);
        if (aggregateRestore_)
            aggregateRestore_->Rollback();
        aggregateRestore_.reset();
        if (structuralReceipt_)
            structuralReceipt_->error_ = MakeError(SceneErrors::StaleEntity);
        structuralReceipt_.reset();
        structuralCommands_.reset();
        structuralResult_.reset();
        operationError_.reset();
        ScenePublicationDetail::ShutdownCandidates(pending_.candidates);
        pending_.scene.reset();
        RetirePublicationReceipt(ScenePublicationStatus::Cancelled);
        publicationCheck_.reset();
        ScenePublicationDetail::ShutdownCandidates(active_.candidates);
        active_.restore.reset();
        vfxFrame_ = {};
        active_.scene.reset();
        transition_ = TransitionKind::None;
        started_ = false;
        shutdown_ = true;
    }

    Result<void> RuntimeSceneService::SubmitPreparationLoads() {
        if (!preparation_ || !assetPreparationServices_.loads)
            return Result<void>::Success();
        for (Preparation::Entry &entry : preparation_->entries) {
            if (preparation_->activeLoads >= assetPreparationServices_.limits.maximumConcurrentLoads)
                break;
            if (entry.payload || entry.load)
                continue;
            Result<Assets::AssetLoadHandle> submitted =
                assetPreparationServices_.loads->LoadAsync(preparation_->snapshot, entry.dependency.id);
            if (submitted.HasError())
                return Result<void>::Failure(WithAssetContext(submitted.ErrorValue(), preparation_->definition.Id(), entry.dependency.id));
            entry.load = std::move(submitted).Value();
            ++preparation_->activeLoads;
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::ProcessCompletedPreparationLoads(const CancellationToken &cancellation) {
        for (Preparation::Entry &entry : preparation_->entries) {
            if (!entry.load || !IsTerminal(entry.load->State()))
                continue;
            Result<Assets::AssetLoadResult> loaded = entry.load->TakeResult();
            entry.load.reset();
            --preparation_->activeLoads;
            if (loaded.HasError())
                return Result<void>::Failure(WithAssetContext(loaded.ErrorValue(), preparation_->definition.Id(), entry.dependency.id));
            Assets::AssetLoadResult result = std::move(loaded).Value();
            if (result.sourceRegistryRevision != preparation_->snapshot.Revision()) {
                return Result<void>::Failure(
                    WithAssetContext(MakeError(SceneErrors::AssetRevisionStale), preparation_->definition.Id(), entry.dependency.id));
            }
            if (result.bytes.empty())
                return Result<void>::Failure(
                    WithAssetContext(MakeError(SceneErrors::AssetPayloadEmpty), preparation_->definition.Id(), entry.dependency.id));
            if (result.bytes.size() > assetPreparationServices_.limits.maximumResidentBytes - preparation_->residentBytes)
                return Result<void>::Failure(
                    WithAssetContext(MakeError(SceneErrors::AssetBudgetExceeded), preparation_->definition.Id(), entry.dependency.id));
            preparation_->residentBytes += result.bytes.size();
            entry.payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(result.bytes));
        }
        if (auto discovered = DiscoverPreparationDependencies(cancellation); discovered.HasError())
            return discovered;
        return SubmitPreparationLoads();
    }

    Result<void> RuntimeSceneService::AppendPreparationDependency(const SceneAssetDependency &dependency) {
        auto &prep = *preparation_;
        const auto existing = std::ranges::find(prep.entries, dependency.id, [](const auto &entry) {
            return entry.dependency.id;
        });
        if (existing != prep.entries.end()) {
            if (existing->dependency.expectedType != dependency.expectedType)
                return Result<void>::Failure(
                    WithAssetContext(MakeError(SceneErrors::AssetTypeMismatch), prep.definition.Id(), dependency.id));
            return Result<void>::Success();
        }
        const auto maximum = std::min<std::size_t>(assetPreparationServices_.limits.maximumDependencies, 16384);
        if (prep.entries.size() >= maximum)
            return Result<void>::Failure(
                WithAssetContext(MakeError(SceneErrors::AssetBudgetExceeded), prep.definition.Id(), dependency.id));
        const auto *record = prep.snapshot.Find(dependency.id);
        if (!record)
            return Result<void>::Failure(WithAssetContext(MakeError(SceneErrors::AssetMissing), prep.definition.Id(), dependency.id));
        if (record->type != dependency.expectedType)
            return Result<void>::Failure(WithAssetContext(MakeError(SceneErrors::AssetTypeMismatch), prep.definition.Id(), dependency.id));
        prep.entries.push_back(Preparation::Entry{dependency});
        return ReusePreparationPayload(prep, prep.entries.size() - 1);
    }

    Result<void> RuntimeSceneService::MergePreparationDependencies(std::span<const SceneAssetDependency> dependencies) {
        for (const auto &dependency : dependencies)
            if (auto appended = AppendPreparationDependency(dependency); appended.HasError())
                return appended;
        return Result<void>::Success();
    }

    Result<std::vector<SceneAssetDependency>> RuntimeSceneService::InvokePreparationDiscovery(std::size_t entryIndex,
                                                                                              std::size_t participantIndex,
                                                                                              const CancellationToken &cancellation) {
        using Return = Result<std::vector<SceneAssetDependency>>;
        try {
            const auto &entry = preparation_->entries[entryIndex];
            const RuntimeSceneAssetView view{entry.dependency.id, &entry.dependency.expectedType, *entry.payload};
            const auto remaining = assetPreparationServices_.limits.maximumDependencyReferences - preparation_->discoveredReferences;
            return participants_[participantIndex]->DiscoverDependencies(view, cancellation, remaining);
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(SceneErrors::AssetBudgetExceeded));
        } catch (...) {
            return Return::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene dependency discovery callback failed."));
        }
    }

    Result<void> RuntimeSceneService::DiscoverPreparationEntry(std::size_t entryIndex, const CancellationToken &cancellation) {
        while (preparation_->entries[entryIndex].inspectedParticipants < participants_.size()) {
            if (cancellation.IsCancellationRequested())
                return JobCancelled();
            const auto participant = preparation_->entries[entryIndex].inspectedParticipants;
            auto dependencies = InvokePreparationDiscovery(entryIndex, participant, cancellation);
            if (dependencies.HasError())
                return Result<void>::Failure(dependencies.ErrorValue());
            if (shutdownDeferred_ || shutdown_ || !started_)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene dependency discovery owner retired."));
            if (cancellation.IsCancellationRequested())
                return JobCancelled();
            const auto remaining = assetPreparationServices_.limits.maximumDependencyReferences - preparation_->discoveredReferences;
            if (dependencies.Value().size() > remaining)
                return Result<void>::Failure(MakeError(SceneErrors::AssetBudgetExceeded));
            preparation_->discoveredReferences += dependencies.Value().size();
            if (auto merged = MergePreparationDependencies(dependencies.Value()); merged.HasError())
                return merged;
            ++preparation_->entries[entryIndex].inspectedParticipants;
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::DiscoverPreparationDependencies(const CancellationToken &cancellation) {
        for (std::size_t index = 0; index < preparation_->entries.size(); ++index) {
            if (!preparation_->entries[index].payload)
                continue;
            if (auto inspected = DiscoverPreparationEntry(index, cancellation); inspected.HasError())
                return inspected;
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::CompletePreparationDefinition() {
        auto &prep = *preparation_;
        std::ranges::sort(prep.entries, {}, [](const auto &entry) {
            return entry.dependency.id;
        });
        if (prep.entries.size() == prep.definition.AssetDependencies().size())
            return Result<void>::Success();
        SceneDefinitionBuilder builder{prep.definition.Id(), prep.definition.Revision()};
        for (const auto &entity : prep.definition.Entities())
            builder.Add(entity);
        for (const auto &entry : prep.entries)
            if (auto required = builder.RequireAsset(entry.dependency); required.HasError())
                return required;
        for (const auto &evidence : prep.definition.TriggerVolumeEvidence())
            if (auto required = builder.RequireTriggerVolume(evidence); required.HasError())
                return required;
        auto complete = std::move(builder).Build();
        if (complete.HasError())
            return Result<void>::Failure(complete.ErrorValue());
        prep.definition = std::move(complete).Value();
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::FinalizePreparation() {
        if (!assetPreparationServices_.registry ||
            assetPreparationServices_.registry->Snapshot().Revision() != preparation_->snapshot.Revision()) {
            Error error = MakeError(SceneErrors::AssetRevisionStale);
            error.diagnostics.emplace_back(DiagnosticCode{"scene.asset.registry_revision"}, DiagnosticSeverity::Note,
                                           "The authoritative registry revision changed before activation.", SourceLocation{});
            return Result<void>::Failure(std::move(error));
        }

        if (auto complete = CompletePreparationDefinition(); complete.HasError())
            return complete;

        std::vector<RuntimeScene::ResolvedAsset> assets;
        assets.reserve(preparation_->entries.size());
        for (Preparation::Entry &entry : preparation_->entries)
            assets.emplace_back(std::move(entry.dependency), std::move(entry.payload));
        if (const auto staged =
                StageCandidate(preparation_->definition, preparation_->config, preparation_->snapshot.Revision(), std::move(assets));
            staged.HasError())
            return staged;
        preparation_.reset();
        transition_ = TransitionKind::Activate;
        return Result<void>::Success();
    }

    /** @brief Synchronous private-candidate borrow; never stored in a restore owner. */
    struct RuntimeSceneService::RestoreCandidateCommands final : SceneRestoreCandidateCommands {
        RestoreCandidateCommands(RuntimeSceneService &owner, RuntimeScene &candidate) noexcept
            : owner(owner), candidate(candidate), runtime(candidate.View().RuntimeId()) {}

        ~RestoreCandidateCommands() override {
            if (seal)
                seal->valid = false;
        }

        /** @brief Requires the exact issued payload control, which public assignment cannot forge or carry across attempts. */
        bool IssuedBatch(const SceneCommandBuffer &commands) const noexcept {
            if (!seal || !seal->valid || builtCommands != &commands || commands.commands_.size() != 1)
                return false;
            const auto *group = std::get_if<SceneCommandBuffer::CreateGroupCommand>(&commands.commands_.front());
            return group && group->restoreSeal && !seal.owner_before(group->restoreSeal) && !group->restoreSeal.owner_before(seal);
        }

        /** @brief Service-issued proof of this exact still-unpublished candidate on its owning lane. */
        bool Admits() const noexcept {
            return std::this_thread::get_id() == owner.restoreRetirementOwner_ && owner.pending_.scene.get() == &candidate &&
                   owner.active_.scene.get() != &candidate && candidate.View().RuntimeId() == runtime && runtime.value != 0 &&
                   owner.aggregateRestore_;
        }

        Result<StructuralCommitResult> Commit(const SceneCommandBuffer &commands) override {
            if (!Admits() || !IssuedBatch(commands))
                return Result<StructuralCommitResult>::Failure(MakeError(SceneErrors::InvalidCandidate));
            seal->valid = false;
            builtCommands = nullptr;
            return candidate.CommitWithRegistry(commands, owner.assetPreparationServices_.registry, owner.structuralParticipants_, nullptr,
                                                RuntimeScene::CommitPurpose::RestoreMaterialization);
        }

        /** @brief Charges the actual private candidate's existing assets plus the new exact resource closure against live host policy. */
        Result<void> ValidateResidentBudget(std::span<const RuntimeGroupAssetLease> resources) const {
            const auto &limits = owner.assetPreparationServices_.limits;
            std::set<Assets::AssetId> dependencies;
            std::size_t bytes{};
            for (const auto &asset : candidate.assets_) {
                dependencies.insert(asset.dependency.id);
                if (asset.payload && asset.payload->size() > limits.maximumResidentBytes - bytes)
                    return Result<void>::Failure(MakeError(SceneErrors::AssetBudgetExceeded));
                if (asset.payload)
                    bytes += asset.payload->size();
            }
            for (const auto &resource : resources) {
                dependencies.insert(resource.metadata.id);
                if (resource.artifact.Bytes().size() > limits.maximumResidentBytes - bytes)
                    return Result<void>::Failure(MakeError(SceneErrors::AssetBudgetExceeded));
                bytes += resource.artifact.Bytes().size();
            }
            return dependencies.size() <= limits.maximumDependencies ? Result<void>::Success()
                                                                     : Result<void>::Failure(MakeError(SceneErrors::AssetBudgetExceeded));
        }

        Result<std::vector<DeferredEntity>> CreateGroup(SceneCommandBuffer &commands, std::vector<RuntimeEntityGroupEntry> entries,
                                                        std::vector<RuntimeGroupAssetLease> resources,
                                                        const CancellationToken &cancellation,
                                                        std::vector<SceneRestoreGroupPartition> partitions) override {
            if (!Admits())
                return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::InvalidCandidate));
            if (builtGroup || !commands.Empty())
                return Result<std::vector<DeferredEntity>>::Failure(MakeError(SceneErrors::InvalidCandidate));
            if (auto valid = ValidateResidentBudget(resources); valid.HasError())
                return Result<std::vector<DeferredEntity>>::Failure(valid.ErrorValue());
            if (auto valid = owner.aggregateRestore_->ValidateRestorePartitions(partitions); valid.HasError())
                return Result<std::vector<DeferredEntity>>::Failure(valid.ErrorValue());
            auto limits = owner.aggregateRestore_->StructuralLimits();
            limits.maximumEntities = std::min(limits.maximumEntities, std::size_t{16'384});
            limits.maximumResources =
                std::min({limits.maximumResources, owner.assetPreparationServices_.limits.maximumDependencies, std::size_t{16'384}});
            limits.maximumReferences = std::min(limits.maximumReferences, std::size_t{65'536});
            limits.maximumPhysicsReferences = std::min(limits.maximumPhysicsReferences, std::size_t{65'536});
            limits.maximumLineageEntries = std::min(limits.maximumLineageEntries, limits.maximumEntities * 16);
            limits.maximumResourceBytes =
                std::min(limits.maximumResourceBytes, owner.assetPreparationServices_.limits.maximumResidentBytes);
            const auto revision = owner.assetPreparationServices_.registry ? owner.assetPreparationServices_.registry->Snapshot().Revision()
                                                                           : Assets::AssetRegistryRevision{};
            auto issuedSeal = std::make_shared<SceneCommandBuffer::RestoreGroupSeal>();
            auto created = commands.CreateGroupInternal(std::move(entries), std::move(resources), {runtime, revision, cancellation},
                                                        {limits, false}, std::move(partitions));
            builtGroup = created.HasValue();
            if (builtGroup) {
                seal = std::move(issuedSeal);
                std::get<SceneCommandBuffer::CreateGroupCommand>(commands.commands_.front()).restoreSeal = seal;
                builtCommands = &commands;
            }
            return created;
        }

        RuntimeSceneService &owner;
        RuntimeScene &candidate;
        SceneRuntimeId runtime;
        bool builtGroup{};
        const SceneCommandBuffer *builtCommands{}; /**< Exact synchronous single-group batch; cleared before its only commit attempt. */
        std::shared_ptr<SceneCommandBuffer::RestoreGroupSeal> seal;
    };

    /** @copydoc RuntimeSceneService::IsRestoreCandidate */
    bool RuntimeSceneService::IsRestoreCandidate(const RuntimeScene &candidate) const noexcept {
        return std::this_thread::get_id() == restoreRetirementOwner_ && aggregateRestore_ && pending_.scene.get() == &candidate &&
               active_.scene.get() != &candidate;
    }

    /** @copydoc RuntimeSceneService::StageCandidate */
    Result<void> RuntimeSceneService::StageCandidate(const RuntimeSceneDefinition &definition, const RuntimeSceneConfig config,
                                                     const Assets::AssetRegistryRevision revision,
                                                     std::vector<RuntimeScene::ResolvedAsset> assets) {
        try {
            auto candidate = RuntimeScene::CreateResolved(definition, SceneRuntimeId{nextRuntimeId_}, config, revision, std::move(assets));
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            pending_.scene = std::move(candidate).Value();
            if (aggregateRestore_) {
                RestoreCandidateCommands commands{*this, *pending_.scene};
                if (const auto prepared = aggregateRestore_->PrepareScene(*pending_.scene, commands); prepared.HasError()) {
                    aggregateRestore_->Rollback();
                    pending_.scene.reset();
                    return prepared;
                }
            }
            if (const auto prepared = PrepareParticipants(definition); prepared.HasError()) {
                if (aggregateRestore_)
                    aggregateRestore_->Rollback();
                pending_.scene.reset();
                return Result<void>::Failure(prepared.ErrorValue());
            }
            // Only a complete private aggregate consumes its identity. Failed preparation may retry
            // the same unpublished ID; unsigned wrap after a successful maximum ID closes admission.
            ++nextRuntimeId_;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            if (aggregateRestore_)
                aggregateRestore_->Rollback();
            ScenePublicationDetail::ShutdownCandidates(pending_.candidates);
            pending_.scene.reset();
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene candidate allocation failed."));
        }
    }

    void RuntimeSceneService::AdvancePreparation(const CancellationToken &cancellation) {
        if (!preparation_)
            return;
        try {
            if (const Result<void> processed = ProcessCompletedPreparationLoads(cancellation); processed.HasError()) {
                const Error error = processed.ErrorValue();
                CancelPreparation(false);
                RejectPendingPublication(error);
                return;
            }
            if (preparation_->activeLoads != 0 || std::ranges::any_of(preparation_->entries, [this](const Preparation::Entry &entry) {
                return !entry.payload || entry.inspectedParticipants != participants_.size();
            }))
                return;
            if (const Result<void> finalized = FinalizePreparation(); finalized.HasError()) {
                const Error error = finalized.ErrorValue();
                CancelPreparation(false);
                RejectPendingPublication(error);
            }
        } catch (const std::bad_alloc &) {
            CancelPreparation(false);
            RejectPendingPublication(MakeError(SceneErrors::AssetBudgetExceeded, "Scene dependency preparation allocation failed."));
        }
    }

    void RuntimeSceneService::CancelPreparation(const bool waitForCompletion) noexcept {
        if (!preparation_)
            return;
        for (Preparation::Entry &entry : preparation_->entries)
            if (entry.load)
                static_cast<void>(entry.load->RequestCancel());
        if (waitForCompletion)
            for (Preparation::Entry &entry : preparation_->entries)
                if (entry.load)
                    static_cast<void>(entry.load->Wait());
        preparation_.reset();
    }

    Result<void> RuntimeSceneService::PrepareParticipantPhase(const RuntimeSceneDefinition &definition, const SceneActivationPhase phase) {
        using enum SceneCanonicalDatasetProjection;
        for (const auto &participant : participants_)
            if (participant->PreparationPhase() == phase)
                if (auto owned = participant->PrepareOwnedScene(*pending_.scene); owned.HasError())
                    return owned;
        for (const auto &participant : participants_) {
            if (participant->PreparationPhase() != phase)
                continue;
            auto prepared = participant->Prepare(definition, pending_.scene->View());
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (!prepared.Value())
                return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Participant returned a null candidate."));
            if (phase != SceneActivationPhase::InitialPlacement && !prepared.Value()->InitialPlacements().empty()) {
                prepared.Value()->Shutdown();
                return Result<void>::Failure(
                    MakeError(SceneErrors::InvalidCandidate, "Derived owner cannot introduce late initial placement."));
            }
            if (const auto datasets = prepared.Value()->CanonicalDatasetProjection(); datasets == PersistentWorld)
                pending_.datasets = PersistentWorld;
            else if (datasets != Absent && pending_.datasets != PersistentWorld)
                pending_.datasets = Unqualified;
            pending_.candidates.push_back(std::move(prepared).Value());
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::PrepareParticipants(const RuntimeSceneDefinition &definition) {
        ScenePublicationDetail::ShutdownCandidates(pending_.candidates);
        pending_.datasets = SceneCanonicalDatasetProjection::Absent;
        const auto reject = [&](Error error) {
            ScenePublicationDetail::ShutdownCandidates(pending_.candidates);
            return Result<void>::Failure(std::move(error));
        };
        try {
            pending_.candidates.reserve(participants_.size());
            if (auto owners = ValidateActivationOwners(definition, participants_); owners.HasError())
                return reject(owners.ErrorValue());
            if (auto phases = ValidateActivationPhases(participants_); phases.HasError())
                return reject(phases.ErrorValue());
            if (auto prepared = PrepareParticipantPhase(definition, SceneActivationPhase::InitialPlacement); prepared.HasError())
                return reject(prepared.ErrorValue());
            std::vector<SceneInitialPlacement> placements;
            for (const auto &candidate : pending_.candidates) {
                const auto projected = candidate->InitialPlacements();
                if (projected.size() > pending_.scene->View().SlotCount() - placements.size())
                    return reject(MakeError(SceneErrors::InvalidCandidate, "Initial placement exceeds candidate entity count."));
                placements.insert(placements.end(), projected.begin(), projected.end());
            }
            if (auto placed = pending_.scene->ApplyInitialPlacements(placements); placed.HasError())
                return reject(placed.ErrorValue());
            if (auto prepared = PrepareParticipantPhase(definition, SceneActivationPhase::DerivedResources); prepared.HasError())
                return reject(prepared.ErrorValue());
        } catch (const std::bad_alloc &) {
            return reject(MakeError(SceneErrors::InvalidCandidate, "Unable to retain the complete scene activation candidate set."));
        }
        return Result<void>::Success();
    }

}  // namespace Horo::Runtime
