#pragma once

/** @file XRCharacterLocomotion.h @brief Owner-bound bridge from routed XR locomotion intent to Character and calibration admission. */

#include "Horo/Physics/CharacterCapability.h"
#include "Horo/Runtime/FrameScheduler.h"
#include "Horo/Runtime/Input.h"
#include "Horo/XR/XRInteraction.h"

#include <memory>
#include <optional>
#include <variant>

namespace Horo::XR {
    /** @brief One actor's translation and absolute heading for the next admitted Character tick. */
    struct XRLocomotionMotion final {
        std::optional<Math::Vec3> velocityMetersPerSecond;
        std::optional<Math::Quaternion> heading;
    };

    /** @brief One gameplay-selected destination in the current World projection. The Character owner still validates clearance. */
    struct XRLocomotionTeleport final {
        XRCoordinateSpace space;
        std::uint64_t destinationRevision{};
        Math::Vec3 position;
        Math::Quaternion heading{Math::Quaternion::Identity()};
    };

    /** @brief Requests a calibration change from the application owner of the selected reference space. */
    struct XRLocomotionRecenter final {
        XRCoordinateSpace reference;
    };

    /** @brief At most one command reaches a locomotion owner per admitted player tick. */
    using XRLocomotionOperation = std::variant<XRLocomotionMotion, XRLocomotionTeleport, XRLocomotionRecenter>;

    /** @brief Exact host-routed tracking and player assignment used to produce this intent. */
    struct XRLocomotionScope final {
        XRInteractionCutoff tracking;
        Input::PlayerId player{};
        std::uint64_t assignmentRevision{};
    };

    /** @brief Owned replay-safe evidence from the gameplay intent producer. It grants no mutable access. */
    struct XRLocomotionIntent final {
        XRLocomotionScope scope;
        std::uint64_t tick{};
        std::uint64_t sequence{};
        XRLocomotionOperation operation;
    };

    /** @brief Outcome of one non-blocking owner submission. */
    enum class XRLocomotionAdmission : std::uint8_t {
        Queued,
        Full,
        Busy
    };

    /** @brief Narrow Character-side admission for a destination verified by the gameplay owner. */
    class IXRLocomotionTeleportOwner {
    public:
        virtual ~IXRLocomotionTeleportOwner() = default;
        /** @brief Queues clearance and placement work for the Character owner.
         * @param intent Exact validated XR source and destination.
         * @param request Checked Character handle, tick and proposed placement.
         * @param lifetime Cancellation of the adapter or its binding. Owners check it before committing queued work.
         * @return Admission outcome. Only Queued transfers ownership. No placement is implied by admission.
         * @details Borrowed input is valid only for this call. Do not run a Character tick, block, or reenter the adapter. */
        [[nodiscard]] virtual Result<XRLocomotionAdmission> QueueTeleport(const XRLocomotionIntent &intent,
                                                                          const Character::CharacterTeleportRequest &request,
                                                                          const CancellationToken &lifetime) = 0;
    };

    /** @brief Narrow application-side admission for recentering the selected native or simulated reference. */
    class IXRLocomotionRecenterOwner {
    public:
        virtual ~IXRLocomotionRecenterOwner() = default;
        /** @brief Queues calibration work with no Character access.
         * @param intent Exact validated player and reference change request.
         * @param lifetime Cancellation of the adapter or its binding. Owners check it before committing queued work.
         * @return Admission outcome. Borrowed input is valid only for this non-blocking call. */
        [[nodiscard]] virtual Result<XRLocomotionAdmission> QueueRecenter(const XRLocomotionIntent &intent,
                                                                          const CancellationToken &lifetime) = 0;
    };

    /** @brief Explicit immutable composition selected by the gameplay host. */
    struct XRCharacterLocomotionBinding final {
        Runtime::RuntimeDispatchSource scheduler;
        XRSessionId session;
        Input::PlayerId player{};
        std::uint64_t assignmentRevision{};
        Character::CharacterControllerHandle controller;
        float maximumSpeedMetersPerSecond{};
        std::weak_ptr<IXRLocomotionTeleportOwner> teleport;
        std::weak_ptr<IXRLocomotionRecenterOwner> recenter;
    };

    /** @brief Move-only bridge borrowing actual scheduler evidence and deriving an exclusive Character command grant. */
    class XRCharacterLocomotionAdapter final {
    public:
        XRCharacterLocomotionAdapter() noexcept = default;
        ~XRCharacterLocomotionAdapter();
        XRCharacterLocomotionAdapter(const XRCharacterLocomotionAdapter &) = delete;
        XRCharacterLocomotionAdapter &operator=(const XRCharacterLocomotionAdapter &) = delete;
        XRCharacterLocomotionAdapter(XRCharacterLocomotionAdapter &&) noexcept = default;
        XRCharacterLocomotionAdapter &operator=(XRCharacterLocomotionAdapter &&other) noexcept;

        /** @brief Binds enabled host-selected locomotion to the exact active controller.
         * @param binding Immutable scheduler, player, controller and finite movement limit.
         * @param authority Parent Character grant. Shutdown closes only the derived child.
         * @return Owned adapter or typed invalid, stale, capability or capacity error. */
        [[nodiscard]] static Result<XRCharacterLocomotionAdapter> Create(XRCharacterLocomotionBinding binding,
                                                                         const Character::CharacterCapability &authority);

        /** @brief Revalidates exact tracking, Input assignment and the actual fixed dispatch, then submits without simulation or Scene
         * reads.
         * @param intent Already routed gameplay request from this exact player and sampled World.
         * @param tracking Current immutable XR publication, borrowed through this call.
         * @param assignmentRevision Current assignment generation observed by the host's Input owner.
         * @param step Actual fixed dispatch context, borrowed through this call.
         * @return Typed accepted, full, busy, unsupported, stale or invalid outcome. Full and busy retain no backlog. */
        [[nodiscard]] Result<XRLocomotionAdmission> Submit(const XRLocomotionIntent &intent, const XRTrackingSnapshot &tracking,
                                                           std::uint64_t assignmentRevision, const Runtime::FixedStepContext &step);

        /** @brief Revokes unfrozen movement and cancels queued calibration/placement lifetime. */
        void Shutdown() noexcept;

    private:
        struct State;
        explicit XRCharacterLocomotionAdapter(std::shared_ptr<State> state) noexcept;
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::XR
