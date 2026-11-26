#include "Horo/Extensions/ExtensionAbi.h"
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "Horo/Packages/PackageMigration.h"
#include "PackageActivationComposition.h"

#include <algorithm>
#include <array>
#include <bit>
#include <map>
#include <ranges>
#include <set>

namespace Horo::Packages::Detail {
    namespace {
        /** @brief Binds selected native artifacts and trust approval to one verified package descriptor. */
        Result<PackageActivationCandidate> Candidate(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                     const std::size_t packageIndex, const EnabledPackageExtension &enabled,
                                                     const Extensions::ExtensionHostEnvironment &host,
                                                     const IPackageExtensionTrust &trust) {
            const auto &package = install->Graph()->packages[packageIndex];
            auto descriptor = ReadDeclaredPackageDescriptor(package, enabled.descriptor);
            if (descriptor.HasError())
                return Result<PackageActivationCandidate>::Failure(descriptor.ErrorValue());
            auto declared = std::move(descriptor).Value();
            const auto &root = declared.root;
            auto &manifest = declared.manifest;
            const auto entries = package.archive->Manifest().Entries();
            // The provider-only profile has one RAII publication owner. Independent catalogs cannot commit a mixed batch atomically.
            const bool providerOnly =
                manifest.contributions.size() == 1 && manifest.contributions.front().type == "platform.services.provider";
            if (!providerOnly && !std::ranges::all_of(manifest.contributions, [](const auto &contribution) {
                return contribution.type == "asset.importer";
            }))
                return Result<PackageActivationCandidate>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto plan = Extensions::ResolveExtensionModules(manifest, host);
            if (plan.HasError())
                return Result<PackageActivationCandidate>::Failure(plan.ErrorValue());
            if (plan.Value().moduleIds.empty())
                return Result<PackageActivationCandidate>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            for (const auto &entry : plan.Value().selectedEntries) {
                auto path = PackagePath::Parse(root + '/' + SelectedPackageNativeEntry(entry));
                if (path.HasError())
                    return Result<PackageActivationCandidate>::Failure(path.ErrorValue());
                const auto file = std::ranges::find(entries, path.Value().Value(), [](const PackageFileEntry &item) {
                    return item.id.path.Value();
                });
                if (file == entries.end() || !file->contributionRoot || file->contributionRoot->Value() != root)
                    return Result<PackageActivationCandidate>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            }
            auto approval = trust.Approve(package, manifest);
            if (approval.HasError())
                return Result<PackageActivationCandidate>::Failure(approval.ErrorValue());
            if (approval.Value().artifactDigest != package.archive->Digest() ||
                approval.Value().manifestDigest != package.archive->PackageManifestDigest())
                return Result<PackageActivationCandidate>::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
            return Result<PackageActivationCandidate>::Success({std::move(install),
                                                                packageIndex,
                                                                enabled.descriptor,
                                                                std::move(manifest),
                                                                std::move(plan).Value(),
                                                                std::move(approval).Value(),
                                                                {}});
        }

        using PendingCandidates = std::map<std::string, PackageActivationCandidate, std::less<>>;

        /** @brief Rejects missing or duplicate enablement selections before any dependency ordering or native loading. */
        Result<PendingCandidates> SelectCandidates(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                   std::span<const EnabledPackageExtension> enabled,
                                                   const Extensions::ExtensionHostEnvironment &host, const IPackageExtensionTrust &trust) {
            PendingCandidates pending;
            const auto &packages = install->Graph()->packages;
            for (const auto &selection : enabled) {
                const auto package = std::ranges::find(packages, selection.package.Value(), [](const PackageRestorePackage &item) {
                    return item.lock.package.Value();
                });
                if (package == packages.end() || pending.contains(selection.package.Value()))
                    return Result<PendingCandidates>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
                auto candidate = Candidate(install, static_cast<std::size_t>(package - packages.begin()), selection, host, trust);
                if (candidate.HasError())
                    return Result<PendingCandidates>::Failure(candidate.ErrorValue());
                pending.try_emplace(selection.package.Value(), std::move(candidate).Value());
            }
            return Result<PendingCandidates>::Success(std::move(pending));
        }

        /** @brief Checks every immutable dependency before deciding whether its selected provider is still pending. */
        Result<bool> DependenciesReady(std::span<const LockedPackageReference> dependencies,
                                       std::span<const PackageRestorePackage> packages, const PendingCandidates &pending) {
            bool ready = true;
            for (const auto &dependency : dependencies) {
                if (const auto provider = std::ranges::find(packages, dependency.package.Value(),
                                                            [](const PackageRestorePackage &package) {
                    return package.lock.package.Value();
                });
                    provider == packages.end() || provider->lock.version != dependency.version)
                    return Result<bool>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
                if (pending.contains(dependency.package.Value()))
                    ready = false;
            }
            return Result<bool>::Success(ready);
        }

        /** @brief Retains selected provider identities in the same order as the installed dependency list. */
        void BindSelectedProviders(PackageActivationCandidate &candidate, std::span<const LockedPackageReference> dependencies,
                                   const std::set<std::string, std::less<>> &selected) {
            for (const auto &dependency : dependencies)
                if (selected.contains(dependency.package.Value()))
                    candidate.providers.push_back(dependency.package.Value());
        }

        /** @brief Orders selected providers first and rejects missing, incompatible or cyclic installed dependencies. */
        Result<std::vector<PackageActivationCandidate>> OrderCandidates(PendingCandidates pending,
                                                                        std::span<const PackageRestorePackage> packages) {
            const std::set<std::string, std::less<>> selected = [&pending] {
                std::set<std::string, std::less<>> result;
                for (const auto &[id, candidate] : pending) {
                    (void)candidate;
                    result.insert(id);
                }
                return result;
            }();
            std::vector<PackageActivationCandidate> ordered;
            ordered.reserve(pending.size());
            while (!pending.empty()) {
                bool advanced = false;
                for (auto item = pending.begin(); item != pending.end();) {
                    const auto &dependencies = packages[item->second.packageIndex].lock.dependencies;
                    auto readiness = DependenciesReady(dependencies, packages, pending);
                    if (readiness.HasError())
                        return Result<std::vector<PackageActivationCandidate>>::Failure(readiness.ErrorValue());
                    if (!readiness.Value()) {
                        ++item;
                        continue;
                    }
                    BindSelectedProviders(item->second, dependencies, selected);
                    ordered.push_back(std::move(item->second));
                    item = pending.erase(item);
                    advanced = true;
                }
                if (!advanced)
                    return Result<std::vector<PackageActivationCandidate>>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            }
            return Result<std::vector<PackageActivationCandidate>>::Success(std::move(ordered));
        }
    }  // namespace

    /** @brief Matches the legacy native manager's explicit platform suffix without scanning for alternatives. */
    std::string SelectedPackageNativeEntry(std::string entry) {
        if (std::filesystem::path{entry}.has_extension())
            return entry;
#if defined(_WIN32)
        return entry + ".dll";
#elif defined(__APPLE__)
        return entry + ".dylib";
#else
        return entry + ".so";
#endif
    }

    /** @brief Supplies the same current process tuple used by the concrete native extension manager. */
    Extensions::ExtensionHostEnvironment PackageHostEnvironment(const PackageLifecycleConfiguration &configuration,
                                                                const std::span<const std::string_view> capabilities) {
        using namespace Extensions;
#if defined(_WIN32)
        constexpr auto platform = ExtensionHostPlatform::Windows;
#elif defined(__APPLE__)
        constexpr auto platform = ExtensionHostPlatform::MacOS;
#else
        constexpr auto platform = ExtensionHostPlatform::Linux;
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        constexpr auto architecture = ExtensionHostArchitecture::Arm64;
#else
        constexpr auto architecture = ExtensionHostArchitecture::X86_64;
#endif
#if defined(NDEBUG)
        constexpr auto build = ExtensionBuildProfile::Release;
#else
        constexpr auto build = ExtensionBuildProfile::Debug;
#endif
        return {configuration.profile, platform, architecture, build, "0.1.0", HORO_EXTENSION_ABI_VERSION, HORO_EXTENSION_ABI_MINOR_VERSION,
                capabilities};
    }

    /** @brief Reads the declared descriptor and binds its identity to the exact installed package. */
    Result<DeclaredPackageDescriptor> ReadDeclaredPackageDescriptor(const PackageRestorePackage &package,
                                                                    const PackagePath &descriptorPath) {
        const auto &name = descriptorPath.Value();
        const auto slash = name.rfind('/');
        if (slash == std::string::npos || !name.ends_with("/extension.json"))
            return Result<DeclaredPackageDescriptor>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        std::string root = name.substr(0U, slash);
        const auto entries = package.archive->Manifest().Entries();
        if (const auto descriptor = std::ranges::find(entries, name,
                                                      [](const PackageFileEntry &entry) {
            return entry.id.path.Value();
        });
            descriptor == entries.end() || !descriptor->contributionRoot || descriptor->contributionRoot->Value() != root ||
            !root.starts_with("extensions/") || root.substr(11U).find('/') != std::string::npos ||
            std::ranges::find(package.lock.contributions, root.substr(11U)) == package.lock.contributions.end())
            return Result<DeclaredPackageDescriptor>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        auto bytes = package.archive->ReadDeclaredFile(descriptorPath, Extensions::ExtensionManifestLimits{}.maximumDocumentBytes);
        if (bytes.HasError())
            return Result<DeclaredPackageDescriptor>::Failure(bytes.ErrorValue());
        const auto &content = bytes.Value();
        std::string text(content.size(), '\0');
        std::ranges::transform(content, text.begin(), [](const std::byte value) {
            return std::bit_cast<char>(value);
        });
        auto manifest = Extensions::ParseExtensionManifest(text);
        if (manifest.HasError())
            return Result<DeclaredPackageDescriptor>::Failure(manifest.ErrorValue());
        if (manifest.Value().id != package.lock.package.Value() || manifest.Value().version != package.lock.version.ToString())
            return Result<DeclaredPackageDescriptor>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        return Result<DeclaredPackageDescriptor>::Success({std::move(root), std::move(manifest).Value()});
    }

    /** @copydoc PreparePackageCandidates */
    Result<std::vector<PackageActivationCandidate>> PreparePackageCandidates(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                                             const std::span<const EnabledPackageExtension> enabled,
                                                                             const IPackageExtensionTrust &trust,
                                                                             const PackageLifecycleConfiguration &configuration) {
        std::vector<std::string_view> capabilities;
        for (const auto &capability : configuration.capabilities)
            capabilities.push_back(capability);
        const auto host = PackageHostEnvironment(configuration, capabilities);
        auto pending = SelectCandidates(install, enabled, host, trust);
        if (pending.HasError())
            return Result<std::vector<PackageActivationCandidate>>::Failure(pending.ErrorValue());
        return OrderCandidates(std::move(pending).Value(), install->Graph()->packages);
    }
}  // namespace Horo::Packages::Detail

namespace Horo::Packages {
    namespace {
        constexpr std::string_view ExtensionPrefix = "extensions/";
        constexpr std::string_view DescriptorSuffix = "/extension.json";
        using RemovalArtifacts = std::map<std::string, std::set<std::string, std::less<>>, std::less<>>;

        /** @brief Identifies descriptor candidates in the sealed inventory before validating their ownership. */
        bool IsExtensionDescriptor(const PackageFileEntry &file) {
            const std::string_view path = file.id.path.Value();
            return path.starts_with(ExtensionPrefix) && path.ends_with(DescriptorSuffix);
        }

        /** @brief Identifies resources controlled by the exact native gameplay declaration. */
        bool IsGameplayRemovalFile(const PackageFileEntry &file, const RemovalArtifacts &byRoot) {
            const auto &path = file.id.path.Value();
            if (path == "horo-package.toml" || path == PackageAssetRenameDeclarationPath || path.starts_with("assets/"))
                return !file.executable;
            if (!file.contributionRoot)
                return false;
            const auto owner = byRoot.find(file.contributionRoot->Value());
            if (owner == byRoot.end())
                return false;
            const auto &[root, artifacts] = *owner;
            if (artifacts.contains(path))
                return true;
            if (file.executable)
                return false;
            constexpr std::string_view SignatureSuffix = ".signature.json";
            const std::string_view name{path};
            if (name.ends_with(SignatureSuffix))
                return artifacts.contains(name.substr(0, name.size() - SignatureSuffix.size()));
            return (name.starts_with(root) && name.substr(root.size()) == DescriptorSuffix) || name.ends_with(".horo_script");
        }

        /** @brief Tests a single native role with no intermodule service or dependency contract. */
        bool IsStandaloneNativeRole(const Extensions::ExtensionModuleManifest &module, const Extensions::ExtensionModuleRole role) {
            return module.kind == "native" && module.imports.empty() && module.exports.empty() &&
                   std::ranges::equal(module.roles, std::array{role});
        }

        /** @brief Recognizes the backend surface covered by the project importer sidecar scan. */
        bool IsImporterOnlyModule(const Extensions::ExtensionModuleManifest &module, const Extensions::ExtensionManifest &manifest) {
            if (!IsStandaloneNativeRole(module, Extensions::ExtensionModuleRole::BackendCapability) || !module.dependencies.empty() ||
                !module.requiredCapabilities.empty())
                return false;
            bool hasImporter = false;
            for (const auto &contribution : manifest.contributions) {
                if (contribution.owningModule != module.id)
                    continue;
                if (contribution.type != "asset.importer")
                    return false;
                hasImporter = true;
            }
            return hasImporter;
        }

        /** @brief Accounts for executable entries and names owners outside the admitted withdrawal routes. */
        void CollectRemovalArtifacts(
            const Detail::DeclaredPackageDescriptor &declared, const PackagePath &descriptorPath, const bool allowImporters,
            std::set<std::string, std::less<>> &artifacts, std::vector<PackageRuntimeRoleDeclaration> &roles, std::vector<Diagnostic> &blockers) {
            const SourceLocation descriptor{descriptorPath.Value()};
            const auto &manifest = declared.manifest;
            std::set<std::string_view, std::less<>> importerModules;
            for (const auto &module : manifest.modules) {
                if (IsStandaloneNativeRole(module, Extensions::ExtensionModuleRole::RuntimeParticipant)) {
                    roles.emplace_back(descriptorPath, module.id);
                } else if (allowImporters && IsImporterOnlyModule(module, manifest)) {
                    importerModules.insert(module.id);
                } else {
                    blockers.emplace_back(DiagnosticCode{"packages.lifecycle.removal_module_owner_required"}, DiagnosticSeverity::Error,
                                          "Module requires its registered lifecycle owner.", descriptor, module.id);
                    continue;
                }
                if (!module.entry.empty())
                    artifacts.emplace(declared.root + '/' + Detail::SelectedPackageNativeEntry(module.entry));
                for (const auto &variant : module.entries)
                    artifacts.emplace(declared.root + '/' + variant.entry);
            }
            for (const auto &contribution : manifest.contributions)
                if (!importerModules.contains(contribution.owningModule))
                    blockers.emplace_back(DiagnosticCode{"packages.lifecycle.removal_contribution_owner_required"},
                                          DiagnosticSeverity::Error, contribution.type + " / " + contribution.owningModule,
                                          descriptor, contribution.id);
        }

        /** @brief Identifies each archive member outside the gameplay owner's file authority. */
        void CollectUnownedFiles(const ValidatedPackageArchive &archive, const RemovalArtifacts &artifacts,
                                 std::vector<Diagnostic> &blockers) {
            for (const auto &file : archive.Manifest().Entries()) {
                if (IsGameplayRemovalFile(file, artifacts))
                    continue;
                const auto &path = file.id.path.Value();
                blockers.emplace_back(DiagnosticCode{"packages.lifecycle.removal_file_owner_required"}, DiagnosticSeverity::Error,
                                      "Archive member requires a separate lifecycle owner.", SourceLocation{path},
                                      file.contributionRoot ? file.contributionRoot->Value() : path);
            }
        }

        /** @brief Reports locked contributions that no validated extension descriptor accounts for. */
        void CollectUnownedContributions(const LockedPackage &lock, const RemovalArtifacts &artifacts, std::vector<Diagnostic> &blockers) {
            for (const auto &type : lock.contributions)
                if (type != "assets" && !artifacts.contains(std::string{ExtensionPrefix} + type))
                    blockers.emplace_back(DiagnosticCode{"packages.lifecycle.removal_contribution_owner_required"}, DiagnosticSeverity::Error,
                                          type, SourceLocation{"packages.lock"}, lock.package.Value());
        }
    }  // namespace

    /** @brief Verifies every archived executable member before allowing its role-specific reference review. */
    static Result<std::vector<PackageRuntimeRoleDeclaration>> InspectRemovalRoles(const PackageRestorePackage &package,
                                                                                  const bool allowImporters) {
        using Roles = Result<std::vector<PackageRuntimeRoleDeclaration>>;
        auto outsideOwner = MakeError(PackageLifecycleErrors::InvalidCandidate);
        if (!package.archive)
            return Roles::Failure(std::move(outsideOwner));
        // The importer sidecar scan identifies importer use, not references to mounted assets contributed by this archive.
        const bool hasMountedAssets = allowImporters && std::ranges::any_of(package.archive->Manifest().Entries(), [](const auto &file) {
            return file.id.path.Value().starts_with("assets/");
        });
        RemovalArtifacts artifacts;
        std::vector<PackageRuntimeRoleDeclaration> roles;
        for (const auto &file : package.archive->Manifest().Entries() | std::views::filter(IsExtensionDescriptor)) {
            const auto &descriptor = file.id.path;
            auto declared = Detail::ReadDeclaredPackageDescriptor(package, descriptor);
            if (declared.HasError())
                return Roles::Failure(WithCause(std::move(outsideOwner), std::move(declared).ErrorValue()));
            CollectRemovalArtifacts(declared.Value(), descriptor, allowImporters && !hasMountedAssets, artifacts[declared.Value().root],
                                    roles, outsideOwner.diagnostics);
        }
        CollectUnownedContributions(package.lock, artifacts, outsideOwner.diagnostics);
        CollectUnownedFiles(*package.archive, artifacts, outsideOwner.diagnostics);
        if (!std::ranges::any_of(artifacts, [](const auto &owner) { return !owner.second.empty(); }) || !outsideOwner.diagnostics.empty())
            return Roles::Failure(std::move(outsideOwner));
        return Roles::Success(std::move(roles));
    }

    /** @copydoc InspectGameplayRemovalRoles */
    Result<std::vector<PackageRuntimeRoleDeclaration>> InspectGameplayRemovalRoles(const PackageRestorePackage &package) {
        return InspectRemovalRoles(package, false);
    }

    /** @copydoc InspectLivePackageRemovalRoles */
    Result<std::vector<PackageRuntimeRoleDeclaration>> InspectLivePackageRemovalRoles(const PackageRestorePackage &package,
                                                                                      const PackageRemovalLease &reservation) {
        return InspectRemovalRoles(package, reservation.AllowsNativeImporterReview(package));
    }

    /** @copydoc InspectPackageRuntimeRoles */
    Result<std::vector<PackageRuntimeRoleDeclaration>> InspectPackageRuntimeRoles(const VerifiedPackageInstallRecord &install,
                                                                                  const HoroPackageId &package) {
        using Roles = Result<std::vector<PackageRuntimeRoleDeclaration>>;
        const auto graph = install.Graph();
        const auto installed = std::ranges::find_if(graph->packages, [&package](const auto &entry) {
            return entry.lock.package == package;
        });
        if (installed == graph->packages.end())
            return Roles::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        std::vector<PackageRuntimeRoleDeclaration> roles;
        for (const auto &file : installed->archive->Manifest().Entries()) {
            if (!IsExtensionDescriptor(file))
                continue;
            const auto declared = Detail::ReadDeclaredPackageDescriptor(*installed, file.id.path);
            if (declared.HasError())
                return Roles::Failure(declared.ErrorValue());
            for (const auto &entry : declared.Value().manifest.modules) {
                if (std::ranges::find(entry.roles, Extensions::ExtensionModuleRole::RuntimeParticipant) != entry.roles.end())
                    roles.emplace_back(file.id.path, entry.id);
            }
        }
        return Roles::Success(std::move(roles));
    }
}  // namespace Horo::Packages
