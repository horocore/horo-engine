#pragma once
/** @file OpenXRApplicationSession.h
 * @brief Non-installed application owner of the verified selected runtime and same-device startup transaction.
 */
#include "Horo/XR/XRInputProjection.h"
#include "Horo/XR/XRProjectSettings.h"
#include "OpenXRApplicationBindings.h"
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
         * @return Real spaces/formats or original typed failure; no Horo session identity is issued.
         */
        [[nodiscard]] Result<XR::OpenXRInternal::NativeSessionCensus> PrepareSession();
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
                                                                            const Editor::EditorViewportRendererVulkan &viewport,
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
         * @param tracking Current canonical origin and observed native base space, supplied by the actual scene host.
         * @return Resolved declaration/profile/Input outcome; failures retain native debt for explicit Close.
         * @post No Horo session or binding revision is predicted and no snapshot/Input value can be published.
         */
        [[nodiscard]] Result<void> PrepareInput(const Input::InputActionCollection &port, const Input::InputRouter &router,
                                                const OpenXRApplicationBindings &bindings,
                                                XR::OpenXRInternal::NativeTrackingSpaceRequest tracking);
        /** @brief Binds the same prepared native children/Input reservation during canonical resource preparation.
         * @param candidate Actual candidate issued by XRSessionLifecycle and already activated on the SAME native session.
         * @param coordinator Candidate's sole canonical binding publication/revision owner.
         * @param router SAME live canonical action registry used for preparation.
         * @param bindings SAME immutable installer product schema used to prepare attached native sets.
         * @return Original native/Input stale or invalid failure, without issuing a replacement identity or registration.
         */
        [[nodiscard]] Result<void> BindInput(const XR::XRSessionId &candidate, XR::XRActionBindingCoordinator &coordinator,
                                             const Input::InputRouter &router, const OpenXRApplicationBindings &bindings);
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
                                               Input::InputCollectionFrame &frame);
        /** @brief Borrows the exact resolved preparation until input retirement; no native identity or sampling authority. */
        [[nodiscard]] const XR::XRResolvedActionBindings *PreparedBindings() const noexcept;
        /** @brief Returns the exact native-created calibrated World identity for the bound candidate.
         * @param candidate Current lifecycle-issued session. @return Owned typed identity or original stale/unavailable error.
         */
        [[nodiscard]] Result<XR::XRCoordinateSpace> WorldSpace(const XR::XRSessionId &candidate) const;
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

        /** @brief Borrowed normalized packet from one already-fenced native Input cutoff. No borrow survives this call. */
        struct PreparedInputSample final {
            const XR::OpenXRInternal::NativePoseObservations &poses;
            const XR::OpenXRInternal::NativeScalarActionSamples &actions;
            const XR::XRSessionSnapshot &state;
            const XR::XRTimeEvidence &time;
            XR::XRWorldOriginRevision origin;
            std::uint64_t bindingRevision;
        };

        /** @brief Publishes the same bounded cutoff through the sole tracking owner and appends without committing.
         * @param sample Complete synchronous evidence captured before publication.
         * @param publication Canonical tracking owner. @param frame Same Input transaction.
         * @return Original publication/projection error; rejected append revokes the candidate publication.
         */
        [[nodiscard]] Result<void> AppendPreparedInput(const PreparedInputSample &sample,
                                                       XR::RuntimeInternal::XRTrackingPublication &publication,
                                                       Input::InputCollectionFrame &frame);

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

        struct InputPreparation final {
            std::unique_ptr<XR::OpenXRInternal::OpenXRActionBindings> native;
            std::optional<XR::XRInputProjection> projection;
            std::optional<XR::XRResolvedActionBindings> bindings;
            std::array<XR::XRActiveInteractionProfile, 2> profiles{}; /**< Actual post-attachment preparation census. */
        };

        InputPreparation input_;

        bool closing_{};
        bool retiringSession_{};
    };
}  // namespace Horo::Application::Internal
