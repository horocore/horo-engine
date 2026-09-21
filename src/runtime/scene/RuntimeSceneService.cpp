#include "Horo/Assets/AssetProvider.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"
#include "RuntimeSceneErrors.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &code, std::string message) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
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

        [[nodiscard]] Result<void> ValidatePreparation(const RuntimeSceneDefinition &definition, const RuntimeSceneConfig config,
                                                       const RuntimeSceneAssetLimits &limits, const std::uint64_t nextRuntimeId) {
            if (nextRuntimeId == 0)
                return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Runtime scene identity space is exhausted."));
            if (config.maximumGeneration == 0 || limits.maximumDependencies == 0 || limits.maximumConcurrentLoads == 0 ||
                limits.maximumResidentBytes == 0)
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
        };

        Preparation(RuntimeSceneDefinition source, const RuntimeSceneConfig sceneConfig, Assets::AssetRegistrySnapshot registrySnapshot)
            : definition(std::move(source)), config(sceneConfig), snapshot(std::move(registrySnapshot)) {}

        RuntimeSceneDefinition definition;
        RuntimeSceneConfig config;
        Assets::AssetRegistrySnapshot snapshot;
        std::vector<Entry> entries;
        std::size_t activeLoads{};
        std::size_t residentBytes{};
    };

    RuntimeSceneService::RuntimeSceneService() = default;

    RuntimeSceneService::RuntimeSceneService(Assets::AssetRegistry &registry, Assets::AssetLoadService &loads,
                                             const RuntimeSceneAssetLimits limits)
        : assetRegistry_(&registry), assetLoads_(&loads), assetLimits_(limits) {}

    RuntimeSceneService::~RuntimeSceneService() {
        Shutdown();
    }

    /** @copydoc RuntimeSceneService::AddActivationParticipant */
    Result<void> RuntimeSceneService::AddActivationParticipant(std::unique_ptr<SceneActivationParticipant> participant) {
        if (!participant)
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene activation participant is null."));
        if (started_ || shutdown_ || transition_ != TransitionKind::None || preparation_ || pending_.scene)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        participants_.push_back(std::move(participant));
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::QueuePreparation */
    Result<void> RuntimeSceneService::QueuePreparation(RuntimeSceneDefinition definition, const RuntimeSceneConfig config) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(SceneErrors::ServiceShutdown));
        if (transition_ != TransitionKind::None || structuralCommands_ || preparation_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        operationError_.reset();
        return BeginPreparation(std::move(definition), config);
    }

    Result<void> RuntimeSceneService::PopulatePreparationEntries(Preparation &prep, const RuntimeSceneDefinition &definition) const {
        prep.entries.reserve(definition.AssetDependencies().size());
        for (const SceneAssetDependency &dependency : definition.AssetDependencies()) {
            Preparation::Entry entry{dependency};
            if (active_.scene && active_.scene->assetRegistryRevision_ == prep.snapshot.Revision()) {
                const auto reusable =
                    std::ranges::find(active_.scene->assets_, dependency.id, [](const RuntimeScene::ResolvedAsset &asset) {
                    return asset.dependency.id;
                });
                if (reusable != active_.scene->assets_.end() && reusable->dependency.expectedType == dependency.expectedType)
                    entry.payload = reusable->payload;
            }
            if (entry.payload) {
                if (entry.payload->size() > assetLimits_.maximumResidentBytes - prep.residentBytes) {
                    return Result<void>::Failure(
                        WithAssetContext(MakeError(SceneErrors::AssetBudgetExceeded), definition.Id(), dependency.id));
                }
                prep.residentBytes += entry.payload->size();
            }
            prep.entries.push_back(std::move(entry));
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::BeginPreparation(RuntimeSceneDefinition definition, const RuntimeSceneConfig config) {
        if (const auto validation = ValidatePreparation(definition, config, assetLimits_, nextRuntimeId_); validation.HasError())
            return validation;

        if (definition.AssetDependencies().empty()) {
            Result<std::unique_ptr<RuntimeScene>> candidate =
                RuntimeScene::CreateResolved(definition, SceneRuntimeId{nextRuntimeId_}, config, {}, {});
            if (candidate.HasError())
                return Result<void>::Failure(candidate.ErrorValue());
            ++nextRuntimeId_;
            pending_.scene = std::move(candidate).Value();
            if (const Result<void> prepared = PrepareParticipants(definition); prepared.HasError()) {
                pending_.scene.reset();
                return prepared;
            }
            transition_ = TransitionKind::Activate;
            return Result<void>::Success();
        }
        if (!assetRegistry_ || !assetLoads_)
            return Result<void>::Failure(MakeError(SceneErrors::AssetServicesUnavailable));

        Assets::AssetRegistrySnapshot snapshot = assetRegistry_->Snapshot();
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
    }

    /** @copydoc RuntimeSceneService::QueueUnload */
    Result<void> RuntimeSceneService::QueueUnload() {
        using enum TransitionKind;
        if (transition_ == Unload)
            return Result<void>::Success();
        if (structuralCommands_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        CancelPreparation(false);
        ShutdownCandidates(pending_.candidates);
        pending_.scene.reset();
        transition_ = None;
        if (!active_.scene)
            return Result<void>::Success();
        transition_ = Unload;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::QueueStructuralCommands */
    Result<void> RuntimeSceneService::QueueStructuralCommands(SceneCommandBuffer commands) {
        if (!active_.scene)
            return Result<void>::Failure(MakeError(SceneErrors::NoActiveScene));
        if (transition_ != TransitionKind::None || structuralCommands_ || preparation_)
            return Result<void>::Failure(MakeError(SceneErrors::OperationInProgress));
        if (!commands.Empty())
            structuralCommands_ = std::move(commands);
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
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Scene service startup was cancelled."));
        started_ = true;
        shutdown_ = false;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::OnPhase */
    Result<void> RuntimeSceneService::OnPhase(const RuntimePhase phase, const FrameContext &) {
        if (phase == RuntimePhase::CommitDeferredLifecycleChanges) {
            AdvancePreparation();
            return CommitDeferredChanges();
        }
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::OnFixedUpdate */
    Result<void> RuntimeSceneService::OnFixedUpdate(const FixedStepContext &) {
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSceneService::Shutdown */
    void RuntimeSceneService::Shutdown() noexcept {
        CancelPreparation(true);
        structuralCommands_.reset();
        structuralResult_.reset();
        operationError_.reset();
        ShutdownCandidates(pending_.candidates);
        pending_.scene.reset();
        ShutdownCandidates(active_.candidates);
        active_.scene.reset();
        transition_ = TransitionKind::None;
        started_ = false;
        shutdown_ = true;
    }

    Result<void> RuntimeSceneService::SubmitPreparationLoads() {
        if (!preparation_ || !assetLoads_)
            return Result<void>::Success();
        for (Preparation::Entry &entry : preparation_->entries) {
            if (preparation_->activeLoads >= assetLimits_.maximumConcurrentLoads)
                break;
            if (entry.payload || entry.load)
                continue;
            Result<Assets::AssetLoadHandle> submitted = assetLoads_->LoadAsync(preparation_->snapshot, entry.dependency.id);
            if (submitted.HasError())
                return Result<void>::Failure(WithAssetContext(submitted.ErrorValue(), preparation_->definition.Id(), entry.dependency.id));
            entry.load = std::move(submitted).Value();
            ++preparation_->activeLoads;
        }
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::ProcessCompletedPreparationLoads() {
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
            if (result.bytes.size() > assetLimits_.maximumResidentBytes - preparation_->residentBytes)
                return Result<void>::Failure(
                    WithAssetContext(MakeError(SceneErrors::AssetBudgetExceeded), preparation_->definition.Id(), entry.dependency.id));
            preparation_->residentBytes += result.bytes.size();
            entry.payload = std::make_shared<const std::vector<std::uint8_t>>(std::move(result.bytes));
        }
        return SubmitPreparationLoads();
    }

    Result<void> RuntimeSceneService::FinalizePreparation() {
        if (!assetRegistry_ || assetRegistry_->Snapshot().Revision() != preparation_->snapshot.Revision()) {
            Error error = MakeError(SceneErrors::AssetRevisionStale);
            error.diagnostics.emplace_back(DiagnosticCode{"scene.asset.registry_revision"}, DiagnosticSeverity::Note,
                                           "The authoritative registry revision changed before activation.", SourceLocation{});
            return Result<void>::Failure(std::move(error));
        }

        std::vector<RuntimeScene::ResolvedAsset> assets;
        assets.reserve(preparation_->entries.size());
        for (Preparation::Entry &entry : preparation_->entries)
            assets.emplace_back(std::move(entry.dependency), std::move(entry.payload));
        Result<std::unique_ptr<RuntimeScene>> candidate =
            RuntimeScene::CreateResolved(preparation_->definition, SceneRuntimeId{nextRuntimeId_}, preparation_->config,
                                         preparation_->snapshot.Revision(), std::move(assets));
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        ++nextRuntimeId_;
        pending_.scene = std::move(candidate).Value();
        if (const Result<void> prepared = PrepareParticipants(preparation_->definition); prepared.HasError()) {
            pending_.scene.reset();
            return Result<void>::Failure(prepared.ErrorValue());
        }
        preparation_.reset();
        transition_ = TransitionKind::Activate;
        return Result<void>::Success();
    }

    void RuntimeSceneService::AdvancePreparation() {
        if (!preparation_)
            return;
        if (const Result<void> processed = ProcessCompletedPreparationLoads(); processed.HasError()) {
            operationError_ = processed.ErrorValue();
            CancelPreparation(false);
            return;
        }
        if (preparation_->activeLoads != 0 || std::ranges::any_of(preparation_->entries, [](const Preparation::Entry &entry) {
            return !entry.payload;
        }))
            return;
        if (const Result<void> finalized = FinalizePreparation(); finalized.HasError()) {
            operationError_ = finalized.ErrorValue();
            CancelPreparation(false);
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

    Result<void> RuntimeSceneService::CommitDeferredChanges() {
        using enum TransitionKind;
        if (structuralCommands_) {
            Result<StructuralCommitResult> committed = active_.scene->Commit(*structuralCommands_);
            structuralCommands_.reset();
            if (committed.HasError())
                operationError_ = committed.ErrorValue();
            else
                structuralResult_ = std::move(committed).Value();
        }
        if (transition_ == Activate) {
            for (const auto &candidate : pending_.candidates) {
                if (const Result<void> result = candidate->ValidatePublication(); result.HasError()) {
                    operationError_ = result.ErrorValue();
                    ShutdownCandidates(pending_.candidates);
                    pending_.scene.reset();
                    transition_ = None;
                    return Result<void>::Success();
                }
            }
            for (const auto &candidate : pending_.candidates)
                candidate->Publish();
            SceneAggregate retired = std::move(active_);
            active_ = std::move(pending_);
            ShutdownCandidates(retired.candidates);
            retired.scene.reset();
        } else if (transition_ == Unload) {
            SceneAggregate retired = std::move(active_);
            ShutdownCandidates(retired.candidates);
            retired.scene.reset();
        }
        transition_ = None;
        return Result<void>::Success();
    }

    Result<void> RuntimeSceneService::PrepareParticipants(const RuntimeSceneDefinition &definition) {
        ShutdownCandidates(pending_.candidates);
        try {
            pending_.candidates.reserve(participants_.size());
            for (const auto &participant : participants_) {
                auto prepared = participant->Prepare(definition, pending_.scene->View());
                if (prepared.HasError()) {
                    ShutdownCandidates(pending_.candidates);
                    return Result<void>::Failure(prepared.ErrorValue());
                }
                if (!prepared.Value()) {
                    ShutdownCandidates(pending_.candidates);
                    return Result<void>::Failure(MakeError(SceneErrors::InvalidCandidate, "Participant returned a null candidate."));
                }
                pending_.candidates.push_back(std::move(prepared).Value());
            }
        } catch (const std::bad_alloc &) {
            ShutdownCandidates(pending_.candidates);
            return Result<void>::Failure(
                MakeError(SceneErrors::InvalidCandidate, "Unable to retain the complete scene activation candidate set."));
        }
        return Result<void>::Success();
    }

    void RuntimeSceneService::ShutdownCandidates(std::vector<std::unique_ptr<SceneActivationCandidate>> &candidates) noexcept {
        for (auto candidate = candidates.rbegin(); candidate != candidates.rend(); ++candidate)
            (*candidate)->Shutdown();
        candidates.clear();
    }

}  // namespace Horo::Runtime
