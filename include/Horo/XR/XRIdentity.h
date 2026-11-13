#pragma once

/**
 * @file XRIdentity.h
 * @brief Backend-neutral generation-safe XR system, session, space, view, action, and device identities.
 */

#include "Horo/Foundation/Handles.h"
#include "Horo/Foundation/Result.h"
#include "Horo/XR/XRErrors.h"

#include <compare>
#include <cstdint>

namespace Horo::XR {
    /** @brief Strong non-zero process-local generation owned by one XR lifecycle boundary. */
    template <typename Tag> class XRGeneration final {
    public:
        /** @brief Constructs the reserved invalid generation. */
        XRGeneration() = default;

        constexpr auto operator<=>(const XRGeneration &) const noexcept = default;

        /**
         * @brief Validates an owner-issued generation value.
         * @param value Non-zero monotonic value that will never be reused by the owner.
         * @return Strong generation or XRErrors::IdentityInvalid.
         */
        [[nodiscard]] static Result<XRGeneration> Create(const std::uint64_t value) {
            XRGeneration candidate;
            candidate.value_ = value;
            return candidate.IsValid() ? Result<XRGeneration>::Success(candidate)
                                       : Result<XRGeneration>::Failure(MakeError(XRErrors::IdentityInvalid));
        }

        /** @brief Returns the process-local owner-issued value. @return Zero only for the invalid generation. */
        [[nodiscard]] constexpr std::uint64_t Value() const noexcept {
            return value_;
        }

        /** @brief Checks representation, not owner liveness. @return Whether the value is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value_ != 0;
        }

    private:
        std::uint64_t value_{};
    };

    struct XRRuntimeGenerationTag;
    struct XRCapabilityRevisionTag;

    /** @brief Process-local XR runtime incarnation; invalidated on backend replacement or shutdown. */
    /** @brief Runtime incarnation, never a persisted artifact identity.
     * The built-in native application host reserves [1, UINT32_MAX] for bounded selection attempts.
     * The built-in Simulator's shared cold owner issuer reserves [2^32, UINT64_MAX-1], never wraps,
     * and preserves its incarnation across canonical session replacement. Arbitrary external providers
     * must coordinate their own identity domain; this contract does not assert their process uniqueness.
     */
    using XRRuntimeGeneration = XRGeneration<XRRuntimeGenerationTag>;
    /** @brief Monotonic identity of one immutable capability publication. */
    using XRCapabilityRevision = XRGeneration<XRCapabilityRevisionTag>;

    struct XRSystemSlotTag;
    struct XRSessionSlotTag;
    struct XRSpaceSlotTag;
    struct XRViewSlotTag;
    struct XRViewConfigurationSlotTag;
    struct XRSwapchainTargetSlotTag;
    struct XRSwapchainImageSlotTag;
    struct XRActionSlotTag;
    struct XRDeviceSlotTag;

    /** @brief Runtime-owned system identity scoped to one exact runtime incarnation. */
    struct XRSystemId final {
        XRRuntimeGeneration runtime;        /**< Runtime incarnation that selected the system. */
        Horo::Handle<XRSystemSlotTag> slot; /**< Runtime-private system slot and generation. */

        /** @brief Checks representation only. @return True when every owner and slot component is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return runtime.IsValid() && slot.IsValid() && slot.generation != 0;
        }

        constexpr auto operator<=>(const XRSystemId &) const noexcept = default;
    };

    /** @brief System-owned session identity invalidated by session replacement or system loss. */
    struct XRSessionId final {
        XRSystemId system;                   /**< Exact system incarnation that owns the session. */
        Horo::Handle<XRSessionSlotTag> slot; /**< System-private session slot and generation. */

        /** @brief Checks representation only. @return True when every owner and slot component is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return system.IsValid() && slot.IsValid() && slot.generation != 0;
        }

        constexpr auto operator<=>(const XRSessionId &) const noexcept = default;
    };

    /** @brief Session-owned generation-safe runtime identity with no native handle or persistent meaning. */
    template <typename Tag> struct XRSessionObjectId final {
        XRSessionId session;    /**< Exact session incarnation that owns the object. */
        Horo::Handle<Tag> slot; /**< Session-private registry slot and non-zero generation. */

        /** @brief Checks representation only. @return True when every owner and slot component is valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return session.IsValid() && slot.IsValid() && slot.generation != 0;
        }

        constexpr auto operator<=>(const XRSessionObjectId &) const noexcept = default;
    };

    /** @brief Live reference-space identity scoped to one XR session. */
    using XRSpaceId = XRSessionObjectId<XRSpaceSlotTag>;
    /** @brief Live view identity scoped to one XR session and never assumed to be a stereo eye index. */
    using XRViewId = XRSessionObjectId<XRViewSlotTag>;
    /** @brief Active view-configuration identity scoped to one exact XR session. */
    using XRViewConfigurationId = XRSessionObjectId<XRViewConfigurationSlotTag>;
    /** @brief Runtime-owned swapchain target identity scoped to one exact XR session. */
    using XRSwapchainTargetId = XRSessionObjectId<XRSwapchainTargetSlotTag>;
    /** @brief Live Horo action identity scoped to one XR session; native paths remain backend-private. */
    using XRActionId = XRSessionObjectId<XRActionSlotTag>;
    /** @brief Live input/tracking device identity scoped to one XR session. */
    using XRDeviceId = XRSessionObjectId<XRDeviceSlotTag>;

    /** @brief Runtime-owned image identity nested under one exact swapchain target generation. */
    struct XRSwapchainImageId final {
        XRSwapchainTargetId target;                 /**< Exact runtime-owned target incarnation. */
        Horo::Handle<XRSwapchainImageSlotTag> slot; /**< Target-private image slot and non-zero generation. */

        /** @brief Checks representation only. @return True when the target and image slot are valid. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return target.IsValid() && slot.IsValid() && slot.generation != 0;
        }

        constexpr auto operator<=>(const XRSwapchainImageId &) const noexcept = default;
    };

    /**
     * @brief Validates a system before runtime-owned registry access.
     * @param system Candidate system identity.
     * @param activeSystem Exact active system identity, or invalid after shutdown.
     * @return Success, XRErrors::IdentityInvalid for malformed/shutdown input, or XRErrors::IdentityStale for replacement.
     */
    [[nodiscard]] Result<void> ValidateXRSystem(const XRSystemId &system, const XRSystemId &activeSystem);

    /**
     * @brief Validates a session before system-owned registry access.
     * @param session Candidate session identity.
     * @param activeSession Exact active session identity, or invalid after shutdown.
     * @return Success, XRErrors::IdentityInvalid for malformed/shutdown input, or XRErrors::IdentityStale for replacement.
     */
    [[nodiscard]] Result<void> ValidateXRSession(const XRSessionId &session, const XRSessionId &activeSession);

    /**
     * @brief Validates a session-owned object before any registry or native access.
     * @param object Candidate space, view, action, or device identity.
     * @param activeSession Exact active session, or invalid after shutdown.
     * @return Success, XRErrors::IdentityInvalid for malformed/shutdown input, or XRErrors::IdentityStale for replacement.
     */
    template <typename Tag>
    [[nodiscard]] Result<void> ValidateXRSessionObject(const XRSessionObjectId<Tag> &object, const XRSessionId &activeSession) {
        if (!object.IsValid() || !activeSession.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::IdentityInvalid));
        if (object.session != activeSession)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        return Result<void>::Success();
    }
}  // namespace Horo::XR
