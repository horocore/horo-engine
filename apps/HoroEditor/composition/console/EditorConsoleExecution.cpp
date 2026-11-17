#include "EditorConsoleExecution.h"

#include "ConsoleMetricCommandInternal.h"
#include "ConsoleSceneCommandInternal.h"
#include "NetworkConsoleInspection.h"

#include <algorithm>
#include <vector>

namespace Horo::Application::Internal {
    namespace {
        /** @brief Resolves only registered process-store/Scene panel controls, without granting access or discovering a producer. */
        std::string_view StoreOverlayName(const Debug::OverlaySource source) {
            if (source == Debug::OverlaySource::Logs)
                return Debug::ConsoleLogOverlayVariableName;
            if (source == Debug::OverlaySource::Scene)
                return Debug::ConsoleSceneOverlayVariableName;
            if (source == Debug::OverlaySource::Network)
                return Debug::ConsoleNetworkOverlayVariableName;
            return source == Debug::OverlaySource::Jobs ? Debug::ConsoleJobsOverlayVariableName : std::string_view{};
        }
    }  // namespace

    /** @copydoc EditorConsoleExecution::EditorConsoleExecution */
    EditorConsoleExecution::EditorConsoleExecution(std::unique_ptr<Debug::CommandExecutor> executor,
                                                   std::shared_ptr<Debug::ConsoleHistory> history,
                                                   std::shared_ptr<OperationStore> operations,
                                                   std::shared_ptr<ConfigurationService> configuration,
                                                   std::shared_ptr<Debug::ConsoleVariableSet> variables,
                                                   const ConfigurationRevision configurationRevision)
        : executor_(std::move(executor)), history_(std::move(history)), operations_(std::move(operations)),
          configuration_(std::move(configuration)), variables_(std::move(variables)), configurationRevision_(configurationRevision) {}

    /** @copydoc EditorConsoleExecution::~EditorConsoleExecution */
    EditorConsoleExecution::~EditorConsoleExecution() {
        Stop();
    }

    /** @copydoc EditorConsoleExecution::Start */
    Result<std::unique_ptr<EditorConsoleExecution>> EditorConsoleExecution::Start(EditorConsolePreparation &preparation, JobSystem &jobs,
                                                                                  std::shared_ptr<OperationStore> operations,
                                                                                  std::shared_ptr<ConfigurationService> configuration,
                                                                                  std::string version,
                                                                                  const std::span<const Debug::OverlaySource> panels,
                                                                                  std::shared_ptr<const INetworkDebuggerQuery> network) {
        if (!preparation.registry || preparation.registry->Profile() != Debug::ProductProfile::Editor || !preparation.history ||
            !operations || !configuration)
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(
                Debug::MakeCommandError(Debug::CommandError::InvalidDescriptor));
        const auto snapshot = configuration->Snapshot();
        const auto historyLimit = Debug::ReadConsoleHistoryLimit(snapshot);
        if (historyLimit.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(historyLimit.ErrorValue());
        const auto resized = preparation.history->SetEntryLimit(historyLimit.Value());
        if (resized.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(resized.ErrorValue());
        Debug::RegistryPolicy policy;
        policy.profile = Debug::ProductProfile::Editor;
        std::vector variableDescriptors{Debug::ConsoleHistoryVariableDescriptor(), Debug::ConsoleVisibilityVariableDescriptor(),
                                        Debug::ConsoleLogOverlayVariableDescriptor(), Debug::ConsoleJobsOverlayVariableDescriptor(),
                                        Debug::ConsoleSceneOverlayVariableDescriptor()};
        if (network)
            variableDescriptors.push_back(Debug::ConsoleNetworkOverlayVariableDescriptor());
        const auto commands = preparation.registry->Discover({}, {15, true, true});
        auto createdVariables = Debug::ConsoleVariableSet::Create(variableDescriptors, commands, policy, configuration);
        if (createdVariables.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(createdVariables.ErrorValue());
        auto variables = std::make_shared<Debug::ConsoleVariableSet>(std::move(createdVariables).Value());
        const auto seeded = variables->Set(Debug::ConsoleVisibleVariableName, preparation.consoleVisible, {});
        if (seeded.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(seeded.ErrorValue());
        for (const auto source : {Debug::OverlaySource::Logs, Debug::OverlaySource::Jobs, Debug::OverlaySource::Scene}) {
            const auto initialized =
                variables->Set(StoreOverlayName(source), std::ranges::find(panels, source) != panels.end(), {3, false, false});
            if (initialized.HasError())
                return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(initialized.ErrorValue());
        }
        if (network) {
            const auto initialized =
                variables->Set(Debug::ConsoleNetworkOverlayVariableName,
                               std::ranges::find(panels, Debug::OverlaySource::Network) != panels.end(), {3, false, false});
            if (initialized.HasError())
                return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(initialized.ErrorValue());
        }
        auto bindings = Debug::BindCoreConsoleCommands(preparation.registry, preparation.history, std::move(version), variables);
        if (bindings.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(bindings.ErrorValue());
        auto metricBindings = Debug::Internal::BindMetricCommand(preparation.registry);
        if (metricBindings.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(metricBindings.ErrorValue());
        auto ownedBindings = std::move(bindings).Value();
        auto ownedMetricBindings = std::move(metricBindings).Value();
        for (auto &binding : ownedMetricBindings)
            ownedBindings.push_back(std::move(binding));
        auto scene = std::make_shared<Debug::Internal::ConsoleSceneInspection>();
        auto sceneBindings = Debug::Internal::BindSceneInspectionCommands(preparation.registry, scene);
        if (sceneBindings.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(sceneBindings.ErrorValue());
        auto ownedSceneBindings = std::move(sceneBindings).Value();
        for (auto &binding : ownedSceneBindings)
            ownedBindings.push_back(std::move(binding));
        auto networkBindings = BindNetworkConsoleInspection(preparation.registry, std::move(network));
        if (networkBindings.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(networkBindings.ErrorValue());
        auto ownedNetworkBindings = std::move(networkBindings).Value();
        for (auto &binding : ownedNetworkBindings)
            ownedBindings.push_back(std::move(binding));
        auto created = Debug::CommandExecutor::Create(preparation.registry, ownedBindings, jobs, operations, std::this_thread::get_id(),
                                                      std::this_thread::get_id(), 128, Debug::CreateConsoleTelemetryAudit(), variables);
        if (created.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(created.ErrorValue());
        auto execution = std::unique_ptr<EditorConsoleExecution>(new EditorConsoleExecution(std::move(created).Value(), preparation.history,
                                                                                            std::move(operations), std::move(configuration),
                                                                                            std::move(variables), snapshot.Revision()));
        execution->scene_ = std::move(scene);
        auto submitted = preparation.startup.Submit(*execution->executor_, {});
        if (submitted.HasError())
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(submitted.ErrorValue());
        auto *partial = std::get_if<Debug::ConsoleStartupPartialFailure>(&submitted.Value());
        execution->receipts_ =
            partial ? std::move(partial->receipts) : std::move(std::get<Debug::ConsoleStartupAccepted>(submitted.Value()).receipts);
        for (const auto &receipt : execution->receipts_)
            execution->history_->Append({0, {}, receipt.redactedText}, receipt.output.text, receipt.operation);
        if (partial)
            return Result<std::unique_ptr<EditorConsoleExecution>>::Failure(partial->error);
        return Result<std::unique_ptr<EditorConsoleExecution>>::Success(std::move(execution));
    }

    /** @copydoc EditorConsoleExecution::OwnerSafePoint */
    Result<void> EditorConsoleExecution::OwnerSafePoint(const std::chrono::steady_clock::time_point now,
                                                        const Runtime::RuntimeSceneView *scene) {
        const auto boundary = executor_->ValidateOwnerBoundary();
        if (boundary.HasError())
            return boundary;
        scene_->source = scene ? std::optional{*scene} : std::nullopt;
        const auto applied = variables_->Apply(Debug::ConsoleVariableApplyPolicy::NextFrame);
        if (applied.HasError()) {
            if (!variableActivationError_ || variableActivationError_->code.Value() != applied.ErrorValue().code.Value())
                variableActivationError_ = applied.ErrorValue();
        } else {
            variableActivationError_.reset();
        }
        const auto configuration = configuration_->Snapshot();
        const auto drained = executor_->DrainOwner(32);
        if (drained.HasError())
            return Result<void>::Failure(drained.ErrorValue());
        if (configuration.Revision() != configurationRevision_) {
            const auto limit = Debug::ReadConsoleHistoryLimit(configuration);
            if (limit.HasError())
                return Result<void>::Failure(limit.ErrorValue());
            const auto resized = history_->SetEntryLimit(limit.Value());
            if (resized.HasError())
                return Result<void>::Failure(resized.ErrorValue());
            configurationRevision_ = configuration.Revision();
        }
        if (history_->PendingResults() == 0 || now < nextRefresh_)
            return Result<void>::Success();
        nextRefresh_ = now + std::chrono::milliseconds{250};
        if (const auto snapshot = operations_->SnapshotIfChanged(revision_)) {
            const auto projected = history_->ProjectOperations(*snapshot);
            if (projected.HasError())
                return Result<void>::Failure(projected.ErrorValue());
            revision_ = snapshot->revision;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorConsoleExecution::RenderSafePoint */
    Result<void> EditorConsoleExecution::RenderSafePoint() {
        const auto drained = executor_->DrainRender(32);
        return drained.HasError() ? Result<void>::Failure(drained.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc EditorConsoleExecution::Stop */
    void EditorConsoleExecution::Stop() {
        executor_->Stop();
    }

    /** @copydoc EditorConsoleExecution::Accepted */
    std::span<const Debug::CommandReceipt> EditorConsoleExecution::Accepted() const noexcept {
        return receipts_;
    }

    /** @copydoc EditorConsoleExecution::Dispatcher */
    Debug::CommandExecutor &EditorConsoleExecution::Dispatcher() noexcept {
        return *executor_;
    }

    /** @copydoc EditorConsoleExecution::Variables */
    const Debug::ConsoleVariableSet &EditorConsoleExecution::Variables() const noexcept {
        return *variables_;
    }

    /** @copydoc EditorConsoleExecution::ConsoleVisible */
    Result<bool> EditorConsoleExecution::ConsoleVisible() const {
        const auto value = variables_->Read(Debug::ConsoleVisibleVariableName, {});
        if (value.HasError())
            return Result<bool>::Failure(value.ErrorValue());
        const auto *visible = std::get_if<bool>(&value.Value().active);
        return visible ? Result<bool>::Success(*visible)
                       : Result<bool>::Failure(Debug::MakeCommandError(Debug::CommandError::InvalidDescriptor));
    }

    /** @copydoc EditorConsoleExecution::RequestConsoleVisibility */
    Result<Debug::CommandReceipt> EditorConsoleExecution::RequestConsoleVisibility(const bool visible) {
        return executor_->Submit(std::string(Debug::ConsoleVisibleVariableName) + (visible ? " true" : " false"), {});
    }

    /** @copydoc EditorConsoleExecution::StoreOverlayVisible */
    Result<bool> EditorConsoleExecution::StoreOverlayVisible(const Debug::OverlaySource source,
                                                             const Debug::DebugCommandContext &principal) const {
        const auto name = StoreOverlayName(source);
        if (name.empty())
            return Result<bool>::Failure(Debug::MakeCommandError(Debug::CommandError::MissingCapability));
        const auto value = variables_->Read(name, principal);
        if (value.HasError())
            return Result<bool>::Failure(value.ErrorValue());
        const auto *visible = std::get_if<bool>(&value.Value().active);
        return visible ? Result<bool>::Success(*visible)
                       : Result<bool>::Failure(Debug::MakeCommandError(Debug::CommandError::InvalidDescriptor));
    }

    /** @copydoc EditorConsoleExecution::RequestStoreOverlayVisibility */
    Result<Debug::CommandReceipt> EditorConsoleExecution::RequestStoreOverlayVisibility(const Debug::OverlaySource source,
                                                                                        const bool visible,
                                                                                        const Debug::DebugCommandContext &principal) {
        const auto name = StoreOverlayName(source);
        if (name.empty())
            return Result<Debug::CommandReceipt>::Failure(Debug::MakeCommandError(Debug::CommandError::MissingCapability));
        return executor_->Submit(std::string(name) + (visible ? " true" : " false"), principal);
    }

    /** @copydoc EditorConsoleExecution::VariableActivationError */
    const std::optional<Error> &EditorConsoleExecution::VariableActivationError() const noexcept {
        return variableActivationError_;
    }
}  // namespace Horo::Application::Internal
