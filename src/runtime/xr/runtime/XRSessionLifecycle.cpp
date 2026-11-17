#include "Horo/XR/XRSessionLifecycle.h"

#include "Horo/Foundation/Assertions.h"
#include "Horo/XR/XRErrors.h"
#include "Horo/XR/XRSessionErrors.h"

#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::XR {
    namespace {
        struct PreparationStep final {
            XRSessionPreparation stage;
            std::string_view name;
        };

        constexpr std::array PreparationOrder{
            PreparationStep{XRSessionPreparation::PlatformLoader, "PlatformLoader"},
            PreparationStep{XRSessionPreparation::InstanceSystem, "InstanceSystem"},
            PreparationStep{XRSessionPreparation::RendererCompatibility, "RendererCompatibility"},
            PreparationStep{XRSessionPreparation::CapabilitiesAndSpaces, "CapabilitiesAndSpaces"},
            PreparationStep{XRSessionPreparation::NativeSessionAndSwapchains, "NativeSessionAndSwapchains"},
            PreparationStep{XRSessionPreparation::RendererTargets, "RendererTargets"},
            PreparationStep{XRSessionPreparation::InputAndUi, "InputAndUi"},
        };
        static_assert(PreparationOrder.size() == static_cast<std::size_t>(XRSessionPreparation::Count));

        [[nodiscard]] XRSessionId CandidateSession(const XRSystemId &system, const std::uint32_t generation) noexcept {
            return {.system = system, .slot = {.index = 0, .generation = generation}};
        }

        void ReleasePrepared(IXRSessionResources &resources, const XRSessionId &candidate, const std::size_t completed) noexcept {
            for (std::size_t remaining = completed; remaining > 0; --remaining)
                resources.Release(PreparationOrder[remaining - 1].stage, candidate);
        }

        /** @brief Holds the exact completed prefix until the candidate publishes or unwinds. */
        class PreparedSession final {
        public:
            PreparedSession(IXRSessionResources &resources, const XRSessionId &session) noexcept
                : resources_(resources), session_(session) {}

            PreparedSession(const PreparedSession &) = delete;
            PreparedSession &operator=(const PreparedSession &) = delete;

            ~PreparedSession() {
                ReleasePrepared(resources_, session_, completed_);
            }

            void CompleteStage() noexcept {
                ++completed_;
            }

            void Commit() noexcept {
                completed_ = 0;
            }

        private:
            IXRSessionResources &resources_;
            XRSessionId session_;
            std::size_t completed_{};
        };

        [[nodiscard]] bool IsLegalEvent(const XRSessionState current, const XRSessionEvent event) noexcept {
            switch (event) {
                case XRSessionEvent::Ready:
                    return current == XRSessionState::Inactive || current == XRSessionState::Ready;
                case XRSessionEvent::Started:
                    return current == XRSessionState::Ready || current == XRSessionState::Running;
                case XRSessionEvent::BecameVisible:
                    return current == XRSessionState::Running || current == XRSessionState::Visible;
                case XRSessionEvent::BecameFocused:
                    return current == XRSessionState::Visible || current == XRSessionState::Focused;
                case XRSessionEvent::FocusLost:
                    return current == XRSessionState::Focused || current == XRSessionState::Visible;
                case XRSessionEvent::VisibilityLost:
                    return current == XRSessionState::Focused || current == XRSessionState::Visible || current == XRSessionState::Running;
                case XRSessionEvent::Stopping:
                    return current == XRSessionState::Ready || current == XRSessionState::Running || current == XRSessionState::Visible ||
                           current == XRSessionState::Focused || current == XRSessionState::Stopping;
                case XRSessionEvent::Idle:
                    return current == XRSessionState::Stopping || current == XRSessionState::Inactive;
                case XRSessionEvent::SessionLost:
                case XRSessionEvent::InstanceLost:
                    return current != XRSessionState::Destroyed;
            }
            return false;
        }

        [[nodiscard]] XRSessionState StateForEvent(const XRSessionEvent event) noexcept {
            switch (event) {
                case XRSessionEvent::Ready:
                    return XRSessionState::Ready;
                case XRSessionEvent::Started:
                case XRSessionEvent::VisibilityLost:
                    return XRSessionState::Running;
                case XRSessionEvent::BecameVisible:
                case XRSessionEvent::FocusLost:
                    return XRSessionState::Visible;
                case XRSessionEvent::BecameFocused:
                    return XRSessionState::Focused;
                case XRSessionEvent::Stopping:
                    return XRSessionState::Stopping;
                case XRSessionEvent::Idle:
                    return XRSessionState::Inactive;
                case XRSessionEvent::SessionLost:
                case XRSessionEvent::InstanceLost:
                    return XRSessionState::Lost;
            }
            return XRSessionState::Destroyed;
        }
    }  // namespace

    /** @copydoc XRSessionLifecycle::XRSessionLifecycle */
    XRSessionLifecycle::XRSessionLifecycle(IXRSessionResources &resources) noexcept : resources_(&resources) {}

    /** @copydoc XRSessionLifecycle::~XRSessionLifecycle */
    XRSessionLifecycle::~XRSessionLifecycle() {
        HORO_INVARIANT_MSG(IsOwnerThread(), "XR session lifecycle destruction requires its control thread");
        Shutdown();
    }

    /** @copydoc XRSessionLifecycle::IsOwnerThread */
    bool XRSessionLifecycle::IsOwnerThread() const noexcept {
        return owner_ == std::this_thread::get_id();
    }

    /** @copydoc XRSessionLifecycle::Activate */
    Result<XRSessionId> XRSessionLifecycle::Activate(const XRCapabilitySnapshot &capabilities, const XRSystemId &activeSystem,
                                                     const XRCapabilityRevision expectedRevision, const XRFeaturePlan &plan,
                                                     const CancellationToken &cancellation) {
        if (!IsOwnerThread())
            return Result<XRSessionId>::Failure(MakeError(XRSessionErrors::WrongThread));
        if (destroyed_)
            return Result<XRSessionId>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (cancellation.IsCancellationRequested())
            return Result<XRSessionId>::Failure(MakeError(XRSessionErrors::ActivationCancelled));
        switch (ValidateXRFeaturePlan(plan, capabilities, activeSystem, expectedRevision)) {
            case XRFeatureNegotiationStatus::Ok:
                break;
            case XRFeatureNegotiationStatus::StaleSystem:
                return Result<XRSessionId>::Failure(MakeError(XRErrors::IdentityStale));
            case XRFeatureNegotiationStatus::StaleRevision:
                return Result<XRSessionId>::Failure(MakeError(XRErrors::CapabilityStale));
            case XRFeatureNegotiationStatus::Unavailable:
                return Result<XRSessionId>::Failure(MakeError(XRErrors::OperationUnavailable));
            case XRFeatureNegotiationStatus::CapacityExceeded:
                return Result<XRSessionId>::Failure(MakeError(XRErrors::CapacityExceeded));
            default:
                return Result<XRSessionId>::Failure(MakeError(XRErrors::OperationInvalid));
        }
        if (lastSessionGeneration_ == std::numeric_limits<std::uint32_t>::max())
            return Result<XRSessionId>::Failure(MakeError(XRErrors::CapacityExceeded));

        // A failed private candidate is never reissued, even if its adapter leaked an observation.
        const XRSessionId candidate = CandidateSession(capabilities.System(), ++lastSessionGeneration_);
        PreparedSession rollback{*resources_, candidate};
        for (const auto &step : PreparationOrder) {
            if (auto prepared = resources_->Prepare(step.stage, candidate, plan); prepared.HasError())
                return Result<XRSessionId>::Failure(WrapError(XRSessionErrors::ActivationFailed, std::move(prepared).ErrorValue(),
                                                              std::string{"XR session activation failed at "} + std::string{step.name}));
            rollback.CompleteStage();
            if (cancellation.IsCancellationRequested())
                return Result<XRSessionId>::Failure(MakeError(XRSessionErrors::ActivationCancelled));
        }

        // Preparation is complete; old-generation retirement and publication cannot fail.
        Retire(ownedSession_);
        ownedSession_ = candidate;
        snapshot_.session = candidate;
        snapshot_.capabilityRevision = capabilities.Revision();
        acceptedPlan_ = plan;
        PublishState(XRSessionState::Ready);
        rollback.Commit();
        return Result<XRSessionId>::Success(candidate);
    }

    /** @copydoc XRSessionLifecycle::ApplyEvent */
    Result<void> XRSessionLifecycle::ApplyEvent(const XRSessionId &source, const XRSessionEvent event) {
        if (!IsOwnerThread())
            return Result<void>::Failure(MakeError(XRSessionErrors::WrongThread));
        if (!source.IsValid() || event > XRSessionEvent::Ready)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (source != ownedSession_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const XRSessionState next = StateForEvent(event);
        if (!IsLegalEvent(snapshot_.state, event))
            return Result<void>::Failure(MakeError(XRSessionErrors::TransitionInvalid));
        if (event == XRSessionEvent::Ready) {
            if (!acceptedPlan_ || acceptedPlan_->System() != ownedSession_.system)
                return Result<void>::Failure(MakeError(XRSessionErrors::TransitionInvalid));
            snapshot_.session = ownedSession_;
            snapshot_.capabilityRevision = acceptedPlan_->Revision();
        }
        PublishState(next);
        if (next == XRSessionState::Inactive || next == XRSessionState::Lost) {
            snapshot_.session = {};
            snapshot_.capabilityRevision = {};
            if (next == XRSessionState::Lost)
                acceptedPlan_.reset();
        }
        return Result<void>::Success();
    }

    /** @copydoc XRSessionLifecycle::Admit */
    Result<void> XRSessionLifecycle::Admit(const XRSessionId &session, const bool needsFocus) const {
        if (!IsOwnerThread())
            return Result<void>::Failure(MakeError(XRSessionErrors::WrongThread));
        if (!session.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        if (session != snapshot_.session)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (needsFocus ? !snapshot_.admitsActions : !snapshot_.admitsFrames)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc XRSessionLifecycle::Snapshot */
    XRSessionSnapshot XRSessionLifecycle::Snapshot() const noexcept {
        return IsOwnerThread() ? snapshot_ : XRSessionSnapshot{};
    }

    /** @copydoc XRSessionLifecycle::AcceptedPlan */
    std::optional<XRFeaturePlan> XRSessionLifecycle::AcceptedPlan() const noexcept {
        return IsOwnerThread() && snapshot_.session.IsValid() ? acceptedPlan_ : std::nullopt;
    }

    void XRSessionLifecycle::Retire(const XRSessionId &session) noexcept {
        if (session.IsValid())
            ReleasePrepared(*resources_, session, PreparationOrder.size());
    }

    void XRSessionLifecycle::PublishState(const XRSessionState state) noexcept {
        using enum XRSessionState;
        snapshot_.state = state;
        snapshot_.admitsFrames = state == Running || state == Visible || state == Focused;
        snapshot_.admitsActions = state == Focused;
    }

    /** @copydoc XRSessionLifecycle::Shutdown */
    void XRSessionLifecycle::Shutdown() noexcept {
        if (!IsOwnerThread() || destroyed_)
            return;
        destroyed_ = true;
        PublishState(XRSessionState::Destroyed);
        Retire(ownedSession_);
        ownedSession_ = {};
        snapshot_.session = {};
        snapshot_.capabilityRevision = {};
        acceptedPlan_.reset();
    }
}  // namespace Horo::XR
