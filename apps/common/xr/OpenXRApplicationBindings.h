#pragma once
/** @file OpenXRApplicationBindings.h
 * @brief Non-installed installer-declared physical bindings joined to the canonical live Input action registry.
 */
#include "OpenXRActionBindings.h"

namespace Horo::Application::Internal {
    /** @brief Product declarations own their schema; registeredActions is supplied only by the application's real InputRouter. */
    struct OpenXRApplicationActionSchema final {
        std::uint32_t version{};
        std::vector<XR::XRActionDeclaration> actions;
        std::vector<XR::XRSuggestedActionBinding> suggestions;
        std::vector<XR::XRBindingMigration> migrations;
        std::vector<XR::XRActionBindingOverride> overrides;
        XR::XRInteractionProfileId genericFallback;
    };

    /** @brief Private native spelling for an explicitly registered profile, owned until host retirement. */
    struct OpenXRApplicationProfile final {
        XR::XRInteractionProfileId id;
        std::string path;
    };

    /** @brief Private physical control mapping; never an Input action, player assignment or native handle. */
    struct OpenXRApplicationControl final {
        XR::XRProfileControl identity;
        std::string path;
    };

    /** @brief Private presentation label; separated from canonical semantic identity. */
    struct OpenXRApplicationActionLabel final {
        Input::ActionId action;
        std::string text;
    };

    /** @brief Private action-set presentation label. */
    struct OpenXRApplicationSetLabel final {
        XR::XRActionSetId set;
        std::string text;
    };

    /** @brief Private registered grip/aim path; the actual native owner creates and fences the resulting space. */
    struct OpenXRApplicationPose final {
        XR::XRInteractionProfileId profile;
        XR::XRActionSetId set;
        XR::XRTrackedDeviceRole role;
        XR::XRTrackedPoseKind kind;
        std::string path;
        std::string label;
    };

    /** @brief Official native dispatch borrowed from the same admitted instance; no callable allocation or ABI conversion. */
    struct OpenXRApplicationActionDispatch final {
        PFN_xrGetInstanceProcAddr getProc{};
    };

    /** @brief Installer-owned immutable bindings, distinct from signature/native-code admission and dynamic Input policy.
     * @details Own the value through a shared_ptr<const ...> in the explicit installer services. Construction is inert;
     * Prepare joins these declarations to InputRouter::Actions and queries actual runtime profiles only after attachment.
     * Unknown profiles remain unknown and can use only the product's existing explicit genericFallback policy.
     */
    struct OpenXRApplicationBindings final {
        OpenXRApplicationActionSchema schema;
        std::vector<OpenXRApplicationProfile> profiles;
        std::vector<OpenXRApplicationControl> controls;
        std::vector<OpenXRApplicationActionLabel> actionLabels;
        std::vector<OpenXRApplicationSetLabel> setLabels;
        std::vector<OpenXRApplicationPose> poses;

        /** @brief Prepares real native actions on the exact unpublished session then resolves actual profile evidence.
         * @param native Same native action owner retained by the application across failure and cleanup debt.
         * @param preparation Same prepared native session/installation attempt; no canonical candidate is invented.
         * @param dispatch Official dispatch borrowed from that exact native instance, never supplied by project metadata.
         * @param registeredActions Current canonical InputRouter::Actions; registration is revalidated by the Input port later.
         * @param tracking Current host-owned origin and observed base-space selection; no default-valid pose is manufactured.
         * @return Owned resolved plan or original declaration/native/profile failure. No Input/Ready publication occurs here.
         * @post Foreign failures may retain native cleanup debt in native; caller closes it before retiring the native session.
         * @throws std::bad_alloc For bounded lifecycle-only binding resolution, never normal sampling.
         */
        [[nodiscard]] Result<XR::XRResolvedActionBindings> Prepare(XR::OpenXRInternal::OpenXRActionBindings &native,
                                                                   const XR::OpenXRInternal::NativeSessionPreparation &preparation,
                                                                   OpenXRApplicationActionDispatch dispatch,
                                                                   std::span<const Input::ActionDescriptor> registeredActions,
                                                                   XR::OpenXRInternal::NativeTrackingSpaceRequest tracking) const;

        /** @brief Publishes the SAME observed product declaration through the canonical binding coordinator.
         * @param coordinator Exact candidate's sole binding revision issuer.
         * @param session Actual lifecycle-issued candidate, never a predicted native handle or ordinal.
         * @param registeredActions SAME live router registry used during preparation.
         * @param observed Actual profiles queried after the native owner's action-set attachment.
         * @return Original schema/profile/coordinator failure; no revision is manufactured by this product record.
         * @pre Cold lifecycle boundary; declaration and native observations remain owned through this synchronous call.
         */
        [[nodiscard]] Result<void> Publish(XR::XRActionBindingCoordinator &coordinator, const XR::XRSessionId &session,
                                           std::span<const Input::ActionDescriptor> registeredActions,
                                           std::span<const XR::XRActiveInteractionProfile> observed) const;
    };
}  // namespace Horo::Application::Internal
