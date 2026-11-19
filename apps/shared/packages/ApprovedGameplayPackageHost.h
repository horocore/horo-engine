#pragma once

/** @file ApprovedGameplayPackageHost.h
 * @brief Explicit launcher inputs and retained execution approval for the ordinary Editor package host.
 */
#include "PackageProjectSession.h"

namespace Horo::Application::Internal {
    struct ApprovedGameplayPackage final {
        Packages::HoroPackageId package;
        std::string extension;
        std::string module;
        Sha256Digest archive;
        Sha256Digest manifest;
        std::uint64_t descriptorRevision{};
    };

    struct ApprovedGameplayPackageArguments final {
        std::filesystem::path publisherTrust;
        std::vector<ApprovedGameplayPackage> approvals;
    };

    /** @brief Reads only explicit launcher options; partial, duplicate, and malformed input fails closed.
     * @param arguments Executable argument vector with its program name.
     * @return Typed arguments, no configuration, or exact argument error. */
    [[nodiscard]] Result<std::optional<ApprovedGameplayPackageArguments>> ParseApprovedGameplayPackageArguments(
        std::span<char *const> arguments);

    /** @brief Retains the two independently required publisher and execution authorities through package host Close. */
    struct ApprovedGameplayPackageHost final {
        std::unique_ptr<const Packages::IPackageExtensionTrust> execution;
        std::unique_ptr<Packages::PackagePublisherVerificationService> publishers;
        PackageProjectPolicy policy;
    };

    /** @brief Prepares an exact single-project runtime role owner without staging or activating any package.
     * @param files Host-owned durable local file authority.
     * @param clock UTC authority for checking publisher expiration.
     * @param policy Existing package application policy with exact project and private cache paths.
     * @param arguments Independently supplied approved identities and trust source.
     * @return Configured owner-lifetime bundle or typed rejection. Retain it until PackageProjectHost finishes Close. */
    [[nodiscard]] Result<ApprovedGameplayPackageHost> PrepareApprovedGameplayPackageHost(DurableFileSystem &files, const WallClock &clock,
                                                                                         PackageProjectPolicy policy,
                                                                                         const ApprovedGameplayPackageArguments &arguments);
}  // namespace Horo::Application::Internal
