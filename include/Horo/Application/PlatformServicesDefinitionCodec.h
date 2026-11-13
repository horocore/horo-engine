#pragma once

/** @file PlatformServicesDefinitionCodec.h
 * @brief Inert project-owned JSON adapters for complete semantic definition snapshots.
 */

#include "Horo/PlatformServices/AchievementDefinitionRegistry.h"
#include "Horo/PlatformServices/PlatformDefinitionRegistries.h"

#include <string>
#include <string_view>

namespace Horo::Application {
    /**
     * @brief Decodes a complete achievement document through the common typed validator.
     * @param source UTF-8 JSON bounded to 4 MiB, eight nesting levels and 65,536 object members.
     * @param stableIds Authoritative captured ledger; owner and fingerprint must match.
     * @param limits Finite typed validation bounds; the document additionally caps definitions at 4096.
     * @return Immutable complete snapshot or typed shape, owner, version, identity or semantic failure.
     * @details Duplicate/unknown members, noncanonical IDs, fractional integers and coerced booleans fail closed.
     * Loading is read-only and never allocates identities, repairs history or selects a provider.
     */
    [[nodiscard]] Result<PlatformServices::AchievementDefinitionRegistry> DecodePlatformAchievementDefinitions(
        std::string_view source, const PlatformServices::PlatformStableIdRegistry &stableIds,
        const PlatformServices::AchievementDefinitionRegistryLimits &limits = {});

    /**
     * @brief Encodes a validated achievement snapshot in stable-ID order.
     * @param registry Complete immutable semantic snapshot.
     * @return Canonical JSON with a final newline or typed uninitialized/oversized snapshot failure.
     * @details Contains only owner, ledger fingerprint and semantic definitions; no salt or provider-native values.
     */
    [[nodiscard]] Result<std::string> EncodePlatformAchievementDefinitions(const PlatformServices::AchievementDefinitionRegistry &registry);

    /**
     * @brief Decodes a complete stat document through the common typed validator.
     * @param source UTF-8 JSON bounded to 4 MiB, eight nesting levels and 65,536 object members.
     * @param stableIds Authoritative captured ledger; owner and fingerprint must match.
     * @param limits Finite typed validation bounds; the document additionally caps definitions at 4096.
     * @return Immutable complete snapshot or typed shape, owner, version, identity or semantic failure.
     * @details Duplicate/unknown members, noncanonical IDs, fractional integers and coerced booleans fail closed.
     * Loading is read-only and never allocates identities, repairs history or selects a provider.
     */
    [[nodiscard]] Result<PlatformServices::StatDefinitionRegistry> DecodePlatformStatDefinitions(
        std::string_view source, const PlatformServices::PlatformStableIdRegistry &stableIds,
        const PlatformServices::PlatformDefinitionRegistryLimits &limits = {});

    /**
     * @brief Encodes a validated stat snapshot in stable-ID order.
     * @param registry Complete immutable semantic snapshot.
     * @return Canonical JSON with a final newline or typed uninitialized/oversized snapshot failure.
     * @details Contains only owner, ledger fingerprint and semantic definitions; no salt or provider-native values.
     */
    [[nodiscard]] Result<std::string> EncodePlatformStatDefinitions(const PlatformServices::StatDefinitionRegistry &registry);

    /**
     * @brief Decodes a complete leaderboard document through the common typed validator.
     * @param source UTF-8 JSON bounded to 4 MiB, eight nesting levels and 65,536 object members.
     * @param stableIds Authoritative captured ledger; owner and fingerprint must match.
     * @param stats Captured compatible stat snapshot for cross-reference validation.
     * @param limits Finite typed validation bounds; the document additionally caps definitions at 4096.
     * @return Immutable complete snapshot or typed shape, owner, version, identity or semantic failure.
     * @details Duplicate/unknown members, noncanonical IDs, fractional integers and coerced booleans fail closed.
     * Loading is read-only and never allocates identities, repairs history or selects a provider.
     */
    [[nodiscard]] Result<PlatformServices::LeaderboardDefinitionRegistry> DecodePlatformLeaderboardDefinitions(
        std::string_view source, const PlatformServices::PlatformStableIdRegistry &stableIds,
        const PlatformServices::StatDefinitionRegistry &stats, const PlatformServices::PlatformDefinitionRegistryLimits &limits = {});

    /**
     * @brief Encodes a validated leaderboard snapshot in stable-ID order.
     * @param registry Complete immutable semantic snapshot.
     * @return Canonical JSON with a final newline or typed uninitialized/oversized snapshot failure.
     * @details Contains only owner, ledger fingerprint and semantic definitions; no salt or provider-native values.
     */
    [[nodiscard]] Result<std::string> EncodePlatformLeaderboardDefinitions(const PlatformServices::LeaderboardDefinitionRegistry &registry);

    /**
     * @brief Decodes a complete presence document through the common typed validator.
     * @param source UTF-8 JSON bounded to 4 MiB, eight nesting levels and 65,536 object members.
     * @param stableIds Authoritative captured ledger; owner and fingerprint must match.
     * @param limits Finite typed validation bounds; the document additionally caps definitions at 4096.
     * @return Immutable complete snapshot or typed shape, owner, version, identity or semantic failure.
     * @details Duplicate/unknown members, noncanonical IDs, fractional integers and coerced booleans fail closed.
     * Loading is read-only and never allocates identities, repairs history or selects a provider.
     */
    [[nodiscard]] Result<PlatformServices::PresenceDefinitionRegistry> DecodePlatformPresenceDefinitions(
        std::string_view source, const PlatformServices::PlatformStableIdRegistry &stableIds,
        const PlatformServices::PlatformDefinitionRegistryLimits &limits = {});

    /**
     * @brief Encodes a validated presence snapshot in stable-ID order.
     * @param registry Complete immutable semantic snapshot.
     * @return Canonical JSON with a final newline or typed uninitialized/oversized snapshot failure.
     * @details Contains only owner, ledger fingerprint and semantic definitions; no salt or provider-native values.
     */
    [[nodiscard]] Result<std::string> EncodePlatformPresenceDefinitions(const PlatformServices::PresenceDefinitionRegistry &registry);

}  // namespace Horo::Application
