#include "Horo/Extensions/ExtensionAbi.h"
#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageActivationComposition.h"

#include <algorithm>
#include <map>
#include <set>

namespace Horo::Packages::Detail {
    namespace {
        /** @brief Supplies the same current process tuple used by the concrete native extension manager. */
        Extensions::ExtensionHostEnvironment Environment(const PackageLifecycleConfiguration &configuration,
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
            return {configuration.profile,
                    platform,
                    architecture,
                    build,
                    "0.1.0",
                    HORO_EXTENSION_ABI_VERSION,
                    HORO_EXTENSION_ABI_MINOR_VERSION,
                    capabilities};
        }

        /** @brief Matches the legacy native manager's explicit platform suffix without scanning for alternatives. */
        std::string SelectedEntry(std::string entry) {
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

        /** @brief Owns the verified contribution root and inert descriptor before host selection or trust. */
        struct DeclaredDescriptor final {
            std::string root;
            Extensions::ExtensionManifest manifest;
        };

        /** @brief Reads the declared descriptor and binds its identity to the exact installed package. */
        Result<DeclaredDescriptor> ReadDeclaredDescriptor(const PackageRestorePackage &package, const PackagePath &descriptorPath) {
            const auto &name = descriptorPath.Value();
            const auto slash = name.rfind('/');
            if (slash == std::string::npos || name.substr(slash + 1U) != "extension.json")
                return Result<DeclaredDescriptor>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            std::string root = name.substr(0U, slash);
            const auto entries = package.archive->Manifest().Entries();
            const auto descriptor = std::ranges::find(entries, name, [](const PackageFileEntry &entry) {
                return entry.id.path.Value();
            });
            if (descriptor == entries.end() || !descriptor->contributionRoot || descriptor->contributionRoot->Value() != root ||
                !root.starts_with("extensions/") || root.substr(11U).find('/') != std::string::npos ||
                std::ranges::find(package.lock.contributions, root.substr(11U)) == package.lock.contributions.end())
                return Result<DeclaredDescriptor>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto bytes = package.archive->ReadDeclaredFile(descriptorPath, Extensions::ExtensionManifestLimits{}.maximumDocumentBytes);
            if (bytes.HasError())
                return Result<DeclaredDescriptor>::Failure(bytes.ErrorValue());
            const auto &content = bytes.Value();
            const std::string text(reinterpret_cast<const char *>(content.data()), content.size());
            auto manifest = Extensions::ParseExtensionManifest(text);
            if (manifest.HasError())
                return Result<DeclaredDescriptor>::Failure(manifest.ErrorValue());
            if (manifest.Value().id != package.lock.package.Value() || manifest.Value().version != package.lock.version.ToString())
                return Result<DeclaredDescriptor>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            return Result<DeclaredDescriptor>::Success({std::move(root), std::move(manifest).Value()});
        }

        /** @brief Binds selected native artifacts and trust approval to one verified package descriptor. */
        Result<PackageActivationCandidate> Candidate(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                     const std::size_t packageIndex, const EnabledPackageExtension &enabled,
                                                     const Extensions::ExtensionHostEnvironment &host,
                                                     const IPackageExtensionTrust &trust) {
            const auto &package = install->Graph()->packages[packageIndex];
            auto descriptor = ReadDeclaredDescriptor(package, enabled.descriptor);
            if (descriptor.HasError())
                return Result<PackageActivationCandidate>::Failure(descriptor.ErrorValue());
            const auto &root = descriptor.Value().root;
            auto &manifest = descriptor.Value().manifest;
            const auto entries = package.archive->Manifest().Entries();
            // This composition exposes only the registered importer catalog. Other extension points need their own explicit host owners.
            if (!std::ranges::all_of(manifest.contributions, [](const auto &contribution) {
                return contribution.type == "asset.importer";
            }))
                return Result<PackageActivationCandidate>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto plan = Extensions::ResolveExtensionModules(manifest, host);
            if (plan.HasError())
                return Result<PackageActivationCandidate>::Failure(plan.ErrorValue());
            if (plan.Value().moduleIds.empty())
                return Result<PackageActivationCandidate>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            for (const auto &entry : plan.Value().selectedEntries) {
                auto path = PackagePath::Parse(root + '/' + SelectedEntry(entry));
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
                pending.emplace(selection.package.Value(), std::move(candidate).Value());
            }
            return Result<PendingCandidates>::Success(std::move(pending));
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
                    bool ready = true;
                    for (const auto &dependency : dependencies) {
                        const auto provider =
                            std::ranges::find(packages, dependency.package.Value(), [](const PackageRestorePackage &package) {
                            return package.lock.package.Value();
                        });
                        if (provider == packages.end() || provider->lock.version != dependency.version)
                            return Result<std::vector<PackageActivationCandidate>>::Failure(
                                MakeError(PackageLifecycleErrors::InvalidCandidate));
                        if (pending.contains(dependency.package.Value()))
                            ready = false;
                    }
                    if (!ready) {
                        ++item;
                        continue;
                    }
                    for (const auto &dependency : dependencies)
                        if (selected.contains(dependency.package.Value()))
                            item->second.providers.push_back(dependency.package.Value());
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

    /** @copydoc PreparePackageCandidates */
    Result<std::vector<PackageActivationCandidate>> PreparePackageCandidates(std::shared_ptr<const VerifiedPackageInstallRecord> install,
                                                                             const std::span<const EnabledPackageExtension> enabled,
                                                                             const IPackageExtensionTrust &trust,
                                                                             const PackageLifecycleConfiguration &configuration) {
        std::vector<std::string_view> capabilities;
        for (const auto &capability : configuration.capabilities)
            capabilities.push_back(capability);
        const auto host = Environment(configuration, capabilities);
        auto pending = SelectCandidates(install, enabled, host, trust);
        if (pending.HasError())
            return Result<std::vector<PackageActivationCandidate>>::Failure(pending.ErrorValue());
        return OrderCandidates(std::move(pending).Value(), install->Graph()->packages);
    }
}  // namespace Horo::Packages::Detail
