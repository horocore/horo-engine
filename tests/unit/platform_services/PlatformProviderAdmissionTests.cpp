#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/PlatformServices/PlatformProviderAdmission.h"
#include "SecurityTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <utility>

namespace Horo::PlatformServices::Tests {
    namespace {
        [[nodiscard]] constexpr Extensions::ExtensionHostPlatform HostPlatform() noexcept {
#if defined(_WIN32)
            return Extensions::ExtensionHostPlatform::Windows;
#elif defined(__APPLE__)
            return Extensions::ExtensionHostPlatform::MacOS;
#else
            return Extensions::ExtensionHostPlatform::Linux;
#endif
        }

        [[nodiscard]] constexpr std::uint32_t HostPlatformMask() noexcept {
            return 1U << static_cast<std::uint8_t>(HostPlatform());
        }

        struct Audit final {
            std::atomic_bool allowRetire{true};
            std::atomic_int created{};
            std::atomic_int retired{};
            std::atomic_int destroyed{};
        };

        HoroExtensionStatus CreateNative(void *context, void **outCandidate) {
            auto *audit = static_cast<Audit *>(context);
            ++audit->created;
            *outCandidate = audit;
            return HORO_EXTENSION_SUCCESS;
        }

        HoroExtensionStatus RetireNative(void *candidate) {
            auto *audit = static_cast<Audit *>(candidate);
            ++audit->retired;
            return audit->allowRetire.load() ? HORO_EXTENSION_SUCCESS : HORO_EXTENSION_ERROR_BUSY;
        }

        void DestroyNative(void *candidate) {
            ++static_cast<Audit *>(candidate)->destroyed;
        }

        [[nodiscard]] Extensions::ExtensionAdmissionPolicy Policy() {
            return {.revision = 1,
                    .knownPermissions = {{"platform.services.use"}},
                    .approvedPermissions = {{"platform.services.use"}},
                    .availableCapabilities = {{"platform.services.provider"}}};
        }

        [[nodiscard]] Extensions::ExtensionPlatformProviderCandidate Candidate(const std::string &key, const std::uint16_t providerId,
                                                                               const std::shared_ptr<Audit> &audit,
                                                                               const std::uint16_t contractMajor = 1) {
            return {.extensionId = "example.extension",
                    .moduleId = "example.module",
                    .providerKey = key,
                    .providerId = providerId,
                    .platformMask = HostPlatformMask(),
                    .profileMask = HORO_PLATFORM_PROVIDER_HEADLESS,
                    .serviceMask = 1U << static_cast<std::uint8_t>(PlatformServiceKind::Achievements),
                    .interfaceMajor = PlatformServicesBackendInterfaceMajor,
                    .interfaceMinor = PlatformServicesBackendInterfaceMinor,
                    .contractMajor = contractMajor,
                    .permissions = {"platform.services.use"},
                    .factoryContext = audit.get(),
                    .createCandidate = CreateNative,
                    .retireCandidate = RetireNative,
                    .destroyCandidate = DestroyNative,
                    .moduleCodeLease = audit};
        }

        [[nodiscard]] Extensions::ExtensionCapabilityAdmission Consumer(const Extensions::ExtensionAdmissionPolicy &policy) {
            Extensions::ExtensionAdmissionRequest request{.extensionId = "example.consumer",
                                                          .moduleId = "consumer.module",
                                                          .activationGeneration = 1,
                                                          .capabilities = {{{"platform.services.provider"}, {{"platform.services.use"}}}}};
            return std::move(Extensions::ExtensionCapabilityAdmission::Evaluate(request, policy)).Value();
        }

        [[nodiscard]] Extensions::ApplicationCapabilityVersionRange Version(const std::uint16_t major) {
            return {{major, 0, 0}, {major, 0, 0}};
        }
    }  // namespace

    TEST_CASE("Revoking one provider preserves another and permits a later generation", "[platform-services][extension][provider]") {
        Extensions::ApplicationCapabilityRegistry capabilities;
        Extensions::BackendServiceRegistry services;
        const auto policy = Policy();
        PlatformProviderAdmission admission{capabilities, services, policy, HostPlatform(), PlatformServicesHostProfile::HeadlessServer};
        auto auditA = std::make_shared<Audit>();
        auto auditB = std::make_shared<Audit>();
        auto publishedA = admission.Commit(Candidate("example.provider-a", 1, auditA));
        auto publishedB = admission.Commit(Candidate("example.provider-b", 2, auditB, 2));
        REQUIRE(publishedA.HasValue());
        REQUIRE(publishedB.HasValue());
        auto consumer = Consumer(policy);
        const auto authority = consumer.Grant({"platform.services.provider"});
        REQUIRE(authority.HasValue());

        std::optional<PlatformProviderRequestLease> requestA;
        {
            auto backendA = admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1);
            REQUIRE(backendA.HasValue());
            REQUIRE(backendA.Value().Provider() == PlatformProviderId{1});
            auto request = backendA.Value().AcquireRequestLease();
            REQUIRE(request.HasValue());
            requestA.emplace(std::move(request).Value());
            publishedA.Value()->Revoke();
            CHECK(backendA.Value().AcquireRequestLease().HasError());
            CHECK(admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1).HasError());
            CHECK(auditA->destroyed == 0);
        }
        auditA->allowRetire = false;
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        requestA.reset();
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        CHECK(auditA->destroyed == 0);
        auditA->allowRetire = true;
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Complete);
        CHECK(auditA->destroyed == 1);

        auto backendB = admission.Create(authority.Value(), Version(2), "example.consumer", "consumer.module", 1);
        REQUIRE(backendB.HasValue());
        CHECK(backendB.Value().Provider() == PlatformProviderId{2});
        auto auditC = std::make_shared<Audit>();
        auto publishedC = admission.Commit(Candidate("example.provider-c", 3, auditC));
        REQUIRE(publishedC.HasValue());
        auto backendC = admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1);
        REQUIRE(backendC.HasValue());
        CHECK(backendC.Value().Provider() == PlatformProviderId{3});
    }

    TEST_CASE("Provider admission rejects invalid and duplicate claims without capability publication",
              "[platform-services][extension][provider]") {
        Extensions::ApplicationCapabilityRegistry capabilities;
        Extensions::BackendServiceRegistry services;
        const auto policy = Policy();
        PlatformProviderAdmission admission{capabilities, services, policy, HostPlatform(), PlatformServicesHostProfile::HeadlessServer};
        auto audit = std::make_shared<Audit>();
        auto invalid = Candidate("example.invalid", 1, audit);
        invalid.platformMask =
            HostPlatformMask() == HORO_PLATFORM_PROVIDER_WINDOWS ? HORO_PLATFORM_PROVIDER_LINUX : HORO_PLATFORM_PROVIDER_WINDOWS;
        CHECK(admission.Commit(std::move(invalid)).HasError());
        invalid = Candidate("example.invalid", 1, audit);
        invalid.permissions = {"platform.services.unapproved"};
        CHECK(admission.Commit(std::move(invalid)).HasError());
        auto first = admission.Commit(Candidate("example.first", 1, audit));
        REQUIRE(first.HasValue());
        CHECK(admission.Commit(Candidate("example.duplicate", 1, audit)).HasError());
        auto consumer = Consumer(policy);
        auto authority = consumer.Grant({"platform.services.provider"});
        REQUIRE(authority.HasValue());
        CHECK(admission.Create(authority.Value(), Version(2), "example.consumer", "consumer.module", 1).HasError());
        CHECK(audit->created == 0);
    }

    TEST_CASE("Manager publication erase retains code until foreign request release and owner-thread drain",
              "[platform-services][extension][provider][lifetime]") {
        Extensions::ApplicationCapabilityRegistry capabilities;
        Extensions::BackendServiceRegistry services;
        const auto policy = Policy();
        PlatformProviderAdmission admission{capabilities, services, policy, HostPlatform(), PlatformServicesHostProfile::HeadlessServer};
        auto audit = std::make_shared<Audit>();
        std::weak_ptr<Audit> code = audit;
        auto published = admission.Commit(Candidate("example.foreign", 1, audit));
        REQUIRE(published.HasValue());
        auto consumer = Consumer(policy);
        auto authority = consumer.Grant({"platform.services.provider"});
        REQUIRE(authority.HasValue());
        std::optional<PlatformProviderRequestLease> request;
        {
            auto backend = admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1);
            REQUIRE(backend.HasValue());
            auto acquired = backend.Value().AcquireRequestLease();
            REQUIRE(acquired.HasValue());
            request.emplace(std::move(acquired).Value());
            Extensions::ExtensionPlatformProviderPublication managerOwner = std::move(published).Value();
            managerOwner.reset();  // Same RAII path as LoadedExtension destruction.
            audit.reset();
            CHECK_FALSE(code.expired());
            CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        }
        std::thread releaseOnForeignThread([lease = std::move(request)]() mutable {
            lease.reset();
        });
        releaseOnForeignThread.join();
        REQUIRE_FALSE(code.expired());
        CHECK(code.lock()->destroyed == 0);
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Complete);
        CHECK(code.expired());
    }

    TEST_CASE("Quarantined retirement survives admission teardown until owner-thread request release",
              "[platform-services][extension][provider][lifetime]") {
        auto audit = std::make_shared<Audit>();
        std::weak_ptr<Audit> code = audit;
        std::optional<PlatformProviderRequestLease> request;
        {
            Extensions::ApplicationCapabilityRegistry capabilities;
            Extensions::BackendServiceRegistry services;
            const auto policy = Policy();
            PlatformProviderAdmission admission{capabilities, services, policy, HostPlatform(),
                                                PlatformServicesHostProfile::HeadlessServer};
            auto publication = admission.Commit(Candidate("example.quarantine", 1, audit));
            REQUIRE(publication.HasValue());
            auto consumer = Consumer(policy);
            auto authority = consumer.Grant({"platform.services.provider"});
            REQUIRE(authority.HasValue());
            {
                auto backend = admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1);
                REQUIRE(backend.HasValue());
                auto acquired = backend.Value().AcquireRequestLease();
                REQUIRE(acquired.HasValue());
                request.emplace(std::move(acquired).Value());
                auto managerOwner = std::move(publication).Value();
                managerOwner.reset();
            }
            audit.reset();
            CHECK_FALSE(code.expired());
            CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        }
        REQUIRE_FALSE(code.expired());
        request.reset();
        CHECK(code.expired());
    }

    TEST_CASE("Foreign-thread revoke defers factory and candidate retirement to the owner",
              "[platform-services][extension][provider][lifetime]") {
        Extensions::ApplicationCapabilityRegistry capabilities;
        Extensions::BackendServiceRegistry services;
        const auto policy = Policy();
        PlatformProviderAdmission admission{capabilities, services, policy, HostPlatform(), PlatformServicesHostProfile::HeadlessServer};
        auto audit = std::make_shared<Audit>();
        std::weak_ptr<Audit> code = audit;
        auto published = admission.Commit(Candidate("example.foreign-revoke", 1, audit));
        REQUIRE(published.HasValue());
        auto consumer = Consumer(policy);
        auto authority = consumer.Grant({"platform.services.provider"});
        REQUIRE(authority.HasValue());
        {
            auto backend = admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1);
            REQUIRE(backend.HasValue());
            auto request = backend.Value().AcquireRequestLease();
            REQUIRE(request.HasValue());
            std::thread revokeOnForeignThread([publication = std::move(published).Value()]() mutable {
                publication.reset();
            });
            revokeOnForeignThread.join();
            audit.reset();
            CHECK_FALSE(code.expired());
            CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        }
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Complete);
        CHECK(code.expired());
    }

    TEST_CASE("Publication rollback guard revokes on every destruction path", "[platform-services][extension][provider]") {
        struct Publication final : Extensions::IExtensionPlatformProviderPublication {
            explicit Publication(bool &revoked) : revoked(revoked) {}

            void Revoke() noexcept override {
                revoked = true;
            }

            bool &revoked;
        };

        bool revoked = false;
        {
            Extensions::ExtensionPlatformProviderPublication publication{new Publication{revoked}};
        }
        CHECK(revoked);
        revoked = false;
        CHECK_THROWS_AS(([&] {
            Extensions::ExtensionPlatformProviderPublication pendingManagerInsertion{new Publication{revoked}};
            throw std::bad_alloc{};  // Simulate allocation failure before manager takes ownership.
        }()),
                        std::bad_alloc);
        CHECK(revoked);
    }

    TEST_CASE("Native provider-only package commits through extension host and drains after unload",
              "[platform-services][extension][provider][abi]") {
        namespace fs = std::filesystem;
        const fs::path root =
            fs::temp_directory_path() / ("horo-provider-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(fs::create_directory(root));

        struct Cleanup final {
            fs::path path;

            ~Cleanup() {
                std::error_code error;
                fs::remove_all(path, error);
            }
        } cleanup{root};

        const fs::path modulePath = root / fs::path{HORO_PLATFORM_PROVIDER_FIXTURE}.filename();
        REQUIRE(fs::copy_file(HORO_PLATFORM_PROVIDER_FIXTURE, modulePath));
        {
            std::ofstream manifest{root / "extension.json"};
            manifest
                << R"({"id":"example.extension","version":"1.0.0","modules":[{"id":"example.module","version":"1.0.0","kind":"native","roles":["backend-capability"],"entry":")"
                << modulePath.filename().generic_string()
                << R"(","requiredCapabilities":["platform.services.provider"]}],"contributions":[{"type":"platform.services.provider","id":"example.provider","module":"example.module"}]})";
            REQUIRE(manifest.good());
        }
        Extensions::ApplicationCapabilityRegistry capabilities;
        Extensions::BackendServiceRegistry services;
        const auto policy = Policy();
        PlatformProviderAdmission admission{capabilities, services, policy, HostPlatform(), PlatformServicesHostProfile::HeadlessServer};
        Extensions::ExtensionManager manager{nullptr,
                                             Extensions::ExtensionHostProfile::Headless,
                                             {"platform.services.provider"},
                                             Horo::Tests::CreateAcceptingArtifactGate(),
                                             {},
                                             [&admission](auto candidate) {
            return admission.Commit(std::move(candidate));
        }};
        auto loaded = manager.LoadExtension(root.string());
        REQUIRE(loaded.HasValue());
        auto consumer = Consumer(policy);
        auto authority = consumer.Grant({"platform.services.provider"});
        REQUIRE(authority.HasValue());
        std::optional<PlatformProviderRequestLease> request;
        {
            auto backend = admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1);
            REQUIRE(backend.HasValue());
            CHECK(backend.Value().Provider() == PlatformProviderId{42});
            auto acquired = backend.Value().AcquireRequestLease();
            REQUIRE(acquired.HasValue());
            request.emplace(std::move(acquired).Value());
            manager.UnloadExtension(loaded.Value());
            CHECK(admission.Create(authority.Value(), Version(1), "example.consumer", "consumer.module", 1).HasError());
            CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        }
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        request.reset();
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Complete);
    }

    TEST_CASE("ABI 1.3 provider operation tail loads and completes through exact host composition",
              "[platform-services][extension][provider][abi]") {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() /
                              ("horo-provider-ops-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        REQUIRE(fs::create_directory(root));

        struct Cleanup final {
            fs::path path;

            ~Cleanup() {
                std::error_code error;
                fs::remove_all(path, error);
            }
        } cleanup{root};

        const fs::path modulePath = root / fs::path{HORO_PLATFORM_PROVIDER_OPERATIONS_FIXTURE}.filename();
        REQUIRE(fs::copy_file(HORO_PLATFORM_PROVIDER_OPERATIONS_FIXTURE, modulePath));
        {
            std::ofstream manifest{root / "extension.json"};
            manifest
                << R"({"id":"example.extension","version":"1.0.0","modules":[{"id":"example.module","version":"1.0.0","kind":"native","roles":["backend-capability"],"entry":")"
                << modulePath.filename().generic_string()
                << R"(","requiredCapabilities":["platform.services.provider"]}],"contributions":[{"type":"platform.services.provider","id":"example.provider","module":"example.module"}]})";
            REQUIRE(manifest.good());
        }
        Extensions::ApplicationCapabilityRegistry capabilities;
        Extensions::BackendServiceRegistry services;
        const auto policy = Policy();
        PlatformProviderAdmission admission{capabilities, services, policy, HostPlatform(), PlatformServicesHostProfile::HeadlessServer};
        Extensions::ExtensionManager manager{nullptr,
                                             Extensions::ExtensionHostProfile::Headless,
                                             {"platform.services.provider"},
                                             Horo::Tests::CreateAcceptingArtifactGate(),
                                             {},
                                             [&admission](auto candidate) {
            return admission.Commit(std::move(candidate));
        }};
        auto loaded = manager.LoadExtension(root.string());
        REQUIRE(loaded.HasValue());
        auto consumer = Consumer(policy);
        auto authority = consumer.Grant({"platform.services.provider"});
        REQUIRE(authority.HasValue());
        PlatformProjectConfigurationCandidate draft{.projectId = "example.project",
                                                    .profile = PlatformServicesHostProfile::HeadlessServer,
                                                    .provider = {.mode = PlatformProviderSelectionMode::ExactProvider,
                                                                 .providerKey = "example.provider"}};
        draft.services[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = PlatformServiceRequirement::Optional;
        PlatformProviderModuleContribution contribution{.module = {"example.module"},
                                                        .providerKey = "example.provider",
                                                        .provider = {42},
                                                        .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                             PlatformServicesBackendInterfaceMinor},
                                                        .allowedProfiles = PlatformServicesHostProfileMask::HeadlessServer};
        contribution.supportedServices[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = true;
        const std::vector contributions{contribution};
        const std::vector trusted{ModuleId{"example.module"}};
        auto configuration = BuildPlatformProjectConfiguration(draft, contributions, trusted);
        REQUIRE(configuration.HasValue());
        auto started = PlatformProviderLifecycleHost::Start(configuration.Value(), admission, {"example.module", "example.provider", 1},
                                                            authority.Value(), Version(1), "example.consumer", "consumer.module", 1);
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        CHECK(host->DispatchCompletions(1) == 1);
        CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
        REQUIRE(host->Close().HasValue());
        manager.UnloadExtension(loaded.Value());
        CHECK(admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Complete);
    }
}  // namespace Horo::PlatformServices::Tests
