#include "EditorProjectSceneHostOwner.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/Platform/NativeProjectReadFiles.h"

#include <chrono>
#include <new>

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @copydoc ProjectPlaySceneHost::PrepareDecisionPerception */
    Result<void> ProjectPlaySceneHost::PrepareDecisionPerception(EditorProjectScenePolicy &policy,
                                                                 const Runtime::RuntimeSceneDefinition &cooked,
                                                                 const Runtime::RuntimeSceneDefinition &live,
                                                                 const Editor::EditorPlayDecisionProviderSource &source,
                                                                 const std::size_t workUnitsLimit) {
        const auto &selection = *policy.decisions;
        if (selection.perceptionPolicy.empty())
            return Result<void>::Success();
        if (!source.perception || selection.fixedStep.ToNanoseconds() <= 0)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
        const auto metadata = LoadProjectMetadata(selection.projectRoot);
        if (metadata.HasError())
            return Result<void>::Failure(metadata.ErrorValue());
        const auto files = Platform::CreateNativeProjectReadFiles(selection.projectRoot, metadata.Value().projectId, 1);
        if (files.HasError())
            return Result<void>::Failure(files.ErrorValue());
        const Platform::ProjectReadContext context{.projectIdentity = metadata.Value().projectId,
                                                   .projectGeneration = 1,
                                                   .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30}};
        auto captured =
            CaptureProjectScenePerception(*files.Value(), context, selection.perceptionPolicy, selection.scene, cooked, workUnitsLimit);
        if (captured.HasError())
            return Result<void>::Failure(std::move(captured).ErrorValue());
        const auto &native = captured.Value()->native;
        if (!native)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
        auto generation = source.perception();
        if (generation.HasError())
            return Result<void>::Failure(std::move(generation).ErrorValue());
        if (!generation.Value() || !generation.Value()->CanAdmit())
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionDependencyMissing));
        auto registry = FreezeProjectNativePerceptionRegistry(*native, generation.Value()->Contracts());
        if (registry.HasError())
            return Result<void>::Failure(std::move(registry).ErrorValue());
        auto controllers = BindProjectPerceptionEndpoints(*captured.Value(), std::move(registry).Value(), policy.aiControllers);
        if (controllers.HasError())
            return Result<void>::Failure(std::move(controllers).ErrorValue());
        try {
            auto prepared = std::make_unique<PlayPerception>();
            prepared->positions =
                MakeProjectPerceptionPositionSources(captured.Value(), ProjectPerceptionSceneProjection{live.Id(), live.Revision()});
            prepared->filtering = captured.Value()->filtering.value_or(AI::PerceptionFilteringPolicy{});
            prepared->execution = {.generation = std::move(generation).Value(),
                                   .capabilities = ProjectNativePerceptionPolicy::capabilities,
                                   .memory = {.fixedStep = std::chrono::nanoseconds{selection.fixedStep.ToNanoseconds()}},
                                   .radiusMillimeters = native->radiusMillimeters,
                                   .visibleLayers = native->visibleLayers,
                                   .workUnitsPerTick = native->workUnitsPerTick};
            playPerception_ = std::move(prepared);
            policy.aiSettings.requirePerceptionEndpoints = true;
            policy.aiControllers = std::move(controllers).Value();
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        }
    }

    /** @copydoc ProjectPlaySceneHost::PollDecisionPerception */
    Result<void> ProjectPlaySceneHost::PollDecisionPerception(const std::uint64_t tick, const Duration fixedStep) {
        if (!playPerception_)
            return Result<void>::Success();
        const auto scene = scenes_->ActiveScene();
        if (!scene)
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
        auto &capture = *playPerception_;
        const auto current = capture.spatial.Current();
        if (!current || current->Scene() != scene->RuntimeId() || current->Revision() == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(AI::AIErrors::PerceptionSpatialStale));
        const auto positions = capture.positions.scene(*scene);
        if (positions.HasError())
            return Result<void>::Failure(positions.ErrorValue());
        const auto published =
            ai_->PublishPerceptionAtSafePoint(*scene, current->Revision() + 1, positions.Value(), capture.spatial, capture.filtering);
        if (published.HasError())
            return Result<void>::Failure(published.ErrorValue());
        return ai_->PollPerceptionAtSensePoll(*scene, capture.spatial, tick, fixedStep);
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
