#include "DiscoveryPaths.h"
#include "DiscoveryRootPolicy.h"
#include "Horo/Extensions/ExtensionDiscovery.h"

#include <algorithm>
#include <utility>

namespace Horo::Extensions::Discovery {
    namespace {
        const ErrorCodeDescriptor InvalidDiscoveryPlan{
            .domain = ErrorDomainId{"horo.extensions"},
            .code = ErrorCode{"invalid_discovery_plan"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The declared package discovery plan is invalid.",
            .remediationHint = "Supply bounded package locations with unique package IDs and known approved roots.",
            .retryable = false,
            .userActionable = true,
        };

        /** @brief Validate graph references before any package path is probed. */
        bool IsInvalidLocation(const PackageLocation &location, const RootSelection &selection) {
            return location.packageId.empty() || location.providerPackageIds.size() > 4096 ||
                   !std::ranges::binary_search(selection.diagnostics, location.rootId, {}, &RootDiagnostic::rootId);
        }

        /** @brief Resolve one graph-owned package directory without treating it as trusted executable content. */
        Result<DiscoveredPackage> ResolvePackage(const PackageLocation &location, const ApprovedRoot &root) {
            auto path = ResolveContainedPath(root.canonicalPath, location.relativePath);
            if (path.HasError())
                return Result<DiscoveredPackage>::Failure(path.ErrorValue());
            if (std::error_code error; !std::filesystem::is_directory(path.Value(), error))
                return Result<DiscoveredPackage>::Failure(
                    MakeError(InvalidDiscoveryPlan,
                              location.packageId + ": " + (error ? error.message() : "Package location is not a directory.")));
            return Result<DiscoveredPackage>::Success({.packageId = location.packageId,
                                                       .rootId = root.id,
                                                       .canonicalPath = std::move(path).Value(),
                                                       .kind = root.kind,
                                                       .providerPackageIds = location.providerPackageIds});
        }
    }  // namespace

    /** @copydoc DiscoverDeclaredPackages */
    Result<DiscoveryPlan> DiscoverDeclaredPackages(std::span<const RootRequest> roots, std::span<const PackageLocation> locations,
                                                   const RootPolicy &policy) {
        if (locations.size() > 4096)
            return Result<DiscoveryPlan>::Failure(MakeError(InvalidDiscoveryPlan, "Too many declared package locations."));
        auto selected = SelectApprovedRoots(roots, policy);
        if (selected.HasError())
            return Result<DiscoveryPlan>::Failure(selected.ErrorValue());
        const auto &selection = selected.Value();
        if (std::ranges::any_of(locations, [&](const PackageLocation &location) {
            return IsInvalidLocation(location, selection);
        }))
            return Result<DiscoveryPlan>::Failure(MakeError(InvalidDiscoveryPlan, "Empty package identity or unknown discovery root."));
        DiscoveryPlan plan;
        for (const auto &location : locations) {
            const auto root = std::ranges::lower_bound(selection.roots, location.rootId, {}, &ApprovedRoot::id);
            if (root == selection.roots.end() || root->id != location.rootId)
                continue;
            auto package = ResolvePackage(location, *root);
            if (package.HasError())
                return Result<DiscoveryPlan>::Failure(package.ErrorValue());
            plan.packages.push_back(std::move(package).Value());
        }
        std::ranges::sort(plan.packages, {}, &DiscoveredPackage::packageId);
        if (const auto duplicate = std::ranges::adjacent_find(plan.packages, {}, &DiscoveredPackage::packageId);
            duplicate != plan.packages.end())
            return Result<DiscoveryPlan>::Failure(MakeError(InvalidDiscoveryPlan, "Duplicate package identity: " + duplicate->packageId));
        plan.rootDiagnostics = selection.diagnostics;
        plan.nonPortable = selection.nonPortable;
        return Result<DiscoveryPlan>::Success(std::move(plan));
    }
}  // namespace Horo::Extensions::Discovery
