#include "OpenXRApplicationSession.h"

#include "Horo/Foundation/Assertions.h"
#include "Horo/XR/XRSessionErrors.h"
#include "OpenXRInputClock.h"

#include <algorithm>
#include <limits>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    /** @copydoc OpenXRApplicationSession::OpenXRApplicationSession */
    OpenXRApplicationSession::OpenXRApplicationSession(OpenXRApplicationAuthorities authorities, XRProjectSettings settings,
                                                       OpenXRInstalledProductCandidate product, const OpenXRInstalledProductPolicy &policy)
        : authorities_(std::move(authorities)), settings_(std::move(settings)), product_(std::move(product)), policy_(policy),
          installed_{product_->component.Install(), product_->component.InstallIdentity()} {}

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
        if (installed.Value() != installed_.record)
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
                      .installRecord = installed_.identity,
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
        native_ = std::make_shared<OpenXRNativeSession>(loader_->Library(), graphics_, *this);
        return DiscoverRuntime();
    }

    namespace {
        /** @brief Prepares exactly the installed extension routes usable by this editor composition. */
        Result<void> PrepareNativeDiscovery(OpenXRNativeSession &native, const XRLoaderPreflightRequest &selection, bool handsRequested,
                                            const XRProjectSettings &settings, const std::optional<NativeExtensionRequest> &passthrough,
                                            bool optionalReprojection, bool lightEstimation) {
            const XRLoaderApiVersion compiled{XR_VERSION_MAJOR(XR_CURRENT_API_VERSION), XR_VERSION_MINOR(XR_CURRENT_API_VERSION),
                                              XR_VERSION_PATCH(XR_CURRENT_API_VERSION)};
            // The editor composition owns a time provider and color projection; its renderer has no native depth provider.
            constexpr std::array<std::string_view, 1> spatialDependencies{XR_EXT_FUTURE_EXTENSION_NAME};
            constexpr std::array<std::string_view, 2> planeDependencies{XR_EXT_FUTURE_EXTENSION_NAME, XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME};

            constexpr std::array<std::string_view, 1> anchorDependencies{XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME};
            const bool planePreview = settings.AllowsPlaneCapturePreview();
            const bool persistence = settings.AllowsSpatialAnchorPersistence();
            const bool fixedFoveation = std::ranges::any_of(settings.Features(), [](const XRFeatureRequest &feature) {
                return feature.capability == XRCapability::FixedFoveation && feature.kind == XRFeatureRequirementKind::Optional;
            });

            struct Selection final {
                NativeExtensionRequest request;
                bool selected;
            };

            const std::array routes{
                Selection{{OpenXRPlatformTimeRoute().extension, true}, true},
                Selection{{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME}, true},
                Selection{{XR_EXT_HAND_TRACKING_EXTENSION_NAME}, handsRequested},
                Selection{{XR_HTC_FOVEATION_EXTENSION_NAME}, fixedFoveation},
                Selection{{XR_ANDROID_LIGHT_ESTIMATION_EXTENSION_NAME}, lightEstimation},
                Selection{{XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME}, settings.AllowsSpatialAnchorPreview() || persistence},
                Selection{{.name = XR_MSFT_SPATIAL_ANCHOR_PERSISTENCE_EXTENSION_NAME, .dependencies = anchorDependencies}, persistence},
                Selection{{XR_EXT_FUTURE_EXTENSION_NAME}, planePreview},
                Selection{{.name = XR_EXT_SPATIAL_ENTITY_EXTENSION_NAME, .dependencies = spatialDependencies}, planePreview},
                Selection{{.name = XR_EXT_SPATIAL_PLANE_TRACKING_EXTENSION_NAME, .dependencies = planeDependencies}, planePreview},
            };
            std::array<NativeExtensionRequest, routes.size() + 1> extensions{};
            std::size_t count{};
            for (const auto &route : routes)
                if (route.selected)
                    extensions[count++] = route.request;
            if (passthrough)
                extensions[count++] = *passthrough;
            const auto requested = std::span{extensions}.first(count);
            const NativeDiscoveryRequest request{selection, std::min(compiled, selection.admittedLoaderVersions.maximum),
                                                 requested, {},
                                                 false,     optionalReprojection};
            return native.PrepareDiscovery(request);
        }
    }  // namespace

    /** @copydoc OpenXRApplicationSession::DiscoverRuntime */
    Result<void> OpenXRApplicationSession::DiscoverRuntime() {
        const bool hands = WantsHands();
        const bool optionalReprojection = std::ranges::any_of(settings_.Features(), [](const XRFeatureRequest &feature) {
            return feature.capability == XRCapability::SpaceWarp && feature.kind == XRFeatureRequirementKind::Optional;
        });
        const auto *owner = native_.get();
        auto passthrough = NativePassthroughExtensionRequest();
        if (passthrough && (!policy_.passthroughPermission || !policy_.passthroughPermission->SupportsReconstruction()))
            passthrough.reset();
        if (!native_ || native_.get() != owner)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const bool lightEstimation = policy_.lightPermission && policy_.lightPermission->SupportsLightEstimation();
        if (auto prepared =
                PrepareNativeDiscovery(*native_, selection_, hands, settings_, passthrough, optionalReprojection, lightEstimation);
            prepared.HasError())
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

    /** @copydoc OpenXRApplicationSession::MayArmPassthrough */
    bool OpenXRApplicationSession::MayArmPassthrough() const {
        const auto provider = NativePassthroughExtensionRequest();
        if (!provider || !policy_.passthroughPermission || !policy_.passthroughPermission->SupportsReconstruction() || !native_)
            return false;
        const auto evidence = native_->ReadExtensionAdmission(selection_.attempt, provider->name);
        return evidence.HasValue() && evidence.Value().state == NativeExtensionState::Enabled &&
               evidence.Value().discoveredVersion >= provider->minimumVersion;
    }

    /** @copydoc OpenXRApplicationSession::AllowsPassthrough */
    bool OpenXRApplicationSession::AllowsPassthrough(const XRPassthroughScope &scope) const noexcept {
        return policy_.passthroughPermission && policy_.passthroughPermission->Allows(scope);
    }

    /** @copydoc OpenXRApplicationSession::AllowsNativeLighting */
    bool OpenXRApplicationSession::AllowsNativeLighting(const XRLightEstimateScope &scope) const noexcept {
        const auto *permission = policy_.lightPermission.get();
        return permission && permission->SupportsLightEstimation() && permission->Allows(scope);
    }

    /** @copydoc OpenXRApplicationSession::MayArmEyeGaze */
    bool OpenXRApplicationSession::MayArmEyeGaze() const noexcept {
        return settings_.AllowsEyeGazePreview() && system_ && system_->eyeGazeProvider == XRCapabilityState::Available;
    }

    bool OpenXRApplicationSession::MayArmEyeGazeRendering() const {
        if (!MayArmEyeGaze() || !std::ranges::any_of(settings_.Features(), [](const auto &feature) {
                return feature.capability == XRCapability::FixedFoveation && feature.kind == XRFeatureRequirementKind::Optional;
            }))
            return false;
        const auto route = native_->ReadExtensionAdmission(selection_.attempt, XR_HTC_FOVEATION_EXTENSION_NAME);
        return route.HasValue() && route.Value().state == NativeExtensionState::Enabled;
    }

    /** @copydoc OpenXRApplicationSession::EyeGazeAvailability */
    XRCapabilityState OpenXRApplicationSession::EyeGazeAvailability() const noexcept {
        const auto observed = system_ ? system_->eyeGazeProvider : XRCapabilityState::Unsupported;
        if (observed != XRCapabilityState::Available)
            return observed;
        if (!settings_.AllowsEyeGazePreview())
            return XRCapabilityState::DisabledByPolicy;
        return input_.gazeAllowed ? XRCapabilityState::Available : XRCapabilityState::Denied;
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
    Result<NativeSessionCensus> OpenXRApplicationSession::PrepareSession(const NativeSessionPreparationScope scope) {
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
        if (auto prepared = native_->PrepareSession(request, scope); prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        auto observed = native_->DiscoverSession(request);
        if (observed.HasError())
            return observed;
        sessionCensus_ = observed.Value();
        return observed;
    }

    /** @copydoc OpenXRApplicationSession::CollectPackagedCompatibility */
    Result<OpenXRPackagedCompatibility> OpenXRApplicationSession::CollectPackagedCompatibility(const NativeSessionCensus &session) {
        using Outcome = Result<OpenXRPackagedCompatibility>;
        const auto preparedSystem = native_->PreparedSystem({*preflight_, selection_.attempt});
        if (preparedSystem.HasError())
            return Outcome::Failure(preparedSystem.ErrorValue());
        const auto discovery = native_->BorrowDiscovery(selection_.attempt);
        if (discovery.HasError())
            return Outcome::Failure(discovery.ErrorValue());

        const auto &observed = *system_;
        if (const auto targets = ValidatePackagedProjectionTargets(observed, session); targets.HasError())
            return Outcome::Failure(targets.ErrorValue());
        const auto spaces = std::span{session.spaces}.first(session.spaceCount);
        if (std::ranges::find(spaces, XR_REFERENCE_SPACE_TYPE_VIEW) == spaces.end() ||
            std::ranges::find(spaces, XR_REFERENCE_SPACE_TYPE_LOCAL) == spaces.end())
            return Outcome::Failure(
                MakeError(XRErrors::OperationUnsupported, "The packaged projection profile needs both view and local reference spaces."));
        if (const auto dispatch = ValidateProjectionDispatch(discovery.Value().instance); dispatch.HasError())
            return Outcome::Failure(dispatch.ErrorValue());
        PFN_xrVoidFunction timeConverter{};
        if (const auto result = getProc_(discovery.Value().instance, OpenXRPlatformTimeRoute().extensionSymbol.data(), &timeConverter);
            result != XR_SUCCESS || !timeConverter)
            return Outcome::Failure(
                MakeError(XRErrors::LoaderIncompatible, "The selected loader did not expose required time conversion."));

        constexpr auto handRoute = XR_EXT_HAND_TRACKING_EXTENSION_NAME;
        const std::array names{std::string_view{graphics_.RequiredExtension()}, OpenXRPlatformTimeRoute().extension,
                               std::string_view{handRoute}, std::string_view{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME}};
        std::array<NativeExtensionAdmission, static_cast<std::size_t>(OpenXRPackagedRoute::Count)> extensions{};
        static_assert(names.size() == extensions.size());
        for (std::size_t index = 0; index < names.size(); ++index) {
            const auto admission = native_->ReadExtensionAdmission(selection_.attempt, names[index]);
            if (admission.HasError())
                return Outcome::Failure(admission.ErrorValue());
            extensions[index] = admission.Value();
        }
        const auto device = graphics_.CaptureCompatibilityDevice();
        if (device.HasError())
            return Outcome::Failure(device.ErrorValue());

        const auto implementation = observed.runtime.runtimeVersion;
        const OpenXRRuntimeVersion runtime{XR_VERSION_MAJOR(implementation), XR_VERSION_MINOR(implementation),
                                           XR_VERSION_PATCH(implementation)};
        const auto &component = loader_->Component();
        const auto &manifest = component.Manifest();
        const auto &deployment = loader_->Deployment();
        return Outcome::Success({*preflight_, selection_.admittedLoaderVersions, runtime, preparedSystem.Value(), manifest.platform,
                                 manifest.identity, settings_.Profile(), component.IdentityDigest(), manifest.loader.digest,
                                 deployment.Evidence().ArtifactDigest(), manifest.licenses, device.Value(), observed.handTracking,
                                 extensions, session.spaceCount, session.formatCount, deployment.OsAbiDependencyCount(),
                                 EyeGazeAvailability()});
    }

    /** @copydoc OpenXRApplicationSession::RunPackagedPreflight */
    Result<OpenXRPackagedPreflight> OpenXRApplicationSession::RunPackagedPreflight(const OpenXRApplicationBindings &bindings) {
        using Outcome = Result<OpenXRPackagedPreflight>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!native_ || !preflight_ || !system_ || sessionOwner_ || input_.native)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        packagedPreflightCurrent_ = false;
        if (packagedPreflightRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto revision = ++packagedPreflightRevision_;
        if (auto admitted = ValidateProjectionPolicy(bindings); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        if (auto retired = RetirePreparation(); retired.HasError())
            return Outcome::Failure(WrapError(XRSessionErrors::SetupCleanupPending, std::move(retired).ErrorValue()));
        const auto prepared = PrepareSession(NativeSessionPreparationScope::Session);
        auto observed = prepared.HasError() ? Result<OpenXRPackagedCompatibility>::Failure(prepared.ErrorValue())
                                            : CollectPackagedCompatibility(prepared.Value());
        if (auto retired = RetirePreparation(); retired.HasError())
            return Outcome::Failure(WrapError(XRSessionErrors::SetupCleanupPending, std::move(retired).ErrorValue()));
        if (observed.HasError())
            return Outcome::Failure(std::move(observed).ErrorValue());
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (auto device = graphics_.ValidateCompatibilityDevice(); device.HasError())
            return Outcome::Failure(device.ErrorValue());
        packagedPreflightCurrent_ = true;
        return Outcome::Success(OpenXRPackagedPreflight{std::move(observed).Value(), loader_, revision});
    }

    /** @copydoc OpenXRApplicationSession::ProbeDesktopTarget */
    Result<OpenXRPackagedPreflight> OpenXRApplicationSession::ProbeDesktopTarget(const OpenXRApplicationBindings &bindings,
                                                                                 const OpenXRDesktopProbeTarget &target) {
        using Outcome = Result<OpenXRPackagedPreflight>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!loader_ || !system_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (target.runtimeName.empty() || target.runtimeName.size() >= XR_MAX_RUNTIME_NAME_SIZE || target.systemName.empty() ||
            target.systemName.size() >= XR_MAX_SYSTEM_NAME_SIZE)
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "XR probe requires bounded runtime and HMD names."));
        const auto &component = loader_->Component();
        const auto platform = component.Manifest().platform.operatingSystem;
        if ((target.operatingSystem != "windows" && target.operatingSystem != "linux") || target.operatingSystem != platform)
            return Outcome::Failure(
                MakeError(XRErrors::OperationUnsupported, "XR probe host does not match the installed Desktop component."));
        if (target.installDigest != component.IdentityDigest())
            return Outcome::Failure(MakeError(XRErrors::LoaderPreflightStale, "XR probe installation digest changed."));
        if (target.runtimeName != system_->runtime.runtimeName || target.systemName != system_->system.systemName)
            return Outcome::Failure(
                MakeError(XRErrors::RuntimeRejected, "Default OpenXR runtime or HMD does not match the selected target."));
        auto result = RunPackagedPreflight(bindings);
        if (result.HasError())
            return result;
        if (auto current = ValidateDeviceQualification(result.Value()); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return result;
    }

    /** @copydoc OpenXRApplicationSession::ValidateDeviceQualification */
    Result<void> OpenXRApplicationSession::ValidateDeviceQualification(const OpenXRPackagedPreflight &evidence) const {
        const auto &observed = evidence.Compatibility();
        if (auto selected = ValidatePreparation(observed.loader, observed.loader.Attempt()); selected.HasError())
            return selected;
        if (!packagedPreflightCurrent_ || !loader_ || evidence.revision_ != packagedPreflightRevision_ ||
            evidence.lease_.owner_before(loader_) || loader_.owner_before(evidence.lease_) ||
            observed.installDigest != loader_->Component().IdentityDigest())
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightStale));
        return graphics_.ValidateCompatibilityDevice();
    }

    /** @copydoc OpenXRApplicationSession::RevokePackagedPreflight */
    void OpenXRApplicationSession::RevokePackagedPreflight() noexcept {
        if (std::this_thread::get_id() == lane_)
            packagedPreflightCurrent_ = false;
    }

    /** @copydoc OpenXRApplicationSession::BorrowPreparation */
    Result<OpenXRApplicationPreparation> OpenXRApplicationSession::BorrowPreparation() const {
        using Outcome = Result<OpenXRApplicationPreparation>;
        if (auto current = Current(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!native_ || !preflight_ || !system_ || !sessionCensus_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (auto prepared = native_->BorrowPreparedSession({*preflight_, selection_.attempt}); prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        auto discovery = native_->BorrowDiscovery(selection_.attempt);
        if (discovery.HasError())
            return Outcome::Failure(discovery.ErrorValue());
        return Outcome::Success({settings_, *preflight_, selection_, *system_, *sessionCensus_, *native_, discovery.Value().getProc});
    }

    /** @copydoc OpenXRApplicationSession::BorrowPreparedGraphics */
    Result<OpenXRApplicationPreparedGraphics> OpenXRApplicationSession::BorrowPreparedGraphics() {
        using Output = Result<OpenXRApplicationPreparedGraphics>;
        const auto preparation = BorrowPreparation();
        if (preparation.HasError())
            return Output::Failure(preparation.ErrorValue());
        const auto binding = graphics_.BorrowPreparedBinding();
        if (binding.HasError())
            return Output::Failure(binding.ErrorValue());
        const auto &facts = preparation.Value();
        const NativeSessionPreparation session{facts.preflight, facts.selection.attempt};
        if (const auto current = native_->BorrowPreparedSession(session); current.HasError())
            return Output::Failure(current.ErrorValue());
        return Output::Success({session, graphics_, *binding.Value()});
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
        if (!preflight_ || preflight.RuntimeGeneration() != preflight_->RuntimeGeneration() ||
            preflight.ProductMode() != selection_.productMode || attempt != selection_.attempt)
            return Result<void>::Failure(MakeError(XRErrors::LoaderPreflightStale));
        return ValidateXRLoaderPreflight(preflight, attempt, selection_.backend, installed_.identity, selection_.productProfile);
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
            Scope(const Scope &) = delete;
            Scope &operator=(const Scope &) = delete;

            bool &value;

            explicit Scope(bool &active) : value(active) {
                active = true;
            }

            ~Scope() {
                value = false;
            }
        };

        Scope scope{retiringSession_};

        RevokeInput();
        sessionCensus_.reset();
        RevokeSpatialAnchor();
        if (auto retired = ProgressSpatialAnchorRetirement(true); retired.HasError())
            return retired;
        if (auto retired = RetirePlaneCapture(); retired.HasError())
            return retired;
        if (auto retired = RetireHands(); retired.HasError())
            return retired;
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
            Retirement(const Retirement &) = delete;
            Retirement &operator=(const Retirement &) = delete;

            bool &active;

            explicit Retirement(bool &value) noexcept : active(value) {
                value = true;
            }

            ~Retirement() {
                active = false;
            }
        };

        Retirement operation{retiringSession_};

        RevokeInput();
        sessionCensus_.reset();
        RevokeSpatialAnchor();
        if (auto retired = ProgressSpatialAnchorRetirement(true); retired.HasError())
            return retired;
        if (auto retired = RetirePlaneCapture(); retired.HasError())
            return retired;
        if (auto retired = RetireHands(); retired.HasError())
            return retired;
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
        RevokeSpatialAnchor();
        if (auto retired = ProgressSpatialAnchorRetirement(true); retired.HasError())
            return retired;
        if (auto retired = RetirePlaneCapture(); retired.HasError())
            return retired;
        if (auto retired = RetireHands(); retired.HasError())
            return retired;
        if (input_.native) {
            if (auto retired = input_.native->Close(); retired.HasError())
                return retired;
            input_.native.reset();
        }
        if (native_) {
            if (auto closed = native_->Close(); closed.HasError())
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
