#include "OpenXRApplicationSession.h"

#include "Horo/Foundation/Assertions.h"

#include <algorithm>
#include <limits>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    /** @copydoc OpenXRApplicationSession::OpenXRApplicationSession */
    OpenXRApplicationSession::OpenXRApplicationSession(OpenXRApplicationAuthorities authorities, XRProjectSettings settings,
                                                       OpenXRInstalledProductCandidate product, const OpenXRInstalledProductPolicy &policy)
        : authorities_(std::move(authorities)), settings_(std::move(settings)), product_(std::move(product)), policy_(policy),
          installed_(product_->component.Install()), installIdentity_(product_->component.InstallIdentity()) {}

    /** @copydoc OpenXRApplicationSession::~OpenXRApplicationSession */
    OpenXRApplicationSession::~OpenXRApplicationSession() {
        const auto closed = Close();
        HORO_INVARIANT_MSG(closed.HasValue(), "Application must retain XR ownership until native retirement succeeds");
    }

    /** @copydoc OpenXRApplicationSession::Current */
    Result<void> OpenXRApplicationSession::Current() const {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR application owner requires its control lane"));
        if (!authorities_.hostAdmission)
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnavailable, "No retained installed-host trust admission is configured"));
        if (auto admitted = authorities_.hostAdmission(); admitted.HasError())
            return admitted;
        if (closing_ || retiringSession_ || authorities_.configuration.Snapshot().Revision() != settings_.Revision())
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        const auto installed = authorities_.packages.InstalledRecord(authorities_.projectRoot, authorities_.packageGeneration);
        if (installed.HasError())
            return Result<void>::Failure(installed.ErrorValue());
        if (installed.Value() != installed_)
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightStale));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::PrepareDiscovery */
    Result<void> OpenXRApplicationSession::PrepareDiscovery() {
        if (const auto current = Current(); current.HasError())
            return current;
        if (!product_ || native_ || preflight_ || lastAttempt_ == std::numeric_limits<std::uint32_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!settings_.Enabled() || settings_.RuntimeSelection() != XRRuntimeSelectionPolicy::SystemDefault)
            return Result<void>::Failure(MakeError(XRErrors::RuntimeOverrideRejected));
        const auto attempt = XRLoaderPreflightAttempt::Create(++lastAttempt_);
        if (attempt.HasError())
            return Result<void>::Failure(attempt.ErrorValue());
        const auto profile = XRProductProfileId::Create(static_cast<std::uint64_t>(settings_.Profile()) + 1);
        if (profile.HasError())
            return Result<void>::Failure(profile.ErrorValue());
        const auto &manifest = product_->component.Manifest();
        selection_ = {.attempt = attempt.Value(),
                      .backend = manifest.identity.backend,
                      .installRecord = installIdentity_,
                      .productProfile = profile.Value(),
                      .loaderSource = XRLoaderSourcePolicy::BundledVerified,
                      .runtimeSelection = settings_.RuntimeSelection(),
                      .admittedLoaderVersions = manifest.loaderVersions};
        auto lease = OpenXRLoaderLease::Prepare(std::move(product_->component), std::move(product_->deployment), policy_.temporaryParent,
                                                policy_.nativeGate, CancellationToken{});
        product_.reset();
        if (lease.HasError())
            return Result<void>::Failure(lease.ErrorValue());
        loader_ = std::move(lease).Value();
        native_ = std::make_unique<OpenXRNativeSession>(loader_->Library(), graphics_, *this);
        return DiscoverRuntime();
    }

    /** @copydoc OpenXRApplicationSession::DiscoverRuntime */
    Result<void> OpenXRApplicationSession::DiscoverRuntime() {
        const XRLoaderApiVersion compiled{XR_VERSION_MAJOR(XR_CURRENT_API_VERSION), XR_VERSION_MINOR(XR_CURRENT_API_VERSION),
                                          XR_VERSION_PATCH(XR_CURRENT_API_VERSION)};
#if defined(_WIN32)
        constexpr const char *timeExtension = "XR_KHR_win32_convert_performance_counter_time";
#else
        constexpr const char *timeExtension = "XR_KHR_convert_timespec_time";
#endif
        const std::array extensions{NativeExtensionRequest{timeExtension, true},
                                    NativeExtensionRequest{"XR_KHR_composition_layer_depth", false}};
        const NativeDiscoveryRequest request{selection_,
                                             std::min(compiled, selection_.admittedLoaderVersions.maximum),
                                             extensions,
                                             {},
                                             false};
        if (auto prepared = native_->PrepareDiscovery(request); prepared.HasError())
            return prepared;
        auto actual = native_->DiscoverSystem(selection_.attempt);
        if (actual.HasError())
            return Result<void>::Failure(actual.ErrorValue());
        auto borrow = native_->BorrowDiscovery(selection_.attempt);
        if (borrow.HasError())
            return Result<void>::Failure(borrow.ErrorValue());
        if (auto admitted = graphics_.Preflight(borrow.Value()); admitted.HasError())
            return admitted;
        // Actual successful loader/instance/system calls are the evidence; no session-dependent capability is fabricated.
        const auto runtime = XRRuntimeGeneration::Create(selection_.attempt.Value());
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        const XRLoaderProbeEvidence evidence{.attempt = selection_.attempt,
                                             .loader = XRLoaderAvailability::Available,
                                             .runtime = XRRuntimeAvailability::Available,
                                             .system = XRSystemAvailability::Supported,
                                             .loaderApiVersion = borrow.Value().acceptedApi,
                                             .runtimeGeneration = runtime.Value(),
                                             .consumedProbeSteps = 3};
        auto preflight = CreateXRLoaderPreflightSnapshot(selection_, evidence);
        if (preflight.HasError())
            return Result<void>::Failure(preflight.ErrorValue());
        system_.emplace(std::move(actual).Value());
        preflight_.emplace(std::move(preflight).Value());
        getProc_ = borrow.Value().getProc;
        return Current();
    }

    /** @copydoc OpenXRApplicationSession::AttachGraphics */
    Result<void> OpenXRApplicationSession::AttachGraphics(Render::IVulkanLoaderPort &platform,
                                                          Render::Detail::VulkanEditorGraphicsBridge &graphics) {
        if (const auto current = Current(); current.HasError())
            return current;
        if (!preflight_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return graphics_.Attach(platform, graphics);
    }

    /** @copydoc OpenXRApplicationSession::LoaderPort */
    Render::IVulkanLoaderPort &OpenXRApplicationSession::LoaderPort() noexcept {
        return graphics_;
    }

    /** @copydoc OpenXRApplicationSession::PrepareSession */
    Result<NativeSessionCensus> OpenXRApplicationSession::PrepareSession() {
        using Outcome = Result<NativeSessionCensus>;
        if (const auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!native_ || !preflight_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        const NativeSessionPreparation request{*preflight_, selection_.attempt};
        if (sessionCensus_) {
            if (auto current = native_->BorrowPreparedSession(request); current.HasError())
                return Outcome::Failure(current.ErrorValue());
            return Outcome::Success(*sessionCensus_);
        }
        if (auto prepared = native_->PrepareSession(request); prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        auto observed = native_->DiscoverSession(request);
        if (observed.HasError())
            return observed;
        sessionCensus_ = observed.Value();
        return observed;
    }

    /** @copydoc OpenXRApplicationSession::BorrowPreparation */
    Result<OpenXRApplicationPreparation> OpenXRApplicationSession::BorrowPreparation() const {
        using Outcome = Result<OpenXRApplicationPreparation>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!native_ || !preflight_ || !system_ || !sessionCensus_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        auto prepared = native_->BorrowPreparedSession({*preflight_, selection_.attempt});
        if (prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        auto discovery = native_->BorrowDiscovery(selection_.attempt);
        if (discovery.HasError())
            return Outcome::Failure(discovery.ErrorValue());
        return Outcome::Success({settings_, *preflight_, selection_, *system_, *sessionCensus_, *native_, discovery.Value().getProc});
    }

    /** @copydoc OpenXRApplicationSession::ActivateSession */
    Result<void> OpenXRApplicationSession::ActivateSession(const XRSessionId &candidate, const XRCapabilitySnapshot &capabilities,
                                                           const XRFeaturePlan &plan) {
        if (auto current = Current(); current.HasError())
            return current;
        if (!native_ || !preflight_ || !system_ || !sessionCensus_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!candidate.IsValid() || sessionOwner_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        // Retain the issued identity before the foreign activation call: a failed native destroy
        // must remain attributable to this candidate, even when native Session() is already revoked.
        sessionOwner_ = candidate;
        const NativeSessionRequest request{candidate, *preflight_, capabilities,          plan, selection_.attempt, {},
                                           {},        false,       selection_.productMode};
        return native_->Activate(request);
    }

    /** @copydoc OpenXRApplicationSession::BorrowGraphics */
    Result<OpenXRSelectedVulkanBinding *> OpenXRApplicationSession::BorrowGraphics(const XRSessionId &candidate) {
        using Outcome = Result<OpenXRSelectedVulkanBinding *>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!native_ || !sessionOwner_ || *sessionOwner_ != candidate)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (auto current = native_->Validate(candidate); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (auto current = graphics_.BorrowBinding(candidate); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return Outcome::Success(&graphics_);
    }

    /** @copydoc OpenXRApplicationSession::ValidatePreparation */
    Result<void> OpenXRApplicationSession::ValidatePreparation(const XRLoaderPreflightSnapshot &preflight,
                                                               XRLoaderPreflightAttempt attempt) const {
        if (const auto current = Current(); current.HasError())
            return current;
        if (!preflight_ || preflight.RuntimeGeneration() != preflight_->RuntimeGeneration() || attempt != selection_.attempt)
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightStale));
        return ValidateXRLoaderPreflight(preflight, attempt, selection_.backend, installIdentity_, selection_.productProfile);
    }

    /** @copydoc OpenXRApplicationSession::Validate */
    Result<void> OpenXRApplicationSession::Validate(const XRSessionId &candidate, const XRLoaderPreflightSnapshot &preflight,
                                                    const XRFeaturePlan &plan) const {
        if (const auto current = ValidatePreparation(preflight, selection_.attempt); current.HasError())
            return current;
        if (!candidate.IsValid() || candidate.system.runtime != preflight_->RuntimeGeneration() || plan.System() != candidate.system)
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        return graphics_.Validate(candidate);
    }

    /** @copydoc OpenXRApplicationSession::RetainedSession */
    std::optional<XRSessionId> OpenXRApplicationSession::RetainedSession() const noexcept {
        return std::this_thread::get_id() == lane_ ? sessionOwner_ : std::nullopt;
    }

    /** @copydoc OpenXRApplicationSession::RetirePreparation */
    Result<void> OpenXRApplicationSession::RetirePreparation() {
        if (std::this_thread::get_id() != lane_ || retiringSession_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (closing_ || sessionOwner_ || !native_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));

        struct Scope final {
            bool &value;

            explicit Scope(bool &active) : value(active) {
                value = true;
            }

            ~Scope() {
                value = false;
            }
        } scope{retiringSession_};

        RevokeInput();
        sessionCensus_.reset();
        if (input_.native) {
            if (auto retired = input_.native->Close(); retired.HasError())
                return retired;
            input_.native.reset();
        }
        return native_->RetireSession();
    }

    /** @copydoc OpenXRApplicationSession::RetireSession */
    Result<void> OpenXRApplicationSession::RetireSession(const XRSessionId &candidate) {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!candidate.IsValid() || !sessionOwner_ || *sessionOwner_ != candidate)
            return Result<void>::Failure(MakeError(XRErrors::CapabilityStale));
        if (closing_ || retiringSession_ || !native_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));

        struct Retirement final {
            bool &active;

            explicit Retirement(bool &value) noexcept : active(value) {
                active = true;
            }

            ~Retirement() {
                active = false;
            }
        } operation{retiringSession_};

        RevokeInput();
        sessionCensus_.reset();
        if (input_.native) {
            if (auto retired = input_.native->Close(); retired.HasError())
                return retired;
            input_.native.reset();
        }
        if (auto retired = native_->RetireSession(); retired.HasError())
            return retired;
        sessionOwner_.reset();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRApplicationSession::Close */
    Result<void> OpenXRApplicationSession::Close() {
        if (std::this_thread::get_id() != lane_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (retiringSession_)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        closing_ = true;
        RevokeInput();
        if (input_.native) {
            if (auto retired = input_.native->Close(); retired.HasError())
                return retired;
            input_.native.reset();
        }
        if (native_) {
            auto closed = native_->Close();
            if (closed.HasError())
                return closed;
            native_.reset();
        }
        sessionCensus_.reset();
        sessionOwner_.reset();
        preflight_.reset();
        system_.reset();
        loader_.reset();
        getProc_ = nullptr;
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
