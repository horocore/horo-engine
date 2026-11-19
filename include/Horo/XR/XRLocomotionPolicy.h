#pragma once

/** @file XRLocomotionPolicy.h @brief Gameplay-owned XR locomotion policies over committed actions and calibrated tracking. */

#include "Horo/XR/XRCharacterLocomotion.h"

namespace Horo::XR {
    /** @brief Disables artificial translation independently of the turn policy. */
    struct XRNoTranslation final {};

    /** @brief Gameplay-axis movement relative to the host's selected heading. */
    struct XRSmoothMove final {
        float maximumSpeed{};
        float deadZone{}; /**< Radial dead zone in [0, 1). */
    };

    /** @brief Bounded forward movement from opposite fore-aft hand velocities. */
    struct XRArmSwingMove final {
        float maximumSpeed{};
        float gain{}; /**< Forward speed per meter per second of paired hand motion. */
    };

    /** @brief Reconciles tracked head displacement with the Character through velocity intent. */
    struct XRRoomScaleMove final {
        float maximumSpeed{};
        float maximumSampleDistance{}; /**< Reject larger displacements as discontinuities. */
    };

    /** @brief Enables teleport edges through the configured destination owner. */
    struct XRTeleportMove final {};

    /** @brief Configurable one-shot turn with separate engage and release thresholds. */
    struct XRSnapTurn final {
        float angleRadians{};
        float engage{};
        float release{};
    };

    /** @brief Exactly one active translation mode. */
    using XRTranslationPolicy = std::variant<XRNoTranslation, XRSmoothMove, XRArmSwingMove, XRRoomScaleMove, XRTeleportMove>;

    /** @brief Authored settings for one player, independent of device capabilities or renderer. */
    struct XRLocomotionProfile final {
        XRTranslationPolicy translation;
        std::optional<XRSnapTurn> turn;
        std::int64_t maximumTrackingGapNanoseconds{}; /**< Required when translation uses pose history. */
    };

    /** @brief A committed host Input sample and tracking projections for the same cut. */
    struct XRLocomotionSample final {
        XRLocomotionScope scope;
        std::uint64_t tick{};
        std::uint64_t sequence{};
        std::int64_t fixedStepNanoseconds{}; /**< Duration of the destination Character tick. */
        Math::Vec2 move;                     /**< Unit gameplay axis with positive Y forward. */
        float turn{};                        /**< Signed unit turn action. */
        bool teleportPressed{};
        float gameplayYawRadians{};          /**< Gameplay's current heading around World +Y. */
        std::optional<Math::Vec3> head;      /**< Tracked head position before presentation-late sampling. */
        std::optional<Math::Vec3> leftHand;  /**< Tracked left hand position from the same cutoff. */
        std::optional<Math::Vec3> rightHand; /**< Tracked right hand position from the same cutoff. */
    };

    /** @brief Gameplay navigation or physics owner that checks a teleport action against its live query cutoff. */
    class IXRLocomotionDestinationOwner {
    public:
        virtual ~IXRLocomotionDestinationOwner() = default;
        /**
         * @brief Resolves a destination with an owner-issued revision and calibrated World space.
         * @param sample Exact committed action and tracking cut. Borrowed for the call.
         * @return Validated candidate, empty for no admissible target, or typed owner failure.
         * @details The owner uses bounded snapshot queries and performs no blocking work. It must not reenter the policy.
         * Character independently checks placement at its own commit boundary.
         */
        [[nodiscard]] virtual Result<std::optional<XRLocomotionTeleport>> Resolve(const XRLocomotionSample &sample) const = 0;
    };

    /** @brief Owner-lane, single-player policy with bounded pose history and explicit focus and configuration transitions. */
    class XRLocomotionPolicy final {
    public:
        XRLocomotionPolicy(const XRLocomotionPolicy &) = delete;
        XRLocomotionPolicy &operator=(const XRLocomotionPolicy &) = delete;
        XRLocomotionPolicy(XRLocomotionPolicy &&) noexcept = default;
        XRLocomotionPolicy &operator=(XRLocomotionPolicy &&) = delete;

        /**
         * @brief Validates and binds one gameplay profile.
         * @param profile Authored translation and turn policies.
         * @param session Admitted XR session.
         * @param player Owning local player.
         * @param assignmentRevision Exact nonzero Input assignment.
         * @param destination Borrowed owner for teleport targeting.
         * @return Policy or typed admission error.
         */
        [[nodiscard]] static Result<XRLocomotionPolicy> Create(XRLocomotionProfile profile, XRSessionId session, Input::PlayerId player,
                                                               std::uint64_t assignmentRevision,
                                                               std::weak_ptr<const IXRLocomotionDestinationOwner> destination = {});

        /**
         * @brief Replaces authored settings at the host safe point.
         * @param profile Next validated gameplay profile.
         * @return Typed failure preserves existing settings and history.
         * @details The first focused sample after replacement emits neutral and clears held edges.
         */
        [[nodiscard]] Result<void> SetProfile(XRLocomotionProfile profile);

        /**
         * @brief Produces at most one command for the exact owner tick.
         * @param sample Committed gameplay input and pose evidence for the requested tick. The host projects tracked
         * positions into calibrated World axes with a fixed tracking reference independent of the Character root.
         * @param tracking Current immutable tracking publication.
         * @return Optional adapter-ready intent or typed failure. Errors preserve history and allow an exact retry.
         * @details Loss of focus or tracking clears history and emits no command. Recovery inserts a neutral tick before
         * movement resumes. The caller can retry a produced intent at the Character adapter while its dispatch is current.
         */
        [[nodiscard]] Result<std::optional<XRLocomotionIntent>> Evaluate(const XRLocomotionSample &sample,
                                                                         const XRTrackingSnapshot &tracking);

        /** @brief Clears history and permanently stops evaluating this binding. */
        void Shutdown() noexcept;

    private:
        struct History final {
            XRInteractionCutoff cutoff;
            std::optional<Math::Vec3> head;
            std::optional<Math::Vec3> leftHand;
            std::optional<Math::Vec3> rightHand;
            bool snapArmed{};
            bool teleportArmed{};
            bool neutralRequired{true};
        };

        XRLocomotionPolicy(XRLocomotionProfile profile, XRSessionId session, Input::PlayerId player, std::uint64_t assignmentRevision,
                           std::weak_ptr<const IXRLocomotionDestinationOwner> destination);
        [[nodiscard]] Result<std::optional<XRLocomotionOperation>> Select(const XRLocomotionSample &sample, History &next) const;
        [[nodiscard]] Result<std::optional<XRLocomotionTeleport>> SelectTeleport(const XRLocomotionSample &sample, History &next) const;
        [[nodiscard]] std::optional<Math::Vec3> Velocity(const XRLocomotionSample &sample, const History &previous,
                                                         std::int64_t elapsed) const noexcept;
        [[nodiscard]] static Math::Vec3 ArmVelocity(const XRArmSwingMove &policy, const XRLocomotionSample &sample, const History &previous,
                                                    std::int64_t elapsed) noexcept;
        [[nodiscard]] Result<void> ValidateScope(const XRLocomotionSample &sample, const XRTrackingSnapshot &tracking) const;
        [[nodiscard]] Result<std::optional<XRLocomotionOperation>> ResolveOperation(const XRLocomotionSample &sample, History &next);
        void Commit(const XRLocomotionSample &sample, History &next);
        XRLocomotionProfile profile_;
        XRSessionId session_;
        Input::PlayerId player_{};
        std::uint64_t assignmentRevision_{};
        std::weak_ptr<const IXRLocomotionDestinationOwner> destination_;
        History history_;
        std::uint64_t tick_{};
        std::uint64_t sequence_{};
        bool stopped_{};
        bool evaluating_{};
    };
}  // namespace Horo::XR
