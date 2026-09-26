#include "Horo/Network/NetworkTargetCapabilities.h"

#include <algorithm>
#include <optional>

namespace Horo::Network {
    namespace {
        using enum NetworkTargetCapabilityKind;
        using enum NetworkTargetFailureReason;

        constexpr auto AllRoles = static_cast<std::uint32_t>(NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client |
                                                             NetworkProjectRoleSet::ListenServer | NetworkProjectRoleSet::DedicatedServer);
        constexpr auto AllPlatforms = (std::uint32_t{1} << static_cast<std::uint8_t>(NetworkTargetPlatform::Count)) - 1;

        bool ValidRoles(const NetworkProjectRoleSet roles) {
            return (static_cast<std::uint32_t>(roles) & ~AllRoles) == 0;
        }

        bool HasNetworkRoles(const NetworkProjectRoleSet roles) {
            using enum NetworkProjectRole;
            return ContainsNetworkProjectRole(roles, Client) || ContainsNetworkProjectRole(roles, ListenServer) ||
                   ContainsNetworkProjectRole(roles, DedicatedServer);
        }

        bool ValidProtocol(const NetworkProjectProtocolPolicy &protocol) {
            return protocol.protocol.IsValid() && protocol.supportedVersions.IsValid() && protocol.schemaFingerprint != 0;
        }

        bool SameProtocol(const NetworkProjectProtocolPolicy &left, const NetworkProjectProtocolPolicy &right,
                          const ProtocolVersion selected) {
            return left.protocol == right.protocol && left.schemaFingerprint == right.schemaFingerprint &&
                   left.supportedVersions.Contains(selected) && right.supportedVersions.Contains(selected);
        }

        bool ValidManifest(const NetworkProductCapabilityManifest &product) {
            if (product.contractVersion != NetworkProductCapabilityManifest::CurrentContractVersion || !product.build.IsValid() ||
                !product.revision.IsValid() || product.platform >= NetworkTargetPlatform::Count || !ValidRoles(product.supportedRoles) ||
                !product.profile.IsValid() || !product.profileRevision.IsValid() || product.providerCount > MaximumNetworkTargetProviders)
                return false;
            if ((HasNetworkRoles(product.supportedRoles) && !product.includesNetworkRuntime) ||
                (product.includesNetworkRuntime && !ValidProtocol(product.protocol)) ||
                (!product.includesNetworkRuntime && product.providerCount != 0))
                return false;
            for (std::size_t index = 0; index < product.providerCount; ++index) {
                const auto &provider = product.providers[index];
                if (!provider.id.IsValid() || provider.allowedPlatforms == 0 || (provider.allowedPlatforms & ~AllPlatforms) != 0 ||
                    !ValidateTransportCapabilities(provider.capabilities))
                    return false;
                for (std::size_t earlier = 0; earlier < index; ++earlier) {
                    if (product.providers[earlier].id == provider.id)
                        return false;
                }
            }
            return true;
        }

        bool ValidInventory(const NetworkTargetPackageInventory &inventory) {
            if (!inventory.build.IsValid() || inventory.platform >= NetworkTargetPlatform::Count || !ValidRoles(inventory.supportedRoles) ||
                inventory.providerCount > MaximumNetworkTargetProviders)
                return false;
            for (std::size_t index = 0; index < inventory.providerCount; ++index) {
                if (!inventory.providers[index].id.IsValid() || inventory.providers[index].allowedPlatforms == 0 ||
                    (inventory.providers[index].allowedPlatforms & ~AllPlatforms) != 0 ||
                    !ValidateTransportCapabilities(inventory.providers[index].capabilities))
                    return false;
                for (std::size_t earlier = 0; earlier < index; ++earlier) {
                    if (inventory.providers[earlier].id == inventory.providers[index].id)
                        return false;
                }
            }
            return true;
        }

        bool ValidHost(const NetworkTargetHostFacts &host) {
            if (!host.revision.IsValid() || host.platform >= NetworkTargetPlatform::Count || !ValidRoles(host.supportedRoles) ||
                host.providerCount > MaximumNetworkTargetProviders ||
                (HasNetworkRoles(host.supportedRoles) && !ValidProtocol(host.protocol)))
                return false;
            for (std::size_t index = 0; index < host.providerCount; ++index) {
                const auto &provider = host.providers[index];
                if (!provider.id.IsValid() || (provider.installed && !ValidateTransportCapabilities(provider.capabilities)) ||
                    (provider.hostSupported && !provider.installed) || (provider.configured && !provider.installed))
                    return false;
                for (std::size_t earlier = 0; earlier < index; ++earlier) {
                    if (host.providers[earlier].id == provider.id)
                        return false;
                }
            }
            return true;
        }

        const NetworkPackagedTransport *FindProduct(const NetworkProductCapabilityManifest &product, const NetworkTransportProviderId id) {
            for (std::size_t index = 0; index < product.providerCount; ++index) {
                if (product.providers[index].id == id)
                    return &product.providers[index];
            }
            return nullptr;
        }

        const NetworkHostTransportFact *FindHost(const NetworkTargetHostFacts &host, const NetworkTransportProviderId id) {
            for (std::size_t index = 0; index < host.providerCount; ++index) {
                if (host.providers[index].id == id)
                    return &host.providers[index];
            }
            return nullptr;
        }

        const NetworkPackagedTransport *FindInventory(const NetworkTargetPackageInventory &inventory, const NetworkTransportProviderId id) {
            for (std::size_t index = 0; index < inventory.providerCount; ++index) {
                if (inventory.providers[index].id == id)
                    return &inventory.providers[index];
            }
            return nullptr;
        }

        NetworkTargetAssessment Reject(NetworkTargetAssessment assessment, const NetworkTargetDiagnostic diagnostic) {
            assessment.diagnostic = diagnostic;
            return assessment;
        }

        void AddProvider(NetworkTargetCapabilityMatrix &matrix, const NetworkTransportProviderId id, const bool packaged,
                         const NetworkHostTransportFact *host, const NetworkTargetRequirements &requirements,
                         const NetworkTargetSelection &selection) {
            if (!id.IsValid())
                return;
            for (std::size_t index = 0; index < matrix.providerCount; ++index) {
                if (matrix.providers[index].id == id)
                    return;
            }
            auto &status = matrix.providers[matrix.providerCount++];
            status = {id,
                      packaged,
                      host != nullptr && host->installed,
                      host != nullptr && host->hostSupported,
                      host != nullptr && host->configured,
                      requirements.requiredProvider == id,
                      selection.provider == id};
        }

        NetworkTargetCapabilityMatrix MakeMatrix(const NetworkProjectSettings &project, const NetworkProductCapabilityManifest &product,
                                                 const NetworkTargetPackageInventory &inventory, const NetworkTargetHostFacts &host,
                                                 const NetworkTargetRequirements &requirements, const NetworkTargetSelection &selection) {
            NetworkTargetCapabilityMatrix matrix{};
            matrix.build = product.build;
            matrix.productRevision = product.revision;
            matrix.hostRevision = host.revision;
            matrix.projectRevision = project.Revision();
            for (std::size_t index = 0; index < matrix.roles.size(); ++index) {
                const auto role = static_cast<NetworkProjectRole>(index);
                matrix.roles[index] = {role, ContainsNetworkProjectRole(inventory.supportedRoles, role),
                                       ContainsNetworkProjectRole(host.supportedRoles, role),
                                       ContainsNetworkProjectRole(requirements.requiredRoles, role), selection.role == role};
            }
            for (std::size_t index = 0; index < product.providerCount; ++index)
                AddProvider(matrix, product.providers[index].id, FindInventory(inventory, product.providers[index].id) != nullptr,
                            FindHost(host, product.providers[index].id), requirements, selection);
            for (std::size_t index = 0; index < host.providerCount; ++index)
                AddProvider(matrix, host.providers[index].id, FindInventory(inventory, host.providers[index].id) != nullptr,
                            &host.providers[index], requirements, selection);
            for (std::size_t index = 0; index < inventory.providerCount; ++index)
                AddProvider(matrix, inventory.providers[index].id, true, FindHost(host, inventory.providers[index].id), requirements,
                            selection);
            AddProvider(matrix, selection.provider, FindInventory(inventory, selection.provider) != nullptr,
                        FindHost(host, selection.provider), requirements, selection);
            AddProvider(matrix, requirements.requiredProvider, FindInventory(inventory, requirements.requiredProvider) != nullptr,
                        FindHost(host, requirements.requiredProvider), requirements, selection);
            matrix.packagePlatformMatches = inventory.platform == product.platform;
            matrix.hostPlatformMatches = host.platform == product.platform;
            matrix.packageRuntimePresent = inventory.includesNetworkRuntime;
            matrix.hostRuntimeInstalled = host.networkRuntimeInstalled;
            matrix.protocolPackaged =
                selection.protocolVersion.IsValid() && SameProtocol(project.Protocol(), inventory.protocol, selection.protocolVersion);
            matrix.protocolHostSupported =
                selection.protocolVersion.IsValid() && SameProtocol(project.Protocol(), host.protocol, selection.protocolVersion);
            matrix.protocolProjectRequired = selection.role != NetworkProjectRole::Standalone && ValidProtocol(project.Protocol());
            matrix.protocolSelected = selection.protocolVersion.IsValid();
            return matrix;
        }

        bool TransportMeetsProject(const TransportCapabilities &capabilities, const NetworkProjectSettings &project) {
            if (project.Transport().requirement == NetworkProjectTransportRequirement::Optional &&
                std::ranges::none_of(project.Transport().capabilities.requiredDelivery, [](const bool required) {
                return required;
            }))
                return ValidateTransportCapabilities(capabilities);
            return !ResolveTransportCapabilities(capabilities, capabilities.revision, project.Transport().capabilities).HasError();
        }

        using Rejection = std::optional<NetworkTargetAssessment>;

        Rejection CheckPackageInventory(const NetworkTargetAssessment &assessment, const NetworkProductCapabilityManifest &product,
                                        const NetworkTargetPackageInventory &inventory) {
            using enum NetworkTargetRemediation;
            if (inventory.build != product.build)
                return Reject(assessment, {PackageInventory, PackageMismatch, RebuildPackage});
            if (inventory.platform != product.platform)
                return Reject(assessment, {Platform, PackageMismatch, RebuildPackage, NetworkProjectRole::Count, {}, {}, product.platform});
            for (const auto &role : assessment.matrix.roles) {
                if (ContainsNetworkProjectRole(product.supportedRoles, role.role) != role.packaged)
                    return Reject(assessment, {Role, PackageMismatch, RebuildPackage, role.role});
            }
            if (inventory.includesNetworkRuntime != product.includesNetworkRuntime)
                return Reject(assessment, {NetworkRuntime, PackageMismatch, RebuildPackage});
            if (inventory.protocol != product.protocol)
                return Reject(assessment,
                              {Protocol, PackageMismatch, RebuildPackage, NetworkProjectRole::Count, {}, product.protocol.protocol});
            for (std::size_t index = 0; index < product.providerCount; ++index) {
                const auto *actual = FindInventory(inventory, product.providers[index].id);
                if (!actual || *actual != product.providers[index])
                    return Reject(assessment,
                                  {Provider, PackageMismatch, RebuildPackage, NetworkProjectRole::Count, product.providers[index].id});
            }
            for (std::size_t index = 0; index < inventory.providerCount; ++index) {
                if (!FindProduct(product, inventory.providers[index].id))
                    return Reject(assessment,
                                  {Provider, PackageMismatch, RebuildPackage, NetworkProjectRole::Count, inventory.providers[index].id});
            }
            return std::nullopt;
        }

        Rejection CheckPublication(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                   const NetworkProductCapabilityManifest &product, const NetworkTargetPackageInventory &inventory,
                                   const NetworkTargetHostFacts &host, const NetworkTargetSelection &selection) {
            using enum NetworkTargetRemediation;
            if (selection.lifecycle != NetworkTargetLifecycle::Active)
                return Reject(assessment, {Lifecycle, selection.lifecycle == NetworkTargetLifecycle::Cancelling ? Cancelled : ShuttingDown,
                                           WaitForRestart});
            if (selection.expectedBuild != product.build || selection.expectedProductRevision != product.revision ||
                selection.expectedHostRevision != host.revision || selection.expectedProjectRevision != project.Revision())
                return Reject(assessment, {Lifecycle, Stale, RefreshSnapshot});
            if (const auto failed = CheckPackageInventory(assessment, product, inventory))
                return failed;
            if (host.platform != product.platform)
                return Reject(assessment,
                              {Platform, HostUnsupported, ChooseSupportedHost, NetworkProjectRole::Count, {}, {}, product.platform});
            if (project.Profile().id != product.profile || project.Profile().revision != product.profileRevision)
                return Reject(assessment, {ProjectProfile, Stale, RebuildPackage});
            return std::nullopt;
        }

        Rejection CheckRequirements(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                    const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                    const NetworkTargetRequirements &requirements) {
            using enum NetworkTargetRemediation;
            for (const auto &role : assessment.matrix.roles) {
                if (!role.projectRequired)
                    continue;
                if (!ContainsNetworkProjectRole(project.SupportedRoles(), role.role))
                    return Reject(assessment, {Role, ProjectUnavailable, ConfigureProject, role.role});
                if (!role.packaged)
                    return Reject(assessment, {Role, NotPackaged, RebuildPackage, role.role});
                if (!role.hostSupported)
                    return Reject(assessment, {Role, HostUnsupported, ChooseSupportedHost, role.role});
            }
            if (!requirements.requiredProvider.IsValid())
                return std::nullopt;
            if (!FindProduct(product, requirements.requiredProvider))
                return Reject(assessment,
                              {Provider, NotPackaged, RebuildPackage, NetworkProjectRole::Count, requirements.requiredProvider});
            const auto *requiredHost = FindHost(host, requirements.requiredProvider);
            if (!requiredHost || !requiredHost->installed)
                return Reject(assessment,
                              {Provider, NotInstalled, InstallTarget, NetworkProjectRole::Count, requirements.requiredProvider});
            if (!requiredHost->hostSupported)
                return Reject(assessment,
                              {Provider, HostUnsupported, ChooseSupportedHost, NetworkProjectRole::Count, requirements.requiredProvider});
            return std::nullopt;
        }

        Rejection CheckSelectedRole(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                    const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                    const NetworkTargetSelection &selection) {
            using enum NetworkTargetRemediation;
            const auto role = selection.role;
            if (!ContainsNetworkProjectRole(project.SupportedRoles(), role))
                return Reject(assessment, {Role, ProjectUnavailable, ConfigureProject, role});
            if (!ContainsNetworkProjectRole(product.supportedRoles, role))
                return Reject(assessment, {Role, NotPackaged, RebuildPackage, role});
            if (!ContainsNetworkProjectRole(host.supportedRoles, role))
                return Reject(assessment, {Role, HostUnsupported, ChooseSupportedHost, role});
            return std::nullopt;
        }

        Rejection CheckSelectedTransport(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                         const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                         const NetworkTargetSelection &selection) {
            using enum NetworkTargetRemediation;
            const auto role = selection.role;
            const auto *packaged = FindProduct(product, selection.provider);
            if (!packaged)
                return Reject(assessment, {Provider, NotPackaged, RebuildPackage, role, selection.provider});
            if (!ContainsNetworkTargetPlatform(packaged->allowedPlatforms, product.platform))
                return Reject(assessment, {Platform, HostUnsupported, ChooseSupportedHost, role, selection.provider, {}, product.platform});
            const auto *installed = FindHost(host, selection.provider);
            if (!installed || !installed->installed)
                return Reject(assessment, {Provider, NotInstalled, InstallTarget, role, selection.provider});
            if (!installed->hostSupported)
                return Reject(assessment, {Provider, HostUnsupported, ChooseSupportedHost, role, selection.provider});
            if (!installed->configured)
                return Reject(assessment, {Provider, ProjectUnavailable, ConfigureProject, role, selection.provider});
            if (!TransportMeetsProject(packaged->capabilities, project))
                return Reject(assessment, {Provider, Incompatible, RebuildPackage, role, selection.provider});
            if (!TransportMeetsProject(installed->capabilities, project))
                return Reject(assessment, {Provider, Incompatible, SelectAvailableCapability, role, selection.provider});
            return std::nullopt;
        }

        Rejection CheckSelectedProtocol(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                        const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                        const NetworkTargetSelection &selection) {
            using enum NetworkTargetRemediation;
            if (!SameProtocol(project.Protocol(), product.protocol, selection.protocolVersion))
                return Reject(assessment,
                              {Protocol, Incompatible, RebuildPackage, selection.role, selection.provider, project.Protocol().protocol});
            if (!SameProtocol(project.Protocol(), host.protocol, selection.protocolVersion))
                return Reject(assessment,
                              {Protocol, Incompatible, UpgradeProtocol, selection.role, selection.provider, project.Protocol().protocol});
            return std::nullopt;
        }
    }  // namespace

    namespace Detail {
        bool ValidateNetworkProductCapabilityManifest(const NetworkProductCapabilityManifest &product) {
            return ValidManifest(product);
        }
    }  // namespace Detail

    /** @copydoc AssessNetworkTarget */
    NetworkTargetAssessment AssessNetworkTarget(const NetworkProjectSettings &project, const NetworkProductCapabilityManifest &product,
                                                const NetworkTargetPackageInventory &inventory, const NetworkTargetHostFacts &host,
                                                const NetworkTargetRequirements &requirements, const NetworkTargetSelection &selection) {
        using enum NetworkTargetRemediation;
        NetworkTargetAssessment assessment{};
        if (!ValidManifest(product) || !ValidInventory(inventory) || !ValidHost(host) || !ValidRoles(requirements.requiredRoles) ||
            selection.role >= NetworkProjectRole::Count || selection.lifecycle >= NetworkTargetLifecycle::Count)
            return Reject(assessment, {Input, Invalid, CorrectInput});

        assessment.matrix = MakeMatrix(project, product, inventory, host, requirements, selection);
        if (const auto failed = CheckPublication(assessment, project, product, inventory, host, selection))
            return *failed;
        if (const auto failed = CheckRequirements(assessment, project, product, host, requirements))
            return *failed;
        if (const auto failed = CheckSelectedRole(assessment, project, product, host, selection))
            return *failed;
        if (selection.role == NetworkProjectRole::Standalone) {
            if (selection.provider.IsValid() || selection.protocolVersion.IsValid())
                return Reject(assessment, {Input, Invalid, CorrectInput});
            assessment.diagnostic.reason = NetworkTargetFailureReason::None;
            return assessment;
        }
        if (!inventory.includesNetworkRuntime)
            return Reject(assessment, {NetworkRuntime, NotPackaged, RebuildPackage});
        if (!host.networkRuntimeInstalled)
            return Reject(assessment, {NetworkRuntime, NotInstalled, InstallTarget});
        if (!selection.provider.IsValid() || !selection.protocolVersion.IsValid())
            return Reject(assessment, {Input, Invalid, CorrectInput});
        if (requirements.requiredProvider.IsValid() && requirements.requiredProvider != selection.provider)
            return Reject(assessment, {Provider, NotSelected, SelectAvailableCapability, selection.role, requirements.requiredProvider});
        if (const auto failed = CheckSelectedTransport(assessment, project, product, host, selection))
            return *failed;
        if (const auto failed = CheckSelectedProtocol(assessment, project, product, host, selection))
            return *failed;
        assessment.diagnostic.reason = NetworkTargetFailureReason::None;
        return assessment;
    }
}  // namespace Horo::Network
