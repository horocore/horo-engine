#pragma once
/** @file OpenXRApplicationSession.h
 * @brief Non-installed application owner of the verified selected runtime and same-device startup transaction.
 */
#include "Horo/XR/XRInputProjection.h"
#include "Horo/XR/XRProjectSettings.h"
#include "OpenXRApplicationBindings.h"
#include "OpenXRHandTracking.h"
#include "OpenXRInstalledProductSelection.h"
#include "OpenXRSelectedVulkanBinding.h"

#include <functional>

namespace Horo::XR::RuntimeInternal {
    class XRTrackingPublication;
}

namespace Horo::Runtime {
    struct FrameContext;
}

namespace Horo::Render {
    class RenderFrontend;
}

namespace Horo::Editor {
    class EditorViewportRendererVulkan;
}

namespace Horo::Application::Internal {
    class OpenXRInputClock;

    /** @brief Exact borrowed canonical authorities; none is discovered, replaced or activated by this value. */
    struct OpenXRApplicationAuthorities final {
        ConfigurationService &configuration;
        PackageProjectHost &packages;
        std::filesystem::path projectRoot;
        std::uint64_t packageGeneration{};
        std::function<Result<void>()> hostAdmission; /**< Same retained executable signature/expiry owner; success allocates no storage. */
        const XR::IXRHandGesturePolicy *handGestures{}; /**< Optional immutable override with lifetime owned by composition. */
    };

    /** @brief Same-owner synchronous preparation facts, never a public session or capability authority.
     * @details References expire at any preparation/activation/retirement call. The App consumes them on
     * the owner lane to form its complete plan before the canonical lifecycle issues the candidate.
     */
    struct OpenXRApplicationPreparation final {
        const XR::XRProjectSettings &settings;
        const XR::XRLoaderPreflightSnapshot &preflight;
        const XR::XRLoaderPreflightRequest &selection;
        const XR::OpenXRInternal::NativeSystemCensus &system;
        const XR::OpenXRInternal::NativeSessionCensus &session;
        XR::OpenXRInternal::OpenXRNativeSession &native;
        PFN_xrGetInstanceProcAddr getProc;
    };

    /** @brief Actual same-owner projection admission, captured before the lifecycle issues its candidate.
     * @details Contains observed capability facts and admitted native allocation intent, never Ready/frame evidence.
     * The selected resource stages still create the native swapchain and cold graph before Scene publication.
     */
    struct OpenXRApplicationProjection final {
        XR::XRCapabilitySnapshot capabilities;
        XR::XRFeaturePlan plan;
        Render::RenderTextureDescriptor color;
        std::array<Render::FramebufferExtent, 2> recommended;
        std::array<Render::FramebufferExtent, 2> maximum;
    };

    /** @brief Vendor implementation version observed through the official loader. Independently versioned from the wire API. */
    struct OpenXRRuntimeVersion final {
        std::uint16_t major{};
        std::uint16_t minor{};
        std::uint32_t patch{};
        constexpr auto operator<=>(const OpenXRRuntimeVersion &) const noexcept = default;
    };

    /** @brief Finite installed application runtime requirements; no vendor names, native paths or handles. */
    enum class OpenXRPackagedRoute : std::uint8_t {
        Graphics,
        PlatformTime,
        HandTracking,
        EyeGazeObservation,
        Count
    };

    /** @brief Owns one bounded row of runtime compatibility evidence from the real reversible session probe. */
    struct OpenXRPackagedCompatibility final {
        XR::XRLoaderPreflightSnapshot loader;
        XR::XRLoaderApiVersionRange admittedLoaderApis;
        OpenXRRuntimeVersion runtimeVersion;
        XR::XRSystemId system;
        Packages::PackagePlatform platform;
        OpenXRComponentIdentity component;
        XR::XRFeatureProfile profile;
        Sha256Digest installDigest;
        Sha256Digest loaderDigest;
        Sha256Digest signedDeploymentDigest;
        Sha256Digest declaredLicensesDigest;
        Render::VulkanApiVersion graphicsApi;
        XR::XRCapabilityState handTracking;
        std::array<XR::OpenXRInternal::NativeExtensionAdmission, static_cast<std::size_t>(OpenXRPackagedRoute::Count)> extensionRoutes;
        std::uint32_t referenceSpaceCount{};
        std::uint32_t runtimeSwapchainFormatCount{}; /**< Total native format inventory, including non-color formats. */
        std::uint8_t externallySuppliedOsAbiCount{}; /**< Signed OS dependencies, never bundled vendor runtime paths. */
        XR::XRCapabilityState eyeGaze{XR::XRCapabilityState::Unsupported}; /**< Policy-gated diagnostic state without gaze samples. */
    };

    /** @brief Runtime-reported HMD selection admitted by the local Desktop probe. */
    struct OpenXRDesktopProbeTarget final {
        std::string_view operatingSystem; /**< Exact native host identity from the packaged component. */
        std::string_view runtimeName;     /**< Exact expected official loader runtime name; never emitted in the receipt. */
        std::string_view systemName;      /**< Exact expected HMD system name; never emitted in the receipt. */
        Sha256Digest installDigest;       /**< Exact operator-supplied installed component digest. */
    };

    /** @brief Sealed runtime preflight offered as input to device qualification.
     * @details Holds no native handles or owning loader references. Results say nothing about physical-device test success.
     */
    class OpenXRPackagedPreflight final {
    public:
        /** @brief Returns immutable owned normalized compatibility facts. */
        [[nodiscard]] const OpenXRPackagedCompatibility &Compatibility() const noexcept {
            return compatibility_;
        }

    private:
        friend class OpenXRApplicationSession;

        OpenXRPackagedPreflight(OpenXRPackagedCompatibility compatibility, std::weak_ptr<const OpenXRLoaderLease> lease,
                                std::uint64_t revision)
            : compatibility_(std::move(compatibility)), lease_(std::move(lease)), revision_(revision) {}

        OpenXRPackagedCompatibility compatibility_;
        std::weak_ptr<const OpenXRLoaderLease> lease_;
        std::uint64_t revision_;
    };

    /** @brief Application-owned native preparation, retained beside its selected renderer through explicit retirement.
     * @details This is the actual composition fence, not a second session/frame authority. Native discovery observes facts;
     * canonical XRSessionLifecycle alone issues a Horo session candidate after complete feature negotiation.
     * Installer policy, canonical configuration/package authorities, and selected renderer ports outlive this owner.
     * Failed native retirement retains children and loader; callers must retry Close before destruction.
     */
    class OpenXRApplicationSession final : public XR::OpenXRInternal::IOpenXRCompositionFence {
    public:
        /** @brief Retains an already verified product and exact captured project policy without native work. */
        OpenXRApplicationSession(OpenXRApplicationAuthorities authorities, XR::XRProjectSettings settings,
                                 OpenXRInstalledProductCandidate product, const OpenXRInstalledProductPolicy &policy);
        ~OpenXRApplicationSession();
        OpenXRApplicationSession(const OpenXRApplicationSession &) = delete;
        OpenXRApplicationSession &operator=(const OpenXRApplicationSession &) = delete;
        /** @brief Opens the admitted loader and observes the sole native instance/system before window/device creation.
         * @return Actual native discovery and exact Vulkan requirements, or preserved admission/native failure.
         * @post No native session, capability availability, Ready state or frame authority is published.
         */
        [[nodiscard]] Result<void> PrepareDiscovery();
        /** @brief Attaches the application's sole selected SDL loader/graphics bridge before backend creation. */
        [[nodiscard]] Result<void> AttachGraphics(Render::IVulkanLoaderPort &platform,
                                                  Render::Detail::VulkanEditorGraphicsBridge &graphics);
        /** @brief Returns the same selected loader adapter for backend registration, never a new native device owner. */
        [[nodiscard]] Render::IVulkanLoaderPort &LoaderPort() noexcept;
        /** @brief Borrows the same admitted graphics adapter only for exact activated native image composition.
         * @param candidate Canonical lifecycle-issued active session.
         * @return Same stable adapter or original admission/device failure. No new device or graphics authority is issued.
         * @pre Owner lane; the borrow ends at session retirement, after every swapchain translator has drained.
         */
        [[nodiscard]] Result<XR::OpenXRInternal::OpenXRSelectedVulkanBinding *> BorrowGraphics(const XR::XRSessionId &candidate);
        /** @brief Creates the same unpublished native session and captures actual session-dependent census.
         * @param scope Whether failed acquisition belongs to full startup or a session-only probe.
         * @return Real spaces/formats or original typed failure; no Horo session identity is issued.
         */
        [[nodiscard]] Result<XR::OpenXRInternal::NativeSessionCensus> PrepareSession(
            XR::OpenXRInternal::NativeSessionPreparationScope scope = XR::OpenXRInternal::NativeSessionPreparationScope::Owner);
        /** @brief Performs one cold test using the selected native owner, graphics binding and configuration.
         * @param bindings Same installed declaration used by Play to check requested action and space limits.
         * @return Actual creation/census and retirement result, or typed failure with any cleanup cause.
         * @pre Owner lane with no Scene, input, image, frame or issued session. The caller serializes Play admission.
         * @post No native session is retained after success. Failure retains cleanup with this owner for an explicit
         * retry. Discovery and the selected device remain usable for subsequent Play preparation.
         */
        [[nodiscard]] Result<OpenXRPackagedPreflight> RunPackagedPreflight(const OpenXRApplicationBindings &bindings);
        /** @brief Runs an explicit Desktop target probe in the selected packaged process.
         * @param bindings Same installed declarations used by Play and XR Setup.
         * @param target Explicit host, runtime-reported HMD and package identity. The desktop connection uses
         * the OS system-default OpenXR loader, and this target requires the selected runtime to match exactly.
         * @return Sealed compatibility after actual cold session retirement or preserved typed failure.
         * @post No physical-device/render-frame result, runtime names or tracking coordinates are published.
         */
        [[nodiscard]] Result<OpenXRPackagedPreflight> ProbeDesktopTarget(const OpenXRApplicationBindings &bindings,
                                                                         const OpenXRDesktopProbeTarget &target);
        /** @brief Rechecks a sealed preflight against the current release installation, runtime and graphics device.
         * @param evidence Previously collected row; safe to retain after shutdown but unavailable for qualification.
         * @return Exact admission or typed stale/trust/device failure. Performs no native OpenXR operation.
         */
        [[nodiscard]] Result<void> ValidateDeviceQualification(const OpenXRPackagedPreflight &evidence) const;
        /** @brief Revokes retained compatibility after the selected native instance becomes unusable. */
        void RevokePackagedPreflight() noexcept;
        /** @brief Borrows actual complete native census and immutable policy from the SAME retained preparation.
         * @return Synchronous owner/attempt-fenced facts or original typed failure; no IDs, limits or flags are synthesized.
         * @pre PrepareSession succeeded; no borrow survives another owner operation.
         */
        [[nodiscard]] Result<OpenXRApplicationPreparation> BorrowPreparation() const;
        /** @brief Produces the real bounded Projection1_0 plan from prepared native/Input and selected renderer evidence.
         * @param frontend SAME actual selected frontend. @param viewport SAME cold-admitted multiview shader owner.
         * @param bindings SAME installed declaration whose native sets/spaces and Input reservation were prepared.
         * @return Complete observed capability/plan/tuple or original unsupported/currentness/capacity failure.
         * @pre PrepareInput succeeded on the exact prepared native session; no candidate or Ready is issued here.
         * @post Unsupported controller/haptic/UI/post-one features remain unsupported. Success never substitutes
         * a native format, another camera/device or a default-valid tracking sample. Each successful publication
         * receives a monotonic private capability revision; no per-frame discovery or allocation is introduced.
         */
        [[nodiscard]] Result<OpenXRApplicationProjection> PrepareProjection(const Render::RenderFrontend &frontend,
                                                                            Editor::EditorViewportRendererVulkan &viewport,
                                                                            const OpenXRApplicationBindings &bindings);
        /** @brief Activates the SAME prepared session during canonical lifecycle resource preparation.
         * @param candidate Exact identity issued by that lifecycle, never an App-predicted ordinal.
         * @param capabilities Actual observed capability publication used to negotiate the plan.
         * @param plan Complete current policy/renderer/Input decision.
         * @return Original native admission/activation error; failed cleanup debt remains in this owner.
         */
        [[nodiscard]] Result<void> ActivateSession(const XR::XRSessionId &candidate, const XR::XRCapabilitySnapshot &capabilities,
                                                   const XR::XRFeaturePlan &plan);
        /** @brief Prepares native action children and reserves the actual workspace Input source before candidate issuance.
         * @param port Existing workspace context/player-bound collection; cannot create actions or assignments.
         * @param router SAME application router whose live Actions are the sole semantic schema authority.
         * @param bindings Explicit installer-owned immutable physical catalog retained through Play retirement.
         * @param tracking Current canonical origin and native base; snapshotted before foreign admission/loader callbacks.
         * @param gazeConsent Explicit single-use operator consent for this Play preparation only.
         * @return Resolved declaration/profile/Input outcome; failures retain native debt for explicit Close.
         * @post No Horo session or binding revision is predicted and no snapshot/Input value can be published.
         */
        [[nodiscard]] Result<void> PrepareInput(const Input::InputActionCollection &port, const Input::InputRouter &router,
                                                const OpenXRApplicationBindings &bindings,
                                                const XR::OpenXRInternal::NativeTrackingSpaceRequest &tracking, bool gazeConsent = false);
        /** @return Whether both signed host policy and observed hardware permit arming the optional gaze preview. */
        [[nodiscard]] bool MayArmEyeGaze() const noexcept;
        /** @return Current supported/denied gaze availability, without exposing any pose. */
        [[nodiscard]] XR::XRCapabilityState EyeGazeAvailability() const noexcept;
        /** @brief Clears the current purpose grant and sample without invoking the native runtime. */
        void RevokeEyeGaze() noexcept;
        /** @return Current consented gaze for the exact immutable cutoff, or empty after loss or replacement. */
        [[nodiscard]] std::optional<XR::XRPoseSample> ReadEyeGaze(const XR::XRTrackingSnapshot &snapshot) const noexcept;
        /** @brief Binds the same prepared native children/Input reservation during canonical resource preparation.
         * @param candidate Actual candidate issued by XRSessionLifecycle and already activated on the SAME native session.
         * @param coordinator Candidate's sole canonical binding publication/revision owner.
         * @param router SAME live canonical action registry used for preparation.
         * @param bindings SAME immutable installer product schema used to prepare attached native sets.
         * @return Original native/Input stale or invalid failure, without issuing a replacement identity or registration.
         */
        [[nodiscard]] Result<void> BindInput(const XR::XRSessionId &candidate, XR::XRActionBindingCoordinator &coordinator,
                                             const Input::InputRouter &router, const OpenXRApplicationBindings &bindings);
        /** @brief Revalidates the exact attached input plan after a runtime profile notification.
         * @param session Current owner-issued session from the event's canonical lifecycle.
         * @return Success for unchanged evidence or confirmation of the selected startup fallback. A profile replacement or loss
         * returns an actionable failure for host-owned retirement and neutralization.
         */
        [[nodiscard]] Result<void> RefreshInputProfiles(const XR::XRSessionId &session);
        /** @brief Closes current native vibration admission without dispatching to the runtime.
         * @param role Exact affected role, or no role to revoke the same Play owner's outputs.
         * @pre Owner lane. Can be requested while a foreign runtime call is still on the stack.
         */
        void RequestHapticCancellation(std::optional<XR::XRTrackedDeviceRole> role = std::nullopt) noexcept;
        /** @brief Progresses exact retained vibration stops from the existing Play event or Input phase.
         * @param session Current canonical identity. @return Original native stop error or success (including no work).
         */
        [[nodiscard]] Result<void> ProgressHapticCancellation(const XR::XRSessionId &session);
        /**
         * @brief Submits the app-admitted finite preview through the exact native action owner.
         * @param session Exact admitted Play session.
         * @param role Exact admitted controller role.
         * @param amplitude Finite normalized vibration amplitude.
         * @param duration Positive bounded duration.
         * @return Runtime submission evidence or the original typed failure.
         */
        [[nodiscard]] Result<void> SubmitHapticPreview(const XR::XRSessionId &session, XR::XRTrackedDeviceRole role, float amplitude,
                                                       Duration duration);
        /** @brief Samples and appends the SAME prepared native source at the actual App collection cutoff.
         * @param sessions Sole canonical session authority, whose current focus gates scalar sampling.
         * @param coordinator SAME issued candidate's actual binding revision/plan owner.
         * @param publication Existing XRRuntime snapshot owner and its actual device identities.
         * @param clock SAME scheduler/native conversion bound during cold activation.
         * @param context Actual opaque BuildInputSnapshot dispatch evidence.
         * @param origin Current exact Scene-origin generation bound to these native spaces.
         * @param frame ONE existing Input collection transaction; this method never commits it.
         * @return Original native/time/snapshot/projection error; failure revokes the candidate snapshot.
         * @post Unfocused input appends nothing, so the host commits neutral XR without erasing physical input.
         * Native pose activity is joined to the SAME XRRuntime device issuer before scalar sampling; profile names and
         * inactive actions never invent physical presence, disconnection or a reconnect generation.
         * Successful sampling/publication/append uses bounded storage without per-frame allocation.
         */
        [[nodiscard]] Result<void> AppendInput(const XR::XRSessionLifecycle &sessions, const XR::XRActionBindingCoordinator &coordinator,
                                               XR::RuntimeInternal::XRTrackingPublication &publication, const OpenXRInputClock &clock,
                                               const Runtime::FrameContext &context, XR::XRWorldOriginRevision origin,
                                               const Input::InputCollectionFrame &frame);
        /** @brief Copies candidates derived at the exact active tracking revision, or empty on revocation.
         * @param snapshot Current immutable tracking publication, borrowed on the owner lane.
         * @return Bounded hand gesture candidates with source ownership.
         */
        [[nodiscard]] XR::XRHandGestureCandidates ReadHandGestures(const XR::XRTrackingSnapshot &snapshot) const noexcept;
        /** @brief Reads redacted hand availability from the exact committed cutoff.
         * @param snapshot Current immutable tracking publication, borrowed on the owner lane.
         * @return Current observed availability or absence after revocation.
         */
        [[nodiscard]] std::optional<XR::XRCapabilityState> ReadHandAvailability(const XR::XRTrackingSnapshot &snapshot) const noexcept;
        /** @brief Borrows the exact resolved preparation until input retirement; no native identity or sampling authority. */
        [[nodiscard]] const XR::XRResolvedActionBindings *PreparedBindings() const noexcept;
        /** @brief Returns the exact native-created calibrated World identity for the bound candidate.
         * @param candidate Current lifecycle-issued session. @return Owned typed identity or original stale/unavailable error.
         */
        [[nodiscard]] Result<XR::XRCoordinateSpace> WorldSpace(const XR::XRSessionId &candidate) const;
        /** @brief Copies the current native reference snapshot.
         * @param candidate Exact active XR session.
         * @return Bounded value evidence or a currentness/availability error.
         */
        [[nodiscard]] Result<XR::XRReferenceSpaceSnapshot> References(const XR::XRSessionId &candidate) const;
        /** @brief Borrows only this owner's current calibrated native World space for synchronous compositor dispatch.
         * @param space Exact typed World/session/origin identity, not a caller-selected native handle.
         * @return Fenced native borrow; no borrow survives another owner operation or retirement.
         */
        [[nodiscard]] Result<XrSpace> BorrowWorldSpace(const XR::XRCoordinateSpace &space) const;
        /** @brief Borrows the current presentation base for synchronous native projection composition.
         * @param space Exact complete reference identity returned by LocatePresentation.
         * @return Fenced native base borrow, or original currentness/stale error; no borrow survives another owner operation.
         */
        [[nodiscard]] Result<XrSpace> BorrowPresentationReference(const XR::XRCoordinateSpace &space) const;
        /** @brief Samples complete actual primary views and calibrated reference at one native frame prediction.
         * @param candidate Exact active session. @param time Actual converted sample and runtime frame prediction, no simulation timestamp.
         * @return Fixed bounded native evidence using the SAME observed system view count; never synthesized cameras or availability.
         */
        [[nodiscard]] Result<XR::OpenXRInternal::NativePresentationViews> LocatePresentation(const XR::XRSessionId &candidate,
                                                                                             const XR::XRTimeEvidence &time) const;
        /** @brief Revokes Input admission without foreign work, allocation, commit, or throwing.
         * @pre Owner lane; the next canonical App cutoff commits neutralization before replacement.
         */
        void RevokeInput() noexcept;
        /** @brief Retires the semantic source and effects while retaining the signed native binding plan. */
        void RetireInputProjection() noexcept;
        /** @brief Rebinds the still-current signed plan to the refreshed Input port after a neutral commit.
         * @param port Refreshed context and player authority.
         * @param coordinator Exact active session binding publication.
         * @param bindings Immutable signed product declaration used for initial admission.
         * @param registeredActions Current canonical Input registry.
         * @return Success or a typed mismatch with the new action map.
         */
        [[nodiscard]] Result<void> RebuildInputProjection(const Input::InputActionCollection &port,
                                                          const XR::XRActionBindingCoordinator &coordinator,
                                                          const OpenXRApplicationBindings &bindings,
                                                          std::span<const Input::ActionDescriptor> registeredActions);
        /** @brief Checks whether the current map admitted a semantic projection. */
        [[nodiscard]] bool HasInputProjection() const noexcept;
        /** @brief Abandons only the SAME unbound native attempt after failed pre-candidate Scene preparation.
         * @return Original cleanup failure, retaining native actions/session and loader for retry.
         * @pre Owner lane; no issued session owner, frame, image or renderer child can exist.
         * @post Success preserves the selected instance/system/graphics and permits a fresh same-owner attempt;
         * no canonical candidate/generation is manufactured to perform cleanup.
         */
        [[nodiscard]] Result<void> RetirePreparation();
        /** @brief Copies only the retained candidate cleanup fence on its owner lane; no native or admission authority. */
        [[nodiscard]] std::optional<XR::XRSessionId> RetainedSession() const noexcept;
        /** @brief Retires one issued candidate without replacing the selected instance, device or loader.
         * @param candidate Exact lifecycle-issued identity retained before native activation, including failed activation debt.
         * @return Success or the original cleanup failure. Failure retains the candidate fence and native parents for retry.
         * @pre The owner lane has retired every image, renderer and GUI reference for this candidate. Normal stopping
         * has ended its native frames. Explicit Lost recovery retains the logical frame/event owners until successful
         * native session destruction supplies quiescence; no concurrent native calls may remain in either path.
         * @post Input publication and preparation borrows are revoked before native calls. Success permits another Scene
         * preparation on the same installed runtime; delayed cleanup for another candidate cannot affect this owner.
         * Trust/configuration expiry never prevents cleanup. This operation does not issue a lifecycle generation.
         */
        [[nodiscard]] Result<void> RetireSession(const XR::XRSessionId &candidate);
        /** @brief Retires native children before selected renderer/device/window and package/configuration owners.
         * @return Success or original native failure; all failed cleanup debt remains owned for explicit recovery.
         * @pre Frame/image producers have stopped and canonical renderer completion has retired every XR acquisition.
         */
        [[nodiscard]] Result<void> Close();
        [[nodiscard]] Result<void> ValidatePreparation(const XR::XRLoaderPreflightSnapshot &preflight,
                                                       XR::XRLoaderPreflightAttempt attempt) const override;
        [[nodiscard]] Result<void> Validate(const XR::XRSessionId &candidate, const XR::XRLoaderPreflightSnapshot &preflight,
                                            const XR::XRFeaturePlan &plan) const override;

    private:
        [[nodiscard]] Result<void> Current() const;
        [[nodiscard]] Result<void> DiscoverRuntime();
        [[nodiscard]] bool WantsHands() const noexcept;
        /** @brief Collects only bounded normalized facts while the exact private native test session exists. */
        [[nodiscard]] Result<OpenXRPackagedCompatibility> CollectPackagedCompatibility(
            const XR::OpenXRInternal::NativeSessionCensus &session);
        /** @brief Verifies the required projection protocol through the one selected native instance. */
        [[nodiscard]] Result<void> ValidateProjectionDispatch(XrInstance instance) const;
        [[nodiscard]] Result<void> PrepareHands();
        [[nodiscard]] Result<void> RetireHands();
        /** @brief Validates the exact native baseline stereo target shared with live Play without allocating swapchains.
         * @param system Current selected instance/system census. @param session Current reversible session census.
         * @return Success only when the native runtime offers the selected stereo target and color format.
         */
        [[nodiscard]] static Result<void> ValidatePackagedProjectionTargets(const XR::OpenXRInternal::NativeSystemCensus &system,
                                                                            const XR::OpenXRInternal::NativeSessionCensus &session);
        /** @brief Validates the same selected profile and static capacity required by Play. */
        [[nodiscard]] Result<void> ValidateProjectionPolicy(const OpenXRApplicationBindings &bindings) const;

        /** @brief An optional hand sample and its independently observed state for this cutoff. */
        struct HandInputSample final {
            std::optional<XR::OpenXRInternal::NativeHandSamples> hands;
            XR::XRCapabilityState availability;
        };

        /** @brief Samples the configured hand provider or withdraws its optional contribution before controller projection. */
        [[nodiscard]] Result<HandInputSample> SampleHands(const XR::XRSessionId &session, const XR::XRSessionLifecycle &sessions,
                                                          const XR::XRTimeEvidence &time);

        /** @brief Borrowed normalized packet from one already-fenced native Input cutoff. No borrow survives this call. */
        struct PreparedInputSample final {
            const XR::OpenXRInternal::NativePoseObservations &poses;
            const XR::OpenXRInternal::NativeScalarActionSamples &actions;
            const XR::XRSessionSnapshot &state;
            const XR::XRTimeEvidence &time;
            XR::XRWorldOriginRevision origin;
            std::uint64_t bindingRevision;
            const HandInputSample &hands;
        };

        /** @brief Publishes the same bounded cutoff through the sole tracking owner and appends without committing.
         * @param sample Complete synchronous evidence captured before publication.
         * @param publication Canonical tracking owner. @param frame Same Input transaction.
         * @return Original publication/projection error; rejected append revokes the candidate publication.
         */
        [[nodiscard]] Result<void> AppendPreparedInput(const PreparedInputSample &sample,
                                                       XR::RuntimeInternal::XRTrackingPublication &publication,
                                                       const Input::InputCollectionFrame &frame);

        /** @brief Validates the same owner/attempt/device and complete native projection protocol before capability construction.
         * @param viewport Same selected renderer owner. @return Actual observed system or the original admission/stale/native error.
         * @pre Owner lane; no candidate or Ready is issued and no native borrow escapes this synchronous validation.
         */
        [[nodiscard]] Result<XR::XRSystemId> ValidateProjectionPreparation(const Editor::EditorViewportRendererVulkan &viewport) const;

        OpenXRApplicationAuthorities authorities_;
        XR::XRProjectSettings settings_;
        std::optional<OpenXRInstalledProductCandidate> product_;
        OpenXRInstalledProductPolicy policy_;

        /** @brief Exact sealed install bytes and the bound compact identity captured from the same verified component. */
        struct InstalledComponentSnapshot final {
            std::shared_ptr<const Packages::VerifiedPackageInstallRecord> record;
            XR::XRInstallRecordId identity;
        };

        InstalledComponentSnapshot installed_;
        const std::thread::id lane_{std::this_thread::get_id()};
        XR::OpenXRInternal::OpenXRSelectedVulkanBinding graphics_;
        std::shared_ptr<const OpenXRLoaderLease> loader_;
        std::unique_ptr<XR::OpenXRInternal::OpenXRNativeSession> native_;
        std::optional<XR::XRLoaderPreflightSnapshot> preflight_;
        std::optional<XR::OpenXRInternal::NativeSystemCensus> system_;
        PFN_xrGetInstanceProcAddr getProc_{}; /**< Same successful installed-instance resolver, retained through session retirement. */
        std::optional<XR::OpenXRInternal::NativeSessionCensus> sessionCensus_;
        std::optional<XR::XRSessionId> sessionOwner_; /**< Exact issued candidate, retained through failed native cleanup. */
        XR::XRLoaderPreflightRequest selection_;
        std::uint32_t lastAttempt_{};
        std::uint64_t lastCapabilityRevision_{};
        std::uint64_t packagedPreflightRevision_{};
        bool packagedPreflightCurrent_{};

        struct HandCutoffObservation final {
            XR::XRTrackingSnapshotRevision revision;
            XR::XRCapabilityState availability;
            XR::XRHandGestureCandidates gestures;
        };

        /** @brief Reads the last derived hand record only while the caller's exact tracking snapshot is current. */
        [[nodiscard]] const HandCutoffObservation *CurrentHandCutoff(const XR::XRTrackingSnapshot &snapshot) const noexcept;

        struct InputPreparation final {
            std::unique_ptr<XR::OpenXRInternal::OpenXRActionBindings> native;
            std::optional<XR::XRInputProjection> projection;
            std::optional<XR::XRResolvedActionBindings> bindings;
            std::optional<XR::OpenXRInternal::OpenXRHandTracking> hands;
            std::optional<HandCutoffObservation> handCutoff;
            std::optional<XR::XRCapabilityState>
                handSamplingBlocked; /**< A denied or unsupported route stays withdrawn for this session. */
            std::optional<XR::XRPoseSample> gaze;
            XR::XRTrackingSnapshotRevision gazeRevision;
            bool gazeAllowed{};
            XR::XRCapabilityState handAvailability{XR::XRCapabilityState::Unsupported};
            XR::XRCapabilityState stageAvailability{XR::XRCapabilityState::Unsupported};
            std::array<XR::XRActiveInteractionProfile, 2> profiles{}; /**< Actual post-attachment preparation census. */
        };

        InputPreparation input_;
        XR::XRContactPinchGesturePolicy pinchPolicy_;

        bool closing_{};
        bool retiringSession_{};
    };
}  // namespace Horo::Application::Internal
