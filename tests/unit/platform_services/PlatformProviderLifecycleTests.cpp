#include "Horo/PlatformServices/PlatformProviderAdmission.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

extern "C" {
struct FixtureAudit;
FixtureAudit *horo_test_provider_audit_create(void);
void horo_test_provider_audit_free(FixtureAudit *);
void horo_test_provider_fail_at(FixtureAudit *, unsigned);
void horo_test_provider_submit_status(FixtureAudit *, unsigned);
void horo_test_provider_set_busy(FixtureAudit *, unsigned);
unsigned horo_test_provider_event_count(const FixtureAudit *);
char horo_test_provider_event(const FixtureAudit *, unsigned);
unsigned horo_test_provider_destroyed(const FixtureAudit *);
unsigned horo_test_provider_post_destroy_callbacks(const FixtureAudit *);
HoroExtensionStatus horo_test_provider_session_changed(FixtureAudit *, std::uint64_t, std::uint32_t);
HoroExtensionStatus horo_test_provider_emit(FixtureAudit *, std::uint64_t, std::uint64_t);
HoroExtensionStatus horo_test_provider_emit_failure(FixtureAudit *, std::uint64_t, std::uint64_t, std::uint32_t);
HoroExtensionStatus horo_test_provider_emit_held(FixtureAudit *, std::uint64_t, std::uint64_t);
unsigned horo_test_provider_held_ready(const FixtureAudit *);
void horo_test_provider_release_held(FixtureAudit *);
HoroPlatformProviderOperations horo_test_provider_operations(void);
HoroPlatformProviderCreateFunc horo_test_provider_create(void);
HoroPlatformProviderRetireFunc horo_test_provider_retire(void);
HoroPlatformProviderDestroyFunc horo_test_provider_destroy(void);
}

namespace Horo::PlatformServices::Tests {
    namespace {
        constexpr auto ServiceMask = (1U << static_cast<std::uint32_t>(PlatformServiceKind::Achievements)) |
                                     (1U << static_cast<std::uint32_t>(PlatformServiceKind::Session));

        [[nodiscard]] Extensions::ExtensionAdmissionPolicy Policy() {
            return {.revision = 1,
                    .knownPermissions = {{"platform.services.use"}},
                    .approvedPermissions = {{"platform.services.use"}},
                    .availableCapabilities = {{"platform.services.provider"}}};
        }

        [[nodiscard]] Extensions::ExtensionCapabilityAdmission Consumer(const Extensions::ExtensionAdmissionPolicy &policy) {
            Extensions::ExtensionAdmissionRequest request{.extensionId = "example.consumer",
                                                          .moduleId = "consumer.module",
                                                          .activationGeneration = 1,
                                                          .capabilities = {{{"platform.services.provider"}, {{"platform.services.use"}}}}};
            return std::move(Extensions::ExtensionCapabilityAdmission::Evaluate(request, policy)).Value();
        }

        [[nodiscard]] Extensions::ExtensionPlatformProviderCandidate Candidate(const std::shared_ptr<FixtureAudit> &audit,
                                                                               std::string key = "example.provider") {
            return {.extensionId = "example.extension",
                    .moduleId = "example.module",
                    .providerKey = std::move(key),
                    .providerId = 41,
                    .platformMask = HORO_PLATFORM_PROVIDER_LINUX,
                    .profileMask = HORO_PLATFORM_PROVIDER_HEADLESS,
                    .serviceMask = ServiceMask,
                    .interfaceMajor = PlatformServicesBackendInterfaceMajor,
                    .interfaceMinor = PlatformServicesBackendInterfaceMinor,
                    .contractMajor = 1,
                    .permissions = {"platform.services.use"},
                    .factoryContext = audit.get(),
                    .createCandidate = horo_test_provider_create(),
                    .retireCandidate = horo_test_provider_retire(),
                    .destroyCandidate = horo_test_provider_destroy(),
                    .operations = horo_test_provider_operations(),
                    .moduleCodeLease = audit};
        }

        [[nodiscard]] PlatformProjectConfiguration Configuration() {
            PlatformProjectConfigurationCandidate candidate{.projectId = "example.project",
                                                            .profile = PlatformServicesHostProfile::HeadlessServer,
                                                            .provider = {.mode = PlatformProviderSelectionMode::ExactProvider,
                                                                         .providerKey = "example.provider"}};
            candidate.services[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = PlatformServiceRequirement::Optional;
            candidate.services[static_cast<std::size_t>(PlatformServiceKind::Session)] = PlatformServiceRequirement::Required;
            PlatformProviderModuleContribution contribution{.module = {"example.module"},
                                                            .providerKey = "example.provider",
                                                            .provider = {41},
                                                            .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                                 PlatformServicesBackendInterfaceMinor},
                                                            .allowedProfiles = PlatformServicesHostProfileMask::HeadlessServer};
            contribution.supportedServices[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = true;
            contribution.supportedServices[static_cast<std::size_t>(PlatformServiceKind::Session)] = true;
            const std::vector contributions{contribution};
            const std::vector trusted{ModuleId{"example.module"}};
            auto built = BuildPlatformProjectConfiguration(candidate, contributions, trusted);
            REQUIRE(built.HasValue());
            return std::move(built).Value();
        }

        [[nodiscard]] std::string Events(const FixtureAudit &audit) {
            std::string events;
            for (unsigned index = 0; index < horo_test_provider_event_count(&audit); ++index)
                events.push_back(horo_test_provider_event(&audit, index));
            return events;
        }

        struct Rig final {
            Extensions::ApplicationCapabilityRegistry capabilities;
            Extensions::BackendServiceRegistry services;
            Extensions::ExtensionAdmissionPolicy policy = Policy();
            PlatformProviderAdmission admission{capabilities, services, policy, Extensions::ExtensionHostPlatform::Linux,
                                                PlatformServicesHostProfile::HeadlessServer};
            Extensions::ExtensionCapabilityAdmission consumer = Consumer(policy);
            std::shared_ptr<FixtureAudit> audit{horo_test_provider_audit_create(), horo_test_provider_audit_free};
            Extensions::ExtensionPlatformProviderPublication publication;
            Extensions::ApplicationCapabilityProviderIdentity identity{"example.module", "example.provider", 1};

            void Publish() {
                auto result = admission.Commit(Candidate(audit));
                REQUIRE(result.HasValue());
                publication = std::move(result).Value();
            }

            [[nodiscard]] Result<std::unique_ptr<PlatformProviderLifecycleHost>> Start() {
                auto authority = consumer.Grant({"platform.services.provider"});
                REQUIRE(authority.HasValue());
                const Extensions::ApplicationCapabilityVersionRange version{{1, 0, 0}, {1, 0, 0}};
                return PlatformProviderLifecycleHost::Start(Configuration(), admission, identity, authority.Value(), version,
                                                            "example.consumer", "consumer.module", 1);
            }
        };
    }  // namespace

    TEST_CASE("Exact selection never substitutes another provider", "[platform-services][lifecycle]") {
        Rig rig;
        rig.Publish();
        rig.identity.providerId = "example.other";
        const auto started = rig.Start();
        REQUIRE(started.HasError());
        CHECK(started.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::InvalidSelection.code.Value());
        CHECK(horo_test_provider_event_count(rig.audit.get()) == 0);
    }

    TEST_CASE("Factory-only provider remains loadable but cannot start operation host", "[platform-services][lifecycle][abi]") {
        Rig rig;
        auto candidate = Candidate(rig.audit);
        candidate.operations = {};
        auto published = rig.admission.Commit(std::move(candidate));
        REQUIRE(published.HasValue());
        rig.publication = std::move(published).Value();
        const auto started = rig.Start();
        REQUIRE(started.HasError());
        CHECK(started.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::UnsupportedProfile.code.Value());
        CHECK(Events(*rig.audit) == "CRZ");
    }

    TEST_CASE("Every failed provider initialization stage rolls back without publication", "[platform-services][lifecycle]") {
        for (const unsigned stage : {1U, 2U, 3U}) {
            Rig rig;
            rig.Publish();
            horo_test_provider_fail_at(rig.audit.get(), stage);
            const auto started = rig.Start();
            REQUIRE(started.HasError());
            CHECK(started.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::InitializationFailed.code.Value());
            const auto events = Events(*rig.audit);
            if (stage == 1)
                CHECK(events == "CIAURZ");
            else if (stage == 2)
                CHECK(events == "CISADTURZ");
            else
                CHECK(events == "CISGAKDTURZ");
            CHECK(horo_test_provider_destroyed(rig.audit.get()) == 1);
        }
    }

    TEST_CASE("Native completion is copied and dispatched after provider callback returns", "[platform-services][lifecycle]") {
        Rig rig;
        rig.Publish();
        auto started = rig.Start();
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        CHECK(host->Session().revision == 1);
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        unsigned observed{};
        auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
            ++observed;
        });
        REQUIRE(subscription.HasValue());
        std::atomic_uint callbackStatus{HORO_EXTENSION_ERROR_INIT_FAILED};
        std::thread callback([&] {
            callbackStatus = horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value);
        });
        callback.join();
        CHECK(callbackStatus == HORO_EXTENSION_SUCCESS);
        CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Running);
        CHECK(observed == 0);
        CHECK(host->DispatchCompletions(64) == 1);
        CHECK(observed == 1);
        const auto completed = host->Query(request.Value());
        REQUIRE(completed.HasValue());
        CHECK(completed.Value().state == PlatformRequestState::Succeeded);
        REQUIRE(host->Close().HasValue());
        CHECK(Events(*rig.audit) == "CISGOAKDTURZ");
    }

    TEST_CASE("BUSY drain retains sink and code until held callback exits", "[platform-services][lifecycle]") {
        Rig rig;
        rig.Publish();
        auto started = rig.Start();
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        unsigned observed{};
        auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
            ++observed;
        });
        REQUIRE(subscription.HasValue());
        horo_test_provider_set_busy(rig.audit.get(), 1);
        std::atomic_uint callbackStatus{HORO_EXTENSION_SUCCESS};
        std::thread callback([&] {
            callbackStatus = horo_test_provider_emit_held(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value);
        });
        while (!horo_test_provider_held_ready(rig.audit.get()))
            std::this_thread::yield();
        const auto firstClose = host->Close();
        REQUIRE(firstClose.HasError());
        CHECK(firstClose.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::DrainBusy.code.Value());
        CHECK(horo_test_provider_destroyed(rig.audit.get()) == 0);
        CHECK(host->UnlockAchievement({1}).HasError());
        horo_test_provider_release_held(rig.audit.get());
        callback.join();
        CHECK(callbackStatus == HORO_EXTENSION_ERROR_OUTPUT_REJECTED);
        horo_test_provider_set_busy(rig.audit.get(), 0);
        REQUIRE(host->Close().HasValue());
        CHECK(horo_test_provider_destroyed(rig.audit.get()) == 1);
        CHECK(horo_test_provider_post_destroy_callbacks(rig.audit.get()) == 0);
        CHECK(observed == 0);
    }

    TEST_CASE("Host translates every provider result to a safe application meaning", "[platform-services][errors]") {
        Rig rig;
        rig.Publish();
        auto started = rig.Start();
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        const std::array<std::pair<std::uint32_t, PlatformProviderFailureCategory>, 10> cases{
            std::pair{HORO_PLATFORM_PROVIDER_OFFLINE, PlatformProviderFailureCategory::Offline},
            std::pair{HORO_PLATFORM_PROVIDER_NOT_SIGNED_IN, PlatformProviderFailureCategory::NotSignedIn},
            std::pair{HORO_PLATFORM_PROVIDER_FORBIDDEN, PlatformProviderFailureCategory::Forbidden},
            std::pair{HORO_PLATFORM_PROVIDER_RATE_LIMITED, PlatformProviderFailureCategory::RateLimited},
            std::pair{HORO_PLATFORM_PROVIDER_PRECONDITION_FAILED, PlatformProviderFailureCategory::PreconditionFailed},
            std::pair{HORO_PLATFORM_PROVIDER_QUOTA_EXCEEDED, PlatformProviderFailureCategory::QuotaExceeded},
            std::pair{HORO_PLATFORM_PROVIDER_INVALID_RESPONSE, PlatformProviderFailureCategory::InvalidResponse},
            std::pair{HORO_PLATFORM_PROVIDER_TRANSIENT_FAILURE, PlatformProviderFailureCategory::TransientFailure},
            std::pair{HORO_PLATFORM_PROVIDER_PERMANENT_FAILURE, PlatformProviderFailureCategory::PermanentFailure},
            std::pair{999U, PlatformProviderFailureCategory::Unknown},
        };
        for (const auto &[code, category] : cases) {
            auto request = host->UnlockAchievement({1});
            REQUIRE(request.HasValue());
            REQUIRE(horo_test_provider_emit_failure(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value,
                                                    code) == HORO_EXTENSION_SUCCESS);
            REQUIRE(host->DispatchCompletions(1) == 1);
            const auto snapshot = host->Query(request.Value());
            REQUIRE(snapshot.HasValue());
            REQUIRE(snapshot.Value().state == PlatformRequestState::Failed);
            REQUIRE(snapshot.Value().terminal);
            REQUIRE(snapshot.Value().terminal->HasError());
            const Error &error = *snapshot.Value().terminal->ErrorValue();
            const auto meaning = category == PlatformProviderFailureCategory::NotSignedIn ? PlatformServiceErrorKind::AuthenticationRequired
                                 : category == PlatformProviderFailureCategory::Forbidden ? PlatformServiceErrorKind::AccessDenied
                                                                                          : PlatformServiceErrorKind::ProviderFailed;
            CHECK(ClassifyPlatformServiceError(error) == meaning);
            CHECK(PlatformProviderCategory(error) == category);
            REQUIRE(error.cause.Get());
            REQUIRE(error.cause.Get()->diagnostics.size() == 1);
            const auto &message = error.cause.Get()->diagnostics.front().message;
            CHECK(message.find(std::to_string(request.Value().Id().value)) != std::string::npos);
            CHECK(message.find("private-account") == std::string::npos);
            CHECK(error.message.find("private-account") == std::string::npos);
        }
        for (const auto &[code, kind, state] :
             std::array{std::tuple{HORO_PLATFORM_PROVIDER_CANCELLED, PlatformServiceErrorKind::Cancelled, PlatformRequestState::Cancelled},
                        std::tuple{HORO_PLATFORM_PROVIDER_TIMED_OUT, PlatformServiceErrorKind::TimedOut, PlatformRequestState::TimedOut},
                        std::tuple{HORO_PLATFORM_PROVIDER_CAPABILITY_UNAVAILABLE, PlatformServiceErrorKind::CapabilityUnavailable,
                                   PlatformRequestState::Failed}}) {
            auto request = host->UnlockAchievement({1});
            REQUIRE(request.HasValue());
            REQUIRE(horo_test_provider_emit_failure(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value,
                                                    code) == HORO_EXTENSION_SUCCESS);
            REQUIRE(host->DispatchCompletions(1) == 1);
            const auto snapshot = host->Query(request.Value());
            REQUIRE(snapshot.HasValue());
            CHECK(snapshot.Value().state == state);
            REQUIRE(snapshot.Value().terminal);
            REQUIRE(snapshot.Value().terminal->HasError());
            CHECK(ClassifyPlatformServiceError(*snapshot.Value().terminal->ErrorValue()) == kind);
        }
        REQUIRE(host->Close().HasValue());
    }

    TEST_CASE("Synchronous provider submission failure keeps cancellation distinct from unknown failure", "[platform-services][errors]") {
        Rig rig;
        rig.Publish();
        auto started = rig.Start();
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        for (const auto [status, expectedState, expectedMeaning] :
             std::array{std::tuple{HORO_EXTENSION_ERROR_CANCELLED, PlatformRequestState::Cancelled, PlatformServiceErrorKind::Cancelled},
                        std::tuple{HORO_EXTENSION_ERROR_INIT_FAILED, PlatformRequestState::Failed,
                                   PlatformServiceErrorKind::ProviderFailed}}) {
            horo_test_provider_submit_status(rig.audit.get(), status);
            auto request = host->UnlockAchievement({1});
            REQUIRE(request.HasValue());
            const auto snapshot = host->Query(request.Value());
            REQUIRE(snapshot.HasValue());
            CHECK(snapshot.Value().state == expectedState);
            REQUIRE(snapshot.Value().terminal);
            REQUIRE(snapshot.Value().terminal->HasError());
            const Error &error = *snapshot.Value().terminal->ErrorValue();
            CHECK(ClassifyPlatformServiceError(error) == expectedMeaning);
            if (expectedMeaning == PlatformServiceErrorKind::ProviderFailed)
                CHECK(PlatformProviderCategory(error) == PlatformProviderFailureCategory::Unknown);
        }
        REQUIRE(host->Close().HasValue());
    }

    TEST_CASE("Session revision change fences an in-flight native completion", "[platform-services][lifecycle]") {
        Rig rig;
        rig.Publish();
        auto started = rig.Start();
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        REQUIRE(horo_test_provider_session_changed(rig.audit.get(), 2, 2) == HORO_EXTENSION_SUCCESS);
        REQUIRE(horo_test_provider_emit(rig.audit.get(), request.Value().Id().value, request.Value().Generation().value) ==
                HORO_EXTENSION_SUCCESS);
        CHECK(host->DispatchCompletions(64) == 1);
        const auto result = host->Query(request.Value());
        REQUIRE(result.HasValue());
        CHECK(result.Value().state == PlatformRequestState::Failed);
        REQUIRE(result.Value().terminal.has_value());
        REQUIRE(result.Value().terminal->HasError());
        CHECK(result.Value().terminal->ErrorValue()->code.Value() == PlatformSessionErrors::StaleSession.code.Value());
        REQUIRE(host->Close().HasValue());
    }

    TEST_CASE("Repeated start and stop releases every native and module lease", "[platform-services][lifecycle]") {
        Rig rig;
        rig.Publish();
        std::weak_ptr<FixtureAudit> code = rig.audit;
        for (unsigned cycle = 0; cycle < 2; ++cycle) {
            auto started = rig.Start();
            REQUIRE(started.HasValue());
            auto host = std::move(started).Value();
            REQUIRE(host->Close().HasValue());
            CHECK(horo_test_provider_destroyed(rig.audit.get()) == 1);
        }
        CHECK(Events(*rig.audit) == "CISGAKDTURZCISGAKDTURZ");
        rig.publication.reset();
        rig.audit.reset();
        CHECK(rig.admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Complete);
        CHECK(code.expired());
    }
}  // namespace Horo::PlatformServices::Tests
