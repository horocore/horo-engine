#pragma once
/** @file PlayTopology.h
 * @brief Portable multiplayer preview profiles and generation-fenced application preflight.
 */
#include "Horo/Foundation/Platform.h"
#include "Horo/Network/NetworkTargetCapabilities.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Application {
    namespace PlayTopologyErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor Unsupported;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Disabled;
        extern const ErrorCodeDescriptor Storage;
    }  // namespace PlayTopologyErrors
    /** @brief Closed preview world topology; a listen host includes its local client. */
    enum class PlayTopologyKind : std::uint8_t {
        Standalone,
        Listen,
        Dedicated,
        Count
    };

    /** @brief Portable project-owned profile. Provider and preset identities are inert references, never factories. */
    struct PlayTopologyProfile final {
        std::uint64_t id{};
        std::string name;
        PlayTopologyKind kind{PlayTopologyKind::Standalone};
        std::uint8_t serverCount{}; /**< Zero for standalone, exactly one for server modes. */
        std::uint8_t clientCount{}; /**< Additional client processes; the listen local client is not counted twice. */
        std::string map;            /**< Canonical project-relative scene path, without traversal or native separators. */
        Network::NetworkTransportProviderId transport;
        std::uint16_t port{};             /**< Server port; zero for standalone. */
        std::uint64_t simulationPreset{}; /**< Zero means no impairment; non-zero requires explicit preview capability. */
        bool operator==(const PlayTopologyProfile &) const = default;
    };

    /** @brief Machine-local port override, persisted separately from the portable project catalog. */
    struct PlayTopologyOverride final {
        std::uint64_t profile{};
        std::uint16_t port{};
        bool operator==(const PlayTopologyOverride &) const = default;
    };

    /** @brief Bounded project catalog, sorted by identity when serialized. */
    struct PlayTopologyCatalog final {
        std::uint64_t revision{1};
        std::vector<PlayTopologyProfile> profiles;
        bool operator==(const PlayTopologyCatalog &) const = default;
    };

    /** @brief Independent user-local revision and port overrides. */
    struct PlayTopologyUserSettings final {
        std::uint64_t revision{1};
        std::vector<PlayTopologyOverride> overrides;
        bool operator==(const PlayTopologyUserSettings &) const = default;
    };

    /** @brief Complete owner-supplied preview evidence; defaults deny activation. No release-profile override exists. */
    struct PlayTopologyCapabilities final {
        std::uint64_t generation{};
        bool enabled{};
        bool preview{};
        bool stopping{};
        Network::NetworkProjectRoleSet roles{Network::NetworkProjectRoleSet::None};
        std::vector<Network::NetworkTransportProviderId> transports;
        std::vector<std::uint64_t> simulationPresets;
        std::vector<std::string> maps;
    };

    /** @brief Backend-neutral child intent; no process is constructed by profile preflight. */
    struct PlayTopologyParticipant final {
        Network::NetworkProjectRole role{Network::NetworkProjectRole::Standalone};
        std::uint16_t port{};
    };

    /** @brief Immutable admitted plan pinned to both profile revisions and the current preview owner generation. */
    struct PlayTopologyPlan final {
        PlayTopologyProfile profile;
        std::uint64_t projectRevision{};
        std::uint64_t userRevision{};
        std::uint64_t generation{};
        std::array<PlayTopologyParticipant, 9> participants{};
        std::size_t participantCount{};
    };

    /** @brief Validate portable bounds, mode/count coherence and path privacy.
     * @param profile Complete draft. @return Success or declared invalid-profile failure. */
    [[nodiscard]] Result<void> ValidatePlayTopology(const PlayTopologyProfile &profile);
    /** @brief Decode closed bounded project JSON, rejecting duplicate keys, unknown fields and future versions.
     * @param document At most 64 KiB UTF-8 JSON. @return Owned validated catalog or typed failure. */
    [[nodiscard]] Result<PlayTopologyCatalog> ParsePlayTopologies(std::string_view document);
    /** @brief Canonically encode the portable catalog without user overrides or credentials.
     * @param catalog Validated data. @return Deterministic version-one JSON or typed failure. */
    [[nodiscard]] Result<std::string> SerializePlayTopologies(const PlayTopologyCatalog &catalog);
    /** @brief Decode the separate closed user-local document.
     * @param document Bounded JSON. @return Validated overrides or typed failure. */
    [[nodiscard]] Result<PlayTopologyUserSettings> ParsePlayTopologyOverrides(std::string_view document);
    /** @brief Encode user-local overrides without modifying project defaults.
     * @param settings Complete bounded overrides. @return Canonical JSON or typed failure. */
    [[nodiscard]] Result<std::string> SerializePlayTopologyOverrides(const PlayTopologyUserSettings &settings);
    /** @brief Preflight every participant before any host or child process can start.
     * @param catalog Immutable project snapshot. @param user Independent machine snapshot.
     * @param profile Exact profile identity. @param capabilities Current owner-issued preview evidence.
     * @return Complete plan or typed invalid, unsupported, disabled or shutdown failure. */
    [[nodiscard]] Result<PlayTopologyPlan> PreflightPlayTopology(const PlayTopologyCatalog &catalog, const PlayTopologyUserSettings &user,
                                                                 std::uint64_t profile, const PlayTopologyCapabilities &capabilities);
    /** @brief Revalidate an admitted plan at the launch safe point.
     * @param plan Captured plan. @param catalog Current project snapshot. @param user Current machine snapshot.
     * @param capabilities Current preview owner evidence. @return Success only for unchanged revisions and generation. */
    [[nodiscard]] Result<void> ValidatePlayTopologyPlan(const PlayTopologyPlan &plan, const PlayTopologyCatalog &catalog,
                                                        const PlayTopologyUserSettings &user, const PlayTopologyCapabilities &capabilities);

    /** @brief Editor-thread application authority; paths remain host-private and never enter projections or serialized profiles. */
    class PlayTopologyStore final {
    public:
        /** @brief Borrow host filesystem through store destruction; paths must be distinct host-owned files.
         * @param files Durable writer. @param project Portable catalog destination. @param user Machine-local destination. */
        PlayTopologyStore(DurableFileSystem &files, std::filesystem::path project, std::filesystem::path user);
        /** @brief Read both complete documents without mutating committed snapshots on failure.
         * @return Success or preserved typed parse/storage failure. */
        [[nodiscard]] Result<void> Reload();
        /** @brief Return an immutable value projection. @return Project catalog copy. */
        [[nodiscard]] PlayTopologyCatalog Project() const;
        /** @brief Return independent immutable user state. @return Machine-local settings copy. */
        [[nodiscard]] PlayTopologyUserSettings User() const;
        /** @brief Atomically publish one profile after validating exact in-memory and on-disk revision under a writer lock.
         * @param revision Expected project revision. @param profile Owned draft. @return Committed snapshot or typed failure. */
        [[nodiscard]] Result<PlayTopologyCatalog> SaveProfile(std::uint64_t revision, PlayTopologyProfile profile);
        /** @brief Atomically publish only one machine-local override; zero port removes it.
         * @param revision Expected user revision. @param value Owned draft. @return Committed user snapshot or typed failure. */
        [[nodiscard]] Result<PlayTopologyUserSettings> SaveOverride(std::uint64_t revision, PlayTopologyOverride value);

    private:
        DurableFileSystem &files_;
        std::filesystem::path projectPath_;
        std::filesystem::path userPath_;
        PlayTopologyCatalog project_;
        PlayTopologyUserSettings user_;
        bool loaded_{};
    };
}  // namespace Horo::Application
