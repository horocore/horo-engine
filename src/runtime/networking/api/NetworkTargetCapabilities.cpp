#include "Horo/Network/NetworkTargetCapabilities.h"

#include <algorithm>
#include <optional>

namespace Horo::Network {
    namespace {
        constexpr auto AllRoles = static_cast<std::uint32_t>(NetworkProjectRoleSet::Standalone | NetworkProjectRoleSet::Client |
                                                             NetworkProjectRoleSet::ListenServer | NetworkProjectRoleSet::DedicatedServer);
        constexpr auto AllPlatforms = (std::uint32_t{1} << static_cast<std::uint8_t>(NetworkTargetPlatform::Count)) - 1;

        bool ValidRoles(const NetworkProjectRoleSet roles) {
            return (static_cast<std::uint32_t>(roles) & ~AllRoles) == 0;
        }

        bool HasNetworkRoles(const NetworkProjectRoleSet roles) {
            return ContainsNetworkProjectRole(roles, NetworkProjectRole::Client) ||
                   ContainsNetworkProjectRole(roles, NetworkProjectRole::ListenServer) ||
                   ContainsNetworkProjectRole(roles, NetworkProjectRole::DedicatedServer);
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

        NetworkTargetAssessment Reject(NetworkTargetAssessment assessment, const NetworkTargetCapabilityKind kind,
                                       const NetworkTargetFailureReason reason, const NetworkTargetRemediation remediation,
                                       const NetworkProjectRole role = NetworkProjectRole::Count,
                                       const NetworkTransportProviderId provider = {}, const ProtocolId protocol = {},
                                       const NetworkTargetPlatform platform = NetworkTargetPlatform::Count) {
            assessment.diagnostic = {kind, reason, remediation, role, provider, protocol, platform};
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
            if (inventory.build != product.build)
                return Reject(assessment, NetworkTargetCapabilityKind::PackageInventory, NetworkTargetFailureReason::PackageMismatch,
                              NetworkTargetRemediation::RebuildPackage);
            if (inventory.platform != product.platform)
                return Reject(assessment, NetworkTargetCapabilityKind::Platform, NetworkTargetFailureReason::PackageMismatch,
                              NetworkTargetRemediation::RebuildPackage, NetworkProjectRole::Count, {}, {}, product.platform);
            for (const auto &role : assessment.matrix.roles) {
                if (ContainsNetworkProjectRole(product.supportedRoles, role.role) != role.packaged)
                    return Reject(assessment, NetworkTargetCapabilityKind::Role, NetworkTargetFailureReason::PackageMismatch,
                                  NetworkTargetRemediation::RebuildPackage, role.role);
            }
            if (inventory.includesNetworkRuntime != product.includesNetworkRuntime)
                return Reject(assessment, NetworkTargetCapabilityKind::NetworkRuntime, NetworkTargetFailureReason::PackageMismatch,
                              NetworkTargetRemediation::RebuildPackage);
            if (inventory.protocol != product.protocol)
                return Reject(assessment, NetworkTargetCapabilityKind::Protocol, NetworkTargetFailureReason::PackageMismatch,
                              NetworkTargetRemediation::RebuildPackage, NetworkProjectRole::Count, {}, product.protocol.protocol);
            for (std::size_t index = 0; index < product.providerCount; ++index) {
                const auto *actual = FindInventory(inventory, product.providers[index].id);
                if (!actual || *actual != product.providers[index])
                    return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::PackageMismatch,
                                  NetworkTargetRemediation::RebuildPackage, NetworkProjectRole::Count, product.providers[index].id);
            }
            for (std::size_t index = 0; index < inventory.providerCount; ++index) {
                if (!FindProduct(product, inventory.providers[index].id))
                    return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::PackageMismatch,
                                  NetworkTargetRemediation::RebuildPackage, NetworkProjectRole::Count, inventory.providers[index].id);
            }
            return std::nullopt;
        }

        Rejection CheckPublication(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                   const NetworkProductCapabilityManifest &product, const NetworkTargetPackageInventory &inventory,
                                   const NetworkTargetHostFacts &host, const NetworkTargetSelection &selection) {
            if (selection.lifecycle != NetworkTargetLifecycle::Active)
                return Reject(assessment, NetworkTargetCapabilityKind::Lifecycle,
                              selection.lifecycle == NetworkTargetLifecycle::Cancelling ? NetworkTargetFailureReason::Cancelled
                                                                                        : NetworkTargetFailureReason::ShuttingDown,
                              NetworkTargetRemediation::WaitForRestart);
            if (selection.expectedBuild != product.build || selection.expectedProductRevision != product.revision ||
                selection.expectedHostRevision != host.revision || selection.expectedProjectRevision != project.Revision())
                return Reject(assessment, NetworkTargetCapabilityKind::Lifecycle, NetworkTargetFailureReason::Stale,
                              NetworkTargetRemediation::RefreshSnapshot);
            if (const auto failed = CheckPackageInventory(assessment, product, inventory))
                return failed;
            if (host.platform != product.platform)
                return Reject(assessment, NetworkTargetCapabilityKind::Platform, NetworkTargetFailureReason::HostUnsupported,
                              NetworkTargetRemediation::ChooseSupportedHost, NetworkProjectRole::Count, {}, {}, product.platform);
            if (project.Profile().id != product.profile || project.Profile().revision != product.profileRevision)
                return Reject(assessment, NetworkTargetCapabilityKind::ProjectProfile, NetworkTargetFailureReason::Stale,
                              NetworkTargetRemediation::RebuildPackage);
            return std::nullopt;
        }

        Rejection CheckRequirements(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                    const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                    const NetworkTargetRequirements &requirements) {
            for (const auto &role : assessment.matrix.roles) {
                if (!role.projectRequired)
                    continue;
                if (!ContainsNetworkProjectRole(project.SupportedRoles(), role.role))
                    return Reject(assessment, NetworkTargetCapabilityKind::Role, NetworkTargetFailureReason::ProjectUnavailable,
                                  NetworkTargetRemediation::ConfigureProject, role.role);
                if (!role.packaged)
                    return Reject(assessment, NetworkTargetCapabilityKind::Role, NetworkTargetFailureReason::NotPackaged,
                                  NetworkTargetRemediation::RebuildPackage, role.role);
                if (!role.hostSupported)
                    return Reject(assessment, NetworkTargetCapabilityKind::Role, NetworkTargetFailureReason::HostUnsupported,
                                  NetworkTargetRemediation::ChooseSupportedHost, role.role);
            }
            if (!requirements.requiredProvider.IsValid())
                return std::nullopt;
            if (!FindProduct(product, requirements.requiredProvider))
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::NotPackaged,
                              NetworkTargetRemediation::RebuildPackage, NetworkProjectRole::Count, requirements.requiredProvider);
            const auto *requiredHost = FindHost(host, requirements.requiredProvider);
            if (!requiredHost || !requiredHost->installed)
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::NotInstalled,
                              NetworkTargetRemediation::InstallTarget, NetworkProjectRole::Count, requirements.requiredProvider);
            if (!requiredHost->hostSupported)
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::HostUnsupported,
                              NetworkTargetRemediation::ChooseSupportedHost, NetworkProjectRole::Count, requirements.requiredProvider);
            return std::nullopt;
        }

        Rejection CheckSelectedRole(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                    const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                    const NetworkTargetSelection &selection) {
            const auto role = selection.role;
            if (!ContainsNetworkProjectRole(project.SupportedRoles(), role))
                return Reject(assessment, NetworkTargetCapabilityKind::Role, NetworkTargetFailureReason::ProjectUnavailable,
                              NetworkTargetRemediation::ConfigureProject, role);
            if (!ContainsNetworkProjectRole(product.supportedRoles, role))
                return Reject(assessment, NetworkTargetCapabilityKind::Role, NetworkTargetFailureReason::NotPackaged,
                              NetworkTargetRemediation::RebuildPackage, role);
            if (!ContainsNetworkProjectRole(host.supportedRoles, role))
                return Reject(assessment, NetworkTargetCapabilityKind::Role, NetworkTargetFailureReason::HostUnsupported,
                              NetworkTargetRemediation::ChooseSupportedHost, role);
            return std::nullopt;
        }

        Rejection CheckSelectedTransport(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                         const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                         const NetworkTargetSelection &selection) {
            const auto role = selection.role;
            const auto *packaged = FindProduct(product, selection.provider);
            if (!packaged)
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::NotPackaged,
                              NetworkTargetRemediation::RebuildPackage, role, selection.provider);
            if (!ContainsNetworkTargetPlatform(packaged->allowedPlatforms, product.platform))
                return Reject(assessment, NetworkTargetCapabilityKind::Platform, NetworkTargetFailureReason::HostUnsupported,
                              NetworkTargetRemediation::ChooseSupportedHost, role, selection.provider, {}, product.platform);
            const auto *installed = FindHost(host, selection.provider);
            if (!installed || !installed->installed)
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::NotInstalled,
                              NetworkTargetRemediation::InstallTarget, role, selection.provider);
            if (!installed->hostSupported)
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::HostUnsupported,
                              NetworkTargetRemediation::ChooseSupportedHost, role, selection.provider);
            if (!installed->configured)
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::ProjectUnavailable,
                              NetworkTargetRemediation::ConfigureProject, role, selection.provider);
            if (!TransportMeetsProject(packaged->capabilities, project))
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::Incompatible,
                              NetworkTargetRemediation::RebuildPackage, role, selection.provider);
            if (!TransportMeetsProject(installed->capabilities, project))
                return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::Incompatible,
                              NetworkTargetRemediation::SelectAvailableCapability, role, selection.provider);
            return std::nullopt;
        }

        Rejection CheckSelectedProtocol(const NetworkTargetAssessment &assessment, const NetworkProjectSettings &project,
                                        const NetworkProductCapabilityManifest &product, const NetworkTargetHostFacts &host,
                                        const NetworkTargetSelection &selection) {
            if (!SameProtocol(project.Protocol(), product.protocol, selection.protocolVersion))
                return Reject(assessment, NetworkTargetCapabilityKind::Protocol, NetworkTargetFailureReason::Incompatible,
                              NetworkTargetRemediation::RebuildPackage, selection.role, selection.provider, project.Protocol().protocol);
            if (!SameProtocol(project.Protocol(), host.protocol, selection.protocolVersion))
                return Reject(assessment, NetworkTargetCapabilityKind::Protocol, NetworkTargetFailureReason::Incompatible,
                              NetworkTargetRemediation::UpgradeProtocol, selection.role, selection.provider, project.Protocol().protocol);
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
        NetworkTargetAssessment assessment{};
        if (!ValidManifest(product) || !ValidInventory(inventory) || !ValidHost(host) || !ValidRoles(requirements.requiredRoles) ||
            selection.role >= NetworkProjectRole::Count || selection.lifecycle >= NetworkTargetLifecycle::Count)
            return Reject(assessment, NetworkTargetCapabilityKind::Input, NetworkTargetFailureReason::Invalid,
                          NetworkTargetRemediation::CorrectInput);

        assessment.matrix = MakeMatrix(project, product, inventory, host, requirements, selection);
        if (const auto failed = CheckPublication(assessment, project, product, inventory, host, selection))
            return *failed;
        if (const auto failed = CheckRequirements(assessment, project, product, host, requirements))
            return *failed;
        if (const auto failed = CheckSelectedRole(assessment, project, product, host, selection))
            return *failed;
        if (selection.role == NetworkProjectRole::Standalone) {
            if (selection.provider.IsValid() || selection.protocolVersion.IsValid())
                return Reject(assessment, NetworkTargetCapabilityKind::Input, NetworkTargetFailureReason::Invalid,
                              NetworkTargetRemediation::CorrectInput);
            assessment.diagnostic.reason = NetworkTargetFailureReason::None;
            return assessment;
        }
        if (!inventory.includesNetworkRuntime)
            return Reject(assessment, NetworkTargetCapabilityKind::NetworkRuntime, NetworkTargetFailureReason::NotPackaged,
                          NetworkTargetRemediation::RebuildPackage);
        if (!host.networkRuntimeInstalled)
            return Reject(assessment, NetworkTargetCapabilityKind::NetworkRuntime, NetworkTargetFailureReason::NotInstalled,
                          NetworkTargetRemediation::InstallTarget);
        if (!selection.provider.IsValid() || !selection.protocolVersion.IsValid())
            return Reject(assessment, NetworkTargetCapabilityKind::Input, NetworkTargetFailureReason::Invalid,
                          NetworkTargetRemediation::CorrectInput);
        if (requirements.requiredProvider.IsValid() && requirements.requiredProvider != selection.provider)
            return Reject(assessment, NetworkTargetCapabilityKind::Provider, NetworkTargetFailureReason::NotSelected,
                          NetworkTargetRemediation::SelectAvailableCapability, selection.role, requirements.requiredProvider);
        if (const auto failed = CheckSelectedTransport(assessment, project, product, host, selection))
            return *failed;
        if (const auto failed = CheckSelectedProtocol(assessment, project, product, host, selection))
            return *failed;
        assessment.diagnostic.reason = NetworkTargetFailureReason::None;
        return assessment;
    }
}  // namespace Horo::Network
