#pragma once

/** @file NavigationCharacterAdapter.h
 * @brief Explicit host bridge from safe Navigation publications to canonical Character admission and feedback.
 */

#include "Horo/CharacterInput/DesiredMotionAdapter.h"
#include "Horo/Navigation/NavigationVelocityPipeline.h"

namespace Horo::Navigation {
    /** @brief Host-selected producer and controller incarnation; constructing this value grants no authority. */
    struct NavigationCharacterBinding final {
        CharacterInput::DesiredMotionPrincipal principal;
        Character::CharacterControllerHandle controller;
        NavigationAgentSceneBinding navigation;
        std::uint64_t characterRevision{}; /**< Nonzero revision shared with the bound velocity pipeline. */
    };

    /** @brief Immutable retry value; copying retains evidence, never Character authority or a world borrow. */
    class NavigationCharacterMotion final {
    public:
        /** @brief Construct inert work rejected by Submit. */
        NavigationCharacterMotion() noexcept = default;

        /** @brief Return the exact fixed simulation tick. @return One-based tick, or zero for inert work. */
        [[nodiscard]] std::uint64_t Tick() const noexcept {
            return frame_.Tick();
        }

        /** @brief Return the exact Character command sequence. @return Host correlation, or zero for inert work. */
        [[nodiscard]] std::uint64_t Correlation() const noexcept {
            return frame_.Correlation();
        }

    private:
        friend class NavigationCharacterAdapter;
        CharacterInput::DesiredMotionFrame frame_;
        NavigationVelocityPublication publication_;
        std::uint64_t rootRevision_{};
        float stepSeconds_{};
    };

    /**
     * @brief Move-only owner-thread adapter composed above NavigationRuntime and CharacterInput.
     * @details The borrowed velocity pipeline and its movement owner outlive this adapter. The host supplies a
     * dedicated Character grant and an ExternalIntent principal, and advances Character with the exact same tick
     * and fixed quantum used by Navigation. Prepare/Submit never advance simulation or write a Transform/Physics body.
     * Character alone resolves root motion, steps, grounding and collisions. One admitted result is retained until
     * ObserveFeedback or DiscardFeedback; full/busy submissions retain no backlog and retry the same immutable work.
     * Successful fixed-tick calls allocate no adapter storage. Shutdown revokes the dedicated grant but does not
     * retire the borrowed pipeline or recall already admitted commands. Before teleport/replacement/reload, the host
     * shuts down this adapter, resets/rebinds Navigation, and creates a fresh bridge with a fresh grant and revision.
     * A rejected publication authorizes no motion; the host retains responsibility for its explicit stop policy.
     */
    class NavigationCharacterAdapter final {
    public:
        /** @brief Construct an inert owner without allocation. */
        NavigationCharacterAdapter() noexcept;
        /** @brief Revoke the owned dedicated Character grant. */
        ~NavigationCharacterAdapter();
        NavigationCharacterAdapter(const NavigationCharacterAdapter &) = delete;
        NavigationCharacterAdapter &operator=(const NavigationCharacterAdapter &) = delete;
        /** @brief Transfer authority, leaving the source inert. @param other Source owner. */
        NavigationCharacterAdapter(NavigationCharacterAdapter &&other) noexcept;
        /** @brief Revoke prior authority then transfer. @param other Source owner. @return This owner. */
        NavigationCharacterAdapter &operator=(NavigationCharacterAdapter &&other) noexcept;

        /** @brief Bind a real Character grant to a host-selected Navigation/controller incarnation.
         * @param pipeline Borrowed Navigation publication and feedback owner.
         * @param capability Dedicated grant; failed creation does not revoke it.
         * @param binding Explicit permitted ExternalIntent producer and exact scene/controller incarnation.
         * @return Prepared owner or original typed admission failure.
         * @throws std::bad_alloc On load-time owner allocation; the caller's grant remains unchanged.
         */
        [[nodiscard]] static Result<NavigationCharacterAdapter> Create(NavigationVelocityPipeline &pipeline,
                                                                       Character::CharacterCapability capability,
                                                                       const NavigationCharacterBinding &binding);
        /** @brief Capture a safe publication after validating live Navigation and Character authority.
         * @param publication Exact current Commit result, including explicit zero stops.
         * @param current Fresh pre-Character observation matching its collision-root position and fixed quantum.
         * @param correlation Nonzero increasing host command sequence, preserved through Character and feedback.
         * @return Owned immutable work or typed failure. A successfully captured tick is not recaptured for retries.
         */
        [[nodiscard]] Result<NavigationCharacterMotion> Prepare(const NavigationVelocityPublication &publication,
                                                                const NavigationMovementObservation &current, std::uint64_t correlation);
        /** @brief Revalidate work at the owner safe point and hand it to the canonical Character command queue.
         * @param motion Exact immutable work from this adapter; caller retains it for full/busy retries.
         * @param current Fresh same-tick observation, read immediately before admission.
         * @return Original Character admission or typed failure; only deferred admission retains one feedback identity.
         */
        [[nodiscard]] Result<CharacterInput::DesiredMotionSubmission> Submit(const NavigationCharacterMotion &motion,
                                                                             const NavigationMovementObservation &current);
        /** @brief Copy the exact post-Character snapshot and forward achieved displacement to existing Nav hysteresis.
         * @return Owned complete Character movement/root/ground evidence or typed stale/revoked failure.
         * @details Only the admitted tick/sequence/controller may supply feedback. A later teleport/root publication
         * invalidates a historical snapshot. Achieved velocity, including root motion and platform carry, is multiplied
         * by the host-declared common quantum; neither desired velocity nor a speculative Transform delta is feedback.
         */
        [[nodiscard]] Result<Character::CharacterLocomotionSnapshot> ObserveFeedback();
        /** @brief Drop pending feedback after host cancellation/replacement; never recalls a Character command. */
        void DiscardFeedback() noexcept;
        /** @brief Revoke this dedicated grant and release local storage; idempotent, leaving Navigation host-owned. */
        void Shutdown() noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
        /** @brief Recheck exact publication, bridge incarnation and pre-tick Character root without mutation. */
        [[nodiscard]] Result<Character::CharacterTransformPublication> Validate(const NavigationVelocityPublication &publication,
                                                                                const NavigationMovementObservation &current) const;
    };
}  // namespace Horo::Navigation
