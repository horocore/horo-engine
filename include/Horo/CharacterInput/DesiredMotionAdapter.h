#pragma once

/** @file DesiredMotionAdapter.h
 * @brief Explicit host-owned bridge from captured input or generic intent to Character admission.
 */

#include "Horo/Physics/CharacterCapability.h"
#include "Horo/Runtime/Input.h"

#include <memory>

namespace Horo::CharacterInput {
    /** @brief The one producer admitted by a host-issued adapter; no system is discovered implicitly. */
    enum class DesiredMotionSource : std::uint8_t {
        GameplayInput,
        ExternalIntent
    };

    /** @brief Explicit composition principal; copied identity is evidence, not authority. */
    struct DesiredMotionPrincipal final {
        std::uint64_t identity{};
        Input::PlayerId player{};
        DesiredMotionSource source{DesiredMotionSource::GameplayInput};
        bool permissionGranted{}; /**< Default denied; only the host decides module/scene access. */
    };

    /** @brief Backend-neutral intent. Absence preserves controller policy; a present zero explicitly requests stopping. */
    struct DesiredMotionIntent final {
        std::optional<Math::Vec3> velocityMetersPerSecond;
        std::optional<Math::Quaternion> heading;
        bool jumpRequested{};
    };

    /** @brief Load-time action binding and finite world-space projection, independent of Navigation or camera ownership. */
    struct DesiredMotionInputBinding final {
        Input::ActionId move;
        Input::ActionId jump;
        Math::Vec3 right{1, 0, 0};
        Math::Vec3 forward{0, 0, -1};
        float speedMetersPerSecond{5};
    };

    /**
     * @brief Owned immutable fixed-size recording/replay value; contains no router, device, world or callback borrow.
     * @details Its issuing grant generation fences reloads. Copying does not grant authority. Replay submits the
     * original tick and correlation through the matching adapter, never recapturing live devices.
     */
    class DesiredMotionFrame final {
    public:
        /** @brief Constructs inert evidence rejected by Submit. */
        DesiredMotionFrame() noexcept = default;

        /** @brief Returns the exact host simulation tick. @return One-based tick, or zero when inert. */
        [[nodiscard]] std::uint64_t Tick() const noexcept {
            return tick_;
        }

        /** @brief Returns the producer correlation preserved as Character command sequence. @return Non-wrapping identity. */
        [[nodiscard]] std::uint64_t Correlation() const noexcept {
            return correlation_;
        }

        /** @brief Returns copied principal identity. @return Exact host-bound principal, or zero when inert. */
        [[nodiscard]] std::uint64_t Principal() const noexcept {
            return principal_;
        }

        /** @brief Returns the exact bound player. @return Host-selected player identity. */
        [[nodiscard]] Input::PlayerId Player() const noexcept {
            return player_;
        }

        /** @brief Returns the exact admitted producer kind. @return GameplayInput or ExternalIntent. */
        [[nodiscard]] DesiredMotionSource Source() const noexcept {
            return source_;
        }

        /** @brief Returns immutable owned intent. @return Reference valid only for this frame's lifetime. */
        [[nodiscard]] const DesiredMotionIntent &Intent() const noexcept {
            return intent_;
        }

    private:
        friend class DesiredMotionAdapter;
        std::uint64_t tick_{};
        std::uint64_t correlation_{};
        std::uint64_t principal_{};
        Input::PlayerId player_{};
        DesiredMotionSource source_{DesiredMotionSource::GameplayInput};
        Character::CharacterCapabilityIdentity grant_;
        Character::CharacterControllerHandle controller_;
        DesiredMotionIntent intent_;
        /** @brief Compares complete issuing evidence; never grants access.
         * @param principal Host-bound producer. @param grant Current grant. @param controller Bound controller.
         * @return Whether every owner generation and producer discriminator matches. */
        [[nodiscard]] bool IssuedFor(const DesiredMotionPrincipal &principal, const Character::CharacterCapabilityIdentity &grant,
                                     const Character::CharacterControllerHandle &controller) const noexcept;
    };

    /** @brief Whether ownership transferred to the existing Character queue, or no motion was authored for this tick. */
    struct DesiredMotionSubmission final {
        bool absent{};
        Character::CharacterCommandAdmission admission;
    };

    /**
     * @brief Move-only preparation-thread owner of one independently issued Character grant and input capture state.
     * @details Successful capture/translation/admission performs bounded work without allocation. Creation/reload may
     * allocate; typed errors retain the existing Character failure. Input capture runs once per committed presentation
     * snapshot after higher-priority consumers, before fixed simulation. Translation and Submit never inspect a router.
     * The bound router address is retained only as capture-owner identity, never dereferenced by translation or admission.
     * The host closes this adapter before destroying/replacing that router, avoiding address-reuse ambiguity.
     * World tick execution remains the host's responsibility. Full/busy admission retains no adapter backlog: the caller
     * may retry the same immutable frame. Shutdown revokes this grant before releasing local state; moved-from owners
     * fail closed. The host must issue a dedicated grant, not one shared with another client it wants to keep alive.
     */
    class DesiredMotionAdapter final {
    public:
        /** @brief Constructs an inert, allocation-free owner. */
        DesiredMotionAdapter() noexcept;
        /** @brief Revokes the owned grant; retained frames remain valid inert recording values. */
        ~DesiredMotionAdapter();
        DesiredMotionAdapter(const DesiredMotionAdapter &) = delete;
        DesiredMotionAdapter &operator=(const DesiredMotionAdapter &) = delete;
        /** @brief Transfers authority and pending capture; leaves the source inert. @param other Source owner. */
        DesiredMotionAdapter(DesiredMotionAdapter &&other) noexcept;
        /** @brief Closes prior authority then transfers the new owner. @param other Source owner. @return This owner. */
        DesiredMotionAdapter &operator=(DesiredMotionAdapter &&other) noexcept;

        /**
         * @brief Creates an explicit principal/controller bridge after live capability and bounded input-map admission.
         * @param principal Host-authorized producer; default permission is denied.
         * @param capability Dedicated grant issued by the Character owner; the caller transfers lifecycle responsibility.
         * @param controller Exact live controller owned by the grant's scene/world.
         * @param binding Finite projection and semantic actions; ignored for ExternalIntent producers.
         * @param router Synchronous load-time borrow; required only for GameplayInput.
         * @param context Synchronous exact Gameplay context borrow; required only for GameplayInput.
         * @return Owned adapter or original typed capability/descriptor/admission failure. Failed creation does not revoke the grant.
         * @throws std::bad_alloc When load-time owner storage or typed diagnostic allocation fails; the caller's grant stays unchanged.
         */
        [[nodiscard]] static Result<DesiredMotionAdapter> Create(DesiredMotionPrincipal principal,
                                                                 Character::CharacterCapability capability,
                                                                 Character::CharacterControllerHandle controller,
                                                                 DesiredMotionInputBinding binding = {},
                                                                 Input::InputRouter *router = nullptr,
                                                                 const Input::InputContextToken *context = nullptr);

        /**
         * @brief Captures owned held axes and pending jump edges once per committed presentation snapshot.
         * @param router Live router borrowed only until return.
         * @param context Exact original context token borrowed only until return.
         * @return Success or typed routing/capacity failure. Focus/preemption clears pending intent. Configuration,
         * assignment or context replacement fail closed until a fresh adapter is created; no old edges cross reload.
         */
        [[nodiscard]] Result<void> CaptureInput(Input::InputRouter &router, const Input::InputContextToken &context);
        /**
         * @brief Converts captured axes and consumes pending jump once for a strictly increasing fixed tick.
         * @param tick Exact one-based simulation tick.
         * @param correlation Exact nonzero producer sequence; preserved without synthesizing identity.
         * @return Immutable frame, including explicit absent intent before eligible capture; never reads devices.
         */
        [[nodiscard]] Result<DesiredMotionFrame> ConsumeInput(std::uint64_t tick, std::uint64_t correlation);
        /**
         * @brief Captures generic fixed-tick intent for an ExternalIntent principal without importing Navigation types.
         * @param tick Exact one-based simulation tick.
         * @param correlation Exact nonzero producer sequence.
         * @param intent Owned finite intent, including absence or explicit zero.
         * @return Immutable frame or typed producer/order/finite-value failure.
         */
        [[nodiscard]] Result<DesiredMotionFrame> CaptureIntent(std::uint64_t tick, std::uint64_t correlation,
                                                               const DesiredMotionIntent &intent);
        /**
         * @brief Admits one owned frame through CharacterCapability::QueueMovementCommand, without advancing simulation.
         * @param frame Exact principal/grant/controller recording value; no storage is retained by this owner.
         * @return Absent outcome, copied full/busy/deferred admission, or original typed lifecycle/order failure.
         * Successful absent frames also advance the adapter watermark. Busy/full retries do not advance it.
         */
        [[nodiscard]] Result<DesiredMotionSubmission> Submit(const DesiredMotionFrame &frame);
        /** @brief Permanently revokes admission and clears pending input; idempotent and callback-free. */
        void Shutdown() noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
        /** @brief Checks exact issuing ownership and submission ordering without changing watermarks.
         * @param frame Immutable candidate. @return Success or a typed inert/stale/order failure. */
        [[nodiscard]] Result<void> ValidateSubmissionFrame(const DesiredMotionFrame &frame) const;
        /** @brief Validates and commits one producer watermark after complete owned-intent admission.
         * @param tick One-based simulation tick. @param correlation Nonzero producer identity. @param intent Owned candidate.
         * @return Published immutable value or a typed failure preserving pending edges and prior watermarks. */
        [[nodiscard]] Result<DesiredMotionFrame> MakeFrame(std::uint64_t tick, std::uint64_t correlation,
                                                           const DesiredMotionIntent &intent);
    };
}  // namespace Horo::CharacterInput
