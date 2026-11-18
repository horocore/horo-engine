#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "PrefabSceneCookState.h"

#include <algorithm>
#include <format>

namespace Horo::Application {
    namespace {
        /** @brief Selects resources and explicit template roots without changing captured source closure or revision authority. */
        Result<Assets::AssetRegistrySnapshot> SelectRuntimeRecords(const Assets::AssetCookInputSnapshot &inputs,
                                                                   const std::span<const Assets::AssetId> runtimeRoots) {
            std::vector<Assets::AssetRecord> runtimeRecords;
            for (const auto &record : inputs.Registry().Records()) {
                if (record.type.Value() != "core.prefab" || std::ranges::find(runtimeRoots, record.id) != runtimeRoots.end())
                    runtimeRecords.push_back(record);
            }
            Assets::AssetRegistry selected;
            if (selected.Publish(std::move(runtimeRecords)).status != Assets::AssetRegistryBuildStatus::Complete)
                return Result<Assets::AssetRegistrySnapshot>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            return Result<Assets::AssetRegistrySnapshot>::Success(selected.Snapshot());
        }

        /** @brief Borrows capture authorities only during the joined cook; rechecks the live registry and host before publication. */
        Result<void> ValidateCurrentHost(const PrefabSceneCookRequest &request, const Assets::AssetRegistry &registry,
                                         const ProjectCompatibilityInspector &compatibility, const PrefabCookDetail::HostCapture &capture,
                                         const CancellationToken &cancellation, const Release::ReleaseExecutionPlan *releasePlan,
                                         const Assets::AssetRegistryRevision capturedRevision) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
            if (registry.Snapshot().Revision() != capturedRevision)
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            if (auto verified = PrefabCookDetail::VerifyHost(request, capture, compatibility, releasePlan); verified.HasError())
                return verified;
            return request.assets.validateHostInputs ? request.assets.validateHostInputs() : Result<void>::Success();
        }
    }  // namespace

    /** @brief Projects the host preflight until ownership passes to AssetCook's publication operation. */
    class PrefabSceneCookHost::PreparationDiagnostics final {
    public:
        PreparationDiagnostics(const Assets::AssetCookRequest &request, const CancellationToken &cancellation)
            : operations_(request.operationStore), output_(request.buildOutputStore), cancellation_(cancellation) {
            if (operations_) {
                id_ = operations_->Begin({.kind = OperationKind::Cook,
                                          .title = "Prepare prefab cook",
                                          .phase = std::string{phase_},
                                          .progress = 0.0F,
                                          .cancellable = static_cast<bool>(request.requestCancel),
                                          .requestCancel = request.requestCancel});
            }
        }

        ~PreparationDiagnostics() {
            if (!finished_)
                Finish(&abandoned_);
        }

        [[nodiscard]] bool Admitted() const noexcept {
            return !operations_ || id_.has_value();
        }

        void Stage(const std::string_view phase, const float progress) {
            phase_ = phase;
            if (id_)
                static_cast<void>(operations_->Update(*id_, {.phase = std::string{phase_}, .progress = progress}));
        }

        void Finish(const Error *error = nullptr) noexcept {
            if (finished_)
                return;
            finished_ = true;
            const bool cancelled =
                error && (cancellation_.IsCancellationRequested() || IsJobCancelled(*error) ||
                          ErrorChainContains(*error, PrefabSceneCookErrors::Cancelled.domain, PrefabSceneCookErrors::Cancelled.code) ||
                          ErrorChainContains(*error, Prefab::PrefabErrors::Cancelled.domain, Prefab::PrefabErrors::Cancelled.code) ||
                          ErrorChainContains(*error, FileStorageErrors::Cancelled.domain, FileStorageErrors::Cancelled.code));
            const std::string_view summary = cancelled ? "Prefab cook preparation cancelled"
                                                       : (error ? "Prefab cook preparation failed" : "Prefab cook inputs prepared");
            try {
                if (id_) {
                    const std::optional<Error> safeError =
                        error ? std::optional<Error>{{.code = error->code,
                                                     .domain = error->domain,
                                                     .severity = error->severity,
                                                     .message = std::string{summary}}}
                              : std::nullopt;
                    static_cast<void>(operations_->Update(
                        *id_, {.state = cancelled ? OperationState::Cancelled : (error ? OperationState::Failed : OperationState::Succeeded),
                               .phase = std::string{phase_},
                               .message = std::string{summary},
                               .progress = error ? std::nullopt : std::optional<float>{1.0F},
                               .error = safeError}));
                }
            } catch (...) {
                // Optional history sinks cannot change the cook result.
            }
            try {
                if (output_ && error) {
                    // Producer messages and native paths can include machine-local data. Only registry-relative identity is shared.
                    const auto session = output_->BeginSession();
                    output_->Append({.timestampUtc = std::chrono::system_clock::now(),
                                     .sessionId = session,
                                     .operationId = id_,
                                     .severity = cancelled ? DiagnosticSeverity::Warning : DiagnosticSeverity::Error,
                                     .result = cancelled ? BuildOutputResult::Cancelled : BuildOutputResult::Failed,
                                     .stage = std::string{phase_},
                                     .code = DiagnosticCode{error->code.Value()},
                                     .message = failedSource ? std::format("{} [{}] {}", summary, failedSource->id.ToString(),
                                                                          failedSource->sourcePath.String())
                                                             : std::string{summary}});
                }
            } catch (...) {
                // Optional diagnostic history cannot replace the authoritative typed failure.
            }
        }

        std::optional<Assets::AssetRecord> failedSource;

    private:
        OperationStore *operations_;
        BuildOutputStore *output_;
        const CancellationToken &cancellation_;
        std::optional<OperationId> id_;
        std::string_view phase_{"prefab.preflight"};
        const Error abandoned_ = MakeError(PrefabSceneCookErrors::Invalid);
        bool finished_{};
    };

    /** @copydoc PrefabSceneCookHost::PrefabSceneCookHost */
    PrefabSceneCookHost::PrefabSceneCookHost(JobSystem &jobs, std::shared_ptr<const Assets::CookerCatalogSnapshot> catalog,
                                             const Assets::AssetRegistry &registry, const ProjectCompatibilityInspector &compatibility,
                                             Editor::ProjectMutationCoordinator &mutations,
                                             const Editor::ProjectMigrationTransactionService &migrations)
        : jobs_(jobs), catalog_(std::move(catalog)), registry_(registry), compatibility_(compatibility), mutations_(mutations),
          migrations_(migrations), expansion_(jobs) {}

    /** @copydoc PrefabSceneCookHost::~PrefabSceneCookHost */
    PrefabSceneCookHost::~PrefabSceneCookHost() {
        static_cast<void>(expansion_.Shutdown({WaitPolicy::MainThreadPumpAllowed, Duration::FromMilliseconds(30'000)}));
        // If interrupted, the expansion member retains and drains the exact owned attempt before
        // destruction can release host authorities. No worker captures those authorities.
    }

    /** @copydoc PrefabSceneCookHost::Cook */
    Result<Assets::AssetCookReport> PrefabSceneCookHost::Cook(const PrefabSceneCookRequest &request,
                                                              const CancellationToken &cancellation) {
        Prefab::PrefabMetricOperation observation{request.metrics, Prefab::PrefabMetricPhase::Cook};
        auto result = CookImpl(request, cancellation, nullptr);
        if (result.HasError()) {
            if (result.ErrorValue().domain.Value() == PrefabSceneCookErrors::Cancelled.domain.Value() &&
                result.ErrorValue().code.Value() == PrefabSceneCookErrors::Cancelled.code.Value())
                observation.Complete(Prefab::PrefabMetricOutcome::Cancelled);
            else
                observation.Fail(result.ErrorValue());
        } else
            observation.Complete();
        return result;
    }

    /** @copydoc PrefabSceneCookHost::CookImpl */
    Result<Assets::AssetCookReport> PrefabSceneCookHost::CookImpl(const PrefabSceneCookRequest &request,
                                                                  const CancellationToken &cancellation,
                                                                  const Release::ReleaseExecutionPlan *releasePlan) {
        PreparationDiagnostics diagnostics{request.assets, cancellation};
        if (!diagnostics.Admitted()) {
            const auto error = MakeError(PrefabSceneCookErrors::Invalid, "Prefab cook preparation could not acquire an operation slot.");
            diagnostics.Finish(&error);
            return Result<Assets::AssetCookReport>::Failure(error);
        }
        auto result = [&]() -> Result<Assets::AssetCookReport> {
            if (cancellation.IsCancellationRequested())
                return Result<Assets::AssetCookReport>::Failure(MakeError(PrefabSceneCookErrors::Cancelled));
            if (!catalog_ || !request.assets.publicationFiles || !request.assets.newPublicationOperationId ||
                request.runtimePrefabRoots.size() > request.assets.limits.maximumAssets)
                return Result<Assets::AssetCookReport>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
            if (const auto limits = Prefab::PrefabLimitProfile::Create(request.prefabPolicy); limits.HasError())
                return Result<Assets::AssetCookReport>::Failure(limits.ErrorValue());
            auto attempt = request.assets.newPublicationOperationId();
            if (attempt.HasError())
                return Result<Assets::AssetCookReport>::Failure(attempt.ErrorValue());
            if (auto projectLease =
                    mutations_.TryAcquire({request.assets.sourceRoot, Editor::ProjectMutationOwner::Asset, attempt.Value().ToString()});
                projectLease.HasError())
                return Result<Assets::AssetCookReport>::Failure(projectLease.ErrorValue());
            else {
                const auto settings = LoadPrefabProjectSettings(request.assets.sourceRoot);
                if (settings.HasError())
                    return Result<Assets::AssetCookReport>::Failure(settings.ErrorValue());
                auto values = PrefabPolicyFieldValues(request.prefabPolicy);
                const auto projectValues = PrefabPolicyFieldValues(settings.Value().settings->Limits().Policy());
                for (std::size_t index = 0; index < values.size(); ++index)
                    values[index] = std::min(values[index], projectValues[index]);
                const auto validated = ValidatePrefabPolicyValues(values);
                if (!validated.profile)
                    return Result<Assets::AssetCookReport>::Failure(MakeError(Prefab::PrefabErrors::LimitProfileInvalid));
                auto effective = request;
                effective.prefabPolicy = validated.profile->Policy();
                return CookWithProjectLease(effective, *validated.profile, cancellation, releasePlan, diagnostics,
                                                std::move(projectLease).Value());
            }
        }();
        if (result.HasError())
            diagnostics.Finish(&result.ErrorValue());
        return result;
    }

    /** @copydoc PrefabSceneCookHost::CookWithProjectLease */
    Result<Assets::AssetCookReport> PrefabSceneCookHost::CookWithProjectLease(const PrefabSceneCookRequest &request,
                                                                              const Prefab::PrefabLimitProfile &limits,
                                                                              const CancellationToken &cancellation,
                                                                              const Release::ReleaseExecutionPlan *releasePlan,
                                                                              PreparationDiagnostics &diagnostics,
                                                                              Editor::ProjectMutationLease) {
        if (const auto recovery = migrations_.InspectPendingRecovery(request.assets.sourceRoot);
            recovery.action != Editor::MigrationRecoveryAction::None)
            return Result<Assets::AssetCookReport>::Failure(recovery.diagnostic.value_or(MakeError(PrefabSceneCookErrors::Invalid)));
        auto host = PrefabCookDetail::CaptureHost(request, compatibility_, releasePlan);
        if (host.HasError())
            return Result<Assets::AssetCookReport>::Failure(host.ErrorValue());
        diagnostics.Stage("prefab.capture_sources", 0.25F);
        auto captured = Assets::AssetCookInputSnapshot::Capture(request.assets.sourceRoot, registry_.Snapshot(), request.assets.limits,
                                                                request.maximumCapturedBytes, cancellation);
        if (captured.HasError())
            return Result<Assets::AssetCookReport>::Failure(captured.ErrorValue());
        auto inputs = std::make_shared<const Assets::AssetCookInputSnapshot>(std::move(captured).Value());
        const auto capturedRevision = inputs->Registry().Revision();
        // Each cook owns a detached source session. Replacement drains any interrupted previous
        // operation before this session can admit work; source closure precedes asset publication.
        const JoinOptions sourceJoin{WaitPolicy::MainThreadPumpAllowed, Duration::FromMilliseconds(30'000)};
        if (auto replaced = expansion_.ReplaceScene(sourceJoin); replaced.HasError())
            return Result<Assets::AssetCookReport>::Failure(replaced.ErrorValue());
        diagnostics.Stage("prefab.validate_sources", 0.5F);
        auto composed = PrefabCookDetail::PrepareCatalog(request, host.Value(), *inputs, limits, *catalog_, cancellation, expansion_,
                                                          diagnostics.failedSource);
        if (auto closed = expansion_.CloseDocument(sourceJoin); closed.HasError())
            return Result<Assets::AssetCookReport>::Failure(closed.ErrorValue());
        if (composed.HasError())
            return Result<Assets::AssetCookReport>::Failure(composed.ErrorValue());
        auto selected = SelectRuntimeRecords(*inputs, request.runtimePrefabRoots);
        if (selected.HasError())
            return Result<Assets::AssetCookReport>::Failure(selected.ErrorValue());
        Assets::AssetCookRequest cook = request.assets;
        cook.registry = std::move(selected).Value();
        cook.pinnedInputs = inputs;
        cook.keyProjection = composed.Value().keyProjection;
        cook.dependentPhase = composed.Value().templates;
        // Capture references are synchronous: AssetCook joins all accepted work before returning, and the owned project lease outlives it.
        cook.validateHostInputs = [this, &request, &host, &cancellation, releasePlan, capturedRevision] {
            return ValidateCurrentHost(request, registry_, compatibility_, host.Value(), cancellation, releasePlan, capturedRevision);
        };
        diagnostics.Finish();
        Assets::AssetCookService operation{jobs_, composed.Value().catalog};
        return operation.Cook(cook, cancellation);
    }

    /** @copydoc PrefabSceneCookHost::CookForRelease */
    Result<Release::ReleaseCookedPayload> PrefabSceneCookHost::CookForRelease(const Release::ReleaseExecutionPlan &plan,
                                                                              Release::IReleasePreflightFactsProvider &facts,
                                                                              const PrefabSceneCookRequest &request,
                                                                              const CancellationToken &cancellation) {
        if (request.assets.sourceRoot != plan.ProjectRoot())
            return Result<Release::ReleaseCookedPayload>::Failure(MakeError(PrefabSceneCookErrors::Invalid));
        PrefabSceneCookRequest releaseRequest = request;
        releaseRequest.assets.validateHostInputs = [&plan, &facts, &request] {
            auto current = facts.Capture(plan);
            if (current.HasError())
                return Result<void>::Failure(current.ErrorValue());
            if (!Release::ValidateReleaseInputFreeze(plan, current.Value()).empty())
                return Result<void>::Failure(MakeError(PrefabSceneCookErrors::Stale));
            return request.assets.validateHostInputs ? request.assets.validateHostInputs() : Result<void>::Success();
        };
        auto cooked = CookImpl(releaseRequest, cancellation, &plan);
        if (cooked.HasError())
            return Result<Release::ReleaseCookedPayload>::Failure(cooked.ErrorValue());
        const auto &generation = cooked.Value().generation;
        return Result<Release::ReleaseCookedPayload>::Success({generation.generationRoot, generation.manifestDigest});
    }
}  // namespace Horo::Application
