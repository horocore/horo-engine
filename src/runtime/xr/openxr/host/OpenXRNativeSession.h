#pragma once

/** @file OpenXRNativeSession.h
 * @brief Non-installed explicit host composition and native session ownership contract.
 */

#include "Horo/XR/XRLoaderPreflight.h"
#include "OpenXRHostBindings.h"
#include "OpenXRNativeCensus.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <thread>
#include <utility>

namespace Horo::XR::OpenXRInternal {
    /** @brief Fixed native discovery limits; oversized publications fail, never truncate. */
    inline constexpr std::size_t MaximumNativeExtensions = 128;
    inline constexpr std::size_t MaximumNativeLayers = 32;
    inline constexpr std::size_t MaximumEnabledExtensions = 16;

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

    private:
        friend class OpenXRHandTracking;

        /** @brief Dispatch already resolved by a native child while preparing this exact native session. */
        struct PreparedFeatureDispatch final {
            std::uint64_t incarnation;
            PFN_xrVoidFunction function;
        };

        /** @brief Publishes an admitted route using existing prepared dispatch or a new lookup on the exact active session. */
        [[nodiscard]] Result<NativeFeatureProviderHandle> RegisterFeatureProvider(const XRSessionId &session,
                                                                                  NativeFeatureProviderRoute route,
                                                                                  std::optional<PreparedFeatureDispatch> prepared);

        /** @brief Checks either prepared discovery or an exact active session before native sizing queries. */
        [[nodiscard]] Result<NativeDiscoveryBorrow> BorrowReprojectionOwner(XRLoaderPreflightAttempt attempt,
                                                                            const std::optional<XRSessionId> &session) const;
        /** @brief Finds the bounded host-requested policy slot for this exact extension name. */
        [[nodiscard]] std::size_t ExtensionSlot(std::string_view name) const noexcept;
        /** @brief Checks the exact generation before lending or revoking a provider. */
        [[nodiscard]] bool OwnsFeatureProvider(const NativeFeatureProviderHandle &handle) const noexcept;
        [[nodiscard]] Result<void> ResolveGlobal();
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
        };

        std::array<XrExtensionProperties, MaximumNativeExtensions> discoveredExtensions_{};
        std::array<ExtensionEvidence, MaximumEnabledExtensions> extensionEvidence_{};
        std::array<FeatureProvider, MaximumEnabledExtensions> featureProviders_{};
        std::shared_ptr<const NativeFeatureProviderIssuer> providerIssuer_;
        std::uint64_t providerGeneration_{};
        bool registeringProvider_{};
        std::uint32_t discoveredExtensionCount_{};
        std::size_t extensionEvidenceCount_{};

        bool graphicsPrepared_{};
        bool retirementCall_{}; /**< Rejects reentrant native destruction while preserving explicit retry after return. */
    };
}  // namespace Horo::XR::OpenXRInternal
