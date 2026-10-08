/** @file
 * @brief Approved, declared extension package discovery without activation or directory scanning.
 */
#pragma once

#include "Horo/Foundation/Result.h"

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace Horo::Extensions::Discovery {
    /** @brief Provenance supplied by package/product composition, never by an extension manifest. */
    enum class RootKind {
        System,
        User,
        Project,
        Development
    };
    /** @brief Local policy approval of a root; this does not approve execution of its contents. */
    enum class RootApproval {
        Pending,
        Approved
    };
    /** @brief Configuration authority from which a root request was obtained. */
    enum class ConfigurationOrigin {
        ProductPolicy,
        UserLocal,
        ProjectPortable
    };
    /** @brief Operation class restricting development overrides. */
    enum class DiscoveryProfile {
        Development,
        Normal,
        ContinuousIntegration,
        Release,
        OfflineReproducible
    };

    /** @brief Trusted host input associating one stable root ID with its policy decision. */
    struct RootRequest {
        std::string id;
        std::filesystem::path path;
        RootKind kind{RootKind::User};
        RootApproval approval{RootApproval::Pending};
        ConfigurationOrigin configuration{ConfigurationOrigin::ProjectPortable};
    };

    /** @brief Explicit invocation policy; development is disabled by default. */
    struct RootPolicy {
        DiscoveryProfile profile{DiscoveryProfile::Normal};
        bool enableDevelopmentOverrides{false};
    };

    /** @brief Reason recorded for every requested root without reading ignored paths. */
    enum class RootDisposition {
        Accepted,
        ApprovalRequired,
        InvalidAuthority,
        DevelopmentDisabled,
        DevelopmentNonPortable
    };

    /** @brief Stable diagnostic identity and policy outcome, suitable for CLI or host presentation. */
    struct RootDiagnostic {
        std::string rootId;
        RootDisposition disposition;
    };

    /**
     * @brief One package location supplied by the owning package graph, not by directory enumeration.
     * @note Identity and approval originate outside extension manifests. Discovery does not verify
     *       signatures, resolve versions, approve execution, or replace a verified install lease.
     */
    struct PackageLocation {
        std::string packageId;
        std::string rootId;
        std::filesystem::path relativePath;
        std::vector<std::string> providerPackageIds; /**< Already resolved package-provider identities; empty for independent packages. */
    };

    /** @brief Canonical package location retaining the source identity for downstream diagnostics. */
    struct DiscoveredPackage {
        std::string packageId;
        std::string rootId;
        std::filesystem::path canonicalPath;
        RootKind kind;
        std::vector<std::string> providerPackageIds; /**< Copied package-authority dependency order inputs, not manifest inference. */
    };

    /** @brief Discovery snapshot; packages are ordered by package ID, never filesystem enumeration. */
    struct DiscoveryPlan {
        std::vector<DiscoveredPackage> packages;
        std::vector<RootDiagnostic> rootDiagnostics;
        bool nonPortable{false};
    };

    /**
     * @brief Discover only explicitly declared package locations from approved roots.
     * @param roots Product/package composition's root requests and approval provenance.
     * @param locations At most 4096 locations from the already selected package graph.
     * @param policy Invocation profile and explicit development opt-in.
     * @return Deterministic snapshot, or an error without partial results.
     * @note Locations under ignored roots are not probed. Unknown roots and duplicate accepted
     *       package IDs fail; discovery never chooses a version or silently shadows a package.
     *       Package-system development substitution must happen before this hand-off.
     */
    [[nodiscard]] Result<DiscoveryPlan> DiscoverDeclaredPackages(std::span<const RootRequest> roots,
                                                                 std::span<const PackageLocation> locations, const RootPolicy &policy);
}  // namespace Horo::Extensions::Discovery
