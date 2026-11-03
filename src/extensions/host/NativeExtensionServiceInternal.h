#pragma once

#include "../capabilities/asset_pipeline_points/ExternalAssetImporter.h"
#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Extensions/NativeExtensionServiceHost.h"

#include <array>
#include <atomic>
#include <optional>
#include <ranges>

namespace Horo::Extensions {
    struct NativeServiceMethod final {
        std::string id;
        std::uint32_t arguments{};
    };

    struct NativeServiceShape final {
        std::string service;
        std::string contract;
        std::string scriptApi;
        ApplicationCapabilityVersion version;
        std::vector<NativeServiceMethod> methods;
    };

    struct NativeServiceCall final {
        std::string_view method;
        std::span<const std::int64_t> arguments;
        HoroExtensionCancellation nativeCancellation{};
    };

    /** @brief Sole concrete backend adapter type; registry retains its real executable lease. */
    class NativeScalarService final {
    public:
        NativeScalarService(NativeServiceShape shape, const HoroExtensionServiceDescriptor &native,
                            std::shared_ptr<ExtensionModuleLifetime> module);
        ~NativeScalarService();
        Result<std::int64_t> Invoke(const NativeServiceCall &request, const BackendServiceCallContext &context) const;
        void Shutdown() noexcept;
        Result<ScriptCallResult> InvokeScript(const ScriptInvocationRequest &, const BackendServiceCallContext &) const;

    private:
        NativeServiceShape shape_;
        void *provider_{};
        HoroExtensionStatus (*invoke_)(void *, const HoroExtensionServiceRequest *, HoroExtensionServiceResult *){};
        void (*shutdown_)(void *){};
        std::shared_ptr<ExtensionModuleLifetime> module_;
    };

    struct NativeServiceModule;

    struct NativeServicePublication final : IExtensionRetirementContribution {
        NativeServiceShape shape;
        std::weak_ptr<NativeServiceModule> owner;
        std::unique_ptr<NativeScalarService> pending;
        ApplicationCapabilityProviderIdentity provider;
        std::optional<ApplicationCapabilityProviderRegistration> capability;
        std::optional<BackendServiceRegistration> service;
        std::shared_ptr<ScriptInvocationProviderRegistration> script;
        ScriptExportDescriptorSnapshotPtr descriptors;
        void Revoke() noexcept override;
    };

    struct NativeServiceModule;

    struct NativeServiceImportClient final {
        NativeServiceImportClient(std::weak_ptr<NativeServiceModule> owner, ResolvedExtensionServiceImport binding)
            : module(std::move(owner)), import(std::move(binding)) {}

        std::weak_ptr<NativeServiceModule> module;
        ResolvedExtensionServiceImport import;
        std::weak_ptr<NativeServicePublication> provider;
    };

    struct NativeServiceModule final : IExtensionRetirementContribution {
        std::weak_ptr<NativeExtensionServiceState> host;
        std::thread::id owner{std::this_thread::get_id()};
        std::weak_ptr<ExtensionModuleLifetime> moduleCode;
        std::weak_ptr<ExtensionRetirement> retirement;
        std::string extension;
        ExtensionModuleManifest declaration;
        std::vector<ResolvedExtensionServiceImport> imports;
        std::optional<ExtensionCapabilityAdmission> admission;
        std::vector<std::shared_ptr<NativeServicePublication>> publications;
        std::vector<std::unique_ptr<NativeServiceImportClient>> clients;
        std::atomic_bool committed{};
        std::atomic_bool revoked{};
        void Revoke() noexcept override;
    };

    struct NativeExtensionServiceState final {
        std::shared_ptr<ApplicationCapabilityRegistry> capabilities;
        std::shared_ptr<BackendServiceRegistry> services;
        std::shared_ptr<ScriptInvocationRegistry> invocations;
        const ExtensionAdmissionPolicy policy;
        const std::thread::id owner{std::this_thread::get_id()};
        std::vector<std::weak_ptr<NativeServiceModule>> modules;
        std::uint64_t nextGeneration{1};
        std::atomic_bool closed{};
    };

    struct NativeExtensionServiceHostAccess final {
        static std::shared_ptr<NativeExtensionServiceState> State(NativeExtensionServiceHost &host) {
            return host.state_;
        }

        static Result<std::shared_ptr<NativeExtensionServiceHost>> HeadlessServices(class HeadlessExtensionHost &);
        static Result<std::shared_ptr<NativeExtensionServiceHost>> ConfiguringHeadlessServices(class HeadlessExtensionHost &);
        static Result<std::shared_ptr<NativeServiceModule>> Stage(NativeExtensionServiceHost &host, AssetImporterRegistrationSession &load,
                                                                  std::span<const ResolvedExtensionServiceImport> imports);
    };

    [[nodiscard]] Result<NativeServiceShape> CopyNativeServiceDescriptor(const HoroExtensionServiceDescriptor &input);
    [[nodiscard]] Result<NativeServiceShape> CopyNativeServiceShape(const HoroExtensionServiceDescriptor &input,
                                                                    const ExtensionModuleManifest &module);
    [[nodiscard]] Result<ApplicationCapabilityVersion> NativeServiceVersion(std::string_view input);
    [[nodiscard]] Result<ScriptExportDescriptorSnapshotPtr> NativeServiceScriptDescriptors(const NativeServiceShape &shape,
                                                                                           std::string_view module);
    [[nodiscard]] Result<std::shared_ptr<NativeServicePublication>> FindNativeService(const NativeExtensionServiceState &host,
                                                                                      const ResolvedExtensionServiceImport &import);
    [[nodiscard]] Result<std::int64_t> InvokeNativeServiceImport(NativeServiceModule &module, const ResolvedExtensionServiceImport &import,
                                                                 const NativeServiceCall &request, const CancellationToken &cancel);
    [[nodiscard]] Result<std::int64_t> InvokeSelectedNativeServiceImport(NativeServiceModule &module,
                                                                         const ResolvedExtensionServiceImport &import,
                                                                         const NativeServicePublication &publication,
                                                                         const NativeServiceCall &request, const CancellationToken &cancel);
    [[nodiscard]] Result<void> CommitNativeServices(std::span<const std::shared_ptr<ExtensionModuleLifetime>> modules);
    [[nodiscard]] Result<void> ValidateNativeServiceClaims(const AssetImporterRegistrationSession &load);
    HoroExtensionStatus RegisterNativeService(void *load, const HoroExtensionServiceDescriptor *descriptor) noexcept;
    HoroExtensionStatus BindNativeServiceImport(void *load, HoroExtensionStringView id, HoroExtensionServiceClient *client) noexcept;
    [[nodiscard]] const HoroExtensionServiceHostApi &NativeServiceHostAbi() noexcept;
}  // namespace Horo::Extensions
