#pragma once

#include "Horo/PlatformServices/PlatformProviderAdmission.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"

#include <algorithm>
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
void horo_test_provider_cancel_status(FixtureAudit *, unsigned);
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

        [[nodiscard]] PlatformProjectConfiguration Configuration(
            const PlatformServicesHostProfile profile = PlatformServicesHostProfile::HeadlessServer) {
            PlatformProjectConfigurationCandidate candidate{.projectId = "example.project",
                                                            .profile = profile,
                                                            .provider = {.mode = PlatformProviderSelectionMode::ExactProvider,
                                                                         .providerKey = "example.provider"}};
            candidate.services[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = PlatformServiceRequirement::Optional;
            candidate.services[static_cast<std::size_t>(PlatformServiceKind::Session)] = PlatformServiceRequirement::Required;
            PlatformProviderModuleContribution contribution{.module = {"example.module"},
                                                            .providerKey = "example.provider",
                                                            .provider = {41},
                                                            .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                                 PlatformServicesBackendInterfaceMinor},
                                                            .allowedProfiles = PlatformServicesHostProfileMask::HeadlessServer |
                                                                               PlatformServicesHostProfileMask::InteractiveDevelopment};
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

            [[nodiscard]] Result<std::unique_ptr<PlatformProviderLifecycleHost>> Start(
                PlatformProviderRequestPolicy requestPolicy = {},
                const PlatformServicesHostProfile profile = PlatformServicesHostProfile::HeadlessServer) {
                auto authority = consumer.Grant({"platform.services.provider"});
                REQUIRE(authority.HasValue());
                const Extensions::ApplicationCapabilityVersionRange version{{1, 0, 0}, {1, 0, 0}};
                const auto configuration = Configuration(profile);
                return PlatformProviderLifecycleHost::Start({configuration, admission, identity, authority.Value(), version,
                                                             "example.consumer", "consumer.module", 1, std::move(requestPolicy)});
            }
        };
    }  // namespace
