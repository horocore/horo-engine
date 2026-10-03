#pragma once

/**
 * @file XRSessionLifecycle.h
 * @brief Backend-neutral XR session activation, runtime-event state, and generation fencing.
 */

#include "Horo/XR/XRFeatureNegotiation.h"

#include <array>
#include <cstdint>
#include <optional>

namespace Horo::XR {
    /** @brief Ordered host-composition resource boundaries; native values remain in the selected adapter. */
    enum class XRSessionPreparation : std::uint8_t {
        PlatformLoader,
        InstanceSystem,
        RendererCompatibility,
        CapabilitiesAndSpaces,
        NativeSessionAndSwapchains,
        RendererTargets,
        InputAndUi,
        Count
    };

    /** @brief Exact lifecycle state of the currently published session generation. */
    enum class XRSessionState : std::uint8_t {
        Inactive,
        Ready,
        Running,
        Visible,
        Focused,
        Stopping,
        Lost,
        Destroyed
    };

    /** @brief Native runtime event normalized by the selected backend, never synthesized by UI/gameplay. */
    enum class XRSessionEvent : std::uint8_t {
        Started,
        BecameVisible,
        BecameFocused,
        FocusLost,
        VisibilityLost,
        Stopping,
        Idle,
        SessionLost,
        InstanceLost
    };

    /** @brief Fixed-size state publication; consumers must revalidate its exact session before use. */
    struct XRSessionSnapshot final {
        XRSessionId session;                            /**< Published owner, invalid when inactive or destroyed. */
        XRCapabilityRevision capabilityRevision;        /**< Accepted evidence revision, invalid without a session. */
        XRSessionState state{XRSessionState::Inactive}; /**< Event-driven state of this publication. */
        bool admitsFrames{};                            /**< True only while running, visible, or focused. */
        bool admitsActions{};                           /**< True only while focused. */
    };

    /**
     * @brief Host-owned selected-backend resource port used only by lifecycle operations, never on the frame path.
     *
     * The port outlives XRSessionLifecycle. Prepare must undo any work of its own failing stage before returning failure;
     * the coordinator releases completed stages in reverse order. Release must finish all work for that exact session,
     * including renderer retirement before native swapchain destruction, and must not affect another generation.
     * Port callbacks must not re-enter the coordinator; the host quiesces producers before replacement or shutdown.
     */
    class IXRSessionResources {
    public:
        /** @brief Destroys a host resource adapter after its coordinator has shut down. */
        virtual ~IXRSessionResources() = default;

        /**
         * @brief Prepares one ordered stage for a private candidate generation.
         * @param stage Stage being prepared.
         * @param candidate Exact unpublished session owner.
         * @param plan Complete accepted feature decisions; adapter must not rediscover or silently substitute a provider.
         * @return Success or an actionable typed adapter failure; a failing stage has no retained resources.
         */
        [[nodiscard]] virtual Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &plan) = 0;

        /**
         * @brief Retires one completed stage in reverse order for an exact owner.
         * @param stage Completed stage to retire.
         * @param session Owner to retire without touching a replacement generation.
         * @post No resources from this stage remain; the host guarantees no failure escapes this operation.
         */
        virtual void Release(XRSessionPreparation stage, const XRSessionId &session) noexcept = 0;
    };

    /**
     * @brief Single-thread-affine session coordinator; host stops producers before destruction.
     *
     * No method may race another method. Normal event/admission success is fixed work with no allocation, blocking I/O,
     * native call or CPU-GPU synchronization. Activation, replacement and shutdown are host lifecycle operations.
     */
    class XRSessionLifecycle final {
    public:
        /** @brief Binds a selected resource owner that must outlive this coordinator. @param resources Host-owned port. */
        explicit XRSessionLifecycle(IXRSessionResources &resources) noexcept;
        /** @brief Retires any remaining exact session resources. */
        ~XRSessionLifecycle();

        XRSessionLifecycle(const XRSessionLifecycle &) = delete;
        XRSessionLifecycle &operator=(const XRSessionLifecycle &) = delete;

        /**
         * @brief Prepares and publishes a new exact session while preserving any old generation until commit.
         * @param capabilities Immutable evidence for the requested system and runtime generation.
         * @param activeSystem System currently selected by host preflight, including its runtime generation.
         * @param expectedRevision Capability revision retained by the host's accepted plan.
         * @param plan Complete immutable profile and optional-feature decision accepted before resource preparation.
         * @return New session ID, or typed admission/activation failure with the adapter cause preserved.
         * @post Failure leaves the prior publication unchanged and retires only the candidate's completed stages.
         */
        [[nodiscard]] Result<XRSessionId> Activate(const XRCapabilitySnapshot &capabilities, const XRSystemId &activeSystem,
                                                   XRCapabilityRevision expectedRevision, const XRFeaturePlan &plan);

        /**
         * @brief Applies a selected-runtime event to its exact published generation.
         * @param source Session generation carried by the normalized backend event.
         * @param event Closed event vocabulary; delayed foreign generations fail stale without state mutation.
         * @return Success for legal or duplicate events, otherwise a typed stale/invalid-transition failure.
         */
        [[nodiscard]] Result<void> ApplyEvent(const XRSessionId &source, XRSessionEvent event);

        /**
         * @brief Checks frame/action admission and exact current generation before downstream work.
         * @param session Candidate owner retained by a consumer.
         * @param needsFocus Whether the operation requires focused action admission.
         * @return Success or typed invalid, stale, or unavailable failure without native access.
         */
        [[nodiscard]] Result<void> Admit(const XRSessionId &session, bool needsFocus) const;

        /** @brief Returns a fixed-size copy of the current state. @return Generation-fenced state publication. */
        [[nodiscard]] XRSessionSnapshot Snapshot() const noexcept;

        /** @brief Copy the active immutable decision plan, if a session remains published. @return Fixed-size plan or no value. */
        [[nodiscard]] std::optional<XRFeaturePlan> AcceptedPlan() const noexcept;

        /** @brief Closes admission and retires resources in reverse order; idempotent. */
        void Shutdown() noexcept;

    private:
        static constexpr std::size_t PreparationCount = static_cast<std::size_t>(XRSessionPreparation::Count);

        void Retire(const XRSessionId &session) noexcept;
        void PublishState(XRSessionState state) noexcept;

        IXRSessionResources *resources_;
        XRSessionId ownedSession_{};
        XRSessionSnapshot snapshot_{};
        std::optional<XRFeaturePlan> acceptedPlan_;
        std::uint32_t lastSessionGeneration_{};
        bool destroyed_{};
    };
}  // namespace Horo::XR
