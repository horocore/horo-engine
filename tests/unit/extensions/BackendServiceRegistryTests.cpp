#include "BackendServiceReference.h"
#include "Horo/Extensions/BackendServiceRegistry.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        using Fixtures::ArithmeticService;
        using Fixtures::ArithmeticServiceAudit;
        using Fixtures::SumRequest;
        using Fixtures::SumResponse;

        class DrainingService final {
        public:
            struct Audit final {
                std::atomic_bool entered{};
                std::atomic_bool exited{};
                std::atomic_bool shutdownAfterExit{};
            };

            explicit DrainingService(std::shared_ptr<Audit> audit) noexcept : audit_(std::move(audit)) {}

            [[nodiscard]] Result<SumResponse> WaitForCancellation(const SumRequest &, const BackendServiceCallContext &context) {
                audit_->entered.store(true, std::memory_order_release);
                audit_->entered.notify_all();
                while (!context.IsCancellationRequested())
                    std::this_thread::yield();
                audit_->exited.store(true, std::memory_order_release);
                return Result<SumResponse>::Failure(MakeError(ExtensionErrors::BackendServiceCancelled));
            }

            void Shutdown() noexcept {
                audit_->shutdownAfterExit.store(audit_->exited.load(std::memory_order_acquire), std::memory_order_release);
            }

        private:
            std::shared_ptr<Audit> audit_;
        };

        class OrderedShutdownService final {
        public:
            OrderedShutdownService(std::shared_ptr<std::vector<int>> order, const int value) noexcept
                : order_(std::move(order)), value_(value) {}

            void Shutdown() noexcept {
                order_->push_back(value_);
            }

        private:
            std::shared_ptr<std::vector<int>> order_;
            int value_{};
        };

        class NoopService final {
        public:
            void Shutdown() noexcept {}
        };

        struct PrepublicationLifetimeAudit final {
            bool serviceDestroyed{};
            bool codeReleasedAfterService{};
        };

        class DestructionObservedService final {
        public:
            explicit DestructionObservedService(std::shared_ptr<PrepublicationLifetimeAudit> audit) noexcept : audit_(std::move(audit)) {}

            ~DestructionObservedService() {
                audit_->serviceDestroyed = true;
            }

            void Shutdown() noexcept {}

        private:
            std::shared_ptr<PrepublicationLifetimeAudit> audit_;
        };

        class DestructionObservedCode final {
        public:
            explicit DestructionObservedCode(std::shared_ptr<PrepublicationLifetimeAudit> audit) noexcept : audit_(std::move(audit)) {}

            ~DestructionObservedCode() {
                audit_->codeReleasedAfterService = audit_->serviceDestroyed;
            }

        private:
            std::shared_ptr<PrepublicationLifetimeAudit> audit_;
        };

        class OwnerThreadShutdownService final {
        public:
            struct Audit final {
                std::atomic_int shutdownCount{};
                std::thread::id shutdownThread;
            };

            explicit OwnerThreadShutdownService(std::shared_ptr<Audit> audit) noexcept : audit_(std::move(audit)) {}

            void Shutdown() noexcept {
                audit_->shutdownThread = std::this_thread::get_id();
                ++audit_->shutdownCount;
            }

        private:
            std::shared_ptr<Audit> audit_;
        };

        class BlockingShutdownService final {
        public:
            struct Audit final {
                std::atomic_bool entered{};
                std::atomic_bool release{};
                std::atomic_int count{};
            };

            explicit BlockingShutdownService(std::shared_ptr<Audit> audit) noexcept : audit_(std::move(audit)) {}

            void Shutdown() noexcept {
                audit_->entered.store(true, std::memory_order_release);
                audit_->entered.notify_all();
                audit_->release.wait(false, std::memory_order_acquire);
                ++audit_->count;
            }

        private:
            std::shared_ptr<Audit> audit_;
        };

        class UnresponsiveService final {
        public:
            struct Audit final {
                std::atomic_bool entered{};
                std::atomic_bool release{};
                std::atomic_int shutdownCount{};
            };

            explicit UnresponsiveService(std::shared_ptr<Audit> audit) noexcept : audit_(std::move(audit)) {}

            [[nodiscard]] Result<SumResponse> IgnoreCancellation(const SumRequest &, const BackendServiceCallContext &) {
                audit_->entered.store(true, std::memory_order_release);
                audit_->entered.notify_all();
                audit_->release.wait(false, std::memory_order_acquire);
                return Result<SumResponse>::Success({42});
            }

            void Shutdown() noexcept {
                ++audit_->shutdownCount;
            }

        private:
            std::shared_ptr<Audit> audit_;
        };

        struct NestedCallControl final {
            std::function<Result<SumResponse>()> invokeInner;
            std::atomic_int shutdownCount{};
            std::shared_ptr<std::vector<int>> shutdownOrder;
        };

        class NestedSelfStoppingService final {
        public:
            NestedSelfStoppingService(BackendServiceRegistry &registry, std::shared_ptr<NestedCallControl> control) noexcept
                : registry_(registry), control_(std::move(control)) {}

            [[nodiscard]] Result<SumResponse> Outer(const SumRequest &, const BackendServiceCallContext &) {
                return control_->invokeInner();
            }

            [[nodiscard]] Result<SumResponse> Inner(const SumRequest &, const BackendServiceCallContext &) {
                static_cast<void>(registry_.BeginShutdown());
                return Result<SumResponse>::Success({42});
            }

            void Shutdown() noexcept {
                ++control_->shutdownCount;
                control_->shutdownOrder->push_back(2);
            }

        private:
            BackendServiceRegistry &registry_;
            std::shared_ptr<NestedCallControl> control_;
        };

        [[nodiscard]] BackendServiceDescriptor Descriptor(const BackendServiceThreadRule threadRule = BackendServiceThreadRule::AnyThread) {
            return {.serviceId = {"com.example.math"},
                    .contractId = {"com.example.math.v1"},
                    .capability = {"com.example.math.use"},
                    .version = {1, 0, 0},
                    .provider = {.moduleId = "com.example.math-module", .providerId = "com.example.math-provider", .generation = 7},
                    .threadRule = threadRule};
        }

        [[nodiscard]] ExtensionModuleManifest ImportConsumer(const bool required = true) {
            return {.id = "com.example.consumer.backend",
                    .version = "1.0.0",
                    .kind = "native",
                    .roles = {ExtensionModuleRole::BackendCapability},
                    .imports = {{.id = "com.example.math.import",
                                 .service = "com.example.math",
                                 .contract = "com.example.math.v1",
                                 .minimumVersion = "1.0.0",
                                 .required = required}}};
        }

        [[nodiscard]] ExtensionHostEnvironment HeadlessHost() {
            return {.profile = ExtensionHostProfile::Headless,
                    .platform = ExtensionHostPlatform::Linux,
                    .architecture = ExtensionHostArchitecture::X86_64,
                    .buildProfile = ExtensionBuildProfile::Debug,
                    .engineVersion = "0.1.0",
                    .abiMajor = 1};
        }

        [[nodiscard]] ResolvedExtensionServiceImport ResolveSingleImport(ExtensionManifest manifest) {
            auto resolved = ResolveExtensionModules(manifest, HeadlessHost());
            REQUIRE(resolved.HasValue());
            REQUIRE(resolved.Value().serviceImports.size() == 1);
            return resolved.Value().serviceImports.front();
        }

        [[nodiscard]] ResolvedExtensionServiceImport ResolvedImport(const std::string &providerVersion = "1.0.0") {
            ExtensionModuleManifest provider{.id = "com.example.math-module",
                                             .version = "1.0.0",
                                             .kind = "native",
                                             .roles = {ExtensionModuleRole::BackendCapability},
                                             .exports = {{.id = "com.example.math",
                                                          .contract = "com.example.math.v1",
                                                          .version = providerVersion}}};
            ExtensionManifest manifest;
            manifest.id = "com.example.consumer";
            manifest.modules = {ImportConsumer(), std::move(provider)};
            return ResolveSingleImport(std::move(manifest));
        }

        [[nodiscard]] ResolvedExtensionServiceImport UnavailableImport() {
            ExtensionManifest manifest;
            manifest.id = "com.example.consumer";
            manifest.modules = {ImportConsumer(false)};
            return ResolveSingleImport(std::move(manifest));
        }

        [[nodiscard]] ExtensionCapabilityAdmission Admission(std::string extensionId = "com.example.consumer",
                                                             std::string moduleId = "com.example.consumer.backend",
                                                             const std::uint64_t generation = 3) {
            ExtensionAdmissionPolicy policy{.revision = 2, .availableCapabilities = {{"com.example.math.use"}}};
            ExtensionAdmissionRequest request{.extensionId = std::move(extensionId),
                                              .moduleId = std::move(moduleId),
                                              .activationGeneration = generation,
                                              .capabilities = {{{"com.example.math.use"}, {}}}};
            auto admission = ExtensionCapabilityAdmission::Evaluate(request, policy);
            REQUIRE(admission.HasValue());
            return std::move(admission).Value();
        }

        [[nodiscard]] ApplicationCapabilityProviderLease CapabilityLease(ApplicationCapabilityRegistry &registry,
                                                                         ExtensionCapabilityAdmission &admission) {
            auto handle = admission.Grant({"com.example.math.use"});
            REQUIRE(handle.HasValue());
            const ExtensionActivationIdentity &consumer = handle.Value().Activation();
            auto lease = registry.Resolve(handle.Value(), {{1, 0, 0}, {1, 0, 0}}, consumer.ExtensionId(), consumer.ModuleId(),
                                          consumer.Generation());
            REQUIRE(lease.HasValue());
            return std::move(lease).Value();
        }

        void RequireErrorCode(const auto &result, const std::string &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }

        void RequireAttributedImportError(const auto &result, const std::string &code) {
            RequireErrorCode(result, code);
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring("com.example.math.import"));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring("com.example.consumer"));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring("com.example.consumer.backend"));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring("com.example.math"));
            CHECK_THAT(result.ErrorValue().message, Catch::Matchers::ContainsSubstring("com.example.math-module"));
        }

        [[nodiscard]] BackendServiceCodeLease TestCodeLease() {
            return BackendServiceCodeLease::Retain(std::make_shared<int>());
        }

        template <typename Service>
        [[nodiscard]] BackendServiceRegistration RegisterService(BackendServiceRegistry &registry, BackendServiceDescriptor descriptor,
                                                                 std::unique_ptr<Service> service) {
            auto registered = registry.Register(std::move(descriptor), std::move(service), TestCodeLease());
            REQUIRE(registered.HasValue());
            return std::move(registered).Value();
        }

        template <typename Service>
        [[nodiscard]] BackendServiceCall<Service> ResolveService(BackendServiceRegistry &services,
                                                                 ApplicationCapabilityRegistry &capabilities,
                                                                 ExtensionCapabilityAdmission &admission) {
            auto call = services.Resolve<Service>(CapabilityLease(capabilities, admission), {"com.example.math"}, {"com.example.math.v1"});
            REQUIRE(call.HasValue());
            return std::move(call).Value();
        }

        template <typename Service, typename Operation>
        [[nodiscard]] std::future<Result<SumResponse>> StartAsyncCall(BackendServiceRegistry &services,
                                                                      ApplicationCapabilityRegistry &capabilities,
                                                                      ExtensionCapabilityAdmission &admission, Operation operation) {
            auto call = ResolveService<Service>(services, capabilities, admission);
            return std::async(std::launch::async, [call = std::move(call), operation]() mutable {
                return std::move(call).Invoke(operation, SumRequest{});
            });
        }

        struct Fixture final {
            Fixture() {
                auto published = capabilities.Register(
                    {{"com.example.math.use"},
                     {1, 0, 0},
                     {.moduleId = "com.example.math-module", .providerId = "com.example.math-provider", .generation = 7}});
                REQUIRE(published.HasValue());
                capabilityRegistration.emplace(std::move(published).Value());
            }

            ApplicationCapabilityRegistry capabilities;
            std::optional<ApplicationCapabilityProviderRegistration> capabilityRegistration;
            ExtensionCapabilityAdmission admission = Admission();
            BackendServiceRegistry services;
        };

        template <typename ConfigureDescriptor>
        void RequireProviderIdentityMismatch(Fixture &fixture, ConfigureDescriptor configureDescriptor) {
            BackendServiceDescriptor descriptor = Descriptor();
            configureDescriptor(descriptor);
            auto registration = RegisterService(fixture.services, std::move(descriptor),
                                                std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()));
            const auto rejected = fixture.services.BindImport(CapabilityLease(fixture.capabilities, fixture.admission), ResolvedImport());
            RequireAttributedImportError(rejected, "backend_service_contract_mismatch");
        }
    }  // namespace

    TEST_CASE("Backend-only service performs typed attributed calls without presentation dependencies", "[Extensions][BackendService]") {
        STATIC_REQUIRE_FALSE(std::is_aggregate_v<ResolvedExtensionServiceImport>);
        STATIC_REQUIRE_FALSE(std::is_default_constructible_v<ResolvedExtensionServiceImport>);
        STATIC_REQUIRE_FALSE(std::is_default_constructible_v<BackendServiceImportBinding>);
        STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<BackendServiceImportBinding>);

        Fixture fixture;
        auto audit = std::make_shared<ArithmeticServiceAudit>();
        auto registration = RegisterService(fixture.services, Descriptor(), std::make_unique<ArithmeticService>(audit));
        auto bound = fixture.services.BindImport(CapabilityLease(fixture.capabilities, fixture.admission), ResolvedImport());
        REQUIRE(bound.HasValue());
        auto call = fixture.services.ResolveImported<ArithmeticService>(std::move(bound).Value());
        REQUIRE(call.HasValue());
        auto response = std::move(call).Value().Invoke(&ArithmeticService::Add, SumRequest{20, 22});
        REQUIRE(response.HasValue());
        CHECK(response.Value().value == 42);
        CHECK(audit->observedProvider == "com.example.math-provider");
    }

    TEST_CASE("Extension retirement revokes a real backend service and drains its admitted call",
              "[Extensions][BackendService][Retirement]") {
        Fixture fixture;
        auto audit = std::make_shared<DrainingService::Audit>();
        auto registration = RegisterService(fixture.services, Descriptor(), std::make_unique<DrainingService>(audit));
        auto retirement =
            std::make_shared<ExtensionRetirement>("com.example.extension", std::vector<std::string>{"com.example.math-module"});
        auto code = std::make_shared<int>(0);
        REQUIRE(retirement->BindModuleCode("com.example.math-module", code));
        REQUIRE(registration.AttachRetirement(retirement));
        CHECK_FALSE(registration.AttachRetirement(retirement));
        const auto active = retirement->Inspect();
        REQUIRE(active.outstanding.size() == 1);
        CHECK(active.outstanding.front().kind == ExtensionLeaseKind::HostService);
        CHECK(active.outstanding.front().subject == "com.example.math");
        auto call = StartAsyncCall<DrainingService>(fixture.services, fixture.capabilities, fixture.admission,
                                                    &DrainingService::WaitForCancellation);
        audit->entered.wait(false, std::memory_order_acquire);
        retirement->CloseAdmission();
        REQUIRE(call.get().HasError());
        CHECK(audit->shutdownAfterExit.load(std::memory_order_acquire));
        CHECK_FALSE(registration.IsRegistered());
        const auto rejected = fixture.services.Resolve<DrainingService>(CapabilityLease(fixture.capabilities, fixture.admission),
                                                                        {"com.example.math"}, {"com.example.math.v1"});
        CHECK(rejected.HasError());
        (void)registration.Reset();
        CHECK(retirement->IsDrained());
    }

    TEST_CASE("Backend service preserves typed provider errors and caller cancellation", "[Extensions][BackendService]") {
        Fixture fixture;
        auto registration = RegisterService(fixture.services, Descriptor(),
                                            std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()));
        auto failure = ResolveService<ArithmeticService>(fixture.services, fixture.capabilities, fixture.admission);
        auto failed = std::move(failure).Invoke(&ArithmeticService::Fail, SumRequest{});
        RequireErrorCode(failed, "backend_service_invocation_failed");
        REQUIRE(failed.ErrorValue().cause);
        CHECK(failed.ErrorValue().cause.Get()->code.Value() == "contribution_rejected");

        CancellationSource cancellation;
        cancellation.RequestCancellation();
        auto cancelled = ResolveService<ArithmeticService>(fixture.services, fixture.capabilities, fixture.admission);
        RequireErrorCode(std::move(cancelled).Invoke(&ArithmeticService::Add, SumRequest{}, cancellation.Token()),
                         "backend_service_cancelled");
    }

    TEST_CASE("Backend service rejects mismatched contracts types and threads before provider invocation", "[Extensions][BackendService]") {
        Fixture fixture;
        auto audit = std::make_shared<ArithmeticServiceAudit>();
        auto registration = RegisterService(fixture.services, Descriptor(BackendServiceThreadRule::ProviderOwnerThread),
                                            std::make_unique<ArithmeticService>(audit));
        RequireErrorCode(fixture.services.Resolve<ArithmeticService>(CapabilityLease(fixture.capabilities, fixture.admission),
                                                                     {"com.example.math"}, {"com.example.other.v1"}),
                         "backend_service_contract_mismatch");
        RequireErrorCode(fixture.services.Resolve<NoopService>(CapabilityLease(fixture.capabilities, fixture.admission),
                                                               {"com.example.math"}, {"com.example.math.v1"}),
                         "backend_service_type_mismatch");
        auto call = ResolveService<ArithmeticService>(fixture.services, fixture.capabilities, fixture.admission);
        auto wrongThread = std::async(std::launch::async, [call = std::move(call)]() mutable {
            return std::move(call).Invoke(&ArithmeticService::Add, SumRequest{});
        });
        RequireErrorCode(wrongThread.get(), "backend_service_thread_violation");
        CHECK(audit->observedProvider.empty());
    }

    TEST_CASE("Backend import binding rejects consumer provider version and generation mismatches",
              "[Extensions][BackendService][Imports]") {
        Fixture fixture;

        SECTION("optional import unavailable") {
            const auto rejected =
                fixture.services.BindImport(CapabilityLease(fixture.capabilities, fixture.admission), UnavailableImport());
            RequireErrorCode(rejected, "backend_service_unavailable");
            CHECK_THAT(rejected.ErrorValue().message, Catch::Matchers::ContainsSubstring("com.example.math.import"));
            CHECK_THAT(rejected.ErrorValue().message, Catch::Matchers::ContainsSubstring("provider-module='<unavailable>'"));
        }

        SECTION("consumer activation") {
            auto registration = RegisterService(fixture.services, Descriptor(),
                                                std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()));
            auto otherAdmission = Admission("com.example.other", "com.example.other.backend", 11);
            const auto rejected = fixture.services.BindImport(CapabilityLease(fixture.capabilities, otherAdmission), ResolvedImport());
            RequireAttributedImportError(rejected, "backend_service_contract_mismatch");
            CHECK_THAT(rejected.ErrorValue().message, Catch::Matchers::ContainsSubstring("generation 11"));
        }

        SECTION("provider semantic version") {
            auto registration = RegisterService(fixture.services, Descriptor(),
                                                std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()));
            const auto rejected =
                fixture.services.BindImport(CapabilityLease(fixture.capabilities, fixture.admission), ResolvedImport("1.1.0"));
            RequireAttributedImportError(rejected, "backend_service_contract_mismatch");
            CHECK_THAT(rejected.ErrorValue().message, Catch::Matchers::ContainsSubstring("version is 1.0.0"));
        }

        SECTION("provider module mapping") {
            RequireProviderIdentityMismatch(fixture, [](BackendServiceDescriptor &descriptor) {
                descriptor.provider.moduleId = "com.example.other-module";
            });
        }

        SECTION("provider generation") {
            RequireProviderIdentityMismatch(fixture, [](BackendServiceDescriptor &descriptor) {
                descriptor.provider.generation = 8;
            });
        }
    }

    TEST_CASE("Backend import binding cannot call after consumer or provider replacement", "[Extensions][BackendService][Imports]") {
        Fixture fixture;
        BackendServiceRegistration registration =
            RegisterService(fixture.services, Descriptor(),
                            std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()));
        auto bound = fixture.services.BindImport(CapabilityLease(fixture.capabilities, fixture.admission), ResolvedImport());
        REQUIRE(bound.HasValue());

        SECTION("consumer admission revoked") {
            fixture.admission.Revoke();
            RequireAttributedImportError(fixture.services.ResolveImported<ArithmeticService>(std::move(bound).Value()),
                                         "backend_service_unavailable");
        }

        SECTION("capability provider unpublished") {
            fixture.capabilityRegistration->Reset();
            RequireAttributedImportError(fixture.services.ResolveImported<ArithmeticService>(std::move(bound).Value()),
                                         "backend_service_unavailable");
        }

        SECTION("backend provider generation replaced") {
            CHECK(registration.Reset() == BackendServiceRetirementDisposition::ShutdownComplete);
            auto replacementDescriptor = Descriptor();
            replacementDescriptor.provider.generation = 8;
            auto replacement = RegisterService(fixture.services, std::move(replacementDescriptor),
                                               std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()));
            RequireAttributedImportError(fixture.services.ResolveImported<ArithmeticService>(std::move(bound).Value()),
                                         "backend_service_unavailable");
        }

        SECTION("typed adapter mismatch") {
            RequireAttributedImportError(fixture.services.ResolveImported<NoopService>(std::move(bound).Value()),
                                         "backend_service_type_mismatch");
        }
    }

    TEST_CASE("Backend service revocation cancels and drains work before shutdown", "[Extensions][BackendService]") {
        Fixture fixture;
        auto audit = std::make_shared<DrainingService::Audit>();
        BackendServiceRegistration registration = RegisterService(fixture.services, Descriptor(), std::make_unique<DrainingService>(audit));
        auto work = StartAsyncCall<DrainingService>(fixture.services, fixture.capabilities, fixture.admission,
                                                    &DrainingService::WaitForCancellation);
        audit->entered.wait(false, std::memory_order_acquire);
        CHECK(registration.Reset() == BackendServiceRetirementDisposition::ShutdownComplete);
        CHECK(audit->exited.load(std::memory_order_acquire));
        CHECK(audit->shutdownAfterExit.load(std::memory_order_acquire));
        RequireErrorCode(work.get(), "backend_service_cancelled");
        CHECK_FALSE(registration.IsRegistered());
        RequireErrorCode(fixture.services.Resolve<DrainingService>(CapabilityLease(fixture.capabilities, fixture.admission),
                                                                   {"com.example.math"}, {"com.example.math.v1"}),
                         "backend_service_unavailable");
    }

    TEST_CASE("Backend service owner-thread shutdown is finalized on the provider thread", "[Extensions][BackendService]") {
        Fixture fixture;
        const std::thread::id ownerThread = std::this_thread::get_id();
        auto audit = std::make_shared<OwnerThreadShutdownService::Audit>();
        BackendServiceRegistration registration =
            RegisterService(fixture.services, Descriptor(BackendServiceThreadRule::ProviderOwnerThread),
                            std::make_unique<OwnerThreadShutdownService>(audit));

        auto wrongThread = std::async(std::launch::async, [&registration] {
            return registration.Reset();
        });
        const auto retired = wrongThread.get();
        CHECK(retired == BackendServiceRetirementDisposition::OwnerThreadFinalizationRequired);
        CHECK_FALSE(registration.IsRegistered());
        CHECK(audit->shutdownCount.load() == 0);

        const auto finalized = fixture.services.FinalizeRetiredOnOwnerThread();
        CHECK(finalized == BackendServiceRetirementDisposition::ShutdownComplete);
        CHECK(audit->shutdownCount.load() == 1);
        CHECK(audit->shutdownThread == ownerThread);
    }

    TEST_CASE("Backend service registry validates publication and shuts down terminally", "[Extensions][BackendService]") {
        BackendServiceRegistry services;
        auto invalid = Descriptor();
        invalid.provider.generation = 0;
        auto lifetimeAudit = std::make_shared<PrepublicationLifetimeAudit>();
        auto codeOwner = std::make_shared<DestructionObservedCode>(lifetimeAudit);
        auto codeLease = BackendServiceCodeLease::Retain(codeOwner);
        codeOwner.reset();
        RequireErrorCode(services.Register(std::move(invalid), std::make_unique<DestructionObservedService>(lifetimeAudit),
                                           std::move(codeLease)),
                         "backend_service_invalid");
        CHECK(lifetimeAudit->serviceDestroyed);
        CHECK(lifetimeAudit->codeReleasedAfterService);
        RequireErrorCode(services.Register(Descriptor(), std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()),
                                           BackendServiceCodeLease::Retain(std::shared_ptr<int>{})),
                         "backend_service_invalid");
        auto audit = std::make_shared<ArithmeticServiceAudit>();
        auto registration = RegisterService(services, Descriptor(), std::make_unique<ArithmeticService>(audit));
        RequireErrorCode(services.Register(Descriptor(), std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()),
                                           TestCodeLease()),
                         "backend_service_duplicate");
        CHECK(services.BeginShutdown() == BackendServiceRetirementDisposition::ShutdownComplete);
        CHECK(services.IsShutdown());
        CHECK(audit->shutdownCount.load() == 1);
        CHECK_FALSE(registration.IsRegistered());
        RequireErrorCode(services.Register(Descriptor(), std::make_unique<ArithmeticService>(std::make_shared<ArithmeticServiceAudit>()),
                                           TestCodeLease()),
                         "backend_service_shutdown");
    }

    TEST_CASE("Backend service registry shuts services down in reverse registration order", "[Extensions][BackendService]") {
        BackendServiceRegistry services;
        auto order = std::make_shared<std::vector<int>>();
        std::vector<BackendServiceRegistration> registrations;
        for (int index = 1; index <= 3; ++index) {
            auto descriptor = Descriptor();
            descriptor.serviceId.value += std::to_string(index);
            registrations.push_back(
                RegisterService(services, std::move(descriptor), std::make_unique<OrderedShutdownService>(order, index)));
        }

        CHECK(services.BeginShutdown() == BackendServiceRetirementDisposition::ShutdownComplete);
        CHECK(*order == std::vector<int>{3, 2, 1});
    }

    TEST_CASE("Backend service registry enforces its publication bound", "[Extensions][BackendService]") {
        BackendServiceRegistry services;
        std::vector<BackendServiceRegistration> registrations;
        registrations.reserve(BackendServiceRegistry::MaximumServices);
        for (std::size_t index = 0; index < BackendServiceRegistry::MaximumServices; ++index) {
            auto descriptor = Descriptor();
            descriptor.serviceId.value = "com.example.service" + std::to_string(index);
            registrations.push_back(RegisterService(services, std::move(descriptor), std::make_unique<NoopService>()));
        }
        auto overflow = Descriptor();
        overflow.serviceId.value = "com.example.overflow";
        RequireErrorCode(services.Register(std::move(overflow), std::make_unique<NoopService>(), TestCodeLease()),
                         "backend_service_capacity_exceeded");
    }

    TEST_CASE("Backend service defers self-initiated shutdown until its active call exits", "[Extensions][BackendService]") {
        Fixture fixture;
        auto control = std::make_shared<NestedCallControl>();
        control->shutdownOrder = std::make_shared<std::vector<int>>();
        auto registration =
            RegisterService(fixture.services, Descriptor(), std::make_unique<NestedSelfStoppingService>(fixture.services, control));
        auto call = ResolveService<NestedSelfStoppingService>(fixture.services, fixture.capabilities, fixture.admission);

        auto stopped = std::move(call).Invoke(&NestedSelfStoppingService::Inner, SumRequest{});
        RequireErrorCode(stopped, "backend_service_cancelled");
        CHECK(control->shutdownCount.load() == 1);
        CHECK(fixture.services.IsShutdown());
    }

    TEST_CASE("Backend service retains an unresponsive provider and requires restart after its drain deadline",
              "[.RestartQuarantine][Extensions][BackendService]") {
        std::weak_ptr<int> codeLifetime;
        {
            ApplicationCapabilityRegistry capabilities;
            auto capabilityRegistration = capabilities.Register(
                {{"com.example.math.use"},
                 {1, 0, 0},
                 {.moduleId = "com.example.math-module", .providerId = "com.example.math-provider", .generation = 7}});
            REQUIRE(capabilityRegistration.HasValue());
            ExtensionCapabilityAdmission admission = Admission();
            BackendServiceRegistry services({.drainDeadline = std::chrono::milliseconds{1}});
            auto audit = std::make_shared<UnresponsiveService::Audit>();
            auto codeOwner = std::make_shared<int>();
            codeLifetime = codeOwner;
            auto published =
                services.Register(Descriptor(), std::make_unique<UnresponsiveService>(audit), BackendServiceCodeLease::Retain(codeOwner));
            REQUIRE(published.HasValue());
            BackendServiceRegistration registration = std::move(published).Value();
            codeOwner.reset();
            auto work = StartAsyncCall<UnresponsiveService>(services, capabilities, admission, &UnresponsiveService::IgnoreCancellation);
            audit->entered.wait(false, std::memory_order_acquire);

            const auto retired = registration.Reset();
            CHECK(retired == BackendServiceRetirementDisposition::RestartRequired);
            CHECK_FALSE(registration.IsRegistered());
            CHECK(audit->shutdownCount.load() == 0);

            audit->release.store(true, std::memory_order_release);
            audit->release.notify_all();
            RequireErrorCode(work.get(), "backend_service_cancelled");
            const auto finalized = services.FinalizeRetiredOnOwnerThread();
            CHECK(finalized == BackendServiceRetirementDisposition::RestartRequired);
            CHECK(audit->shutdownCount.load() == 0);
            CHECK_FALSE(codeLifetime.expired());
        }
        CHECK_FALSE(codeLifetime.expired());
    }

    TEST_CASE("Backend service finalization is not complete before Shutdown returns", "[.RestartQuarantine][Extensions][BackendService]") {
        BackendServiceRegistry services({.drainDeadline = std::chrono::milliseconds{1}});
        auto audit = std::make_shared<BlockingShutdownService::Audit>();
        BackendServiceRegistration registration = RegisterService(services, Descriptor(), std::make_unique<BlockingShutdownService>(audit));
        auto firstRetirement = std::async(std::launch::async, [&registration] {
            return registration.Reset();
        });
        audit->entered.wait(false, std::memory_order_acquire);

        const auto concurrentRetirement = services.BeginShutdown();
        CHECK(concurrentRetirement == BackendServiceRetirementDisposition::RestartRequired);
        CHECK(audit->count.load() == 0);

        audit->release.store(true, std::memory_order_release);
        audit->release.notify_all();
        const auto completedCallback = firstRetirement.get();
        CHECK(completedCallback == BackendServiceRetirementDisposition::RestartRequired);
        CHECK(audit->count.load() == 1);
    }

    TEST_CASE("Backend service nested re-entrant shutdown finalizes exactly once after the outermost call",
              "[Extensions][BackendService]") {
        Fixture fixture;
        auto control = std::make_shared<NestedCallControl>();
        auto shutdownOrder = std::make_shared<std::vector<int>>();
        control->shutdownOrder = shutdownOrder;
        auto earlierDescriptor = Descriptor();
        earlierDescriptor.serviceId.value = "com.example.earlier";
        auto earlier =
            RegisterService(fixture.services, std::move(earlierDescriptor), std::make_unique<OrderedShutdownService>(shutdownOrder, 1));
        auto registration =
            RegisterService(fixture.services, Descriptor(), std::make_unique<NestedSelfStoppingService>(fixture.services, control));
        control->invokeInner = [&fixture] {
            auto inner = fixture.services.Resolve<NestedSelfStoppingService>(CapabilityLease(fixture.capabilities, fixture.admission),
                                                                             {"com.example.math"}, {"com.example.math.v1"});
            if (inner.HasError())
                return Result<SumResponse>::Failure(inner.ErrorValue());
            return std::move(inner).Value().Invoke(&NestedSelfStoppingService::Inner, SumRequest{});
        };
        auto outer = ResolveService<NestedSelfStoppingService>(fixture.services, fixture.capabilities, fixture.admission);

        RequireErrorCode(std::move(outer).Invoke(&NestedSelfStoppingService::Outer, SumRequest{}), "backend_service_cancelled");
        CHECK(control->shutdownCount.load() == 1);
        CHECK(*shutdownOrder == std::vector<int>{2, 1});
        CHECK(fixture.services.BeginShutdown() == BackendServiceRetirementDisposition::ShutdownComplete);
        CHECK(control->shutdownCount.load() == 1);
    }
}  // namespace Horo::Extensions::Tests
