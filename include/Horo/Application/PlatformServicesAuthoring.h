#pragma once

/** @file PlatformServicesAuthoring.h
 * @brief Bounded project-owned Platform Services identity and policy serialization shared by host adapters.
 */

#include "Horo/Application/ProjectSourceDocument.h"
#include "Horo/PlatformServices/PlatformProjectConfiguration.h"
#include "Horo/PlatformServices/PlatformStableIdRegistry.h"

#include <string>
#include <string_view>

namespace Horo::Application {
    /** @brief Canonical project-owned source paths; private provider mappings remain adapter-owned. */
    namespace PlatformServicesDocumentPaths {
        inline constexpr std::string_view Ledger = ".horo/platform_services.ids.json";
        inline constexpr std::string_view Achievements = ".horo/platform_services.achievements.json";
        inline constexpr std::string_view Stats = ".horo/platform_services.stats.json";
        inline constexpr std::string_view Leaderboards = ".horo/platform_services.leaderboards.json";
        inline constexpr std::string_view Presence = ".horo/platform_services.presence.json";
    }  // namespace PlatformServicesDocumentPaths

    /** @brief Complete unpublished first generation; the root is the only persisted salt authority. */
    struct PlatformServicesInitialDocuments final {
        std::string projectSource;
        std::string ledgerSource;
        std::string achievementSource;
        std::string statSource;
        std::string leaderboardSource;
        std::string presenceSource;
    };

    /**
     * @brief Prepares the complete first platform generation without filesystem writes or entropy discovery.
     * @param projectSource Strict canonical root capture with no existing platform salt or policy.
     * @param salt Nonzero bytes obtained once from the host-injected cryptographic source.
     * @return Preserved root semantics plus explicit Null policy, empty ledger and all four semantic documents, or typed failure.
     * @pre The creation or explicit migration caller has verified that no platform identity, references or private mappings exist.
     * @details Existing platform root fields fail closed. This is never a repair path and must not run during ordinary open/cook.
     * The host publishes all documents in its existing creation/migration transaction and rechecks source authority before commit.
     */
    [[nodiscard]] Result<PlatformServicesInitialDocuments> PreparePlatformServicesInitialDocuments(
        std::string_view projectSource, const PlatformServices::PlatformServicesIdSalt &salt);

    /**
     * @brief Decodes a complete ledger without writing or repairing project metadata.
     * @param source UTF-8 ledger JSON, bounded to 4 MiB with at most eight nesting levels.
     * @param projectId Authoritative identity read from the captured root project metadata.
     * @param salt Authoritative salt read from that same root metadata; never read from the ledger.
     * @return Immutable registry or typed shape, identity, derivation or uniqueness failure.
     * @details Parsing admits at most 32,768 object members and 64 bytes per member name. Duplicate and unknown members,
     * noncanonical IDs, unsupported versions/algorithms and foreign owners fail closed.
     * GUI, CLI, headless and cook adapters must use this same inert operation. Publication and migration remain host-owned.
     */
    [[nodiscard]] Result<PlatformServices::PlatformStableIdRegistry> DecodePlatformServicesLedger(
        std::string_view source, std::string_view projectId, const PlatformServices::PlatformServicesIdSalt &salt);

    /**
     * @brief Encodes every active entry, alias and permanent tombstone in deterministic order.
     * @param registry Complete validated immutable ledger generation.
     * @return Canonical UTF-8 JSON ending in one newline, or typed invalid-ledger failure for an uninitialized or oversized snapshot.
     * @details Output contains no duplicated salt or provider-private values.
     */
    [[nodiscard]] Result<std::string> EncodePlatformServicesLedger(const PlatformServices::PlatformStableIdRegistry &registry);

    /**
     * @brief Resolves the sole project-root salt from the canonical captured project source.
     * @param source One immutable root capture; legacy absence is not repaired by this read-only operation.
     * @return Canonical nonzero salt or typed invalid-salt failure.
     */
    [[nodiscard]] Result<PlatformServices::PlatformServicesIdSalt> ResolveProjectPlatformServicesSalt(const ProjectSourceDocument &source);

    /**
     * @brief Decodes portable policy into a detached candidate without resolving a provider or activating services.
     * @param source Canonical captured root, including its sole valid salt authority.
     * @param profile Explicit host-owned profile, never read from project text.
     * @param limits Finite portable validation bounds.
     * @return Shape-validated candidate or typed salt, syntax, enum, range or Null-required-capability failure.
     * @details Migration preserves exact provider requirements even when no provider is installed in that process.
     * A successful candidate is not an activation policy: hosts must use the shared resolver with actual trusted contributions.
     */
    [[nodiscard]] Result<PlatformServices::PlatformProjectConfigurationCandidate> DecodeProjectPlatformServicesConfigurationCandidate(
        const ProjectSourceDocument &source, PlatformServices::PlatformServicesHostProfile profile,
        const PlatformServices::PlatformProjectConfigurationLimits &limits = {});

    /**
     * @brief Validates captured portable project policy through the shared typed Platform Services validator.
     * @param source One canonical root capture containing explicit settings.platformServices.
     * @param profile Explicit host-owned execution profile, never selected by project text.
     * @param contributions Complete inert provider metadata selected by composition.
     * @param trustedModules Exact modules approved by the host trust boundary.
     * @param limits Finite typed policy limits.
     * @return Immutable policy or typed shape, selection, trust or capability failure; absence never means implicit Null.
     */
    [[nodiscard]] Result<PlatformServices::PlatformProjectConfiguration> ResolveProjectPlatformServicesConfiguration(
        const ProjectSourceDocument &source, PlatformServices::PlatformServicesHostProfile profile,
        std::span<const PlatformServices::PlatformProviderModuleContribution> contributions, std::span<const ModuleId> trustedModules,
        const PlatformServices::PlatformProjectConfigurationLimits &limits = {});

    /**
     * @brief Encodes validated portable policy for settings.platformServices.
     * @param configuration Immutable validated project policy.
     * @return Canonical JSON value or typed invalid-configuration failure for an uninitialized snapshot.
     * @details Host profile, revision evidence and provider-native values are excluded.
     */
    [[nodiscard]] Result<std::string> EncodeProjectPlatformServicesConfiguration(
        const PlatformServices::PlatformProjectConfiguration &configuration);
}  // namespace Horo::Application
