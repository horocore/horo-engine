#include "Horo/PlatformServices/PlatformServicesComposition.h"

#include <utility>

namespace Horo::PlatformServices {
    namespace {
        /** @brief Stable provider ABI profile used by each product composition. */
        [[nodiscard]] PlatformServicesHostProfile HostProfile(const PlatformServicesProductProfile profile) noexcept {
            using enum PlatformServicesHostProfile;
            using enum PlatformServicesProductProfile;
            switch (profile) {
                case ShippingGame:
                    return Certification;
                case Headless:
                case DedicatedServer:
                    return HeadlessServer;
                case UnsupportedPlatform:
                    return Cook;
                default:
                    return InteractiveDevelopment;
            }
        }

        /** @brief Product manifests authorize only their corresponding shipping/server compositions. */
        [[nodiscard]] PlatformServicesProviderProvenance Provenance(const PlatformServicesProductProfile profile) noexcept {
            using enum PlatformServicesProviderProvenance;
            using enum PlatformServicesProductProfile;
            switch (profile) {
                case ShippingGame:
                    return ShippingManifest;
                case Headless:
                case DedicatedServer:
                    return ServerManifest;
                case Test:
                    return PublicTestFixture;
                default:
                    return TrustedDevelopment;
            }
        }

        /** @brief Require the product allowlist to stay within manifest claims and include every required service. */
        [[nodiscard]] Result<void> ValidateServiceAdmission(const PlatformServicesCompositionRequest &request) {
            const auto requirements = request.configuration.ServiceRequirements();
            const auto claims = request.configuration.SelectedServices();
            for (std::size_t index = 0; index < requirements.size(); ++index) {
                const bool allowed = request.evidence->allowedServices[index];
                if (allowed && !claims[index])
                    return Result<void>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
                if (requirements[index] == PlatformServiceRequirement::Required && !allowed)
                    return Result<void>::Failure(MakeError(BackendErrors::RequiredServiceUnavailable));
            }
            return Result<void>::Success();
        }

        /** @brief Validate evidence for an exact provider without invoking its factory. */
        [[nodiscard]] Result<void> ValidateProviderSelection(const PlatformServicesCompositionRequest &request,
                                                             const PlatformServicesCompositionFactory &factory) {
            if (const bool fixture = request.mode == PlatformServicesCompositionMode::Test;
                request.configuration.UsesNullProvider() || !factory || !request.evidence ||
                request.profile == PlatformServicesProductProfile::UnsupportedPlatform ||
                fixture != (request.profile == PlatformServicesProductProfile::Test))
                return Result<void>::Failure(MakeError(PlatformProjectConfigurationErrors::ProfileDenied));
            if (const auto &evidence = *request.evidence; evidence.provenance != Provenance(request.profile) ||
                                                          evidence.generation != request.generation ||
                                                          evidence.configurationFingerprint != request.configuration.Fingerprint())
                return Result<void>::Failure(MakeError(PlatformProjectConfigurationErrors::ProfileDenied));
            return ValidateServiceAdmission(request);
        }

        /** @brief Validate complete product selection without invoking factories or reading ambient state. */
        [[nodiscard]] Result<void> ValidateSelection(const PlatformServicesCompositionRequest &request,
                                                     const PlatformServicesCompositionFactory &factory) {
            using enum PlatformServicesCompositionMode;
            if (request.profile >= PlatformServicesProductProfile::Count || request.mode > Absent || !request.generation.IsValid() ||
                request.configuration.Profile() != HostProfile(request.profile))
                return Result<void>::Failure(MakeError(FrontendErrors::InvalidComposition));
            if (const bool noProvider = request.mode == Null || request.mode == Absent; !noProvider)
                return ValidateProviderSelection(request, factory);
            if (!request.configuration.UsesNullProvider() || request.evidence ||
                (request.mode == Absent && request.profile != PlatformServicesProductProfile::UnsupportedPlatform))
                return Result<void>::Failure(MakeError(FrontendErrors::InvalidComposition));
            return Result<void>::Success();
        }

        /** @brief Intersect project/product admission without changing the provider's raw snapshot. */
        [[nodiscard]] Result<PlatformServicesOperationPolicy> ProjectCapabilities(const PlatformServicesCompositionRequest &request,
                                                                                  const PlatformServiceCapabilitySnapshot &snapshot,
                                                                                  PlatformServicesCompositionDiagnostics &diagnostics) {
            PlatformServicesOperationPolicy policy;
            const auto requirements = request.configuration.ServiceRequirements();
            const auto claims = request.configuration.SelectedServices();
            for (const auto &service : snapshot.services) {
                const auto index = static_cast<std::size_t>(service.service);
                if (service.availability == PlatformServiceAvailability::Available && !claims[index])
                    return Result<PlatformServicesOperationPolicy>::Failure(MakeError(BackendErrors::InvalidCapabilitySnapshot));
                policy.deniedServices[index] =
                    requirements[index] == PlatformServiceRequirement::Disabled || !request.evidence->allowedServices[index];
                diagnostics.services[index] =
                    policy.deniedServices[index] ? PlatformServiceAvailability::Unavailable : service.availability;
                diagnostics.reasons[index] =
                    policy.deniedServices[index] ? PlatformServiceUnavailableReason::HostPolicyDenied : service.unavailableReason;
            }
            return Result<PlatformServicesOperationPolicy>::Success(policy);
        }

        /** @brief Require exact candidate identity, session generation and structurally valid capabilities before activation. */
        [[nodiscard]] Result<PlatformServiceCapabilitySnapshot> InspectCandidate(const PlatformServicesCompositionRequest &request,
                                                                                 const IPlatformServicesBackend &backend,
                                                                                 const PlatformSessionSnapshot &session) {
            auto inspected = backend.InspectCapabilities();
            if (inspected.HasError())
                return inspected;
            const auto &snapshot = inspected.Value();
            if (snapshot.provider != request.configuration.SelectedProvider() || snapshot.providerGeneration != request.generation ||
                session.ProviderGeneration() != request.generation)
                return Result<PlatformServiceCapabilitySnapshot>::Failure(MakeError(FrontendErrors::InvalidComposition));
            if (auto valid = ValidatePlatformServiceCapabilitySnapshot(snapshot, request.configuration.BackendConfig()); valid.HasError())
                return Result<PlatformServiceCapabilitySnapshot>::Failure(valid.ErrorValue());
            return inspected;
        }

        /** @brief Retain failed rollback ownership so another generation cannot replace undrained state. */
        class CandidateRollback final {
        public:
            CandidateRollback(std::shared_ptr<IPlatformServicesBackend> backend, std::shared_ptr<IPlatformServicesBackend> &retained,
                              std::optional<Error> &failure) noexcept
                : backend_(std::move(backend)), retained_(retained), failure_(failure) {}

            CandidateRollback(const CandidateRollback &) = delete;
            CandidateRollback &operator=(const CandidateRollback &) = delete;

            ~CandidateRollback() {
                if (!backend_)
                    return;
                try {
                    if (auto closed = backend_->Shutdown(); closed.HasError())
                        Retain(closed.ErrorValue());
                } catch (...) {
                    Retain(MakeError(BackendErrors::ServiceUnavailable));
                }
            }

            void Commit() noexcept {
                backend_.reset();
            }

        private:
            void Retain(Error error) {
                retained_ = backend_;
                failure_ = std::move(error);
            }

            std::shared_ptr<IPlatformServicesBackend> backend_;
            std::shared_ptr<IPlatformServicesBackend> &retained_;
            std::optional<Error> &failure_;
        };
    }  // namespace

    /** @copydoc PlatformServicesComposition::Start */
    Result<std::unique_ptr<PlatformServicesComposition>> PlatformServicesComposition::Start(
        const PlatformServicesCompositionRequest &request, const PlatformServicesCompositionFactory &factory) {
        using StartResult = Result<std::unique_ptr<PlatformServicesComposition>>;
        if (auto valid = ValidateSelection(request, factory); valid.HasError())
            return StartResult::Failure(valid.ErrorValue());
        auto composition = std::make_unique<PlatformServicesComposition>(CreationKey{});
        if (auto initialized = composition->Initialize(request, factory); initialized.HasError())
            return StartResult::Failure(initialized.ErrorValue());
        return StartResult::Success(std::move(composition));
    }

    /** @copydoc PlatformServicesComposition::Initialize */
    template <typename Factory>
    Result<void> PlatformServicesComposition::Initialize(const PlatformServicesCompositionRequest &request,
                                                         const Factory &factory) noexcept {
        try {
            diagnostics_ = {.profile = request.profile, .mode = request.mode, .generation = request.generation};
            diagnostics_.services.fill(PlatformServiceAvailability::Unavailable);
            if (request.mode == PlatformServicesCompositionMode::Null || request.mode == PlatformServicesCompositionMode::Absent) {
                diagnostics_.reasons.fill(request.mode == PlatformServicesCompositionMode::Null
                                              ? PlatformServiceUnavailableReason::NullProviderSelected
                                              : PlatformServiceUnavailableReason::NoProviderSelected);
                open_ = true;
                return Result<void>::Success();
            }
            auto created = factory(request);
            if (created.HasError())
                return Result<void>::Failure(created.ErrorValue());
            auto candidate = std::move(created).Value();
            if (!candidate.backend)
                return Result<void>::Failure(MakeError(FrontendErrors::InvalidComposition));
            auto initialized = InitializeCandidate(request, std::move(candidate));
            if (rollbackError_)
                return Result<void>::Failure(*rollbackError_);
            open_ = initialized.HasValue();
            return initialized;
        } catch (...) {
            return Result<void>::Failure(rollbackError_.value_or(MakeError(BackendErrors::ServiceUnavailable)));
        }
    }

    /** @copydoc PlatformServicesComposition::InitializeCandidate */
    Result<void> PlatformServicesComposition::InitializeCandidate(const PlatformServicesCompositionRequest &request,
                                                                  PlatformServicesCompositionCandidate candidate) {
        std::shared_ptr<IPlatformServicesBackend> backend = std::move(candidate.backend);
        CandidateRollback rollback{backend, retainedBackend_, rollbackError_};
        auto inspected = InspectCandidate(request, *backend, candidate.session);
        if (inspected.HasError())
            return Result<void>::Failure(inspected.ErrorValue());
        auto snapshot = std::move(inspected).Value();
        auto projected = ProjectCapabilities(request, snapshot, diagnostics_);
        if (projected.HasError())
            return Result<void>::Failure(projected.ErrorValue());
        const auto policy = std::move(projected).Value();
        if (auto activated = backend->Activate(request.configuration.BackendConfig()); activated.HasError())
            return activated;
        auto frontend = PlatformServicesFrontend::Create(backend, std::move(snapshot), std::move(candidate.session), policy);
        if (frontend.HasError())
            return Result<void>::Failure(frontend.ErrorValue());
        frontend_ = std::make_shared<PlatformServicesFrontend>(std::move(frontend).Value());
        diagnostics_.provider = request.configuration.SelectedProvider();
        rollback.Commit();
        return Result<void>::Success();
    }

    PlatformServicesComposition::~PlatformServicesComposition() {
        static_cast<void>(Close());
    }

    /** @copydoc PlatformServicesComposition::Frontend */
    Result<std::shared_ptr<const PlatformServicesFrontend>> PlatformServicesComposition::Frontend() const {
        using ViewResult = Result<std::shared_ptr<const PlatformServicesFrontend>>;
        if (!open_ || diagnostics_.mode == PlatformServicesCompositionMode::Absent)
            return ViewResult::Failure(MakeError(FrontendErrors::Unavailable));
        if (diagnostics_.mode == PlatformServicesCompositionMode::Null)
            return ViewResult::Failure(MakeError(FrontendErrors::NullProvider));
        return ViewResult::Success(frontend_);
    }

    /** @copydoc PlatformServicesComposition::Diagnostics */
    PlatformServicesCompositionDiagnostics PlatformServicesComposition::Diagnostics() const noexcept {
        auto result = diagnostics_;
        if (!open_) {
            result.provider.reset();
            result.services.fill(PlatformServiceAvailability::Unavailable);
            result.reasons.fill(PlatformServiceUnavailableReason::NoProviderSelected);
        }
        return result;
    }

    /** @copydoc PlatformServicesComposition::Close */
    Result<void> PlatformServicesComposition::Close() {
        open_ = false;
        if (rollbackError_)
            return Result<void>::Failure(*rollbackError_);
        if (!frontend_)
            return Result<void>::Success();
        auto closed = frontend_->Close();
        if (closed.HasValue())
            frontend_.reset();
        return closed;
    }

    /** @copydoc PlatformServicesComposition::Transition */
    Result<void> PlatformServicesComposition::Transition(const PlatformServicesCompositionRequest &request,
                                                         const PlatformServicesCompositionFactory &factory) {
        if (request.generation.value <= diagnostics_.generation.value)
            return Result<void>::Failure(MakeError(PlatformProjectConfigurationErrors::StaleReplacement));
        if (auto valid = ValidateSelection(request, factory); valid.HasError())
            return valid;
        if (auto closed = Close(); closed.HasError())
            return closed;
        return Initialize(request, factory);
    }
}  // namespace Horo::PlatformServices
