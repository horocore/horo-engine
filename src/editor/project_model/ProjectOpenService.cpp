#include "Horo/Editor/ProjectOpenService.h"

#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectMigrationCatalog.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "editor/EditorServiceErrors.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/project_model/ProjectMetadata.h"
#include "editor/project_model/RendererAvailability.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstring>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <system_error>
#include <utility>

namespace Horo::Editor {
    namespace {
        using namespace Application;

        [[nodiscard]] Error OpenError(const ErrorCodeDescriptor &code, std::string message = {}) {
            return MakeError(code, std::move(message));
        }

        [[nodiscard]] float PhaseProgress(const ProjectOpenPhase phase) noexcept {
            using enum ProjectOpenPhase;
            switch (phase) {
                case Inspecting:
                    return 0.05F;
                case CleaningRecovery:
                    return 0.12F;
                case Recovering:
                    return 0.20F;
                case ValidatingCompatibility:
                    return 0.30F;
                case PlanningMigration:
                    return 0.40F;
                case Migrating:
                    return 0.62F;
                case UpdatingProjectMetadata:
                    return 0.68F;
                case ValidatingDefaultScene:
                    return 0.74F;
                case RebuildingDerivedState:
                    return 0.80F;
                case RendererPreflight:
                    return 0.88F;
                case PreparingWorkspace:
                    return 0.96F;
                case ReadyToActivate:
                case RequiresRendererRestart:
                case Failed:
                case Cancelled:
                    return 1.0F;
            }
            return 0.0F;
        }

        [[nodiscard]] std::vector<std::byte> Bytes(const std::string_view text) {
            std::vector<std::byte> result(text.size());
            std::memcpy(result.data(), text.data(), text.size());
            return result;
        }

        [[nodiscard]] std::string SourceFingerprint(const std::filesystem::path &projectRoot) {
            const auto path = projectRoot / ".horo/project.json";
            std::ifstream input(path, std::ios::binary | std::ios::ate);
            if (!input)
                return {};
            const std::streamsize size = input.tellg();
            if (size <= 0 || size > 64 * 1024)
                return {};
            std::string contents(static_cast<std::size_t>(size), '\0');
            input.seekg(0);
            input.read(contents.data(), size);
            return input ? contents : std::string{};
        }

        [[nodiscard]] Result<void> UpdatePatchMarker(DurableFileSystem &files, const ProjectMutationCoordinator &mutations,
                                                     const std::filesystem::path &projectRoot, const ReleaseCompatibilityDecision &target,
                                                     const ProjectOpenOperationId operation) {
            if (auto lease =
                    mutations.TryAcquire({projectRoot, ProjectMutationOwner::Migration, std::format("project-open-{}", operation.value)});
                lease.HasError()) {
                return Result<void>::Failure(lease.ErrorValue());
            }

            const auto metadataPath = projectRoot / ".horo/project.json";
            std::ifstream stream(metadataPath, std::ios::binary);
            if (!stream)
                return Result<void>::Failure(OpenError(ProjectOpenErrors::MetadataUpdateFailed));
            nlohmann::json metadata;
            try {
                stream >> metadata;
            } catch (const nlohmann::json::exception &) {
                return Result<void>::Failure(OpenError(ProjectOpenErrors::MetadataUpdateFailed));
            }
            metadata["horoVersion"] = FormatHoroVersion(target.release.value);
            metadata["persistentContract"] = FormatPersistentContractHash(target.persistentContract);
            metadata.erase("compatibilityProof");
            const std::string serialized = metadata.dump(2) + "\n";
            const auto temporary = projectRoot / ".horo/project.json.open.tmp";
            if (auto written = files.WriteDurable(temporary, Bytes(serialized)); written.HasError())
                return written;
            return files.AtomicReplace(temporary, metadataPath);
        }

        struct PreparedDerivedState {
            std::string contributorId;
            std::unique_ptr<IPreparedProjectOpenDerivedState> candidate;
        };

        struct BackgroundResult {
            ProjectMetadata metadata;
            std::vector<PreparedDerivedState> derived;
            bool cancellationDeferred{};
        };

        struct BackgroundCompletion {
            std::mutex mutex;
            std::optional<BackgroundResult> result;
            std::optional<Error> error;
            std::atomic<ProjectOpenPhase> phase{ProjectOpenPhase::Inspecting};
        };

        void SetPhase(const std::shared_ptr<BackgroundCompletion> &completion, const ProjectOpenPhase phase) noexcept {
            completion->phase.store(phase, std::memory_order::seq_cst);
        }
    }  // namespace

    struct ProjectSessionActivationLease::State {
        enum class Status : std::uint8_t {
            Empty,
            Ready,
            Reserved,
            Consumed
        };

        std::mutex mutex;
        Status status{Status::Empty};
        std::optional<ProjectSessionCandidate> candidate;
        bool shutdown{};
    };

    struct ProjectOpenService::State {
        struct Operation {
            ProjectOpenProgressSnapshot snapshot;
            ProjectOpenRequest request;
            CancellationSource cancellation;
            std::shared_ptr<BackgroundCompletion> completion;
            std::optional<JobHandle> job;
        };

        JobSystem &jobs;
        DurableFileSystem &files;
        ProjectOpenPreflightService &preflight;
        ProjectMutationCoordinator &mutations;
        ProjectMigrationTransactionService &transactions;
        const RendererAvailabilitySnapshot &rendererAvailability;
        std::vector<IProjectOpenDerivedStateContributor *> derivedContributors;
        std::optional<Operation> operation;
        std::shared_ptr<ProjectSessionActivationLease::State> sessions{std::make_shared<ProjectSessionActivationLease::State>()};
        std::uint64_t nextOperation{1};
        std::uint64_t nextSession{1};
        bool shutdown{};
    };

    namespace {
        struct CompatibilityResolution {
            ProjectCompatibilitySnapshot snapshot;
            bool cancellationDeferred{};
        };

        template <typename StateType>
        [[nodiscard]] Result<CompatibilityResolution> ResolveOpenCompatibility(StateType &state,
                                                                               const std::shared_ptr<BackgroundCompletion> &completion,
                                                                               const ProjectOpenRequest &request,
                                                                               const ProjectOpenOperationId id,
                                                                               const CancellationToken &cancellation) {
            SetPhase(completion, ProjectOpenPhase::ValidatingCompatibility);
            ProjectOpenPreflightSnapshot preflight = state.preflight.Inspect(request.projectRoot);
            ProjectCompatibilitySnapshot compatibility = preflight.compatibility;
            LOG_DEBUG("editor.project_open", "Compatibility classified operation=%llu status=%d migration_plan=%s.",
                      static_cast<unsigned long long>(id.value), static_cast<int>(compatibility.status),
                      preflight.migrationPlan.has_value() ? "yes" : "no");
            bool cancellationDeferred{};
            if (compatibility.status == ProjectCompatibilityStatus::CompatibleReleaseLine && compatibility.markerUpdateRequired) {
                SetPhase(completion, ProjectOpenPhase::UpdatingProjectMetadata);
                const auto *target = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
                if (target == nullptr)
                    return Result<CompatibilityResolution>::Failure(OpenError(ProjectOpenErrors::MigrationPlanMissing));
                if (auto updated = UpdatePatchMarker(state.files, state.mutations, request.projectRoot, *target, id); updated.HasError())
                    return Result<CompatibilityResolution>::Failure(updated.ErrorValue());
                compatibility = state.preflight.Inspect(request.projectRoot).compatibility;
            } else if (compatibility.status == ProjectCompatibilityStatus::AutomaticMigrationRequired) {
                SetPhase(completion, ProjectOpenPhase::PlanningMigration);
                if (!preflight.migrationPlan.has_value() || !compatibility.sourceBaseline.has_value() ||
                    !compatibility.metadata.has_value())
                    return Result<CompatibilityResolution>::Failure(
                        compatibility.diagnostic.value_or(OpenError(ProjectOpenErrors::MigrationPlanMissing)));
                const auto *target = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
                if (target == nullptr)
                    return Result<CompatibilityResolution>::Failure(OpenError(ProjectOpenErrors::MigrationPlanMissing));
                ProjectMigrationPlan plan = std::move(*preflight.migrationPlan);
                const std::string sourceVersion = FormatHoroVersion(compatibility.metadata->horoVersion.value);
                const std::string targetVersion = FormatHoroVersion(target->release.value);
                LOG_INFO("editor.project_open", "Automatic migration selected operation=%llu source=%s target=%s.",
                         static_cast<unsigned long long>(id.value), sourceVersion.c_str(), targetVersion.c_str());
                LOG_DEBUG("editor.project_open", "Migration selection aggregate operation=%llu definitions=%zu.",
                          static_cast<unsigned long long>(id.value), plan.definitions.size());
                SetPhase(completion, ProjectOpenPhase::Migrating);
                if (state.jobs.WorkerCount() < 2)
                    return Result<CompatibilityResolution>::Failure(OpenError(ProjectOpenErrors::WorkerCapacityInsufficient));
                ProjectMigrationTransactionRequest transaction{.projectRoot = request.projectRoot,
                                                               .sourceMetadata = *compatibility.metadata,
                                                               .sourceBaseline = *compatibility.sourceBaseline,
                                                               .targetDecision = *target,
                                                               .plan = plan,
                                                               .engineBuildIdentity = request.engineBuildIdentity,
                                                               .limits = request.migrationLimits,
                                                               .cancellation = cancellation};
                auto migrated = state.transactions.Execute(transaction);
                if (migrated.HasError())
                    return Result<CompatibilityResolution>::Failure(migrated.ErrorValue());
                cancellationDeferred = migrated.Value().cancellationDeferred;
                compatibility = state.preflight.Inspect(request.projectRoot).compatibility;
            } else if (compatibility.status != ProjectCompatibilityStatus::Current &&
                       compatibility.status != ProjectCompatibilityStatus::CompatibleReleaseLine) {
                return Result<CompatibilityResolution>::Failure(
                    compatibility.diagnostic.value_or(OpenError(ProjectOpenErrors::CompatibilityBlocked)));
            }

            if (!compatibility.metadata.has_value())
                return Result<CompatibilityResolution>::Failure(OpenError(ProjectOpenErrors::CompatibilityBlocked));
            if (cancellation.IsCancellationRequested() && !cancellationDeferred)
                return Result<CompatibilityResolution>::Failure(OpenError(ProjectOpenErrors::Cancelled));
            return Result<CompatibilityResolution>::Success({std::move(compatibility), cancellationDeferred});
        }

        template <typename StateType>
        [[nodiscard]] Result<void> RunProjectOpenBackground(StateType &state, const std::shared_ptr<BackgroundCompletion> &completion,
                                                            const ProjectOpenRequest &request, const ProjectOpenOperationId id,
                                                            const CancellationToken &cancellation) {
            const auto fail = [&completion, id](Error error) {
                LOG_ERROR("editor.project_open", "Project open failed operation=%llu code=%s.", static_cast<unsigned long long>(id.value),
                          error.code.Value().c_str());
                std::lock_guard lock(completion->mutex);
                completion->error = error;
                if (ErrorChainContains(error, ProjectOpenErrors::Cancelled.domain, ProjectOpenErrors::Cancelled.code))
                    return JobCancelled(std::move(error));
                return Result<void>::Failure(std::move(error));
            };

            SetPhase(completion, ProjectOpenPhase::CleaningRecovery);
            // Cleanup is non-authoritative; a failure remains a warning and must not block open.
            static_cast<void>(state.transactions.CleanupCommittedMigrations(request.projectRoot));
            if (state.transactions.InspectPendingRecovery(request.projectRoot).action != MigrationRecoveryAction::None) {
                SetPhase(completion, ProjectOpenPhase::Recovering);
                if (auto recovered = state.transactions.Recover(request.projectRoot, cancellation); recovered.HasError())
                    return fail(recovered.ErrorValue());
            }
            if (cancellation.IsCancellationRequested())
                return fail(OpenError(ProjectOpenErrors::Cancelled));

            auto resolved = ResolveOpenCompatibility(state, completion, request, id, cancellation);
            if (resolved.HasError())
                return fail(resolved.ErrorValue());
            CompatibilityResolution compatibility = std::move(resolved).Value();

            SetPhase(completion, ProjectOpenPhase::ValidatingDefaultScene);
            auto defaultScene = LoadProjectDefaultScene(request.projectRoot);
            if (defaultScene.HasError())
                return fail(OpenError(ProjectOpenErrors::ScenePreflightFailed, defaultScene.ErrorValue().message));
            if (defaultScene.Value().has_value()) {
                if (!defaultScene.Value()->existed)
                    return fail(OpenError(ProjectOpenErrors::ScenePreflightFailed, "The configured project default scene does not exist."));
                SceneDocument sceneValidation;
                if (auto validated = sceneValidation.LoadSaved(defaultScene.Value()->objects, defaultScene.Value()->prefabInstances);
                    validated.HasError())
                    return fail(OpenError(ProjectOpenErrors::ScenePreflightFailed, validated.ErrorValue().message));
            }
            if (cancellation.IsCancellationRequested() && !compatibility.cancellationDeferred)
                return fail(OpenError(ProjectOpenErrors::Cancelled));

            SetPhase(completion, ProjectOpenPhase::RebuildingDerivedState);
            BackgroundResult result{.metadata = *compatibility.snapshot.metadata,
                                    .cancellationDeferred = compatibility.cancellationDeferred};
            result.derived.reserve(state.derivedContributors.size());
            for (IProjectOpenDerivedStateContributor *contributor : state.derivedContributors) {
                auto prepared = contributor->Prepare(request.projectRoot, cancellation);
                if (prepared.HasError())
                    return fail(OpenError(ProjectOpenErrors::DerivedStateFailed,
                                          std::string(contributor->Id()) + ": " + prepared.ErrorValue().message));
                result.derived.emplace_back(std::string(contributor->Id()), std::move(prepared).Value());
            }
            std::lock_guard lock(completion->mutex);
            completion->result.emplace(std::move(result));
            return Result<void>::Success();
        }
    }  // namespace

    ProjectSessionActivationLease::ProjectSessionActivationLease(std::shared_ptr<State> state, const ProjectSessionCandidateId id) noexcept
        : state_(std::move(state)), id_(id) {}

    ProjectSessionActivationLease::ProjectSessionActivationLease(ProjectSessionActivationLease &&other) noexcept
        : state_(std::move(other.state_)), id_(other.id_), committed_(other.committed_) {
        other.committed_ = true;
    }

    ProjectSessionActivationLease &ProjectSessionActivationLease::operator=(ProjectSessionActivationLease &&other) noexcept {
        if (this != &other) {
            Release();
            state_ = std::move(other.state_);
            id_ = other.id_;
            committed_ = other.committed_;
            other.committed_ = true;
        }
        return *this;
    }

    ProjectSessionActivationLease::~ProjectSessionActivationLease() {
        Release();
    }

    const ProjectSessionCandidate &ProjectSessionActivationLease::Candidate() const noexcept {
        assert(state_ && state_->candidate.has_value() && state_->candidate->id == id_);
        return *state_->candidate;
    }

    Result<void> ProjectSessionActivationLease::Commit() {
        if (!state_)
            return Result<void>::Failure(OpenError(ProjectOpenErrors::SessionStale));
        std::lock_guard lock(state_->mutex);
        if (state_->status != State::Status::Reserved || !state_->candidate.has_value() || state_->candidate->id != id_)
            return Result<void>::Failure(OpenError(ProjectOpenErrors::SessionStale));
        state_->status = State::Status::Consumed;
        committed_ = true;
        return Result<void>::Success();
    }

    void ProjectSessionActivationLease::Release() noexcept {
        if (!state_ || committed_)
            return;
        std::lock_guard lock(state_->mutex);
        if (!state_->shutdown && state_->status == State::Status::Reserved && state_->candidate.has_value() && state_->candidate->id == id_)
            state_->status = State::Status::Ready;
        committed_ = true;
    }

    /** @copydoc ProjectOpenPreflightService::ProjectOpenPreflightService */
    ProjectOpenPreflightService::ProjectOpenPreflightService(ProjectMigrationTransactionService &transactions)
        : transactions_(transactions) {
        if (auto catalog = BuildBuiltInProjectMigrationCatalog(); catalog.HasError())
            catalogError_ = catalog.ErrorValue();
        else {
            if (auto registry = ProjectMigrationRegistry::Create(catalog.Value()); registry.HasError())
                catalogError_ = registry.ErrorValue();
            else
                registry_.emplace(std::move(registry).Value());
        }
        if (auto support = BuildBuiltInProjectMigrationSupportDescriptor(); support.HasError())
            catalogError_ = support.ErrorValue();
        else
            support_.emplace(std::move(support).Value());
    }

    /** @copydoc ProjectOpenPreflightService::Inspect */
    ProjectOpenPreflightSnapshot ProjectOpenPreflightService::Inspect(const std::filesystem::path &projectRoot) const {
        using enum ProjectCompatibilityStatus;
        ProjectOpenPreflightSnapshot snapshot{.compatibility = InspectProjectCompatibility(projectRoot),
                                              .recovery = transactions_.InspectPendingRecovery(projectRoot)};
        if (snapshot.recovery.action != MigrationRecoveryAction::None) {
            snapshot.compatibility.status = RecoveryRequired;
            snapshot.compatibility.diagnostic = snapshot.recovery.diagnostic;
            return snapshot;
        }
        if ((snapshot.compatibility.status == MigrationPathMissing || snapshot.compatibility.status == AutomaticMigrationRequired) &&
            snapshot.compatibility.metadata.has_value() && snapshot.compatibility.sourceBaseline.has_value()) {
            if (!registry_.has_value() || !support_.has_value()) {
                snapshot.compatibility.status = MigrationPathMissing;
                snapshot.compatibility.diagnostic = catalogError_;
                return snapshot;
            }
            auto plan =
                registry_->Plan(*snapshot.compatibility.sourceBaseline, snapshot.compatibility.metadata->persistentContract, *support_);
            if (plan.HasValue()) {
                snapshot.compatibility.status = AutomaticMigrationRequired;
                snapshot.migrationPlan.emplace(std::move(plan).Value());
            } else {
                snapshot.compatibility.status = plan.ErrorValue().code.Value() == "project.migration.provider_missing"
                                                    ? RequiredProviderUnavailable
                                                    : MigrationPathMissing;
                snapshot.compatibility.diagnostic = plan.ErrorValue();
            }
        }
        return snapshot;
    }

    /** @copydoc ProjectOpenService::ProjectOpenService */
    ProjectOpenService::ProjectOpenService(JobSystem &jobs, DurableFileSystem &files, ProjectOpenPreflightService &preflight,
                                           ProjectMutationCoordinator &mutations, ProjectMigrationTransactionService &transactions,
                                           const RendererAvailabilitySnapshot &rendererAvailability,
                                           const std::span<IProjectOpenDerivedStateContributor *const> contributors) {
        state_ = std::make_unique<State>(
            State{jobs, files, preflight, mutations, transactions, rendererAvailability, {contributors.begin(), contributors.end()}});
    }

    ProjectOpenService::~ProjectOpenService() {
        Shutdown();
    }

    /** @copydoc ProjectOpenService::Start */
    Result<ProjectOpenOperationHandle> ProjectOpenService::Start(ProjectOpenRequest request) {
        if (state_->shutdown || (state_->operation.has_value() && state_->operation->snapshot.outcome == ProjectOpenOutcome::Running))
            return Result<ProjectOpenOperationHandle>::Failure(OpenError(ProjectOpenErrors::Busy));
        {
            using enum ProjectSessionActivationLease::State::Status;
            std::lock_guard lock(state_->sessions->mutex);
            if (state_->sessions->status == Ready || state_->sessions->status == Reserved)
                return Result<ProjectOpenOperationHandle>::Failure(OpenError(ProjectOpenErrors::Busy));
            state_->sessions->candidate.reset();
            state_->sessions->status = Empty;
        }

        const ProjectOpenOperationId id{state_->nextOperation++};
        LOG_INFO("editor.project_open", "Project open admitted operation=%llu.", static_cast<unsigned long long>(id.value));
        ProjectOpenProgressSnapshot snapshot{.operationId = id,
                                             .phase = ProjectOpenPhase::Inspecting,
                                             .outcome = ProjectOpenOutcome::Running,
                                             .progress = PhaseProgress(ProjectOpenPhase::Inspecting),
                                             .projectRoot = request.projectRoot,
                                             .projectName = request.expectedProjectName};
        auto completion = std::make_shared<BackgroundCompletion>();
        State::Operation operation{std::move(snapshot), std::move(request), {}, completion};

        const auto requestCopy = operation.request;
        const CancellationToken cancellation = operation.cancellation.Token();
        auto submitted =
            state_->jobs.SubmitResult(JobDescriptor{.parentCancellation = cancellation},
                                      [state = state_.get(), completion, requestCopy, id](const CancellationToken &jobCancellation) {
            return RunProjectOpenBackground(*state, completion, requestCopy, id, jobCancellation);
        });
        if (submitted.HasError())
            return Result<ProjectOpenOperationHandle>::Failure(submitted.ErrorValue());
        operation.job.emplace(std::move(submitted).Value());
        state_->operation.emplace(std::move(operation));
        return Result<ProjectOpenOperationHandle>::Success(ProjectOpenOperationHandle{id});
    }

    /** @copydoc ProjectOpenService::Query */
    std::optional<ProjectOpenProgressSnapshot> ProjectOpenService::Query(const ProjectOpenOperationId operation) const {
        if (!state_->operation.has_value() || state_->operation->snapshot.operationId != operation)
            return std::nullopt;
        return state_->operation->snapshot;
    }

    /** @copydoc ProjectOpenService::RequestCancel */
    Result<void> ProjectOpenService::RequestCancel(const ProjectOpenOperationId operation) {
        if (!state_->operation.has_value() || state_->operation->snapshot.operationId != operation)
            return Result<void>::Failure(OpenError(ProjectOpenErrors::NotFound));
        if (state_->operation->snapshot.outcome != ProjectOpenOutcome::Running)
            return Result<void>::Success();
        state_->operation->cancellation.RequestCancellation();
        LOG_WARN("editor.project_open", "Project open cancellation requested operation=%llu.",
                 static_cast<unsigned long long>(operation.value));
        if (state_->operation->job.has_value())
            static_cast<void>(state_->jobs.RequestCancel(state_->operation->job->Id()));
        return Result<void>::Success();
    }

    Result<ProjectSessionActivationLease> ProjectOpenService::ReserveSession(const ProjectSessionCandidateId session) const {
        std::lock_guard lock(state_->sessions->mutex);
        if (state_->sessions->shutdown || state_->sessions->status != ProjectSessionActivationLease::State::Status::Ready ||
            !state_->sessions->candidate.has_value() || state_->sessions->candidate->id != session ||
            SourceFingerprint(state_->sessions->candidate->projectRoot) != state_->sessions->candidate->sourceFingerprint)
            return Result<ProjectSessionActivationLease>::Failure(OpenError(ProjectOpenErrors::SessionStale));
        state_->sessions->status = ProjectSessionActivationLease::State::Status::Reserved;
        return Result<ProjectSessionActivationLease>::Success(ProjectSessionActivationLease{state_->sessions, session});
    }

    Result<void> ProjectOpenService::DiscardSession(const ProjectSessionCandidateId session) const {
        std::lock_guard lock(state_->sessions->mutex);
        if (!state_->sessions->candidate.has_value() || state_->sessions->candidate->id != session ||
            state_->sessions->status == ProjectSessionActivationLease::State::Status::Reserved)
            return Result<void>::Failure(OpenError(ProjectOpenErrors::SessionStale));
        state_->sessions->candidate.reset();
        state_->sessions->status = ProjectSessionActivationLease::State::Status::Empty;
        return Result<void>::Success();
    }

    void ProjectOpenService::PumpOwnerThread() {
        if (!state_->operation.has_value() || state_->operation->snapshot.outcome != ProjectOpenOutcome::Running)
            return;
        auto &operation = *state_->operation;
        auto &snapshot = operation.snapshot;
        snapshot.phase = operation.completion->phase.load(std::memory_order::seq_cst);
        snapshot.progress = std::max(snapshot.progress, PhaseProgress(snapshot.phase));
        if (!operation.job.has_value())
            return;
        const JobSnapshot job = state_->jobs.Query(operation.job->Id());
        if (job.state == JobState::Queued || job.state == JobState::Running)
            return;

        std::optional<BackgroundResult> result;
        std::optional<Error> error;
        {
            std::lock_guard lock(operation.completion->mutex);
            result = std::move(operation.completion->result);
            error = operation.completion->error;
        }
        if (!result.has_value()) {
            snapshot.phase =
                operation.cancellation.Token().IsCancellationRequested() ? ProjectOpenPhase::Cancelled : ProjectOpenPhase::Failed;
            snapshot.outcome =
                operation.cancellation.Token().IsCancellationRequested() ? ProjectOpenOutcome::Cancelled : ProjectOpenOutcome::Failed;
            snapshot.progress = 1.0F;
            if (error.has_value())
                snapshot.diagnostic = std::move(error);
            else
                snapshot.diagnostic = job.error;
            if (snapshot.outcome == ProjectOpenOutcome::Cancelled)
                LOG_WARN("editor.project_open", "Project open cancelled operation=%llu.",
                         static_cast<unsigned long long>(snapshot.operationId.value));
            return;
        }
        snapshot.projectName = result->metadata.name;
        snapshot.cancellationDeferred = result->cancellationDeferred;
        if (result->cancellationDeferred || operation.cancellation.Token().IsCancellationRequested()) {
            snapshot.phase = ProjectOpenPhase::Cancelled;
            snapshot.outcome = ProjectOpenOutcome::Cancelled;
            snapshot.progress = 1.0F;
            LOG_WARN("editor.project_open", "Project open cancelled after safe boundary operation=%llu deferred=%s.",
                     static_cast<unsigned long long>(snapshot.operationId.value), result->cancellationDeferred ? "yes" : "no");
            return;
        }

        snapshot.phase = ProjectOpenPhase::RebuildingDerivedState;
        std::vector<std::string> revisions;
        revisions.reserve(result->derived.size());
        for (auto &prepared : result->derived) {
            auto installed = prepared.candidate->Install();
            if (installed.HasError()) {
                snapshot.phase = ProjectOpenPhase::Failed;
                snapshot.outcome = ProjectOpenOutcome::Failed;
                snapshot.progress = 1.0F;
                snapshot.diagnostic =
                    OpenError(ProjectOpenErrors::DerivedStateFailed, prepared.contributorId + ": " + installed.ErrorValue().message);
                return;
            }
            revisions.push_back(prepared.contributorId + "@" + installed.Value());
        }

        snapshot.phase = ProjectOpenPhase::RendererPreflight;
        snapshot.progress = std::max(snapshot.progress, PhaseProgress(snapshot.phase));
        const ProjectOpenPreflight preflight = PreflightProjectOpen(operation.request.projectRoot, state_->rendererAvailability);
        snapshot.requiredRendererBackend = preflight.requiredBackendId;
        if (preflight.status == ProjectOpenPreflightStatus::RequiresRendererRestart) {
            snapshot.phase = ProjectOpenPhase::RequiresRendererRestart;
            snapshot.outcome = ProjectOpenOutcome::RequiresRendererRestart;
            snapshot.progress = 1.0F;
            LOG_WARN("editor.project_open", "Project open requires renderer restart operation=%llu backend=%s.",
                     static_cast<unsigned long long>(snapshot.operationId.value), snapshot.requiredRendererBackend.c_str());
            return;
        }
        if (preflight.status != ProjectOpenPreflightStatus::Ready) {
            snapshot.phase = ProjectOpenPhase::Failed;
            snapshot.outcome = ProjectOpenOutcome::Failed;
            snapshot.progress = 1.0F;
            snapshot.diagnostic = OpenError(ProjectOpenErrors::CompatibilityBlocked, preflight.diagnostic);
            return;
        }

        snapshot.phase = ProjectOpenPhase::PreparingWorkspace;
        const std::string fingerprint = SourceFingerprint(operation.request.projectRoot);
        if (fingerprint.empty()) {
            snapshot.phase = ProjectOpenPhase::Failed;
            snapshot.outcome = ProjectOpenOutcome::Failed;
            snapshot.progress = 1.0F;
            snapshot.diagnostic = OpenError(ProjectOpenErrors::SessionStale);
            return;
        }
        const ProjectSessionCandidateId sessionId{state_->nextSession++};
        {
            std::lock_guard lock(state_->sessions->mutex);
            state_->sessions->candidate = ProjectSessionCandidate{sessionId,
                                                                  snapshot.operationId,
                                                                  operation.request.projectRoot,
                                                                  snapshot.projectName,
                                                                  snapshot.requiredRendererBackend,
                                                                  fingerprint,
                                                                  std::move(revisions)};
            state_->sessions->status = ProjectSessionActivationLease::State::Status::Ready;
        }
        snapshot.readySession = sessionId;
        snapshot.phase = ProjectOpenPhase::ReadyToActivate;
        snapshot.outcome = ProjectOpenOutcome::ReadyToActivate;
        snapshot.progress = 1.0F;
        LOG_INFO("editor.project_open", "Project open ready operation=%llu backend=%s.",
                 static_cast<unsigned long long>(snapshot.operationId.value), snapshot.requiredRendererBackend.c_str());
    }

    void ProjectOpenService::Shutdown() noexcept {
        if (!state_ || state_->shutdown)
            return;
        state_->shutdown = true;
        if (state_->operation.has_value() && state_->operation->snapshot.outcome == ProjectOpenOutcome::Running) {
            state_->operation->cancellation.RequestCancellation();
            if (state_->operation->job.has_value()) {
                static_cast<void>(state_->jobs.RequestCancel(state_->operation->job->Id()));
                static_cast<void>(state_->operation->job->Wait());
            }
            state_->operation->snapshot.phase = ProjectOpenPhase::Cancelled;
            state_->operation->snapshot.outcome = ProjectOpenOutcome::Cancelled;
            state_->operation->snapshot.progress = 1.0F;
        }
        std::lock_guard lock(state_->sessions->mutex);
        state_->sessions->shutdown = true;
        state_->sessions->candidate.reset();
        state_->sessions->status = ProjectSessionActivationLease::State::Status::Empty;
    }
}  // namespace Horo::Editor
