#pragma once

/** @file PackagePlatformProviderTestSupport.h @brief Verified provider package fixture and native ownership audit. */
#include "../../fixtures/extensions/PlatformPackageFixture.h"
#include "Horo/PlatformServices/PlatformProviderAdmission.h"
#include "PackageLifecycleTestSupport.h"

#include <array>
#include <utility>

namespace Horo::Packages::Tests::ProviderSupport {
    using namespace Support;
    using namespace PlatformServices;

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
    [[nodiscard]] inline Extensions::ExtensionAdmissionPolicy ProviderPolicy() {
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
    [[nodiscard]] inline nlohmann::json ProviderDescriptor(const bool mixed = false) {
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
    [[nodiscard]] inline PlatformProjectConfiguration ProviderConfiguration() {
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
            policy.libraryLoader = [this](const std::string &path) {
                return LoadImage(path);
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
                            base.Install(Graph(HORO_PACKAGE_PROVIDER_FIXTURE, true, true, "com.example.transaction", ProviderDescriptor()));
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

        /** @brief Loads one image and captures provider audit hooks before native registration. */
        Result<std::unique_ptr<Platform::DynamicLibrary>> LoadImage(const std::string &path) {
            auto image = Platform::LoadDynamicLibrary(path);
            if (image.HasError())
                return image;
            if (std::filesystem::path{path}.filename().string().starts_with("tool")) {
                ++toolingLoads;
                return image;
            }
            auto audit = std::make_shared<ImageAudit>();
            audit->configure = reinterpret_cast<HoroPackageProviderConfigure>(image.Value()->GetSymbol("horo_package_provider_configure"));
            audit->inspect = reinterpret_cast<HoroPackageProviderInspect>(image.Value()->GetSymbol("horo_package_provider_inspect"));
            REQUIRE(audit->configure);
            REQUIRE(audit->inspect);
            audit->configure(loadFlags);
            audit->root = std::filesystem::path{path}.parent_path();
            images.push_back(audit);
            return Result<std::unique_ptr<Platform::DynamicLibrary>>::Success(
                std::make_unique<AuditedImage>(std::move(image).Value(), std::move(audit)));
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
            auto graph =
                Graph(HORO_PACKAGE_PROVIDER_FIXTURE, true, true, "com.example.transaction", std::move(descriptor), std::move(extraFiles));
            graph->packages.front().lock.version = Parse<PackageVersion>(packageVersion);
            base.Install(std::move(graph));
        }

        [[nodiscard]] Result<void> Activate(const bool enabled = true) {
            if (!roleAware)
                return base.service->Activate(enabled ? std::span<const EnabledPackageExtension>{base.enabled}
                                                      : std::span<const EnabledPackageExtension>{},
                                              PackageActivationBoundary::Quiescent, base.cancellation.Token());
            std::vector modules{EnabledPackageModule{base.enabled.front().package, base.enabled.front().descriptor,
                                                     "com.example.transaction.native", Extensions::ExtensionModuleRole::BackendCapability}};
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
    inline void CheckReleased(const ImageAudit &image) {
        REQUIRE(image.released);
        const auto evidence = image.Snapshot();
        CHECK(evidence.created == evidence.destroyed);
        CHECK(evidence.sinkRetained == 0);
        CHECK(evidence.unloaded == 1);
        CHECK_FALSE(std::filesystem::exists(image.root));
    }
}  // namespace Horo::Packages::Tests::ProviderSupport
