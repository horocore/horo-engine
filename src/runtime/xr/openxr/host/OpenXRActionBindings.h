#pragma once

/** @file OpenXRActionBindings.h
 * @brief Non-installed OpenXR action ownership, suggested paths and active-profile adapter.
 */

#include "Horo/XR/XRActionBindings.h"
#include "OpenXRNativeSession.h"

namespace Horo::XR::OpenXRInternal {
    /** @brief Registered native profile path; native spellings never enter persistence or gameplay identity. */
    struct NativeInteractionProfilePath final {
        XRInteractionProfileId profile;
        std::string_view path;
    };

    /** @brief Registered native component path joined by exact Horo profile/control/role identity. */
    struct NativeActionControlPath final {
        XRInteractionProfileId profile;
        XRPhysicalControlId control;
        XRTrackedDeviceRole role;
        std::string_view path;
    };

    /** @brief Host-localized native binding-editor label, separate from canonical action identity. */
    struct NativeActionLabel final {
        Input::ActionId action;
        std::string_view label;
    };

    /** @brief Host-localized native set label, independent of native names and dynamic Input contexts. */
    struct NativeActionSetLabel final {
        XRActionSetId set;
        std::string_view label;
    };

    /** @brief Borrowed localized presentation copied only during native action creation. */
    struct NativeActionLabels final {
        std::span<const NativeActionLabel> actions;
        std::span<const NativeActionSetLabel> sets;
    };

    /** @brief Synchronously borrowed native creation request; dispatch and catalogs stay inside the host boundary. */
    struct NativeActionBindingRequest final {
        PFN_xrGetInstanceProcAddr getProc; /**< Official dispatch for the exact retained loader/instance. */
        const XRActionBindingSchema &schema;
        std::span<const XRProfileControl> catalog;
        std::span<const XRActionBindingOverride> overrides;
        std::span<const NativeInteractionProfilePath> profiles;
        std::span<const NativeActionControlPath> controls;
        NativeActionLabels labels;
    };

    /**
     * @brief Single-control-thread native action transaction; session/loader owner must outlive it.
     *
     * Creation validates every schema, override and native path before allocation, creates sets/actions,
     * suggests all registered profiles, then attaches exactly once before host Ready publication.
     * Input contexts do not create/destroy native sets. The host quiesces sampling, neutralizes the
     * XR/Input coordinator, closes this owner, and only then closes the native session.
     */
    class OpenXRActionBindings final {
    public:
        /** @brief Borrows the exact fenced session owner without resolving native dispatch. */
        explicit OpenXRActionBindings(OpenXRNativeSession &session) noexcept;
        /** @brief Retires native actions; explicit successful Close is required before destruction. */
        ~OpenXRActionBindings();
        OpenXRActionBindings(const OpenXRActionBindings &) = delete;
        OpenXRActionBindings &operator=(const OpenXRActionBindings &) = delete;

        /**
         * @brief Creates and attaches a fully validated native action candidate atomically.
         * @param session Exact current Horo session generation.
         * @param request Product schema, registered private paths, Input overrides, localized labels and official native dispatch.
         * @return Success or typed validation/native/rollback error; failed retirement retains handles for Close retry.
         * @throws Non-standard native callback exceptions after attempting rollback; the original exception propagates.
         */
        [[nodiscard]] Result<void> Create(const XRSessionId &session, const NativeActionBindingRequest &request);
        /**
         * @brief Queries both active controller profiles as Horo IDs; unrecognized native paths map to unknown identity.
         * @param session Exact retained session generation.
         * @return Bounded per-role evidence or typed stale/native failure. Host resolves and neutralizes changes before Sync/sample.
         */
        [[nodiscard]] Result<std::array<XRActiveInteractionProfile, 2>> Profiles(const XRSessionId &session) const;
        /** @brief Synchronizes attached native sets after host binding neutralization/admission.
         * @param session Exact retained owner.
         * @return Success or typed focus/session/native failure; no gameplay or Input mutation occurs here.
         */
        [[nodiscard]] Result<void> Sync(const XRSessionId &session) const;
        /** @brief Quiescent idempotent reverse-order retirement; failed destruction retains exact handles for retry.
         * @return Success or native retirement failure. No further sync/profile query is admitted after Close begins.
         */
        [[nodiscard]] Result<void> Close();

    private:
        struct Dispatch final {
            PFN_xrStringToPath stringToPath{};
            PFN_xrCreateActionSet createSet{};
            PFN_xrDestroyActionSet destroySet{};
            PFN_xrCreateAction createAction{};
            PFN_xrDestroyAction destroyAction{};
            PFN_xrSuggestInteractionProfileBindings suggest{};
            PFN_xrAttachSessionActionSets attach{};
            PFN_xrGetCurrentInteractionProfile profile{};
            PFN_xrSyncActions sync{};
        };

        struct Set final {
            XRActionSetId id;
            XrActionSet native{XR_NULL_HANDLE};
        };

        struct Action final {
            Input::ActionId id;
            XrAction native{XR_NULL_HANDLE};
        };

        struct Profile final {
            XRInteractionProfileId id;
            XrPath native{XR_NULL_PATH};
        };

        [[nodiscard]] Result<void> Resolve(const NativeActionBindingRequest &request, XrInstance instance);
        [[nodiscard]] Result<void> Prepare(const XRActionBindingSchema &schema, std::span<const XRProfileControl> catalog,
                                           std::span<const XRActionBindingOverride> overrides,
                                           std::span<const NativeInteractionProfilePath> profiles,
                                           std::span<const NativeActionControlPath> controls, const NativeActionLabels &labels);
        [[nodiscard]] Result<void> CreateActions(const XRActionBindingSchema &schema, const NativeActionLabels &labels);
        [[nodiscard]] Result<void> Suggest(const XRActionBindingSchema &schema, std::span<const XRProfileControl> catalog,
                                           std::span<const XRActionBindingOverride> overrides,
                                           std::span<const NativeInteractionProfilePath> profiles,
                                           std::span<const NativeActionControlPath> controls);
        [[nodiscard]] Result<NativeSessionBorrow> Current(const XRSessionId &session) const;
        OpenXRNativeSession *session_;
        XRSessionId owner_;
        XRSessionId attachedSession_;
        NativeSessionBorrow native_{};
        Dispatch dispatch_;
        std::array<XrPath, 2> hands_{};
        std::vector<Set> sets_;
        std::vector<Action> actions_;
        std::vector<Profile> profiles_;
        bool active_{};
    };
}  // namespace Horo::XR::OpenXRInternal
