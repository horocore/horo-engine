#include "Horo/PlatformServices/PlatformServicesComposition.h"
#include "PlatformServicesFrontendTestSupport.h"

namespace Horo::PlatformServices {
    namespace {
        PlatformServicesHostProfile AdmissionProfile(const PlatformServicesProductProfile profile) {
            if (profile == PlatformServicesProductProfile::ShippingGame)
                return PlatformServicesHostProfile::Certification;
            if (profile == PlatformServicesProductProfile::Headless || profile == PlatformServicesProductProfile::DedicatedServer)
                return PlatformServicesHostProfile::HeadlessServer;
            if (profile == PlatformServicesProductProfile::UnsupportedPlatform)
                return PlatformServicesHostProfile::Cook;
            return PlatformServicesHostProfile::InteractiveDevelopment;
        }

        PlatformProjectConfiguration Configuration(const PlatformServicesProductProfile profile, const bool null = false,
                                                   const PlatformServiceRequirement achievements = PlatformServiceRequirement::Optional,
                                                   const bool achievementsDeclared = true) {
            PlatformProjectConfigurationCandidate candidate{.projectId = "composition-project", .profile = AdmissionProfile(profile)};
            candidate.services.fill(PlatformServiceRequirement::Optional);
            candidate.services[0] = achievements;
            if (!null)
                candidate.provider = {.mode = PlatformProviderSelectionMode::ExactProvider, .providerKey = "test.provider"};
            PlatformProviderModuleContribution contribution{.module = {"test.module"},
                                                            .providerKey = "test.provider",
                                                            .provider = {41},
                                                            .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                                 PlatformServicesBackendInterfaceMinor},
                                                            .allowedProfiles = static_cast<PlatformServicesHostProfileMask>(15)};
            contribution.supportedServices.fill(true);
            contribution.supportedServices[0] = achievementsDeclared;
            const std::array contributions{contribution};
            const std::array modules{contribution.module};
            auto built = BuildPlatformProjectConfiguration(candidate, contributions, modules);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        PlatformServicesCompositionRequest Request(const PlatformProjectConfiguration &config, const PlatformServicesProductProfile profile,
                                                   const std::uint64_t generation = 7) {
            PlatformServicesCompositionRequest request{.configuration = config, .profile = profile, .generation = {generation}};
            if (config.UsesNullProvider())
                return request;
            request.mode = profile == PlatformServicesProductProfile::Test ? PlatformServicesCompositionMode::Test
                                                                           : PlatformServicesCompositionMode::ExactProvider;
            auto provenance = PlatformServicesProviderProvenance::TrustedDevelopment;
            if (profile == PlatformServicesProductProfile::ShippingGame)
                provenance = PlatformServicesProviderProvenance::ShippingManifest;
            else if (profile == PlatformServicesProductProfile::Headless || profile == PlatformServicesProductProfile::DedicatedServer)
                provenance = PlatformServicesProviderProvenance::ServerManifest;
            else if (profile == PlatformServicesProductProfile::Test)
                provenance = PlatformServicesProviderProvenance::PublicTestFixture;
            request.evidence = PlatformServicesProductEvidence{.provenance = provenance,
                                                               .configurationFingerprint = config.Fingerprint(),
                                                               .generation = {generation}};
            request.evidence->allowedServices.fill(true);
            return request;
        }

        PlatformServicesCompositionFactory Factory(unsigned &calls, RoutingBackend *&backend) {
            return [&](const PlatformServicesCompositionRequest &request) {
                ++calls;
                auto candidate = std::make_unique<RoutingBackend>();
                candidate->snapshot = Capabilities(request.generation);
                backend = candidate.get();
                return Result<PlatformServicesCompositionCandidate>::Success({std::move(candidate), Session(request.generation)});
            };
        }
    }  // namespace

    TEST_CASE("Every product supports explicit fail-closed Null without invoking a factory", "[platform-services][composition]") {
        for (unsigned index = 0; index < static_cast<unsigned>(PlatformServicesProductProfile::Count); ++index) {
            const auto profile = static_cast<PlatformServicesProductProfile>(index);
            const auto config = Configuration(profile, true);
            unsigned calls{};
            RoutingBackend *backend{};
            auto started = PlatformServicesComposition::Start(Request(config, profile), Factory(calls, backend));
            REQUIRE(started.HasValue());
            auto host = std::move(started).Value();
            CHECK(calls == 0);
            RequireError(host->Frontend(), FrontendErrors::NullProvider);
            CHECK_FALSE(host->Diagnostics().provider.has_value());
            for (const auto availability : host->Diagnostics().services)
                CHECK(availability == PlatformServiceAvailability::Unavailable);
            REQUIRE(host->Close().HasValue());
            RequireError(host->Frontend(), FrontendErrors::Unavailable);
            REQUIRE(host->Close().HasValue());
        }
    }

    TEST_CASE("All supported products admit only their explicit provider provenance", "[platform-services][composition]") {
        for (const auto profile : {PlatformServicesProductProfile::Editor, PlatformServicesProductProfile::DevelopmentGame,
                                   PlatformServicesProductProfile::ShippingGame, PlatformServicesProductProfile::Headless,
                                   PlatformServicesProductProfile::DedicatedServer, PlatformServicesProductProfile::Test}) {
            const auto config = Configuration(profile);
            auto request = Request(config, profile);
            unsigned calls{};
            RoutingBackend *backend{};
            auto started = PlatformServicesComposition::Start(request, Factory(calls, backend));
            REQUIRE(started.HasValue());
            CHECK(calls == 1);
            auto host = std::move(started).Value();
            auto view = host->Frontend();
            REQUIRE(view.HasValue());
            CHECK(view.Value()->QueryCurrentSession().HasValue());
            CHECK(host->Diagnostics().provider == PlatformProviderId{41});
            REQUIRE(host->Close().HasValue());
            CHECK(backend->shutdownCalls == 1);
            CHECK_FALSE(view.Value()->IsOpen());
        }
    }

    TEST_CASE("Restricted profiles reject fixtures, client credentials and stale product manifests before creation",
              "[platform-services][composition]") {
        for (const auto profile : {PlatformServicesProductProfile::ShippingGame, PlatformServicesProductProfile::Headless,
                                   PlatformServicesProductProfile::DedicatedServer}) {
            const auto config = Configuration(profile);
            auto request = Request(config, profile);
            unsigned calls{};
            RoutingBackend *backend{};
            const auto factory = Factory(calls, backend);
            request.evidence.reset();
            CHECK(PlatformServicesComposition::Start(request, factory).HasError());
            request.evidence = Request(config, profile).evidence;
            request.evidence->provenance = PlatformServicesProviderProvenance::TrustedDevelopment;
            CHECK(PlatformServicesComposition::Start(request, factory).HasError());
            request.evidence->provenance = PlatformServicesProviderProvenance::PublicTestFixture;
            CHECK(PlatformServicesComposition::Start(request, factory).HasError());
            request.evidence = Request(config, profile).evidence;
            request.evidence->configurationFingerprint = {};
            CHECK(PlatformServicesComposition::Start(request, factory).HasError());
            request.evidence = Request(config, profile).evidence;
            request.evidence->generation = {8};
            CHECK(PlatformServicesComposition::Start(request, factory).HasError());
            request.evidence = Request(config, profile).evidence;
            request.mode = PlatformServicesCompositionMode::Test;
            CHECK(PlatformServicesComposition::Start(request, factory).HasError());
            CHECK(calls == 0);
        }
    }

    TEST_CASE("Shipping exposes only project and manifest admitted capabilities", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::ShippingGame, false, PlatformServiceRequirement::Disabled);
        auto request = Request(config, PlatformServicesProductProfile::ShippingGame);
        request.evidence->allowedServices[static_cast<std::size_t>(PlatformServiceKind::Friends)] = false;
        unsigned calls{};
        RoutingBackend *backend{};
        auto started = PlatformServicesComposition::Start(request, Factory(calls, backend));
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        const auto view = host->Frontend().Value();
        const auto subject = *Session(request.generation).Subject();
        RequireError(view->UnlockAchievement({subject, {1}}), FrontendErrors::OperationDenied);
        RequireError(view->QueryFriends({subject, 1}), FrontendErrors::OperationDenied);
        CHECK(backend->TotalCalls() == 0);
        const auto diagnostics = host->Diagnostics();
        CHECK(diagnostics.services[0] == PlatformServiceAvailability::Unavailable);
        CHECK(diagnostics.reasons[0] == PlatformServiceUnavailableReason::HostPolicyDenied);
        CHECK(diagnostics.services[static_cast<std::size_t>(PlatformServiceKind::Friends)] == PlatformServiceAvailability::Unavailable);
        CHECK(view->ServiceLimits(PlatformServiceKind::Session).HasValue());
    }

    TEST_CASE("Required capabilities cannot be suppressed by product evidence or resolve to Null", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::ShippingGame, false, PlatformServiceRequirement::Required);
        auto request = Request(config, PlatformServicesProductProfile::ShippingGame);
        request.evidence->allowedServices[0] = false;
        unsigned calls{};
        RoutingBackend *backend{};
        RequireError(PlatformServicesComposition::Start(request, Factory(calls, backend)), BackendErrors::RequiredServiceUnavailable);
        CHECK(calls == 0);
        PlatformProjectConfigurationCandidate candidate{.projectId = "required-project",
                                                        .profile = PlatformServicesHostProfile::HeadlessServer};
        candidate.services[0] = PlatformServiceRequirement::Required;
        RequireError(BuildPlatformProjectConfiguration(candidate, {}, {}),
                     PlatformProjectConfigurationErrors::RequiredCapabilityUnsupported);
    }

    TEST_CASE("Unsupported products publish explicit absence and never try another provider", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::UnsupportedPlatform, true);
        auto request = Request(config, PlatformServicesProductProfile::UnsupportedPlatform);
        request.mode = PlatformServicesCompositionMode::Absent;
        auto started = PlatformServicesComposition::Start(request);
        REQUIRE(started.HasValue());
        const auto &host = started.Value();
        RequireError(host->Frontend(), FrontendErrors::Unavailable);
        CHECK(host->Diagnostics().mode == PlatformServicesCompositionMode::Absent);
        CHECK(host->Diagnostics().reasons[0] == PlatformServiceUnavailableReason::NoProviderSelected);
        const auto exactConfig = Configuration(PlatformServicesProductProfile::UnsupportedPlatform);
        unsigned calls{};
        RoutingBackend *backend{};
        CHECK(PlatformServicesComposition::Start(Request(exactConfig, PlatformServicesProductProfile::UnsupportedPlatform),
                                                 Factory(calls, backend))
                  .HasError());
        CHECK(calls == 0);
    }

    TEST_CASE("Transition retires old state and retained views before a fresh factory starts", "[platform-services][composition]") {
        const auto editor = Configuration(PlatformServicesProductProfile::Editor);
        unsigned calls{};
        RoutingBackend *backend{};
        auto started = PlatformServicesComposition::Start(Request(editor, PlatformServicesProductProfile::Editor), Factory(calls, backend));
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        const auto oldView = host->Frontend().Value();
        auto *oldBackend = backend;
        const auto headless = Configuration(PlatformServicesProductProfile::Headless, true);
        RequireError(host->Transition(Request(headless, PlatformServicesProductProfile::Headless, 7)),
                     PlatformProjectConfigurationErrors::StaleReplacement);
        CHECK(oldView->IsOpen());
        REQUIRE(host->Transition(Request(headless, PlatformServicesProductProfile::Headless, 8)).HasValue());
        CHECK(oldBackend->shutdownCalls == 1);
        CHECK_FALSE(oldView->IsOpen());
        RequireError(oldView->QueryCurrentSession(), FrontendErrors::Unavailable);
        REQUIRE(host->Transition(Request(editor, PlatformServicesProductProfile::Editor, 9), [&](const auto &request) {
            CHECK(oldBackend->shutdownCalls == 1);
            CHECK_FALSE(oldView->IsOpen());
            return Factory(calls, backend)(request);
        }).HasValue());
        CHECK(calls == 2);
        CHECK(host->Diagnostics().generation == PlatformProviderGeneration{9});
    }

    TEST_CASE("Rejected or failed replacement cannot reopen an old provider generation", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::Editor);
        unsigned calls{};
        RoutingBackend *backend{};
        auto started = PlatformServicesComposition::Start(Request(config, PlatformServicesProductProfile::Editor), Factory(calls, backend));
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        const auto view = host->Frontend().Value();
        auto invalid = Request(config, PlatformServicesProductProfile::Editor, 8);
        invalid.generation = {};
        CHECK(host->Transition(invalid).HasError());
        CHECK(view->IsOpen());
        const PlatformServicesCompositionFactory failed = [](const auto &) {
            return Result<PlatformServicesCompositionCandidate>::Failure(MakeError(BackendErrors::ServiceUnavailable));
        };
        RequireError(host->Transition(Request(config, PlatformServicesProductProfile::Editor, 8), failed),
                     BackendErrors::ServiceUnavailable);
        CHECK_FALSE(view->IsOpen());
        RequireError(host->Frontend(), FrontendErrors::Unavailable);
        CHECK_FALSE(host->Diagnostics().provider.has_value());
        RequireError(host->Transition(Request(config, PlatformServicesProductProfile::Editor, 8), Factory(calls, backend)),
                     PlatformProjectConfigurationErrors::StaleReplacement);
        REQUIRE(host->Transition(Request(config, PlatformServicesProductProfile::Editor, 9), Factory(calls, backend)).HasValue());
    }

    TEST_CASE("Malformed factories and mismatched candidate generations cannot publish", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::Test);
        auto request = Request(config, PlatformServicesProductProfile::Test);
        CHECK(PlatformServicesComposition::Start(request).HasError());
        const PlatformServicesCompositionFactory empty = [](const auto &input) {
            return Result<PlatformServicesCompositionCandidate>::Success({{}, Session(input.generation)});
        };
        RequireError(PlatformServicesComposition::Start(request, empty), FrontendErrors::InvalidComposition);
        const PlatformServicesCompositionFactory stale = [](const auto &input) {
            auto backend = std::make_unique<RoutingBackend>();
            backend->snapshot = Capabilities({input.generation.value + 1});
            return Result<PlatformServicesCompositionCandidate>::Success({std::move(backend), Session(input.generation)});
        };
        RequireError(PlatformServicesComposition::Start(request, stale), FrontendErrors::InvalidComposition);
        request.profile = PlatformServicesProductProfile::Count;
        RequireError(PlatformServicesComposition::Start(request, empty), FrontendErrors::InvalidComposition);
        request.profile = PlatformServicesProductProfile::Test;
        request.mode = static_cast<PlatformServicesCompositionMode>(255);
        RequireError(PlatformServicesComposition::Start(request, empty), FrontendErrors::InvalidComposition);
    }

    TEST_CASE("Provider failures remain typed and never publish partial capability state", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::Test);
        const auto request = Request(config, PlatformServicesProductProfile::Test);
        const PlatformServicesCompositionFactory throwing = [](const auto &) -> Result<PlatformServicesCompositionCandidate> {
            throw std::runtime_error("private provider diagnostic");
        };
        RequireError(PlatformServicesComposition::Start(request, throwing), BackendErrors::ServiceUnavailable);
        const PlatformServicesCompositionFactory wrongProvider = [](const auto &input) {
            auto backend = std::make_unique<RoutingBackend>();
            backend->snapshot = Capabilities(input.generation);
            backend->snapshot.provider = {99};
            return Result<PlatformServicesCompositionCandidate>::Success({std::move(backend), Session(input.generation)});
        };
        RequireError(PlatformServicesComposition::Start(request, wrongProvider), FrontendErrors::InvalidComposition);
        const PlatformServicesCompositionFactory wrongSession = [](const auto &input) {
            auto backend = std::make_unique<RoutingBackend>();
            backend->snapshot = Capabilities(input.generation);
            return Result<PlatformServicesCompositionCandidate>::Success({std::move(backend), Session({input.generation.value + 1})});
        };
        RequireError(PlatformServicesComposition::Start(request, wrongSession), FrontendErrors::InvalidComposition);
        const PlatformServicesCompositionFactory malformed = [](const auto &input) {
            auto backend = std::make_unique<RoutingBackend>();
            backend->snapshot = Capabilities(input.generation);
            backend->snapshot.services[0].binding.reset();
            return Result<PlatformServicesCompositionCandidate>::Success({std::move(backend), Session(input.generation)});
        };
        RequireError(PlatformServicesComposition::Start(request, malformed), BackendErrors::InvalidCapabilitySnapshot);
    }

    TEST_CASE("Null and absence reject incompatible configuration, evidence and host profiles before creation",
              "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::Headless, true);
        auto request = Request(config, PlatformServicesProductProfile::Headless);
        request.mode = PlatformServicesCompositionMode::Absent;
        RequireError(PlatformServicesComposition::Start(request), FrontendErrors::InvalidComposition);
        request.mode = PlatformServicesCompositionMode::Null;
        request.evidence.emplace();
        RequireError(PlatformServicesComposition::Start(request), FrontendErrors::InvalidComposition);
        request.evidence.reset();
        request.profile = PlatformServicesProductProfile::Editor;
        RequireError(PlatformServicesComposition::Start(request), FrontendErrors::InvalidComposition);
        const auto exact = Configuration(PlatformServicesProductProfile::Editor);
        auto exactRequest = Request(exact, PlatformServicesProductProfile::Editor);
        exactRequest.mode = PlatformServicesCompositionMode::Null;
        exactRequest.evidence.reset();
        RequireError(PlatformServicesComposition::Start(exactRequest), FrontendErrors::InvalidComposition);
    }

    TEST_CASE("Providers and product evidence cannot invent undeclared services", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::Test, false, PlatformServiceRequirement::Optional, false);
        CHECK_FALSE(config.SelectedServices()[0]);
        auto request = Request(config, PlatformServicesProductProfile::Test);
        unsigned calls{};
        RoutingBackend *backend{};
        const auto factory = Factory(calls, backend);
        RequireError(PlatformServicesComposition::Start(request, factory), BackendErrors::InvalidCapabilitySnapshot);
        CHECK(calls == 0);
        request.evidence->allowedServices[0] = false;
        RequireError(PlatformServicesComposition::Start(request, factory), BackendErrors::InvalidCapabilitySnapshot);
        CHECK(calls == 1);
        auto denied = Request(config, PlatformServicesProductProfile::Editor);
        CHECK(PlatformServicesComposition::Start(denied, factory).HasError());
        CHECK(calls == 1);
    }

    TEST_CASE("Failed provider shutdown blocks transition and revokes capability truth", "[platform-services][composition]") {
        const auto config = Configuration(PlatformServicesProductProfile::Test);
        unsigned calls{};
        RoutingBackend *backend{};
        auto started = PlatformServicesComposition::Start(Request(config, PlatformServicesProductProfile::Test), Factory(calls, backend));
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        const auto oldView = host->Frontend().Value();
        backend->shutdownFails = true;
        const auto headless = Configuration(PlatformServicesProductProfile::Headless, true);
        CHECK(host->Transition(Request(headless, PlatformServicesProductProfile::Headless, 8)).HasError());
        CHECK(host->Transition(Request(config, PlatformServicesProductProfile::Test, 9), Factory(calls, backend)).HasError());
        CHECK(calls == 1);
        CHECK(backend->shutdownCalls == 1);
        CHECK_FALSE(oldView->IsOpen());
        CHECK_FALSE(host->Diagnostics().provider.has_value());
        RequireError(host->Frontend(), FrontendErrors::Unavailable);
    }

    TEST_CASE("Failed candidate rollback retains the generation and blocks later factories", "[platform-services][composition]") {
        const auto empty = Configuration(PlatformServicesProductProfile::Test, true);
        const auto exact = Configuration(PlatformServicesProductProfile::Test);
        for (unsigned failure = 0; failure != 3; ++failure) {
            auto started = PlatformServicesComposition::Start(Request(empty, PlatformServicesProductProfile::Test));
            REQUIRE(started.HasValue());
            auto host = std::move(started).Value();
            RoutingBackend *retained{};
            unsigned calls{};
            const PlatformServicesCompositionFactory factory = [&](const auto &input) {
                ++calls;
                auto backend = std::make_unique<RoutingBackend>();
                backend->snapshot = Capabilities(input.generation);
                backend->snapshot.provider = {99};
                backend->shutdownFails = failure == 0;
                backend->shutdownThrows = failure == 1;
                backend->shutdownThrowsNonStandard = failure == 2;
                retained = backend.get();
                return Result<PlatformServicesCompositionCandidate>::Success({std::move(backend), Session(input.generation)});
            };
            RequireError(host->Transition(Request(exact, PlatformServicesProductProfile::Test, 8), factory),
                         BackendErrors::ServiceUnavailable);
            REQUIRE(retained != nullptr);
            CHECK(retained->shutdownCalls == 1);
            CHECK_FALSE(host->Diagnostics().provider.has_value());
            RequireError(host->Frontend(), FrontendErrors::Unavailable);
            RequireError(host->Close(), BackendErrors::ServiceUnavailable);
            RequireError(host->Transition(Request(exact, PlatformServicesProductProfile::Test, 9), factory),
                         BackendErrors::ServiceUnavailable);
            CHECK(calls == 1);
            CHECK(retained->shutdownCalls == 1);
            retained->shutdownFails = false;
            retained->shutdownThrows = false;
            retained->shutdownThrowsNonStandard = false;
        }
    }

    TEST_CASE("Non-standard factory exceptions leave a retired generation closed", "[platform-services][composition]") {
        const auto empty = Configuration(PlatformServicesProductProfile::Test, true);
        const auto exact = Configuration(PlatformServicesProductProfile::Test);
        auto started = PlatformServicesComposition::Start(Request(empty, PlatformServicesProductProfile::Test));
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        const PlatformServicesCompositionFactory throwing = [](const auto &) -> Result<PlatformServicesCompositionCandidate> {
            throw 17;
        };
        RequireError(host->Transition(Request(exact, PlatformServicesProductProfile::Test, 8), throwing),
                     BackendErrors::ServiceUnavailable);
        RequireError(host->Frontend(), FrontendErrors::Unavailable);
        CHECK_FALSE(host->Diagnostics().provider.has_value());
        RequireError(host->Transition(Request(exact, PlatformServicesProductProfile::Test, 8), throwing),
                     PlatformProjectConfigurationErrors::StaleReplacement);
    }
}  // namespace Horo::PlatformServices
