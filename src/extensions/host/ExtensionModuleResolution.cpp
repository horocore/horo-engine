#include "Horo/Extensions/ExtensionModuleResolution.h"

#include "ExtensionModuleSelection.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>

namespace Horo::Extensions {
    ResolvedExtensionServiceImport::ResolvedExtensionServiceImport(Fields fields)
        : consumerExtensionId_(std::move(fields.consumerExtensionId)), consumerModuleId_(std::move(fields.consumerModuleId)),
          importId_(std::move(fields.importId)), serviceId_(std::move(fields.serviceId)), contractId_(std::move(fields.contractId)),
          providerModuleId_(std::move(fields.providerModuleId)), providerVersion_(std::move(fields.providerVersion)),
          status_(fields.status), required_(fields.required) {}

    /** @copydoc ResolvedExtensionServiceImport::ConsumerExtensionId */
    const std::string &ResolvedExtensionServiceImport::ConsumerExtensionId() const noexcept {
        return consumerExtensionId_;
    }

    /** @copydoc ResolvedExtensionServiceImport::ConsumerModuleId */
    const std::string &ResolvedExtensionServiceImport::ConsumerModuleId() const noexcept {
        return consumerModuleId_;
    }

    /** @copydoc ResolvedExtensionServiceImport::ImportId */
    const std::string &ResolvedExtensionServiceImport::ImportId() const noexcept {
        return importId_;
    }

    /** @copydoc ResolvedExtensionServiceImport::ServiceId */
    const std::string &ResolvedExtensionServiceImport::ServiceId() const noexcept {
        return serviceId_;
    }

    /** @copydoc ResolvedExtensionServiceImport::ContractId */
    const std::string &ResolvedExtensionServiceImport::ContractId() const noexcept {
        return contractId_;
    }

    /** @copydoc ResolvedExtensionServiceImport::ProviderModuleId */
    const std::string &ResolvedExtensionServiceImport::ProviderModuleId() const noexcept {
        return providerModuleId_;
    }

    /** @copydoc ResolvedExtensionServiceImport::ProviderVersion */
    const std::string &ResolvedExtensionServiceImport::ProviderVersion() const noexcept {
        return providerVersion_;
    }

    /** @copydoc ResolvedExtensionServiceImport::Status */
    ExtensionServiceImportStatus ResolvedExtensionServiceImport::Status() const noexcept {
        return status_;
    }

    /** @copydoc ResolvedExtensionServiceImport::IsRequired */
    bool ResolvedExtensionServiceImport::IsRequired() const noexcept {
        return required_;
    }

    namespace {
        struct SemanticVersionCore {
            std::uint64_t major{};
            std::uint64_t minor{};
            std::uint64_t patch{};
            bool prerelease{};
        };

        struct ResolvedImportFields {
            std::string consumerExtensionId;
            std::string consumerModuleId;
            std::string importId;
            std::string serviceId;
            std::string contractId;
            std::string providerModuleId;
            std::string providerVersion;
            ExtensionServiceImportStatus status;
            bool required;
        };

        [[nodiscard]] std::optional<SemanticVersionCore> ParseVersionCore(std::string_view version) {
            SemanticVersionCore result;
            result.prerelease = version.find('-') != std::string_view::npos;
            const std::array parts{&result.major, &result.minor, &result.patch};
            for (std::uint64_t *part : parts) {
                const std::size_t end = version.find_first_of(".-");
                const std::string_view digits = version.substr(0, end);
                const auto [parsedEnd, error] = std::from_chars(digits.data(), digits.data() + digits.size(), *part);
                if (error != std::errc{} || parsedEnd != digits.data() + digits.size())
                    return std::nullopt;
                if (end == std::string_view::npos)
                    break;
                version.remove_prefix(end + 1);
            }
            return result;
        }

        [[nodiscard]] bool IsCompatibleVersion(const std::string_view provided, const std::string_view required) {
            const auto actual = ParseVersionCore(provided);
            const auto minimum = ParseVersionCore(required);
            if (!actual.has_value() || !minimum.has_value() || actual->major != minimum->major)
                return false;
            const auto actualCore = std::tie(actual->minor, actual->patch);
            const auto minimumCore = std::tie(minimum->minor, minimum->patch);
            return actualCore > minimumCore || (actualCore == minimumCore && (!actual->prerelease || minimum->prerelease));
        }

        [[nodiscard]] bool IsVersionAtMost(const std::string_view actualVersion, const std::string_view maximumVersion) {
            const auto actual = ParseVersionCore(actualVersion);
            const auto maximum = ParseVersionCore(maximumVersion);
            if (!actual.has_value() || !maximum.has_value())
                return false;
            return std::tie(actual->major, actual->minor, actual->patch) <= std::tie(maximum->major, maximum->minor, maximum->patch);
        }

        [[nodiscard]] std::string_view PlatformName(const ExtensionHostPlatform platform) noexcept {
            using enum ExtensionHostPlatform;
            if (platform == Windows)
                return "windows";
            if (platform == MacOS)
                return "macos";
            if (platform == Linux)
                return "linux";
            return "unknown";
        }

        [[nodiscard]] std::string_view ArchitectureName(const ExtensionHostArchitecture architecture) noexcept {
            using enum ExtensionHostArchitecture;
            if (architecture == X86_64)
                return "x86_64";
            if (architecture == Arm64)
                return "arm64";
            return "unknown";
        }

        [[nodiscard]] std::string_view BuildProfileName(const ExtensionBuildProfile profile) noexcept {
            using enum ExtensionBuildProfile;
            if (profile == Debug)
                return "debug";
            if (profile == Release)
                return "release";
            return "unknown";
        }

        [[nodiscard]] bool LegacyPlatformMatches(const std::string_view declared, const ExtensionHostEnvironment &host) {
            const std::string_view platform = PlatformName(host.platform);
            if (declared == platform)
                return true;
            return declared == std::string{platform} + '-' + std::string{ArchitectureName(host.architecture)};
        }

        [[nodiscard]] ExtensionModuleCompatibility Rejection(const ExtensionModuleManifest &moduleManifest,
                                                             const ExtensionCompatibilityStatus status, std::string requirement) {
            return {.status = status, .moduleId = moduleManifest.id, .requirement = std::move(requirement)};
        }

        [[nodiscard]] bool IsPresentationOnly(const ExtensionModuleManifest &moduleManifest) {
            return moduleManifest.roles.size() == 1 && moduleManifest.roles.front() == ExtensionModuleRole::EditorPresentation;
        }

        [[nodiscard]] std::string JoinModuleIds(const std::set<std::string, std::less<>> &moduleIds) {
            std::ostringstream output;
            for (auto iterator = moduleIds.begin(); iterator != moduleIds.end(); ++iterator) {
                if (iterator != moduleIds.begin())
                    output << ", ";
                output << *iterator;
            }
            return std::move(output).str();
        }

        struct ExportOwner {
            const ExtensionServiceExportManifest *service{};
            const ExtensionModuleManifest *ownerModule{};
        };

        using ModuleIndex = std::map<std::string, const ExtensionModuleManifest *, std::less<>>;
        using ExportIndex = std::map<std::string, ExportOwner, std::less<>>;
        using DependencyIndex = std::map<std::string, std::set<std::string, std::less<>>, std::less<>>;

        struct ServiceDependencyOutputs {
            DependencyIndex &dependencies;
            DependencyIndex &requiredDependencies;
            std::vector<ResolvedExtensionServiceImport> &bindings;
        };

        [[nodiscard]] bool HasRole(const ExtensionModuleManifest &moduleManifest, const ExtensionModuleRole role) {
            return std::ranges::find(moduleManifest.roles, role) != moduleManifest.roles.end();
        }

        [[nodiscard]] Result<void> ValidateCompatibilityAuthority(const ExtensionManifest &manifest,
                                                                  const ExtensionModuleManifest &moduleManifest) {
            if ((!manifest.sdkAbi.empty() && moduleManifest.abi.has_value()) ||
                (!manifest.platforms.empty() && !moduleManifest.entries.empty()))
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed,
                              "Legacy and typed compatibility authority cannot compete. Involved modules: " + moduleManifest.id + '.'));
            if (!moduleManifest.entry.empty() && !moduleManifest.entries.empty())
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed,
                              "Legacy and typed module entries cannot compete. Involved modules: " + moduleManifest.id + '.'));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateNativeEntrySelectors(const ExtensionModuleManifest &moduleManifest) {
            std::set<std::tuple<ExtensionHostPlatform, ExtensionHostArchitecture, ExtensionBuildProfile>> entrySelectors;
            for (const ExtensionNativeEntryManifest &entry : moduleManifest.entries) {
                if (entry.platform >= ExtensionHostPlatform::Count || entry.architecture >= ExtensionHostArchitecture::Count ||
                    entry.buildProfile >= ExtensionBuildProfile::Count || entry.entry.empty())
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed,
                                  "A native entry selector is malformed. Involved modules: " + moduleManifest.id + '.'));
                if (!entrySelectors.emplace(entry.platform, entry.architecture, entry.buildProfile).second)
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed,
                                  "A native entry selector is duplicated. Involved modules: " + moduleManifest.id + '.'));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateModuleDefinition(const ExtensionManifest &manifest,
                                                            const ExtensionModuleManifest &moduleManifest,
                                                            const ExtensionManifestLimits &limits) {
            if (moduleManifest.roles.empty())
                return Result<void>::Failure(MakeError(ExtensionErrors::ModuleResolutionFailed,
                                                       "A module has no explicit role. Involved modules: " + moduleManifest.id + '.'));
            if (moduleManifest.roles.size() > 5 || moduleManifest.dependencies.size() > limits.maximumModules ||
                moduleManifest.exports.size() > limits.maximumContributions || moduleManifest.imports.size() > limits.maximumContributions)
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed,
                              "Module metadata exceeds a bounded count. Involved modules: " + moduleManifest.id + '.'));
            if (!moduleManifest.exports.empty() && !HasRole(moduleManifest, ExtensionModuleRole::BackendCapability))
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed,
                              "A service export requires backend-capability authority. Involved modules: " + moduleManifest.id + '.'));
            if (auto authorityResult = ValidateCompatibilityAuthority(manifest, moduleManifest); authorityResult.HasError())
                return authorityResult;
            return ValidateNativeEntrySelectors(moduleManifest);
        }

        [[nodiscard]] Result<ModuleIndex> ValidateAndIndexModules(const ExtensionManifest &manifest) {
            const ExtensionManifestLimits limits;
            if (manifest.modules.empty() || manifest.modules.size() > limits.maximumModules)
                return Result<ModuleIndex>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed, "Module count is outside the supported package bounds."));
            if (manifest.contributions.size() > limits.maximumContributions)
                return Result<ModuleIndex>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed, "Contribution count exceeds the supported package bound."));

            ModuleIndex modules;
            for (const ExtensionModuleManifest &moduleManifest : manifest.modules) {
                if (auto validationResult = ValidateModuleDefinition(manifest, moduleManifest, limits); validationResult.HasError())
                    return Result<ModuleIndex>::Failure(validationResult.ErrorValue());
                if (!modules.try_emplace(moduleManifest.id, &moduleManifest).second)
                    return Result<ModuleIndex>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed,
                                  "A module identity is duplicated. Involved modules: " + moduleManifest.id + '.'));
            }
            return Result<ModuleIndex>::Success(std::move(modules));
        }

        [[nodiscard]] Result<void> ValidateContributionOwners(const ExtensionManifest &manifest, const ModuleIndex &modules) {
            std::set<std::string, std::less<>> identities;
            for (const ExtensionContributionManifest &contribution : manifest.contributions) {
                const auto owner = modules.find(contribution.owningModule);
                if (owner == modules.end())
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed,
                                  "A contribution references an absent owner. Involved modules: " + contribution.owningModule + '.'));
                if (!identities.insert(contribution.id).second)
                    return Result<void>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed,
                                  "A contribution identity is duplicated. Involved modules: " + contribution.owningModule + '.'));
                if (contribution.type == "asset.importer" && !HasRole(*owner->second, ExtensionModuleRole::BackendCapability))
                    return Result<void>::Failure(MakeError(ExtensionErrors::ModuleResolutionFailed,
                                                           "An asset importer requires backend-capability authority. Involved modules: " +
                                                               contribution.owningModule + '.'));
                const bool editorSurface = contribution.type == "editor.panel" || contribution.type == "editor.activity_item" ||
                                           contribution.type == "editor.settings_page" || contribution.type == "editor.modal" ||
                                           contribution.type == "editor.modal_page" || contribution.type == "editor.command" ||
                                           contribution.type == "editor.theme";
                if (editorSurface && !HasRole(*owner->second, ExtensionModuleRole::EditorPresentation))
                    return Result<void>::Failure(MakeError(ExtensionErrors::ModuleResolutionFailed,
                                                           "An editor surface requires editor-presentation authority. Involved modules: " +
                                                               contribution.owningModule + '.'));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<ExportIndex> IndexExports(const ExtensionManifest &manifest) {
            ExportIndex exports;
            for (const ExtensionModuleManifest &moduleManifest : manifest.modules) {
                for (const ExtensionServiceExportManifest &service : moduleManifest.exports) {
                    const auto [position, inserted] = exports.try_emplace(service.id, ExportOwner{&service, &moduleManifest});
                    if (!inserted)
                        return Result<ExportIndex>::Failure(
                            MakeError(ExtensionErrors::ModuleResolutionFailed,
                                      "Duplicate service export '" + service.id +
                                          "'. Involved modules: " + std::min(position->second.ownerModule->id, moduleManifest.id) + ", " +
                                          std::max(position->second.ownerModule->id, moduleManifest.id) + '.'));
                }
            }
            return Result<ExportIndex>::Success(std::move(exports));
        }

        [[nodiscard]] Result<void> AddDeclaredDependencies(const ExtensionManifest &manifest, const ModuleIndex &modules,
                                                           DependencyIndex &dependencies, DependencyIndex &requiredDependencies) {
            for (const ExtensionModuleManifest &moduleManifest : manifest.modules) {
                auto &moduleDependencies = dependencies[moduleManifest.id];
                auto &requiredModuleDependencies = requiredDependencies[moduleManifest.id];
                for (const std::string &dependency : moduleManifest.dependencies) {
                    if (!modules.contains(dependency))
                        return Result<void>::Failure(
                            MakeError(ExtensionErrors::ModuleResolutionFailed, "Missing local module dependency '" + dependency +
                                                                                   "'. Involved modules: " + moduleManifest.id + '.'));
                    if (dependency == moduleManifest.id)
                        return Result<void>::Failure(
                            MakeError(ExtensionErrors::ModuleResolutionFailed,
                                      "A module cannot depend on itself. Involved modules: " + moduleManifest.id + '.'));
                    moduleDependencies.insert(dependency);
                    requiredModuleDependencies.insert(dependency);
                }
            }
            return Result<void>::Success();
        }

        template <typename MakeResolvedImport>
        [[nodiscard]] Result<void> AddServiceDependency(const ExtensionManifest &manifest, const ExtensionModuleManifest &moduleManifest,
                                                        const ExtensionServiceImportManifest &serviceImport, const ExportIndex &exports,
                                                        ServiceDependencyOutputs outputs, const MakeResolvedImport &makeResolvedImport) {
            auto makeBinding = [&](std::string providerModuleId, std::string providerVersion, const ExtensionServiceImportStatus status) {
                return makeResolvedImport(ResolvedImportFields{manifest.id, moduleManifest.id, serviceImport.id, serviceImport.service,
                                                               serviceImport.contract, std::move(providerModuleId),
                                                               std::move(providerVersion), status, serviceImport.required});
            };
            const auto found = exports.find(serviceImport.service);
            if (found == exports.end()) {
                if (!serviceImport.required) {
                    outputs.bindings.push_back(makeBinding({}, {}, ExtensionServiceImportStatus::Unavailable));
                    return Result<void>::Success();
                }
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed,
                              "Missing service export '" + serviceImport.service + "'. Involved modules: " + moduleManifest.id + '.'));
            }
            const ExportOwner &provider = found->second;
            if (provider.ownerModule->id == moduleManifest.id)
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed,
                              "A module cannot import its own service. Involved modules: " + moduleManifest.id + '.'));
            if (provider.service->contract != serviceImport.contract ||
                !IsCompatibleVersion(provider.service->version, serviceImport.minimumVersion)) {
                if (!serviceImport.required) {
                    outputs.bindings.push_back(
                        makeBinding(provider.ownerModule->id, provider.service->version, ExtensionServiceImportStatus::Incompatible));
                    return Result<void>::Success();
                }
                const std::set<std::string, std::less<>> involved{moduleManifest.id, provider.ownerModule->id};
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ModuleResolutionFailed, "Incompatible service export '" + serviceImport.service +
                                                                           "'. Involved modules: " + JoinModuleIds(involved) + '.'));
            }
            outputs.bindings.push_back(
                makeBinding(provider.ownerModule->id, provider.service->version, ExtensionServiceImportStatus::Bound));
            outputs.dependencies[moduleManifest.id].insert(provider.ownerModule->id);
            if (serviceImport.required)
                outputs.requiredDependencies[moduleManifest.id].insert(provider.ownerModule->id);
            return Result<void>::Success();
        }

        template <typename MakeResolvedImport>
        [[nodiscard]] Result<void> AddServiceDependencies(const ExtensionManifest &manifest, const ExportIndex &exports,
                                                          DependencyIndex &dependencies, DependencyIndex &requiredDependencies,
                                                          std::vector<ResolvedExtensionServiceImport> &bindings,
                                                          const MakeResolvedImport &makeResolvedImport,
                                                          const std::set<std::string, std::less<>> *selected) {
            std::set<std::pair<std::string, std::string>> importIds;
            for (const ExtensionModuleManifest &moduleManifest : manifest.modules) {
                if (selected && !selected->contains(moduleManifest.id))
                    continue;
                for (const ExtensionServiceImportManifest &serviceImport : moduleManifest.imports) {
                    if (!importIds.emplace(moduleManifest.id, serviceImport.id).second)
                        return Result<void>::Failure(
                            MakeError(ExtensionErrors::ModuleResolutionFailed,
                                      "Duplicate service import '" + serviceImport.id + "'. Involved modules: " + moduleManifest.id + '.'));
                    if (auto added = AddServiceDependency(manifest, moduleManifest, serviceImport, exports,
                                                          ServiceDependencyOutputs{dependencies, requiredDependencies, bindings},
                                                          makeResolvedImport);
                        added.HasError())
                        return added;
                }
            }
            std::ranges::sort(bindings, [](const auto &left, const auto &right) {
                return std::tie(left.ConsumerModuleId(), left.ImportId()) < std::tie(right.ConsumerModuleId(), right.ImportId());
            });
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<std::string>> SortModules(const DependencyIndex &dependencies) {
            DependencyIndex remaining = dependencies;
            std::vector<std::string> order;
            order.reserve(remaining.size());
            while (!remaining.empty()) {
                const auto ready = std::ranges::find_if(remaining, [](const auto &entry) {
                    return entry.second.empty();
                });
                if (ready == remaining.end()) {
                    std::set<std::string, std::less<>> involved;
                    for (const auto &[moduleId, moduleDependencies] : remaining) {
                        involved.insert(moduleId);
                        involved.insert(moduleDependencies.begin(), moduleDependencies.end());
                    }
                    return Result<std::vector<std::string>>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed,
                                  "The local module dependency graph contains a cycle. Involved modules: " + JoinModuleIds(involved) +
                                      '.'));
                }
                const std::string moduleId = ready->first;
                order.push_back(moduleId);
                remaining.erase(ready);
                for (auto &[candidate, candidateDependencies] : remaining) {
                    static_cast<void>(candidate);
                    candidateDependencies.erase(moduleId);
                }
            }
            return Result<std::vector<std::string>>::Success(std::move(order));
        }

        [[nodiscard]] Result<void> ValidateHeadlessDependencies(const DependencyIndex &dependencies, const ModuleIndex &modules) {
            for (const auto &[moduleId, moduleDependencies] : dependencies) {
                if (IsPresentationOnly(*modules.at(moduleId)))
                    continue;
                for (const std::string &dependency : moduleDependencies) {
                    if (IsPresentationOnly(*modules.at(dependency))) {
                        const std::set<std::string, std::less<>> involved{moduleId, dependency};
                        return Result<void>::Failure(
                            MakeError(ExtensionErrors::ModuleResolutionFailed,
                                      "A headless module requires an excluded presentation-only module. Involved modules: " +
                                          JoinModuleIds(involved) + '.'));
                    }
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] std::optional<ExtensionModuleCompatibility> EvaluateEngineCompatibility(const ExtensionManifest &manifest,
                                                                                              const ExtensionModuleManifest &moduleManifest,
                                                                                              const ExtensionHostEnvironment &host) {
            if ((!manifest.engineMin.empty() && !IsCompatibleVersion(host.engineVersion, manifest.engineMin)) ||
                (!manifest.engineMax.empty() && !IsVersionAtMost(host.engineVersion, manifest.engineMax)))
                return Rejection(moduleManifest, ExtensionCompatibilityStatus::EngineVersionUnsupported, std::string{host.engineVersion});
            return std::nullopt;
        }

        [[nodiscard]] std::optional<ExtensionModuleCompatibility> EvaluateAbiCompatibility(const ExtensionManifest &manifest,
                                                                                           const ExtensionModuleManifest &moduleManifest,
                                                                                           const ExtensionHostEnvironment &host) {
            if (const ExtensionAbiRequirement abi = moduleManifest.abi.value_or(ExtensionAbiRequirement{});
                (!manifest.sdkAbi.empty() && manifest.sdkAbi != "horo.extension-1") || abi.major != host.abiMajor ||
                abi.minimumMinor > host.abiMinor)
                return Rejection(moduleManifest, ExtensionCompatibilityStatus::AbiUnsupported,
                                 std::format("{}.{}", abi.major, abi.minimumMinor));
            return std::nullopt;
        }

        [[nodiscard]] std::optional<ExtensionModuleCompatibility> EvaluateCapabilities(const ExtensionModuleManifest &moduleManifest,
                                                                                       const ExtensionHostEnvironment &host) {
            if (const auto missing = std::ranges::find_if(moduleManifest.requiredCapabilities,
                                                          [&host](const std::string &capability) {
                return std::ranges::find(host.capabilities, capability) == host.capabilities.end();
            });
                missing != moduleManifest.requiredCapabilities.end())
                return Rejection(moduleManifest, ExtensionCompatibilityStatus::CapabilityUnavailable, *missing);
            return std::nullopt;
        }

        [[nodiscard]] ExtensionModuleCompatibility SelectNativeEntry(const ExtensionManifest &manifest,
                                                                     const ExtensionModuleManifest &moduleManifest,
                                                                     const ExtensionHostEnvironment &host) {
            using enum ExtensionCompatibilityStatus;
            if (moduleManifest.entries.empty()) {
                if (!manifest.platforms.empty() && std::ranges::none_of(manifest.platforms, [&host](const std::string_view platform) {
                    return LegacyPlatformMatches(platform, host);
                }))
                    return Rejection(moduleManifest, HostPlatformUnsupported, std::string{PlatformName(host.platform)});
                return {.moduleId = moduleManifest.id, .selectedEntry = moduleManifest.entry};
            }

            if (const auto platform = std::ranges::find(moduleManifest.entries, host.platform, &ExtensionNativeEntryManifest::platform);
                platform == moduleManifest.entries.end())
                return Rejection(moduleManifest, HostPlatformUnsupported, std::string{PlatformName(host.platform)});
            if (const auto architecture = std::ranges::find_if(moduleManifest.entries,
                                                               [&host](const ExtensionNativeEntryManifest &entry) {
                return entry.platform == host.platform && entry.architecture == host.architecture;
            });
                architecture == moduleManifest.entries.end())
                return Rejection(moduleManifest, HostArchitectureUnsupported, std::string{ArchitectureName(host.architecture)});
            const auto selected = std::ranges::find_if(moduleManifest.entries, [&host](const ExtensionNativeEntryManifest &entry) {
                return entry.platform == host.platform && entry.architecture == host.architecture &&
                       entry.buildProfile == host.buildProfile;
            });
            if (selected == moduleManifest.entries.end())
                return Rejection(moduleManifest, BuildProfileUnsupported, std::string{BuildProfileName(host.buildProfile)});
            return {.moduleId = moduleManifest.id, .selectedEntry = selected->entry};
        }

        [[nodiscard]] Result<void> ApplyHostProfile(DependencyIndex &dependencies, const DependencyIndex &requiredDependencies,
                                                    const ModuleIndex &modules, const ExtensionHostProfile profile) {
            if (profile != ExtensionHostProfile::Headless)
                return Result<void>::Success();
            if (auto headlessResult = ValidateHeadlessDependencies(requiredDependencies, modules); headlessResult.HasError())
                return headlessResult;
            std::erase_if(dependencies, [&modules](const auto &entry) {
                return IsPresentationOnly(*modules.at(entry.first));
            });
            for (auto &[moduleId, moduleDependencies] : dependencies) {
                static_cast<void>(moduleId);
                std::erase_if(moduleDependencies, [&modules](const std::string &dependency) {
                    return IsPresentationOnly(*modules.at(dependency));
                });
            }
            return Result<void>::Success();
        }

        template <typename MakeResolvedImport>
        [[nodiscard]] Result<ExtensionModulePlan> BuildCompatiblePlan(std::vector<std::string> &&order,
                                                                      std::vector<ResolvedExtensionServiceImport> &&bindings,
                                                                      const ExtensionManifest &manifest, const ModuleIndex &modules,
                                                                      const ExtensionHostEnvironment &host,
                                                                      const MakeResolvedImport &makeResolvedImport) {
            ExtensionModulePlan plan{.moduleIds = std::move(order), .serviceImports = std::move(bindings)};
            plan.selectedEntries.reserve(plan.moduleIds.size());
            for (const std::string &moduleId : plan.moduleIds) {
                const ExtensionModuleCompatibility compatibility =
                    EvaluateExtensionModuleCompatibility(manifest, *modules.at(moduleId), host);
                if (!compatibility.IsCompatible())
                    return Result<ExtensionModulePlan>::Failure(
                        MakeError(ExtensionErrors::ModuleResolutionFailed,
                                  "Module '" + moduleId + "' rejected compatibility requirement '" + compatibility.requirement + "'."));
                plan.selectedEntries.push_back(compatibility.selectedEntry);
            }

            const std::set<std::string, std::less<>> selected(plan.moduleIds.begin(), plan.moduleIds.end());
            std::erase_if(plan.serviceImports, [&selected](const ResolvedExtensionServiceImport &binding) {
                return !selected.contains(binding.ConsumerModuleId());
            });
            for (ResolvedExtensionServiceImport &binding : plan.serviceImports) {
                if (!binding.ProviderModuleId().empty() && !selected.contains(binding.ProviderModuleId())) {
                    binding = makeResolvedImport(ResolvedImportFields{binding.ConsumerExtensionId(),
                                                                      binding.ConsumerModuleId(),
                                                                      binding.ImportId(),
                                                                      binding.ServiceId(),
                                                                      binding.ContractId(),
                                                                      {},
                                                                      {},
                                                                      ExtensionServiceImportStatus::Unavailable,
                                                                      binding.IsRequired()});
                }
            }
            for (const ExtensionContributionManifest &contribution : manifest.contributions) {
                if (selected.contains(contribution.owningModule))
                    plan.contributions.push_back(contribution);
            }
            std::ranges::sort(plan.contributions, {}, &ExtensionContributionManifest::id);
            return Result<ExtensionModulePlan>::Success(std::move(plan));
        }

        /** @brief Prunes only dependency edges outside the already validated explicit module closure. */
        void RestrictDependencies(DependencyIndex &dependencies, DependencyIndex &requiredDependencies,
                                  const std::set<std::string, std::less<>> &selected) {
            std::erase_if(dependencies, [&selected](const auto &entry) {
                return !selected.contains(entry.first);
            });
            std::erase_if(requiredDependencies, [&selected](const auto &entry) {
                return !selected.contains(entry.first);
            });
            for (auto &[id, providers] : dependencies) {
                (void)id;
                std::erase_if(providers, [&selected](const auto &provider) {
                    return !selected.contains(provider);
                });
            }
        }

    }  // namespace

    /** @copydoc EvaluateExtensionModuleCompatibility */
    ExtensionModuleCompatibility EvaluateExtensionModuleCompatibility(const ExtensionManifest &manifest,
                                                                      const ExtensionModuleManifest &moduleManifest,
                                                                      const ExtensionHostEnvironment &host) {
        if (auto engine = EvaluateEngineCompatibility(manifest, moduleManifest, host))
            return std::move(*engine);
        if (auto abi = EvaluateAbiCompatibility(manifest, moduleManifest, host))
            return std::move(*abi);
        if (auto capability = EvaluateCapabilities(moduleManifest, host))
            return std::move(*capability);
        return SelectNativeEntry(manifest, moduleManifest, host);
    }

    /** @copydoc ResolveExtensionModules */
    Result<ExtensionModulePlan> ResolveExtensionModules(const ExtensionManifest &manifest, const ExtensionHostEnvironment &host,
                                                        const std::span<const std::string> selectedModules) {
        const auto makeResolvedImport = [](ResolvedImportFields fields) {
            return ResolvedExtensionServiceImport{ResolvedExtensionServiceImport::Fields{
                .consumerExtensionId = std::move(fields.consumerExtensionId),
                .consumerModuleId = std::move(fields.consumerModuleId),
                .importId = std::move(fields.importId),
                .serviceId = std::move(fields.serviceId),
                .contractId = std::move(fields.contractId),
                .providerModuleId = std::move(fields.providerModuleId),
                .providerVersion = std::move(fields.providerVersion),
                .status = fields.status,
                .required = fields.required,
            }};
        };
        auto modulesResult = ValidateAndIndexModules(manifest);
        if (modulesResult.HasError())
            return Result<ExtensionModulePlan>::Failure(modulesResult.ErrorValue());
        const ModuleIndex &modules = modulesResult.Value();
        if (auto contributionResult = ValidateContributionOwners(manifest, modules); contributionResult.HasError())
            return Result<ExtensionModulePlan>::Failure(contributionResult.ErrorValue());

        auto exportsResult = IndexExports(manifest);
        if (exportsResult.HasError())
            return Result<ExtensionModulePlan>::Failure(exportsResult.ErrorValue());

        DependencyIndex dependencies;
        DependencyIndex requiredDependencies;
        std::vector<ResolvedExtensionServiceImport> bindings;
        if (auto result = AddDeclaredDependencies(manifest, modules, dependencies, requiredDependencies); result.HasError())
            return Result<ExtensionModulePlan>::Failure(result.ErrorValue());
        auto closure = Detail::SelectModuleClosure(manifest, selectedModules);
        if (closure.HasError())
            return Result<ExtensionModulePlan>::Failure(closure.ErrorValue());
        const auto &selected = closure.Value();
        if (auto result = AddServiceDependencies(manifest, exportsResult.Value(), dependencies, requiredDependencies, bindings,
                                                 makeResolvedImport, selectedModules.empty() ? nullptr : &selected);
            result.HasError())
            return Result<ExtensionModulePlan>::Failure(result.ErrorValue());

        if (!selectedModules.empty())
            RestrictDependencies(dependencies, requiredDependencies, selected);

        if (auto profileResult = ApplyHostProfile(dependencies, requiredDependencies, modules, host.profile); profileResult.HasError())
            return Result<ExtensionModulePlan>::Failure(profileResult.ErrorValue());

        auto orderResult = SortModules(dependencies);
        if (orderResult.HasError())
            return Result<ExtensionModulePlan>::Failure(orderResult.ErrorValue());
        std::vector<std::string> order = std::move(orderResult).Value();

        return BuildCompatiblePlan(std::move(order), std::move(bindings), manifest, modules, host, makeResolvedImport);
    }
}  // namespace Horo::Extensions
