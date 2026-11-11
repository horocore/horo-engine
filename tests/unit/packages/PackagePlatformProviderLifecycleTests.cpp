#include "../../fixtures/extensions/PlatformPackageFixture.h"
#include "Horo/PlatformServices/PlatformProviderAdmission.h"
#include "PackageLifecycleTestSupport.h"

#include <array>
#include <utility>

namespace Horo::Packages::Tests {
    using namespace Support;
    using namespace PlatformServices;

    namespace {
        constexpr std::string_view ProviderCapability = "platform.services.provider";

        /** @brief Keeps audit data readable after unloading without retaining native code. */
        struct ImageAudit final {
            HoroPackageProviderConfigure configure{};
            HoroPackageProviderInspect inspect{};
            HoroPackageProviderAudit final{};
            std::filesystem::path root;
            bool released{};

            [[nodiscard]] HoroPackageProviderAudit Snapshot() const {
                auto result = final;
                if (!released)
                    inspect(&result);
                return result;
            }
        };

        /** @brief Records native-resource and module-unload evidence immediately before releasing the image. */
        class AuditedImage final : public Platform::DynamicLibrary {
        public:
            AuditedImage(std::unique_ptr<Platform::DynamicLibrary> image, std::shared_ptr<ImageAudit> audit)
                : image_(std::move(image)), audit_(std::move(audit)) {}

            ~AuditedImage() override {
                audit_->inspect(&audit_->final);
                audit_->released = true;
                audit_->configure = nullptr;
                audit_->inspect = nullptr;
            }

            void *GetSymbol(const std::string_view name) const noexcept override {
                return image_->GetSymbol(name);
            }

        private:
            std::unique_ptr<Platform::DynamicLibrary> image_;
            std::shared_ptr<ImageAudit> audit_;
        };

        /** @brief Selects the actual test host without inferring provider availability. */
        [[nodiscard]] constexpr Extensions::ExtensionHostPlatform HostPlatform() noexcept {
#if defined(_WIN32)
            return Extensions::ExtensionHostPlatform::Windows;
#elif defined(__APPLE__)
            return Extensions::ExtensionHostPlatform::MacOS;
#else
            return Extensions::ExtensionHostPlatform::Linux;
#endif
        }

        /** @brief Grants the neutral provider capability without private permissions. */
        [[nodiscard]] Extensions::ExtensionAdmissionPolicy ProviderPolicy() {
            return {.revision = 1, .availableCapabilities = {{std::string{ProviderCapability}}}};
        }

        /** @brief One application-owned registry with monotonically increasing provider generations. */
        struct ProviderRealm final {
            Extensions::ApplicationCapabilityRegistry capabilities;
            Extensions::BackendServiceRegistry services;
            PlatformProviderAdmission admission;

            explicit ProviderRealm(Extensions::ExtensionAdmissionPolicy policy)
                : admission(capabilities, services, std::move(policy), HostPlatform(), PlatformServicesHostProfile::HeadlessServer) {}
        };

        /** @brief Declares exact backend-only package metadata or a forbidden mixed contribution batch. */
        [[nodiscard]] nlohmann::json ProviderDescriptor(const bool mixed = false) {
            const auto library = std::filesystem::path{HORO_PACKAGE_PROVIDER_FIXTURE};
            auto descriptor = NativeDescriptor("com.example.transaction", "module" + library.extension().string(), true);
            descriptor["modules"][0]["requiredCapabilities"] = {ProviderCapability};
            descriptor["contributions"] = {
                {{"type", ProviderCapability}, {"id", "example.provider"}, {"module", "com.example.transaction.native"}}};
            if (mixed)
                descriptor["contributions"].push_back(
                    {{"type", "asset.importer"}, {"id", "fixture.importer"}, {"module", "com.example.transaction.native"}});
            return descriptor;
        }

        /** @brief Builds a required-service configuration bound to the fixture's exact Horo provider. */
        [[nodiscard]] PlatformProjectConfiguration ProviderConfiguration() {
            PlatformProjectConfigurationCandidate draft{.projectId = "example.project",
                                                        .profile = PlatformServicesHostProfile::HeadlessServer,
                                                        .provider = {.mode = PlatformProviderSelectionMode::ExactProvider,
                                                                     .providerKey = "example.provider"}};
            draft.services[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = PlatformServiceRequirement::Required;
            PlatformProviderModuleContribution contribution{.module = {"com.example.transaction.native"},
                                                            .providerKey = "example.provider",
                                                            .provider = {42},
                                                            .interfaceVersion = {PlatformServicesBackendInterfaceMajor,
                                                                                 PlatformServicesBackendInterfaceMinor},
                                                            .allowedProfiles = PlatformServicesHostProfileMask::HeadlessServer};
            contribution.supportedServices[static_cast<std::size_t>(PlatformServiceKind::Achievements)] = true;
            const std::array contributions{contribution};
            const std::array trusted{ModuleId{"com.example.transaction.native"}};
            auto result = BuildPlatformProjectConfiguration(draft, contributions, trusted);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        struct ProviderFixture final {
            Fixture base;
            std::vector<std::shared_ptr<ImageAudit>> images;
            std::shared_ptr<ProviderRealm> realm;
            std::vector<std::uint64_t> generations;
            std::shared_ptr<PackageModuleRetirementQueue> retirements{std::make_shared<PackageModuleRetirementQueue>(16)};
            std::uint32_t flags{};
            std::uint32_t loadFlags{};
            bool cancelAfterCommit{};
            bool staleAfterCommit{};
            bool roleAware{};
            bool tooling{};
            unsigned toolingLoads{};
            Extensions::ExtensionAdmissionPolicy rootPolicy{ProviderPolicy()};
            PlatformProjectConfiguration configuration{ProviderConfiguration()};
            std::optional<Extensions::ExtensionCapabilityAdmission> consumer;
            std::optional<Extensions::ExtensionCapabilityHandle> authority;

            explicit ProviderFixture(const bool useRoles = false, const bool exposeProvider = true) : roleAware(useRoles) {
                base.service.reset();
                base.trust.capabilities = {{std::string{ProviderCapability}}};
                PackageLifecycleConfiguration policy{.temporaryRoot = std::filesystem::canonical(base.temporary.Path()),
                                                     .capabilities = {std::string{ProviderCapability}},
                                                     .artifactGate = ::Horo::Tests::CreateAcceptingArtifactGate()};
                policy.libraryLoader = [this](const std::string &path) -> Result<std::unique_ptr<Platform::DynamicLibrary>> {
                    auto image = Platform::LoadDynamicLibrary(path);
                    if (image.HasError())
                        return image;
                    if (std::filesystem::path{path}.filename().string().starts_with("tool")) {
                        ++toolingLoads;
                        return image;
                    }
                    auto audit = std::make_shared<ImageAudit>();
                    audit->configure =
                        reinterpret_cast<HoroPackageProviderConfigure>(image.Value()->GetSymbol("horo_package_provider_configure"));
                    audit->inspect =
                        reinterpret_cast<HoroPackageProviderInspect>(image.Value()->GetSymbol("horo_package_provider_inspect"));
                    REQUIRE(audit->configure);
                    REQUIRE(audit->inspect);
                    audit->configure(loadFlags);
                    audit->root = std::filesystem::path{path}.parent_path();
                    images.push_back(audit);
                    return Result<std::unique_ptr<Platform::DynamicLibrary>>::Success(
                        std::make_unique<AuditedImage>(std::move(image).Value(), std::move(audit)));
                };
                if (exposeProvider)
                    policy.platformProviderCommit = [this](Extensions::ExtensionPlatformProviderCandidate candidate) {
                        if (!realm)
                            realm = std::make_shared<ProviderRealm>(rootPolicy);
                        auto published = realm->admission.Commit(std::move(candidate));
                        if (published.HasValue()) {
                            generations.push_back(generations.size() + 1);
                            if (cancelAfterCommit)
                                base.cancellation.RequestCancellation();
                            if (staleAfterCommit)
                                base.Install(
                                    Graph(HORO_PACKAGE_PROVIDER_FIXTURE, true, true, "com.example.transaction", ProviderDescriptor()));
                        }
                        return published;
                    };
                if (roleAware) {
                    policy.moduleHosts = {std::make_shared<NativePackageModuleHost>(policy)};
                    policy.moduleRetirements = retirements;
                }
                auto service = PackageLifecycleService::Create(base.install, base.trust, std::move(policy));
                REQUIRE(service.HasValue());
                base.service = std::move(service).Value();
                Extensions::ExtensionAdmissionRequest request{.extensionId = "example.consumer",
                                                              .moduleId = "consumer.module",
                                                              .activationGeneration = 1,
                                                              .capabilities = {{{std::string{ProviderCapability}}, {}}}};
                auto admitted = Extensions::ExtensionCapabilityAdmission::Evaluate(request, ProviderPolicy());
                REQUIRE(admitted.HasValue());
                consumer.emplace(std::move(admitted).Value());
                auto granted = consumer->Grant({std::string{ProviderCapability}});
                REQUIRE(granted.HasValue());
                authority.emplace(std::move(granted).Value());
            }

            ~ProviderFixture() {
                base.service->Shutdown();
                Finalize();
                CHECK(retirements->Close().HasValue());
            }

            void Install(const bool mixed = false, const unsigned version = 1) {
                loadFlags = flags | (version == 2 ? HORO_PACKAGE_PROVIDER_CONTRACT_V2 : 0);
                auto descriptor = ProviderDescriptor(mixed);
                std::vector<std::pair<std::string, std::string>> extraFiles;
                if (tooling) {
                    const auto entry = "tool" + std::filesystem::path{HORO_PACKAGE_EMPTY_FIXTURE}.extension().string();
                    descriptor["modules"].push_back({{"id", "com.example.transaction.tool"},
                                                     {"version", "1.0.0"},
                                                     {"kind", "native"},
                                                     {"roles", {"headless-tooling"}},
                                                     {"entry", entry},
                                                     {"dependencies", {"com.example.transaction.native"}}});
                    extraFiles.emplace_back("extensions/native/" + entry, FileBytes(HORO_PACKAGE_EMPTY_FIXTURE));
                }
                const auto packageVersion = std::to_string(version) + ".0.0";
                descriptor["version"] = packageVersion;
                auto graph = Graph(HORO_PACKAGE_PROVIDER_FIXTURE, true, true, "com.example.transaction", std::move(descriptor),
                                   std::move(extraFiles));
                graph->packages.front().lock.version = Parse<PackageVersion>(packageVersion);
                base.Install(std::move(graph));
            }

            [[nodiscard]] Result<void> Activate(const bool enabled = true) {
                if (!roleAware)
                    return base.service->Activate(enabled ? std::span<const EnabledPackageExtension>{base.enabled}
                                                          : std::span<const EnabledPackageExtension>{},
                                                  PackageActivationBoundary::Quiescent, base.cancellation.Token());
                std::vector modules{EnabledPackageModule{base.enabled.front().package, base.enabled.front().descriptor,
                                                         "com.example.transaction.native",
                                                         Extensions::ExtensionModuleRole::BackendCapability}};
                if (tooling)
                    modules.push_back({base.enabled.front().package, base.enabled.front().descriptor, "com.example.transaction.tool",
                                       Extensions::ExtensionModuleRole::HeadlessTooling});
                return base.service->ActivateModules(enabled ? std::span<const EnabledPackageModule>{modules}
                                                             : std::span<const EnabledPackageModule>{},
                                                     PackageActivationBoundary::Quiescent, base.cancellation.Token());
            }

            [[nodiscard]] Result<std::unique_ptr<PlatformProviderLifecycleHost>> Start(const std::size_t index) {
                return PlatformProviderLifecycleHost::Start({configuration,
                                                             realm->admission,
                                                             {"com.example.transaction.native", "example.provider", generations.at(index)},
                                                             *authority,
                                                             {{1, 0, 0}, {2, 0, 0}},
                                                             "example.consumer",
                                                             "consumer.module",
                                                             1});
            }

            void Finalize() {
                if (realm) {
                    const auto disposition = realm->admission.FinalizeOnOwnerThread();
                    CHECK(disposition != PlatformProviderRetirementDisposition::RestartRequired);
                    CHECK(disposition != PlatformProviderRetirementDisposition::OwnerThreadRequired);
                }
                base.service->FinalizeRetirements();
                CHECK(retirements->Drain() == 0);
            }
        };

        /** @brief Requires both native resource balance and verified staged-file removal after retirement. */
        void CheckReleased(const ImageAudit &image) {
            REQUIRE(image.released);
            const auto evidence = image.Snapshot();
            CHECK(evidence.created == evidence.destroyed);
            CHECK(evidence.sinkRetained == 0);
            CHECK(evidence.unloaded == 1);
            CHECK_FALSE(std::filesystem::exists(image.root));
        }
    }  // namespace

    TEST_CASE("Verified platform packages publish only after explicit trusted activation", "[packages][platform-services][provider]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.Install();
            CHECK(fixture.images.empty());
            CHECK(fixture.generations.empty());
            REQUIRE(fixture.Activate().HasValue());
            REQUIRE(fixture.generations.size() == 1);
            auto started = fixture.Start(0);
            REQUIRE(started.HasValue());
            auto host = std::move(started).Value();
            auto request = host->UnlockAchievement({1});
            REQUIRE(request.HasValue());
            CHECK(host->DispatchCompletions(0) == 0);
            CHECK(host->DispatchCompletions(1) == 1);
            CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
            REQUIRE(host->Close().HasValue());
            const auto stopped = fixture.images.front()->Snapshot();
            CHECK(stopped.servicesStopped == 1);
            CHECK(stopped.sessionStopped == 1);
            CHECK(stopped.sinkRetained == 0);
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CHECK(fixture.Start(0).HasError());
            CheckReleased(*fixture.images.front());
        }
    }

    TEST_CASE("Provider package trust stale approvals mixed claims and absent host reject before native loading",
              "[packages][platform-services][provider]") {
        for (const unsigned failure : {0U, 1U, 2U, 3U}) {
            ProviderFixture fixture{false, failure != 3};
            fixture.Install(failure == 2);
            fixture.base.trust.deny = failure == 0;
            fixture.base.trust.stale = failure == 1;
            CHECK(fixture.Activate().HasError());
            CHECK(fixture.images.empty());
            CHECK(fixture.generations.empty());
            CHECK_FALSE(fixture.base.service->Active());
        }
    }

    TEST_CASE("Malformed and unapproved package provider claims cannot publish a capability", "[packages][platform-services][provider]") {
        for (const std::uint32_t flags : {HORO_PACKAGE_PROVIDER_MALFORMED, HORO_PACKAGE_PROVIDER_UNAPPROVED,
                                          HORO_PACKAGE_PROVIDER_WRONG_PROFILE, HORO_PACKAGE_PROVIDER_INCOMPATIBLE_ABI}) {
            ProviderFixture fixture;
            fixture.flags = flags;
            fixture.Install();
            CHECK(fixture.Activate().HasError());
            CHECK(fixture.generations.empty());
            CHECK_FALSE(fixture.base.service->Active());
            REQUIRE(fixture.images.size() == 1);
            CHECK(fixture.images.front()->released);
            CHECK(fixture.images.front()->Snapshot().created == 0);
        }
    }

    TEST_CASE("Package approval bounds provider permissions even when the process policy is broader",
              "[packages][platform-services][provider]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.flags = HORO_PACKAGE_PROVIDER_UNAPPROVED;
            fixture.rootPolicy.knownPermissions = {{"platform.services.private"}};
            fixture.rootPolicy.approvedPermissions = fixture.rootPolicy.knownPermissions;
            fixture.Install();
            CHECK(fixture.Activate().HasError());
            CHECK(fixture.generations.empty());
            CHECK_FALSE(fixture.base.service->Active());
            REQUIRE(fixture.images.size() == 1);
            CHECK(fixture.images.front()->released);
            CHECK(fixture.images.front()->Snapshot().created == 0);
        }
    }

    TEST_CASE("Unavailable SDK and partial candidate creation release package resources without fallback",
              "[packages][platform-services][provider]") {
        for (const std::uint32_t flags : {HORO_PACKAGE_PROVIDER_SDK_UNAVAILABLE, HORO_PACKAGE_PROVIDER_PARTIAL_CREATE}) {
            ProviderFixture fixture;
            fixture.flags = flags;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            CHECK(fixture.Start(0).HasError());
            CHECK(fixture.images.front()->Snapshot().created == 1);
            CHECK(fixture.images.front()->Snapshot().destroyed == 1);
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CheckReleased(*fixture.images.front());
        }
    }

    TEST_CASE("Exact package and process approvals jointly admit a provider permission", "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        fixture.flags = HORO_PACKAGE_PROVIDER_UNAPPROVED;
        fixture.rootPolicy.knownPermissions = {{"platform.services.private"}};
        fixture.rootPolicy.approvedPermissions = fixture.rootPolicy.knownPermissions;
        fixture.base.trust.permissions = fixture.rootPolicy.knownPermissions;
        fixture.base.trust.approvals = fixture.base.trust.permissions;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        auto started = fixture.Start(0);
        REQUIRE(started.HasValue());
        REQUIRE(started.Value()->Close().HasValue());
        fixture.base.service->Shutdown();
        fixture.Finalize();
        CheckReleased(*fixture.images.front());
    }

    TEST_CASE("Package replacement retains revoked native work and verified files until owner drain",
              "[packages][platform-services][provider]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.flags = HORO_PACKAGE_PROVIDER_DELAY | HORO_PACKAGE_PROVIDER_BUSY;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            auto oldStarted = fixture.Start(0);
            REQUIRE(oldStarted.HasValue());
            auto oldHost = std::move(oldStarted).Value();
            auto oldRequest = oldHost->UnlockAchievement({1});
            REQUIRE(oldRequest.HasValue());
            CHECK(oldHost->DispatchCompletions(0) == 0);
            CHECK(fixture.images.front()->Snapshot().submitted == 1);
            std::weak_ptr<const VerifiedPackageInstallRecord> previousInstall = fixture.base.install.InstalledRecord();
            fixture.flags = 0;
            fixture.Install(false, 2);
            REQUIRE(fixture.Activate().HasValue());
            REQUIRE(fixture.generations.size() == 2);
            CHECK(fixture.Start(0).HasError());
            CHECK_FALSE(fixture.images.front()->released);
            CHECK_FALSE(previousInstall.expired());
            CHECK(std::filesystem::exists(fixture.images.front()->root));
            auto replacement = fixture.Start(1);
            REQUIRE(replacement.HasValue());
            auto host = std::move(replacement).Value();
            auto request = host->UnlockAchievement({2});
            REQUIRE(request.HasValue());
            CHECK(host->DispatchCompletions(0) == 0);
            CHECK(host->DispatchCompletions(1) == 1);
            CHECK(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
            REQUIRE(oldHost->Close().HasError());
            CHECK(fixture.images.front()->Snapshot().cancelled >= 1);
            REQUIRE_FALSE(fixture.images.front()->released);
            fixture.images.front()->configure(0);
            REQUIRE(oldHost->Close().HasValue());
            const auto retiredRequest = oldHost->Query(oldRequest.Value());
            REQUIRE(retiredRequest.HasValue());
            CHECK(retiredRequest.Value().terminal);
            fixture.Finalize();
            CheckReleased(*fixture.images.front());
            CHECK(previousInstall.expired());
            REQUIRE(host->Close().HasValue());
            REQUIRE(fixture.Activate(false).HasValue());
            fixture.Finalize();
            CheckReleased(*fixture.images.back());
        }
    }

    TEST_CASE("A failed partially created provider keeps its package image until native retirement succeeds",
              "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        fixture.flags = HORO_PACKAGE_PROVIDER_PARTIAL_CREATE | HORO_PACKAGE_PROVIDER_BUSY;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        REQUIRE(fixture.Start(0).HasError());
        fixture.base.service->Shutdown();
        REQUIRE(fixture.images.size() == 1);
        auto &image = *fixture.images.front();
        REQUIRE_FALSE(image.released);
        CHECK(image.Snapshot().created == 1);
        CHECK(image.Snapshot().destroyed == 0);
        CHECK(std::filesystem::exists(image.root));
        CHECK(fixture.realm->admission.FinalizeOnOwnerThread() == PlatformProviderRetirementDisposition::Busy);
        image.configure(0);
        fixture.Finalize();
        CheckReleased(image);
    }

    TEST_CASE("Cancelled and stale package provider updates revoke staged registrations and preserve the old realm",
              "[packages][platform-services][provider]") {
        for (const bool stale : {false, true}) {
            ProviderFixture fixture;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            const auto previous = fixture.base.service->Active();
            fixture.Install(false, 2);
            fixture.cancelAfterCommit = !stale;
            fixture.staleAfterCommit = stale;
            REQUIRE(fixture.Activate().HasError());
            CHECK(fixture.base.service->Active() == previous);
            REQUIRE(fixture.generations.size() == 2);
            CHECK(fixture.Start(1).HasError());
            auto prior = fixture.Start(0);
            REQUIRE(prior.HasValue());
            REQUIRE(prior.Value()->Close().HasValue());
            fixture.Finalize();
            CheckReleased(*fixture.images.back());
        }
    }

    TEST_CASE("Provider package rollback and uninstall retire each exact installed generation", "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        for (const unsigned version : {1U, 2U, 1U}) {
            fixture.Install(false, version);
            REQUIRE(fixture.Activate().HasValue());
            const auto index = fixture.generations.size() - 1;
            auto started = fixture.Start(index);
            REQUIRE(started.HasValue());
            REQUIRE(started.Value()->Close().HasValue());
            fixture.Finalize();
            if (index != 0) {
                CHECK(fixture.Start(index - 1).HasError());
                CheckReleased(*fixture.images[index - 1]);
            }
        }
        auto removed = std::make_shared<PackageRestoreGraph>(*fixture.base.install.ActiveGraph());
        removed->packages.clear();
        fixture.base.Install(std::move(removed));
        REQUIRE(fixture.Activate(false).HasValue());
        fixture.Finalize();
        CHECK(fixture.base.service->Active()->install->Graph()->packages.empty());
        for (std::size_t index = 0; index < fixture.images.size(); ++index) {
            CHECK(fixture.Start(index).HasError());
            CheckReleased(*fixture.images[index]);
        }
    }

    TEST_CASE("Same-contract provider updates require explicit disable before registry reuse", "[packages][platform-services][provider]") {
        ProviderFixture fixture;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        const auto previous = fixture.base.service->Active();
        fixture.Install();
        REQUIRE(fixture.Activate().HasError());
        CHECK(fixture.base.service->Active() == previous);
        REQUIRE(fixture.generations.size() == 1);
        auto prior = fixture.Start(0);
        REQUIRE(prior.HasValue());
        REQUIRE(prior.Value()->Close().HasValue());
        REQUIRE(fixture.Activate(false).HasValue());
        fixture.Finalize();
        CHECK(fixture.Start(0).HasError());
        REQUIRE(fixture.Activate().HasValue());
        REQUIRE(fixture.generations.size() == 2);
        auto replacement = fixture.Start(1);
        REQUIRE(replacement.HasValue());
        CHECK(fixture.generations[1] > fixture.generations[0]);
        REQUIRE(replacement.Value()->Close().HasValue());
    }

    TEST_CASE("Provider and headless tooling roles activate once through their exact native owners",
              "[packages][platform-services][provider][hybrid]") {
        for (const bool roleAware : {false, true}) {
            ProviderFixture fixture{roleAware};
            fixture.tooling = true;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            CHECK(fixture.toolingLoads == 1);
            REQUIRE(fixture.generations.size() == 1);
            if (roleAware) {
                const auto snapshot = fixture.base.service->Active();
                REQUIRE(snapshot->modules.size() == 2);
                for (const auto &module : snapshot->modules)
                    CHECK(module.outcome == PackageModuleOutcome::Active);
            }
            auto started = fixture.Start(0);
            REQUIRE(started.HasValue());
            REQUIRE(started.Value()->Close().HasValue());
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CHECK(fixture.Start(0).HasError());
            CheckReleased(*fixture.images.front());
        }
    }

    TEST_CASE("BUSY provider close retries native cancellation only once even when cancellation is rejected",
              "[packages][platform-services][provider][shutdown]") {
        ProviderFixture fixture;
        fixture.flags = HORO_PACKAGE_PROVIDER_DELAY | HORO_PACKAGE_PROVIDER_BUSY | HORO_PACKAGE_PROVIDER_CANCEL_REJECTED;
        fixture.Install();
        REQUIRE(fixture.Activate().HasValue());
        auto started = fixture.Start(0);
        REQUIRE(started.HasValue());
        auto host = std::move(started).Value();
        auto request = host->UnlockAchievement({1});
        REQUIRE(request.HasValue());
        CHECK(host->DispatchCompletions(0) == 0);
        auto &image = *fixture.images.front();
        REQUIRE(image.Snapshot().submitted == 1);
        for (unsigned attempt = 0; attempt < 3; ++attempt) {
            const auto closed = host->Close();
            REQUIRE(closed.HasError());
            CHECK(closed.ErrorValue().code.Value() == PlatformProviderLifecycleErrors::DrainBusy.code.Value());
            CHECK(image.Snapshot().cancelled == 1);
            CHECK(image.Snapshot().destroyed == 0);
            CHECK(host->Query(request.Value()).Value().terminal);
            CHECK(host->UnlockAchievement({2}).HasError());
        }
        REQUIRE_FALSE(image.released);
        image.configure(0);
        REQUIRE(host->Close().HasValue());
        CHECK(image.Snapshot().cancelled == 1);
        CHECK(image.Snapshot().destroyed == 1);
        fixture.base.service->Shutdown();
        fixture.Finalize();
        CheckReleased(image);
    }

    TEST_CASE("Failed provider close withdraws deferred user delivery before native retirement",
              "[packages][platform-services][provider][shutdown]") {
        for (const std::uint32_t failure :
             {HORO_PACKAGE_PROVIDER_BUSY, HORO_PACKAGE_PROVIDER_CLOSE_ADMISSION_FAILED, HORO_PACKAGE_PROVIDER_CLOSE_INGRESS_FAILED}) {
            ProviderFixture fixture;
            fixture.Install();
            REQUIRE(fixture.Activate().HasValue());
            auto started = fixture.Start(0);
            REQUIRE(started.HasValue());
            auto host = std::move(started).Value();
            auto request = host->UnlockAchievement({1});
            REQUIRE(request.HasValue());
            CHECK(host->DispatchCompletions(0) == 0);
            CHECK(host->DispatchCompletions(1) == 1);
            REQUIRE(host->Query(request.Value()).Value().state == PlatformRequestState::Succeeded);
            unsigned deliveries{};
            auto subscription = host->OnComplete(request.Value(), [&](const auto &) {
                ++deliveries;
            });
            REQUIRE(subscription.HasValue());
            CHECK(deliveries == 0);
            auto &image = *fixture.images.front();
            image.configure(failure);
            REQUIRE(host->Close().HasError());
            REQUIRE_FALSE(image.released);
            CHECK(image.Snapshot().destroyed == 0);
            CHECK(image.Snapshot().sinkRetained == 1);
            CHECK(host->DispatchCompletions(16) == 0);
            CHECK(deliveries == 0);
            CHECK(host->OnComplete(request.Value(), [&](const auto &) {
                ++deliveries;
            }).HasError());
            CHECK(host->UnlockAchievement({2}).HasError());
            image.configure(0);
            REQUIRE(host->Close().HasValue());
            CHECK(host->DispatchCompletions(16) == 0);
            CHECK(deliveries == 0);
            fixture.base.service->Shutdown();
            fixture.Finalize();
            CheckReleased(image);
        }
    }
}  // namespace Horo::Packages::Tests
