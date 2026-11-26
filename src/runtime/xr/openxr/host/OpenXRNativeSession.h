#pragma once

/** @file OpenXRNativeSession.h
 * @brief Non-installed explicit host composition and native session ownership contract.
 */

#include "Horo/XR/XRLightEstimate.h"
#include "Horo/XR/XRLoaderPreflight.h"
#include "Horo/XR/XRPassthrough.h"
#include "Horo/XR/XRSpatialAnchors.h"
#include "OpenXRHostBindings.h"
#include "OpenXRNativeCensus.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace Horo::XR::OpenXRInternal {
    /** @brief Synchronous caller-owned authorization for an exact native passthrough grant. */
    struct NativePassthroughAuthorization final {
        void *context{};
        bool (*allows)(void *, const XRPassthroughScope &){};
    };

    /** @brief Synchronous platform and operator authorization for one exact light-estimate scope. */
    struct NativeLightAuthorization final {
        void *context{};
        bool (*allows)(void *, const XRLightEstimateScope &) noexcept{};
    };
    class OpenXRSwapchain;

    /** @brief Fixed native discovery limits; oversized publications fail, never truncate. */
    inline constexpr std::size_t MaximumNativeExtensions = 128;
    inline constexpr std::size_t MaximumNativeLayers = 32;
    inline constexpr std::size_t MaximumEnabledExtensions = 16;

    /** @brief Current selected native ownership for one exact passthrough scope. */
    enum class NativePassthroughState : std::uint8_t {
        Absent,
        Active,
        Retiring,
    };

    /** @brief Host facts established before making a native extension eligible for discovery. */
    enum class NativeExtensionEligibility : std::uint8_t {
        Eligible,
        Unsupported,
        Denied
    };

    /** @brief Private native extension policy; host construction copies names only during bounded activation. */
    struct NativeExtensionRequest final {
        std::string_view name;
        bool required{};
        std::uint32_t minimumVersion{1};                    /**< Minimum version for the extension entry point. */
        std::span<const std::string_view> dependencies{};   /**< Other explicitly admitted routes required by this request. */
        std::optional<XRLoaderApiVersion> promotedToCore{}; /**< Earliest usable core API when the host implements that route. */
        NativeExtensionEligibility eligibility{};           /**< Selected host/platform support and policy admission. */
    };

    /** @brief Returns passthrough negotiation metadata supplied by the product's linked provider.
     * @return The optional extension request. A backend without a provider returns an empty value. */
    [[nodiscard]] std::optional<NativeExtensionRequest> NativePassthroughExtensionRequest() noexcept;

    /** @brief Native extension evidence retained for the exact selected discovery attempt. */
    enum class NativeExtensionState : std::uint8_t {
        Absent,
        Discovered,
        Enabled,
        PromotedToCore,
        VersionIncompatible,
        DependencyMissing,
        Unsupported,
        Denied,
        ProviderFailed,
    };

    /** @brief Value-only observation of native discovery and extension policy. */
    struct NativeExtensionAdmission final {
        NativeExtensionState state{};
        std::uint32_t discoveredVersion{};
        std::uint32_t minimumVersion{};                  /**< Zero when the host did not request this extension. */
        std::optional<XRLoaderApiVersion> coreVersion{}; /**< Selected native API when the core route is admitted. */
    };

    /** @brief Non-owning private native tuple; valid only during a fenced control-thread host operation before Close. */
    struct NativeSessionBorrow final {
        XrInstance instance;
        XrSystemId system;
        XrSession session;
        std::uint32_t maximumActions; /**< Zero for discovery-only borrows; negotiated bound after activation. */
        std::uint32_t maximumSpaces;  /**< Zero before activation; exact accepted feature-plan space capacity. */
        std::uint64_t incarnation;    /**< Native child creation receipt; never a Horo session/candidate identity or clock. */
    };

    /** @brief Exact configuration and complete view count selected by the admitted session plan. */
    struct NativeProjectionConfiguration final {
        XrViewConfigurationType type;
        std::uint32_t viewCount;
    };

    class OpenXRNativeSession;
    struct NativeFeatureProviderIssuer;

    /** @brief Non-callable receipt for one admitted provider on an exact native owner and session. */
    class NativeFeatureProviderHandle final {
        friend class OpenXRNativeSession;

        NativeFeatureProviderHandle(std::weak_ptr<const NativeFeatureProviderIssuer> issuer, XRSessionId session, std::uint64_t incarnation,
                                    std::size_t slot, std::uint64_t generation) noexcept
            : issuer_(std::move(issuer)), session_(session), incarnation_(incarnation), slot_(slot), generation_(generation) {}

        std::weak_ptr<const NativeFeatureProviderIssuer> issuer_;
        XRSessionId session_;
        std::uint64_t incarnation_;
        std::size_t slot_;
        std::uint64_t generation_;
    };

    /** @brief Symbol alternatives for the same typed provider contract. */
    struct NativeFeatureProviderRoute final {
        std::string_view extension;       /**< Exact host-requested extension policy. */
        std::string_view extensionSymbol; /**< Official ABI on the enabled extension route. */
        std::string_view coreSymbol{};    /**< Equivalent ABI when the policy admits the core route. */
    };

    /** @brief Generation-fenced callable borrow, valid only for one serialized native operation. */
    struct NativeFeatureProviderBorrow final {
        NativeSessionBorrow native;
        NativeExtensionAdmission admission;
        PFN_xrVoidFunction function;
    };

    /** @brief Non-callable evidence that one exact renderer-admitted native synthesis route is registered. */
    class NativeReprojectionProvider final {
        friend class OpenXRNativeSession;

        NativeReprojectionProvider(NativeFeatureProviderHandle handle, XRLoaderPreflightAttempt attempt,
                                   NativeReprojectionOffer offer) noexcept
            : handle_(std::move(handle)), attempt_(attempt), offer_(offer) {}

        NativeFeatureProviderHandle handle_;
        XRLoaderPreflightAttempt attempt_;
        NativeReprojectionOffer offer_;
    };

    /** @brief Non-callable proof of the selected dispatch and both private native allocations. */
    class NativePreparedReprojectionProvider final {
        friend class OpenXRNativeSession;

        NativePreparedReprojectionProvider(XRLoaderPreflightAttempt attempt, std::uint64_t incarnation, NativeReprojectionOffer offer,
                                           std::array<const OpenXRSwapchain *, 2> owners, std::array<XrSwapchain, 2> allocations,
                                           PFN_xrVoidFunction end) noexcept
            : attempt_(attempt), incarnation_(incarnation), offer_(offer), owners_(owners), allocations_(allocations), end_(end) {}

        XRLoaderPreflightAttempt attempt_;
        std::uint64_t incarnation_;
        NativeReprojectionOffer offer_;
        std::array<const OpenXRSwapchain *, 2> owners_;
        std::array<XrSwapchain, 2> allocations_;
        PFN_xrVoidFunction end_;
    };

    /** @brief Synchronous typed dispatch and exact native eye limits from the registered synthesis provider. */
    struct NativeReprojectionProviderBorrow final {
        XRLoaderPreflightAttempt attempt;
        NativeReprojectionOffer offer;
        XrSession session;
        PFN_xrEndFrame end;
    };

    /** @brief Exact host publication fence, called before acquisition and before exposing a native candidate. */
    class IOpenXRCompositionFence {
    public:
        virtual ~IOpenXRCompositionFence() = default;
        /** @brief Fences exact preparation before session-dependent capabilities can be observed.
         * @param preflight Actual successful instance/system discovery for the retained installation/attempt.
         * @param activeAttempt Current application-owned attempt.
         * @return Exact currentness/selected-device admission or original typed failure. No capability facts are invented.
         */
        [[nodiscard]] virtual Result<void> ValidatePreparation(const XRLoaderPreflightSnapshot &preflight,
                                                               XRLoaderPreflightAttempt activeAttempt) const = 0;
        /** @brief Validates current runtime, capability, selected graphics device and installation attempt. */
        [[nodiscard]] virtual Result<void> Validate(const XRSessionId &candidate, const XRLoaderPreflightSnapshot &preflight,
                                                    const XRFeaturePlan &plan) const = 0;
    };

    /** @brief Exact admitted product input before actual runtime/system discovery; no availability assertion. */
    struct NativeDiscoveryRequest final {
        XRLoaderPreflightRequest selection;
        XRLoaderApiVersion requestedApi;
        std::span<const NativeExtensionRequest> extensions;
        std::span<const std::string_view> layers;
        bool developmentLayersApproved{};
        bool optionalReprojection{}; /**< Explicit host request to census native auxiliary input contracts. */
    };

    /** @brief Native facts borrowed only during one serialized prepared-owner operation. */
    struct NativeDiscoveryBorrow final {
        XrInstance instance;
        XrSystemId system;
        PFN_xrGetInstanceProcAddr getProc;
        std::span<const char *const> enabledExtensions;
        XRLoaderApiVersion acceptedApi;
    };

    /** @brief Pre-session preparation admits no capability claim dependent on an existing native session. */
    struct NativeSessionPreparation final {
        const XRLoaderPreflightSnapshot &preflight;
        XRLoaderPreflightAttempt activeAttempt;
    };

    /** @brief Describes which prefix belongs to one failed native session preparation. */
    enum class NativeSessionPreparationScope : std::uint8_t {
        Owner,   /**< Full initial startup owns the selected instance and system. */
        Session, /**< Reversible probe borrows the already selected instance and system. */
    };

    /** @brief Borrowed activation input; no spans or snapshot references escape Create. */
    struct NativeSessionRequest final {
        XRSessionId candidate;
        const XRLoaderPreflightSnapshot &preflight;
        const XRCapabilitySnapshot &capabilities;
        const XRFeaturePlan &plan;
        XRLoaderPreflightAttempt activeAttempt;
        std::span<const NativeExtensionRequest> extensions;
        std::span<const std::string_view> layers;
        bool developmentLayersApproved{};
        XRPreflightProductMode productMode{XRPreflightProductMode::Shipping};
    };

    /**
     * @brief Concrete single-control-thread native transaction composed only by the application host.
     *
     * Loader shared ownership outlives every dispatch/native object. Host graphics/fence ports outlive this owner.
     * Create prepares beside any other separately owned generation; it never changes the XRRuntime publication.
     * The host publishes Ready only after its remaining spaces/swapchain/Renderer/Input stages succeed.
     * No operation selects a renderer, searches for a loader or accepts a headless graphics binding.
     */
    class OpenXRNativeSession final {
    public:
        /** @brief Retains an exact verified loader lease and borrows host-selected ports. */
        OpenXRNativeSession(std::shared_ptr<const Platform::DynamicLibrary> loader, IOpenXRGraphicsBinding &graphics,
                            const IOpenXRCompositionFence &fence) noexcept;
#if defined(__ANDROID__)
        /** @brief Retains Activity-scoped bootstrap inputs alongside the exact selected loader.
         * @param loader Verified loader for the admitted Android package.
         * @param graphics Exact host-selected graphics binding, borrowed through teardown.
         * @param fence Exact host composition fence, borrowed through teardown.
         * @param application Host-owned VM and retained Activity and application context references.
         */
        OpenXRNativeSession(std::shared_ptr<const Platform::DynamicLibrary> loader, IOpenXRGraphicsBinding &graphics,
                            const IOpenXRCompositionFence &fence, NativeOpenXRAndroidApplication application) noexcept;
#endif
        /** @brief Performs reverse-order cleanup; host must quiesce native users and Renderer first. */
        ~OpenXRNativeSession();
        OpenXRNativeSession(const OpenXRNativeSession &) = delete;
        OpenXRNativeSession &operator=(const OpenXRNativeSession &) = delete;

        /** @brief Prepares the sole native instance/system before creating the selected renderer.
         * @param request Exact retained-install/product/attempt selection and bounded native extension policy.
         * @return Actual preparation result; typed native failures preserve evidence and failed-destroy debt.
         * @post No XrSession or Ready publication. Extension/layer names are copied to existing bounded owner storage.
         */
        [[nodiscard]] Result<void> PrepareDiscovery(const NativeDiscoveryRequest &request);
        /** @brief Borrows same-owner native discovery for the exact successful prepared attempt.
         * @param attempt Current host attempt, revoked on cancellation/shutdown.
         * @return Same instance/system/dispatch and immutable accepted wire API, never runtime implementation version.
         * @pre Control-lane serialized use; borrow cannot survive activation, close or replacement.
         */
        [[nodiscard]] Result<NativeDiscoveryBorrow> BorrowDiscovery(XRLoaderPreflightAttempt attempt) const;
        /** @brief Queries bounded actual instance/system/view/blend facts without creating a native session.
         * @param attempt Exact current prepared attempt.
         * @return Complete observed publications or original typed native/count/representation failure.
         * @post Does not publish capabilities, choose a renderer or claim session-dependent space/format facts.
         */
        [[nodiscard]] Result<NativeSystemCensus> DiscoverSystem(XRLoaderPreflightAttempt attempt) const;

        /** @brief Reads the same live native session's immutable projection selection. */
        [[nodiscard]] Result<NativeProjectionConfiguration> ProjectionConfiguration(const XRSessionId &session) const;

        /** @brief Reads optional quad configuration geometry for the exact current discovery attempt. */
        [[nodiscard]] Result<NativeQuadViewCensus> DiscoverQuadViews(XRLoaderPreflightAttempt attempt,
                                                                     const NativeSystemCensus &observed) const;
        /** @brief Creates an unpublished native session so real session-dependent facts can be enumerated.
         * @param request Exact discovery/preflight/attempt with independently validated selected graphics binding; no Horo session identity
         * exists yet.
         * @param scope Ownership rollback scope on failure. Session-scoped probes retain the selected discovery.
         * @return Preparation outcome; failure reverses its owned resource prefix or retains exact cleanup debt.
         * @post Session() remains invalid. No feature plan or space/format availability is fabricated.
         */
        [[nodiscard]] Result<void> PrepareSession(const NativeSessionPreparation &request,
                                                  NativeSessionPreparationScope scope = NativeSessionPreparationScope::Owner);
        /** @brief Borrows an unpublished session solely for bounded backend discovery/negotiation.
         * @param request Exact successful preparation attempt and retained install/runtime.
         * @return Prepared native tuple; no activation or frame-use authority is transferred.
         * @pre Serialized backend-only discovery; borrow cannot survive activation/close/replacement.
         */
        [[nodiscard]] Result<NativeSessionBorrow> BorrowPreparedSession(const NativeSessionPreparation &request) const;
        /** @brief Returns this native owner's observed system incarnation under the admitted runtime generation.
         * @param request Exact successful prepared attempt and sealed loader evidence.
         * @return Owner-issued system identity only after successful native GetSystem, or the original stale/admission error.
         * @post No session candidate, capability availability or frame authority is issued. The identity stays stable
         * across session retirement on the same instance, and advances after a successfully rediscovered native system.
         */
        [[nodiscard]] Result<XRSystemId> PreparedSystem(const NativeSessionPreparation &request) const;
        /** @brief Queries actual reference-space and format publications from the SAME unpublished native session.
         * @param request Exact successful attempt/install/runtime lineage, fenced before and after foreign queries.
         * @return Complete observed publications or typed native/count/currentness failure.
         * @post No capability/Ready/frame publication occurs; renderer tuple compatibility remains a separate admission.
         */
        [[nodiscard]] Result<NativeSessionCensus> DiscoverSession(const NativeSessionPreparation &request) const;
        /** @brief Validates complete observed session capabilities/feature plan and publishes the SAME native session.
         * @param request Complete real loader/capability/renderer feature evidence matching retained preparation.
         * @return Activation result. Failure retains the unpublished prepared parents and issued candidate fence:
         * the staged caller first retires any attached native children, then retries admission or explicitly retires.
         * Legacy Create wraps the same stages with its original atomic reverse-cleanup contract.
         * @post Failure admits no frames and occupies only this owner's existing single preparation slot. The host
         * must either retry the same candidate after restoring current admission, or retire attached children and
         * call RetireSession/Close before releasing their parents. Failed destruction remains owned for explicit retry;
         * neither another candidate nor a new preparation may silently replace that debt.
         */
        [[nodiscard]] Result<void> Activate(const NativeSessionRequest &request);
        /** @brief Acquires one exact instance/system/session atomically or rolls candidate resources back. */
        [[nodiscard]] Result<void> Create(const NativeSessionRequest &request);
        /** @brief Fences retained Horo identity before downstream native use. */
        [[nodiscard]] Result<void> Validate(const XRSessionId &session) const;
        /** @brief Idempotently retires native resources; failed destruction retains handles/loader for explicit retry. */
        [[nodiscard]] Result<void> Close();
        /** @brief Retires only the quiescent native session while retaining the same discovered instance/system/loader.
         * @return Success or original native retirement failure; failed debt remains retryable and all borrows are revoked.
         * @pre No native call is in progress. Host retired all submitted scene/GUI/image references and destroyed
         * action spaces/actions/swapchains first. Normal stopping ends its frames/session through official transitions.
         * Explicit Lost recovery may instead abandon outstanding frame protocol through successful xrDestroySession;
         * its logical frame/compositor gate must remain retained until this call actually succeeds. A requested or
         * failed destruction is no quiescence proof, even though admission borrows are already invalidated.
         * @post Successful retirement permits PrepareSession on the same discovery attempt. Canonical lifecycle generation
         * history and native child creation receipts are never reset. A full Close retirement cannot be reopened here.
         */
        [[nodiscard]] Result<void> RetireSession();
        /** @brief Returns only Horo identity, invalid until complete candidate creation succeeds. */
        [[nodiscard]] XRSessionId Session() const noexcept;
        /** @brief Returns whether negotiation enabled an exact extension; false while unpublished. */
        [[nodiscard]] bool HasExtension(std::string_view name) const noexcept;
        /** @brief Inspects actual native synthesis offers without enabling them or claiming a Renderer producer.
         * @param attempt Exact retained discovery attempt.
         * @param session Exact published owner when checking a proposed swapchain on the active session.
         * @return Bounded native-only evidence or the original stale/owner error. Optional query errors become offer state.
         */
        [[nodiscard]] Result<NativeReprojectionOffer> DiscoverReprojection(XRLoaderPreflightAttempt attempt,
                                                                           std::optional<XRSessionId> session = std::nullopt) const;
        /** @brief Registers the exact enabled wire route once the selected renderer feature plan admits synthesis.
         * @param session Exact published owner. @param attempt Exact retained native selection.
         * @return An owner-fenced native provider or typed policy, sizing or dispatch error.
         */
        [[nodiscard]] Result<NativeReprojectionProvider> RegisterReprojectionProvider(const XRSessionId &session,
                                                                                      XRLoaderPreflightAttempt attempt);
        /** @brief Seals the native route and dispatch for both exact staged auxiliary swapchains.
         * @param request Same private native session. @param motion Staged motion owner. @param depth Staged depth owner.
         * @return Dispatch and allocation proof, or typed failure. Does not admit the capability or register a provider.
         * @pre The caller retains both supplied owners through adoption or canceled preparation.
         */
        [[nodiscard]] Result<NativePreparedReprojectionProvider> PrepareReprojectionProvider(const NativeSessionPreparation &request,
                                                                                             const OpenXRSwapchain &motion,
                                                                                             const OpenXRSwapchain &depth) const;
        /** @brief Registers the sealed dispatch after the exact staged allocations have been adopted.
         * @param session Issued session. @param prepared Sealed route. @param motion Same adopted motion owner.
         * @param depth Same adopted depth owner.
         * @return One registered provider or typed rejection. No rediscovery or dispatch lookup occurs.
         */
        [[nodiscard]] Result<NativeReprojectionProvider> RegisterPreparedReprojectionProvider(
            const XRSessionId &session, const NativePreparedReprojectionProvider &prepared, const OpenXRSwapchain &motion,
            const OpenXRSwapchain &depth);
        /** @brief Obtains typed xrEndFrame dispatch and exact native bounds for a synchronous call.
         * @param provider Owner-issued registration. @return Borrow or its original stale or native owner error.
         */
        [[nodiscard]] Result<NativeReprojectionProviderBorrow> BorrowReprojectionProvider(const NativeReprojectionProvider &provider) const;
        /** @brief Revokes this renderer route before its resources retire. @param provider Exact issued registration. */
        void RevokeReprojectionProvider(const NativeReprojectionProvider &provider) noexcept;
        /** @brief Copies extension evidence while the exact discovered owner is current on the control thread.
         * @param attempt Exact discovery attempt. @param name Known extension policy name or advertised native name.
         * @return Owned observation or typed invalid, stale or wrong-thread failure.
         */
        [[nodiscard]] Result<NativeExtensionAdmission> ReadExtensionAdmission(XRLoaderPreflightAttempt attempt,
                                                                              std::string_view name) const;
        /** @brief Revalidates the active session and copies its extension admission before binding a native provider.
         * @param session Exact live session. @param name Extension route used by the provider.
         * @return Evidence for that route or the typed session/extension inspection failure.
         */
        [[nodiscard]] Result<NativeExtensionAdmission> ReadExtensionAdmission(const XRSessionId &session, std::string_view name) const;
        /** @brief Resolves exactly one admitted symbol and publishes the versioned provider for this session.
         * @param session Exact active session. @param route Requested extension and applicable symbol alternatives.
         * @return Non-callable receipt, or a typed policy, stale or provider-resolution failure.
         */
        [[nodiscard]] Result<NativeFeatureProviderHandle> RegisterFeatureProvider(const XRSessionId &session,
                                                                                  NativeFeatureProviderRoute route);
        /** @brief Revalidates the complete owner before a bounded, allocation-free successful callable borrow.
         * @param handle Exact provider receipt. @return Current tuple and function, or typed stale/owner failure.
         */
        [[nodiscard]] Result<NativeFeatureProviderBorrow> BorrowFeatureProvider(const NativeFeatureProviderHandle &handle) const;
        /** @brief Revokes only the captured provider publication; stale or foreign receipts have no effect.
         * @param handle Exact provider registration to retire on the owner lane.
         */
        void RevokeFeatureProvider(const NativeFeatureProviderHandle &handle) noexcept;
        /**
         * @brief Borrows the exact complete native tuple for explicitly composed downstream native adapters.
         * @param session Exact Horo identity; stale owner/capability/device evidence rejects the borrow.
         * @return Native tuple or actionable admission failure. No ownership is transferred.
         * @pre The host serializes native use and teardown on the declared control thread.
         * @post The borrow must not escape that operation or survive Close, replacement or capability revocation.
         */
        [[nodiscard]] Result<NativeSessionBorrow> Borrow(const XRSessionId &session) const;

        /** @brief Opens an enabled native anchor route for an explicitly granted World scope. */
        [[nodiscard]] Result<void> StartSpatialAnchors(const XRAnchorScope &scope, XrSpace world, const XRAnchorLimits &limits,
                                                       const std::shared_ptr<XRSpatialAnchors> &publication);
        /** @brief Creates one native anchor and its portable ID on the control lane. */
        [[nodiscard]] Result<XRSpatialAnchorId> CreateSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRAnchorPose &pose,
                                                                    XRRuntimeTime now);
        /** @brief Locates at most one anchor in the original World and publishes its bounded observation. */
        [[nodiscard]] Result<void> SampleSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRSpatialAnchorId &id,
                                                       XRRuntimeTime now);
        /** @brief Invalidates one portable ID before releasing its native children. */
        [[nodiscard]] Result<void> RetireSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRSpatialAnchorId &id);
        /** @brief Revokes portable access and destroys native children, retaining cleanup debt on failure. */
        [[nodiscard]] Result<void> StopSpatialAnchors(const XRAnchorScope &scope, XRAnchorWithdrawal reason);

        /** @brief Opens cold access after the host grants separate protected-local persistence consent. */
        [[nodiscard]] Result<void> OpenSpatialAnchorStore(const XRAnchorScope &scope, XrSpace world, const CancellationSource &grant);
        /** @brief Checks exact live store authority without enumerating or reading durable storage. */
        [[nodiscard]] Result<std::uint64_t> CheckSpatialAnchorStore(const XRAnchorScope &scope, XrSpace world);
        /** @brief Saves one freshly localized anchor under a host-created opaque name. */
        [[nodiscard]] Result<void> PersistSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRSpatialAnchorId &id,
                                                        XRRuntimeTime now, std::string_view name);
        /** @brief Lists bounded provider-private names for the protected store adapter. Cold operation. */
        [[nodiscard]] Result<std::size_t> ReadSpatialAnchorStore(const XRAnchorScope &scope, XrSpace world,
                                                                 std::span<XrSpatialAnchorPersistenceNameMSFT> names);
        /** @brief Resolves a stored name into a fresh Locating identity in the current session. */
        [[nodiscard]] Result<std::optional<XRSpatialAnchorId>> RestoreSpatialAnchor(const XRAnchorScope &scope, XrSpace world,
                                                                                    XRRuntimeTime now, std::string_view name);
        /** @brief Revokes and retires the connection independently of the live anchors. */
        [[nodiscard]] Result<void> CloseSpatialAnchorStore(const XRAnchorScope &scope);

        /** @brief Creates an estimator on the exact enabled session after separate app and OS approval.
         * @param scope Already consented purpose and calibrated World scope. @param limits Explicit cadence and expiry.
         * @param authorization Live owner-lane approval, borrowed only for this operation.
         * @return Success, or typed capability, permission, identity, resource or validation failure.
         * @pre App must check focus and actual OS clearance and provide a fresh grant on the native control lane.
         */
        [[nodiscard]] Result<void> StartLightEstimation(const XRLightEstimateScope &scope, const XRLightEstimateLimits &limits,
                                                        const NativeLightAuthorization &authorization);
        /** @brief Samples one bounded native result after the minimum cadence.
         * @param scope Exact still-approved scope. @param world Synchronous borrow of the same calibrated World child.
         * @param now Current runtime-clock cutoff for request, admission and freshness.
         * @param authorization Live approval, checked before and after borrowing the provider sample.
         * @return True for a new publication, false for cadence or the same provider update, or a typed failure.
         * @pre The application must suspend this producer and retire the grant on focus loss, origin replacement or privacy revocation.
         */
        [[nodiscard]] Result<bool> AdvanceLightEstimation(const XRLightEstimateScope &scope, XrSpace world, XRRuntimeTime now,
                                                          const NativeLightAuthorization &authorization);
        /** @brief Copies current estimate evidence without calling the native feature.
         * @param scope Exact still-approved scope. @param now Cutoff in the same runtime clock.
         * @param authorization Live approval, checked even when no estimate is cached.
         * @return Current estimate, or a typed denied, stale, incompatible or unavailable result.
         */
        [[nodiscard]] Result<XRLightEstimate> ReadLightEstimation(const XRLightEstimateScope &scope, XRRuntimeTime now,
                                                                  const NativeLightAuthorization &authorization);
        /** @brief Checks whether withdrawal left a native handle to retire for this exact scope.
         * @param scope Exact candidate or previously active light grant.
         * @return True only on the owner lane for an inactive estimator still retained for destruction.
         */
        [[nodiscard]] bool NeedsLightRetirement(const XRLightEstimateScope &scope) const noexcept;
        /** @brief Revokes exact publication and retires the native child before its parent.
         * @param scope Exact scope to revoke; stale scopes cannot revoke a current grant.
         * @return Success or a retryable typed error retaining native cleanup debt.
         */
        [[nodiscard]] Result<void> StopLightEstimation(const XRLightEstimateScope &scope);

        /** @brief Creates and starts one selected FB reconstruction underlay after app and runtime permission.
         * @param scope Fresh, explicitly approved visual composition grant for a focused session and exact World.
         * @param authorization Live permission check borrowed for this call and rechecked between native operations.
         * @return Success or typed unsupported, denied, invalid, capacity or native failure.
         */
        [[nodiscard]] Result<void> StartPassthrough(const XRPassthroughScope &scope, const NativePassthroughAuthorization &authorization);
        /** @brief Immediately stops publishing the exact grant and releases its native layer and feature.
         * @param scope Exact current grant. @return Success or retryable native cleanup debt.
         */
        [[nodiscard]] Result<void> StopPassthrough(const XRPassthroughScope &scope);
        /** @brief Reads exact scope authorization and retained cleanup on the native control lane. */
        [[nodiscard]] NativePassthroughState ReadPassthrough(const XRPassthroughScope &scope) const noexcept;
        /** @brief Withdraws exact scope admission and requests pause; retains native cleanup ownership. */
        void RevokePassthrough(const XRPassthroughScope &scope) noexcept;

    private:
        friend class OpenXRProtectedAnchorStore;

        /** @brief Compensates an unpublished write on its exact connection, even after scope withdrawal. */
        [[nodiscard]] Result<void> DiscardSpatialAnchorWrite(const XRAnchorScope &scope, std::uint64_t generation, std::string_view name);
        friend class OpenXRFrameCompositor;
        friend class OpenXRSessionEvents;
        friend class OpenXRActionBindings;
        friend class OpenXRHandTracking;
        friend class OpenXRPlaneCapture;

        /** @brief Dispatch already resolved by a native child while preparing this exact native session. */
        struct PreparedFeatureDispatch final {
            std::uint64_t incarnation;
            PFN_xrVoidFunction function;
        };

        /** @brief Publishes an admitted route using existing prepared dispatch or a new lookup on the exact active session. */
        [[nodiscard]] Result<NativeFeatureProviderHandle> RegisterFeatureProvider(const XRSessionId &session,
                                                                                  NativeFeatureProviderRoute route,
                                                                                  std::optional<PreparedFeatureDispatch> prepared);

        /** @brief Checks either prepared discovery or an exact active session before native feature queries. */
        [[nodiscard]] Result<NativeDiscoveryBorrow> BorrowFeatureDiscovery(XRLoaderPreflightAttempt attempt,
                                                                           const std::optional<XRSessionId> &session) const;
        /** @brief Reads exact active owner plane capabilities before creating a spatial context. */
        [[nodiscard]] Result<NativePlaneCensus> DiscoverPlaneProvider(XRLoaderPreflightAttempt attempt, const XRSessionId &session) const;

        /** @brief Finds the bounded host-requested policy slot for this exact extension name. */
        [[nodiscard]] std::size_t ExtensionSlot(std::string_view name) const noexcept;
        /** @brief Checks the exact generation before lending or revoking a provider. */
        [[nodiscard]] bool OwnsFeatureProvider(const NativeFeatureProviderHandle &handle) const noexcept;
        /** @brief Resolves global dispatch from the selected loader, bootstrapping Android before enumeration. */
        [[nodiscard]] Result<void> ResolveGlobal(const NativeDiscoveryRequest &request);
#if defined(__ANDROID__)
        /** @brief Initializes the selected loader through the retained Android application context and mandatory policy. */
        [[nodiscard]] Result<void> InitializeAndroidLoader(std::span<const NativeExtensionRequest> requested);
#endif
        [[nodiscard]] Result<void> DiscoverAndNegotiate(const NativeDiscoveryRequest &request);
        /** @brief Validates a bounded layer publication and admits only explicitly approved requested layers. */
        [[nodiscard]] Result<void> DiscoverLayers(std::span<const std::string_view> requested);
        /** @brief Copies one valid, unique and available layer into owner-held fixed storage. */
        [[nodiscard]] Result<void> EnableLayer(std::string_view name, std::span<const XrApiLayerProperties> available);
        /** @brief Combines runtime/layer publications and admits host graphics before optional feature extensions. */
        [[nodiscard]] Result<void> NegotiateExtensions(std::span<const NativeExtensionRequest> requested);
        /** @brief Resolves validated feature requests against the census and retains the resulting policy evidence. */
        [[nodiscard]] Result<void> AdmitRequestedExtensions(std::span<const NativeExtensionRequest> requested, std::string_view bindingName,
                                                            std::span<const XrExtensionProperties> publication);
        /** @brief Copies one unique, admitted name into the native instance request. */
        [[nodiscard]] Result<void> EnableExtension(std::string_view name);
        /** @brief Enumerates one bounded global/layer extension publication without truncation. */
        [[nodiscard]] Result<void> CollectExtensions(const char *layer,
                                                     std::array<XrExtensionProperties, MaximumNativeExtensions> &available,
                                                     std::uint32_t &availableCount) const;
        /** @brief Returns exactly the provider-owned optional extension requests, in selection order. */
        [[nodiscard]] static std::span<const NativeExtensionRequest> ReprojectionExtensionRequests() noexcept;
        /** @brief Publishes the required extension closure before the sole extension-refusal retry. */
        [[nodiscard]] bool OmitOptionalExtensions() noexcept;
        [[nodiscard]] Result<void> CreateInstance(const NativeDiscoveryRequest &request);
        [[nodiscard]] Result<void> ResolveInstance();
        [[nodiscard]] Result<void> SelectSystem();
        [[nodiscard]] Result<void> CreateSession(const NativeSessionPreparation &request);
        [[nodiscard]] Result<void> Fail(Error error);
        /** @brief Preserves legacy atomic Create rollback without retrying failed native retirement inside one call. */
        [[nodiscard]] Result<void> FinishCreate(Result<void> outcome);
        [[nodiscard]] Result<void> CheckCurrent(const NativeSessionRequest &request) const;
        /** @brief Runs bounded native preparation; the outer entry point owns exception rollback. */
        [[nodiscard]] Result<void> PrepareTransaction(const NativeSessionRequest &request);
        /** @brief Runs the single owned native discovery transaction; outer entry owns exception rollback. */
        [[nodiscard]] Result<void> PrepareDiscoveryTransaction(const NativeDiscoveryRequest &request);
        /** @brief Fences exact prepared lineage and graphics independently of session-dependent capabilities. */
        [[nodiscard]] Result<void> CheckPreparation(const NativeSessionPreparation &request) const;
        /** @brief Destroys a native session and releases its graphics binding only after native success. */
        [[nodiscard]] Result<void> DestroySession();
        /** @brief Revokes evidence and retires the light child, retaining failed native retirement for retry. */
        [[nodiscard]] Result<void> RetireLightEstimation();
        /** @brief Withdraws the exact light grant and cached observations without destroying native handles. */
        void RevokeLightEstimation() noexcept;
        /** @brief Rechecks the native session and light grant after a foreign call. */
        [[nodiscard]] Result<void> RecheckLightObservation(const XRLightEstimateScope &scope,
                                                            const NativeLightAuthorization &authorization);
        /** @brief Validates scope and owner lane before borrowing or changing light observations. */
        [[nodiscard]] Result<void> CheckLightAccess(const XRLightEstimateScope &scope) const;
        /** @brief Publishes only a fresh decoded observation, dropping absent lobes on repeated samples. */
        [[nodiscard]] Result<bool> PublishLightObservation(const XRLightEstimate &sample, XRRuntimeTime now);
        /** @brief Withdraws native light, anchor and passthrough admission before an owner-lane lifecycle transition. */
        void RevokeCompositionObservations();
        [[nodiscard]] Result<void> RetireSpatialAnchors(XRAnchorWithdrawal reason);
        [[nodiscard]] Result<void> RetireSpatialAnchorStore();
        [[nodiscard]] Result<std::shared_ptr<XRSpatialAnchors>> BorrowAnchorStore(const XRAnchorScope &scope, XrSpace world) const;
        [[nodiscard]] Result<std::shared_ptr<XRSpatialAnchors>> BorrowAnchorPublication(const XRAnchorScope &scope, XrSpace world) const;

        /** @brief Passes an otherwise unhandled OpenXR event to the linked passthrough provider.
         * @param buffer Exact native event borrowed during the owner-lane poll. */
        void ObservePassthroughEvent(const XrEventDataBuffer &buffer) noexcept;
        /** @brief Cleans up the current passthrough child before native session retirement. */
        [[nodiscard]] Result<void> RetirePassthrough();
        /** @brief Redacts the layer and attempts native pause on consent/focus/origin/runtime loss. */
        void RevokePassthrough() noexcept;
        /** @brief Calls native frame end with the exact live reconstruction underlay and prevalidated projection. */
        [[nodiscard]] Result<XrResult> EndPassthroughFrame(const XRPassthroughScope &scope, XrSession session,
                                                           const XrFrameEndInfo &projection, PFN_xrEndFrame dispatch,
                                                           const NativePassthroughAuthorization &authorization);
        /** @brief Preserves the existing native phase/result-to-Horo mapping across split retirement implementation. */
        [[nodiscard]] static Error NativeFailure(const char *operation, XrResult result);

        enum class DiscoveryPhase : std::uint8_t {
            Preparing,
            Prepared,
            PreparingSession,
            SessionPrepared,
            Active,
            RetiringSession,
            Retiring
        };

        /** @brief One preparation lineage retained until all corresponding native handles retire. */
        struct DiscoveryState final {
            XRLoaderPreflightRequest selection;
            XRLoaderApiVersion acceptedApi;
            DiscoveryPhase phase{DiscoveryPhase::Preparing};
        };

        std::optional<DiscoveryState> discovery_;

        const std::thread::id controlThread_{std::this_thread::get_id()};

#if defined(__ANDROID__)
        NativeOpenXRAndroidApplication androidApplication_; /**< Outlives loader_ during reverse member destruction. */
#endif
        std::shared_ptr<const Platform::DynamicLibrary> loader_;
        IOpenXRGraphicsBinding *graphics_;
        const IOpenXRCompositionFence *fence_;

        /** @brief Official loader/candidate dispatch, valid only while loader_ and native owners survive. */
        struct NativeDispatch final {
            PFN_xrGetInstanceProcAddr getProc{};
            PFN_xrEnumerateApiLayerProperties enumerateLayers{};
            PFN_xrEnumerateInstanceExtensionProperties enumerateExtensions{};
            PFN_xrCreateInstance createInstance{};
            PFN_xrDestroyInstance destroyInstance{};
            PFN_xrGetSystem getSystem{};
            PFN_xrCreateSession createSession{};
            PFN_xrDestroySession destroySession{};
        };

        NativeDispatch dispatch_;

        XrInstance instance_{XR_NULL_HANDLE};
        XrSystemId system_{XR_NULL_SYSTEM_ID};
        XrSession session_{XR_NULL_HANDLE};
        XRSessionId owner_{};
        XRSessionId preparing_{};
        std::optional<XRLoaderPreflightSnapshot> retainedPreflight_;
        std::optional<XRFeaturePlan> retainedPlan_;
        std::uint64_t lastRuntime_{};
        std::uint32_t lastSessionGeneration_{};
        std::uint64_t nativeSessionIncarnation_{};
        std::uint32_t nativeSystemIncarnation_{}; /**< Monotonic successful native selection; never reset by Close. */

        /** @brief Bounded negotiated names and stable ABI borrows into this immovable owner. */
        struct NegotiatedNames final {
            std::array<std::array<char, XR_MAX_EXTENSION_NAME_SIZE>, MaximumEnabledExtensions> extensionNames{};
            std::array<const char *, MaximumEnabledExtensions> extensions{};
            std::array<std::array<char, XR_MAX_API_LAYER_NAME_SIZE>, MaximumNativeLayers> layerNames{};
            std::array<const char *, MaximumNativeLayers> layers{};
            std::uint32_t extensionCount{};
            std::uint32_t layerCount{};
        };

        NegotiatedNames names_;

        struct FeatureProvider final {
            PFN_xrVoidFunction function{};
            std::uint64_t generation{};
            bool failed{};
        };

        struct ExtensionEvidence final {
            std::array<char, XR_MAX_EXTENSION_NAME_SIZE> name{};
            NativeExtensionAdmission admission;
            NativeExtensionState fallbackState{};
        };

        std::array<XrExtensionProperties, MaximumNativeExtensions> discoveredExtensions_{};
        std::array<ExtensionEvidence, MaximumEnabledExtensions> extensionEvidence_{};
        std::array<FeatureProvider, MaximumEnabledExtensions> featureProviders_{};
        std::shared_ptr<const NativeFeatureProviderIssuer> providerIssuer_;
        std::uint64_t providerGeneration_{};
        bool registeringProvider_{};
        std::uint32_t discoveredExtensionCount_{};
        std::size_t extensionEvidenceCount_{};

        struct LightEstimator final {
            XrLightEstimatorANDROID handle{XR_NULL_HANDLE};
            PFN_xrDestroyLightEstimatorANDROID destroy{};
            PFN_xrGetLightEstimateANDROID get{};
            XrSpace world{XR_NULL_HANDLE};
            XRLightEstimateScope scope;
            XRLightEstimateLimits limits;
            mutable XRRuntimeTime lastClock; /**< Control-lane high-water mark shared by sampling and reads. */
            XRRuntimeTime lastRequest;
            XRRuntimeTime lastSample;
            std::optional<XRLightEstimate> current;
            bool active{};
            bool withdrawn{}; /**< A lifecycle event revoked publication, including during estimator creation. */
        };

        LightEstimator light_;
        XRLightEstimateGrant lastLightGrant_;
        mutable bool lightCall_{}; /**< Rejects teardown or reentrant use while a native light operation borrows the owner. */

        struct SpatialAnchors final {
            struct Slot final {
                XRSpatialAnchorId id;
                XrSpatialAnchorMSFT anchor{XR_NULL_HANDLE};
                XrSpace space{XR_NULL_HANDLE};
                XRRuntimeTime lastSample;
            };

            std::weak_ptr<XRSpatialAnchors> publication;
            XRAnchorScope scope;
            XrSpace world{XR_NULL_HANDLE};
            PFN_xrCreateSpatialAnchorMSFT create{};
            PFN_xrCreateSpatialAnchorSpaceMSFT createSpace{};
            PFN_xrDestroySpatialAnchorMSFT destroy{};
            PFN_xrDestroySpace destroySpace{};
            PFN_xrLocateSpace locate{};
            std::array<Slot, XRSpatialAnchors::MaximumAnchors> slots{};
            std::uint32_t maximumAnchors{};
            bool admitted{};
            bool withdrawn{}; /**< A lifecycle event withdrew consent, including during provider preparation. */

            [[nodiscard]] bool Resolve(const NativeSessionBorrow &native, PFN_xrGetInstanceProcAddr getProc);
            /** @brief Creates the reference space for an owned anchor, retaining any handle returned on success for retirement. */
            [[nodiscard]] XrResult AttachSpace(Slot &slot, XrSession session);
            [[nodiscard]] Result<void> Release(Slot &slot);
            void Withdraw(XRAnchorWithdrawal reason);
        } anchors_;

        struct SpatialAnchorStore final {
            XrSpatialAnchorStoreConnectionMSFT connection{XR_NULL_HANDLE};
            PFN_xrDestroySpatialAnchorStoreConnectionMSFT destroy{};
            PFN_xrPersistSpatialAnchorMSFT persist{};
            PFN_xrEnumeratePersistedSpatialAnchorNamesMSFT enumerate{};
            PFN_xrCreateSpatialAnchorFromPersistedNameMSFT restore{};
            PFN_xrUnpersistSpatialAnchorMSFT remove{};
            std::optional<XrSpatialAnchorPersistenceNameMSFT> pendingRemoval;
            CancellationToken consent;
            bool admitted{};

            [[nodiscard]] Result<std::size_t> Read(std::span<XrSpatialAnchorPersistenceNameMSFT> names) const;
            [[nodiscard]] Result<void> UndoPendingWrite();
        } anchorStore_;

        std::uint64_t anchorStoreGeneration_{};

        XRAnchorGrant lastAnchorGrant_;
        bool anchorCall_{};

        struct PassthroughStartup;

        struct Passthrough final {
            XRPassthroughScope scope;
            XrPassthroughFB feature{XR_NULL_HANDLE};
            XrPassthroughLayerFB layer{XR_NULL_HANDLE};
            PFN_xrPassthroughPauseFB pause{};
            PFN_xrDestroyPassthroughLayerFB destroyLayer{};
            PFN_xrDestroyPassthroughFB destroy{};
            bool started{}; /**< Native start obligation remains even after logical revocation. */
            bool admitted{};
        };

        Passthrough passthrough_;
        XRPassthroughGrant lastPassthroughGrant_;
        const XRPassthroughScope *preparingPassthrough_{}; /**< Borrowed only for the synchronous startup call. */
        bool passthroughCall_{};
        bool graphicsPrepared_{};
        bool retirementCall_{}; /**< Rejects reentrant native destruction while preserving explicit retry after return. */
        std::uint64_t observationRevision_{1}; /**< Owner-lane revocation fence for separately owned observation providers. */
    };
}  // namespace Horo::XR::OpenXRInternal
