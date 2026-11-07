#include "SimulatorPlayState.h"

#include <algorithm>
#include <limits>

namespace Horo::Application::Internal {
    using namespace XR;

    Result<void> SimulatorPlayHost::Impl::Prepare(const XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &) {
        if (stage != XRSessionPreparation::InputAndUi)
            return Result<void>::Success();
        if (!scene || !input)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto registered = composition.router.Actions();
        const auto action = std::ranges::find_if(registered, [](const auto &value) {
            return value.id.Value() == "gameplay.move" && value.context.Value() == "editor.workspace" &&
                   value.valueType == Input::ActionValueType::Axis2D;
        });
        if (action == registered.end())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        auto prepared = std::make_unique<BoundInput>();
        prepared->session = candidate;
        prepared->coordinator = std::make_unique<XRActionBindingCoordinator>(candidate, *this);
        const std::array declarations{XRActionDeclaration{action->id,
                                                          action->context,
                                                          action->valueType,
                                                          {1},
                                                          XRTrackedDeviceRole::LeftController,
                                                          action->required}};
        const std::array suggestions{XRSuggestedActionBinding{action->id, {1}, {1}}};
        const std::array controls{XRProfileControl{{1}, {1}, XRTrackedDeviceRole::LeftController, action->valueType}};
        const std::array profiles{XRActiveInteractionProfile{XRTrackedDeviceRole::LeftController, {1}}};
        if (auto bound = prepared->coordinator->Update(candidate, {1, declarations, suggestions, {}, {1}, registered}, controls, profiles);
            bound.HasError())
            return bound;
        auto projection =
            XRInputProjection::Create(*input, candidate, *prepared->coordinator->Current(), prepared->coordinator->Revision());
        if (projection.HasError())
            return Result<void>::Failure(projection.ErrorValue());
        prepared->projection.emplace(std::move(projection).Value());
        candidateInput = std::move(prepared);
        return Result<void>::Success();
    }

    void SimulatorPlayHost::Impl::Release(const XRSessionPreparation stage, const XRSessionId &session) noexcept {
        if (stage != XRSessionPreparation::InputAndUi)
            return;
        if (candidateInput && candidateInput->session == session)
            candidateInput.reset();
        if (activeInput && activeInput->session == session)
            activeInput.reset();
    }

    void SimulatorPlayHost::Impl::Neutralize(const XRSessionId &session, const std::uint64_t) noexcept {
        if (activeInput && activeInput->session == session)
            preview.reset();
    }

    const XRActionBindingCoordinator *SimulatorPlayHost::Impl::Coordinator() const noexcept {
        const auto *current = candidateInput ? candidateInput.get() : activeInput.get();
        return current ? current->coordinator.get() : nullptr;
    }

    Result<XRSimulatorCutoff> SimulatorPlayHost::Impl::Cutoff(const Runtime::FrameContext &context) const {
        Runtime::RuntimeDispatchFacts facts;
        if (context.dispatchEvidence.Read(composition.dispatch, Runtime::RuntimePhase::BuildInputSnapshot, facts) !=
            Runtime::RuntimeDispatchStatus::Valid)
            return Result<XRSimulatorCutoff>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto quantum = composition.fixedStep.ToNanoseconds();
        if (quantum <= 0 || (facts.committedTick && facts.committedDuration != composition.fixedStep) ||
            facts.committedTick > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / quantum))
            return Result<XRSimulatorCutoff>::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto simulation = XRSimulationTime::Create(static_cast<std::int64_t>(facts.committedTick) * quantum);
        if (simulation.HasError())
            return Result<XRSimulatorCutoff>::Failure(simulation.ErrorValue());
        return Result<XRSimulatorCutoff>::Success({facts.frame, simulation.Value()});
    }

    Result<void> SimulatorPlayHost::Impl::Append(const Runtime::FrameContext &context, const Input::InputCollectionFrame &frame) {
        const auto cutoff = Cutoff(context);
        preview.reset();
        if (cutoff.HasError())
            return Result<void>::Failure(cutoff.ErrorValue());
        const auto advanced = pendingStep ? runtime->Step(cutoff.Value()) : runtime->Tick(cutoff.Value());
        pendingStep = false;
        if (advanced.HasError())
            return Result<void>::Failure(advanced.ErrorValue());
        if (!advanced.Value().consumed)
            return Result<void>::Success();
        ObserveView(advanced.Value());
        const auto *tracking = runtime->Tracking();
        if (!tracking)
            return Result<void>::Success();
        if (!activeInput || !activeInput->projection || !activeInput->coordinator->Current())
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        return activeInput->projection->Append(*tracking, activeInput->session, tracking->Revision(), tracking->WorldOriginRevision(),
                                               activeInput->coordinator->Revision(), frame);
    }

    /** @copydoc SimulatorPlayHost::BuildInputSnapshot */
    Result<void> SimulatorPlayHost::BuildInputSnapshot(const Runtime::FrameContext &context, const Input::RawInputSnapshot &raw) {
        if (auto lane = impl_->Lane(); lane.HasError())
            return lane;
        Impl::Operation operation{*impl_};
        const bool sampling = impl_->play == Editor::EditorPlayState::Playing || impl_->pendingStep;
        if (!impl_->admitted || !impl_->runtime || !impl_->input || !sampling) {
            impl_->composition.router.BeginFrame(raw);
            return Result<void>::Success();
        }
        auto opened = impl_->input->Begin(raw);
        if (opened.HasError()) {
            impl_->Revoke();
            impl_->composition.router.BeginFrame(raw);
            return Result<void>::Failure(opened.ErrorValue());
        }
        auto frame = std::move(opened).Value();
        const auto sampled = impl_->Append(context, frame);
        if (auto committed = frame.Commit(); committed.HasError()) {
            impl_->Revoke();
            return committed;
        }
        // Failed simulation publishes no synthetic candidate; the same commit preserves physical input.
        return sampled;
    }
}  // namespace Horo::Application::Internal
