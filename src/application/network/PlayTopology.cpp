#include "Horo/Application/PlayTopology.h"

#include "Horo/Foundation/Utf8.h"
#include "PlayTopologyInternal.h"

#include <algorithm>
#include <utility>

namespace Horo::Application {
    namespace PlayTopologyErrors {
        namespace {
            const ErrorDomainId Domain{"horo.application.play_topology"};
        }

        const ErrorCodeDescriptor Invalid{Domain, ErrorCode{"play_topology.invalid"}, ErrorSeverity::Error,
                                          "The preview topology is invalid.",
                                          "Choose bounded counts and portable project-relative scene paths."};
        const ErrorCodeDescriptor Unsupported{Domain, ErrorCode{"play_topology.unsupported"}, ErrorSeverity::Error,
                                              "The selected preview topology is unavailable.",
                                              "Select a supported mode, map, transport and simulation preset."};
        const ErrorCodeDescriptor Stale{Domain, ErrorCode{"play_topology.stale"}, ErrorSeverity::Error, "The preview profile changed.",
                                        "Reload the profile before saving or launching."};
        const ErrorCodeDescriptor Disabled{Domain, ErrorCode{"play_topology.disabled"}, ErrorSeverity::Error,
                                           "Multiplayer preview is disabled.", "Enable an admitted preview session before launching."};
        const ErrorCodeDescriptor Storage{Domain, ErrorCode{"play_topology.storage"}, ErrorSeverity::Error,
                                          "The preview profile could not be persisted.",
                                          "Check the project and user-local storage permissions."};
    }  // namespace PlayTopologyErrors

    namespace {
        /** @brief Require canonical relative components, without traversal or empty path segments. */
        bool PortableComponents(const std::string_view map) {
            std::size_t offset{};
            while (offset < map.size()) {
                const auto end = map.find('/', offset);
                if (const auto part = map.substr(offset, end == std::string_view::npos ? map.size() - offset : end - offset);
                    part.empty() || part == "." || part == "..")
                    return false;
                if (end == std::string_view::npos)
                    break;
                offset = end + 1;
            }
            return true;
        }

        /** @brief Validate bounded human-readable UTF-8 without control bytes. */
        bool ValidName(const std::string &name) {
            return !name.empty() && name.size() <= 96 && IsValidUtf8ScalarSequence(name) &&
                   std::ranges::none_of(name, [](unsigned char byte) {
                return byte < 32 || byte == 127;
            });
        }

        /** @brief Enforce the standalone absence of every network-only setting. */
        bool StandaloneCounts(const PlayTopologyProfile &profile) {
            return profile.serverCount == 0 && profile.clientCount == 0 && !profile.transport.IsValid() && profile.port == 0 &&
                   profile.simulationPreset == 0;
        }

        /** @brief Bound server/client topology, counting the listen host's local client once. */
        bool NetworkCounts(const PlayTopologyProfile &profile) {
            if (profile.serverCount != 1 || !profile.transport.IsValid() || profile.port == 0)
                return false;
            return profile.kind != PlayTopologyKind::Listen || profile.clientCount <= 7;
        }

        /** @brief Reject control bytes and native absolute path syntax before any file or launch operation. */
        bool PortableMap(const std::string &map) {
            if (map.empty() || map.size() > 256 || !IsValidUtf8ScalarSequence(map) || map.front() == '/' || map.back() == '/' ||
                map.find_first_of("\\:") != std::string::npos)
                return false;
            if (std::ranges::any_of(map, [](unsigned char byte) {
                return byte < 32 || byte == 127;
            }))
                return false;
            return PortableComponents(map) && map.ends_with(".horo");
        }

        /** @brief Selects the server or standalone role independently of presentation/backend details. */
        Network::NetworkProjectRole HostRole(const PlayTopologyKind kind) {
            using enum Network::NetworkProjectRole;
            switch (kind) {
                case PlayTopologyKind::Standalone:
                    return Standalone;
                case PlayTopologyKind::Listen:
                    return ListenServer;
                case PlayTopologyKind::Dedicated:
                    return DedicatedServer;
                case PlayTopologyKind::Count:
                    break;
            }
            return Count;
        }
    }  // namespace

    namespace {
        /** @brief Preview activation requires live explicitly enabled owner evidence. */
        bool ActivePreview(const PlayTopologyCapabilities &capabilities) {
            return capabilities.enabled && capabilities.preview && !capabilities.stopping && capabilities.generation != 0;
        }

        /** @brief Check complete role and map admission before any participant is planned. */
        bool SupportsWorlds(const PlayTopologyProfile &profile, const PlayTopologyCapabilities &capabilities) {
            if (!Network::ContainsNetworkProjectRole(capabilities.roles, HostRole(profile.kind)))
                return false;
            if (profile.clientCount != 0 && !Network::ContainsNetworkProjectRole(capabilities.roles, Network::NetworkProjectRole::Client))
                return false;
            return std::ranges::find(capabilities.maps, profile.map) != capabilities.maps.end();
        }

        /** @brief Require exact provider and simulator references, without backend discovery or fallback. */
        bool SupportsReferences(const PlayTopologyProfile &profile, const PlayTopologyCapabilities &capabilities) {
            if (profile.transport.IsValid() &&
                std::ranges::find(capabilities.transports, profile.transport) == capabilities.transports.end())
                return false;
            return profile.simulationPreset == 0 ||
                   std::ranges::find(capabilities.simulationPresets, profile.simulationPreset) != capabilities.simulationPresets.end();
        }

        /** @brief Apply a machine override to the owned plan, never to the portable project snapshot. */
        Result<void> ApplyOverride(PlayTopologyProfile &profile, const PlayTopologyUserSettings &user) {
            const auto portOverride = std::ranges::find(user.overrides, profile.id, &PlayTopologyOverride::profile);
            if (portOverride == user.overrides.end())
                return Result<void>::Success();
            if (profile.kind == PlayTopologyKind::Standalone)
                return Result<void>::Failure(MakeError(PlayTopologyErrors::Invalid));
            profile.port = portOverride->port;
            return Result<void>::Success();
        }
    }  // namespace

    namespace Detail {
        /** @brief Validates all finite catalog identities, including entries not selected for launch. */
        bool ValidPlayTopologyCatalog(const PlayTopologyCatalog &catalog) {
            if (catalog.revision == 0 || catalog.profiles.size() > 16)
                return false;
            for (std::size_t index = 0; index < catalog.profiles.size(); ++index) {
                if (ValidatePlayTopology(catalog.profiles[index]).HasError())
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (catalog.profiles[prior].id == catalog.profiles[index].id)
                        return false;
            }
            return true;
        }

        /** @brief Validates independent user overrides before applying an exact profile match. */
        bool ValidPlayTopologyOverrides(const PlayTopologyUserSettings &user) {
            if (user.revision == 0 || user.overrides.size() > 16)
                return false;
            for (std::size_t index = 0; index < user.overrides.size(); ++index) {
                if (user.overrides[index].profile == 0 || user.overrides[index].port == 0)
                    return false;
                for (std::size_t prior = 0; prior < index; ++prior)
                    if (user.overrides[prior].profile == user.overrides[index].profile)
                        return false;
            }
            return true;
        }
    }  // namespace Detail

    /** @copydoc ValidatePlayTopology */
    Result<void> ValidatePlayTopology(const PlayTopologyProfile &profile) {
        if (profile.id == 0 || !ValidName(profile.name) || !PortableMap(profile.map) || profile.kind >= PlayTopologyKind::Count ||
            profile.clientCount > 8)
            return Result<void>::Failure(MakeError(PlayTopologyErrors::Invalid));
        if (const bool countsValid = profile.kind == PlayTopologyKind::Standalone ? StandaloneCounts(profile) : NetworkCounts(profile);
            !countsValid)
            return Result<void>::Failure(MakeError(PlayTopologyErrors::Invalid));
        return Result<void>::Success();
    }

    /** @copydoc PreflightPlayTopology */
    Result<PlayTopologyPlan> PreflightPlayTopology(const PlayTopologyCatalog &catalog, const PlayTopologyUserSettings &user,
                                                   const std::uint64_t identity, const PlayTopologyCapabilities &capabilities) {
        using Plan = Result<PlayTopologyPlan>;
        if (!Detail::ValidPlayTopologyCatalog(catalog) || !Detail::ValidPlayTopologyOverrides(user))
            return Plan::Failure(MakeError(PlayTopologyErrors::Invalid));
        if (!ActivePreview(capabilities))
            return Plan::Failure(MakeError(PlayTopologyErrors::Disabled));
        const auto found = std::ranges::find(catalog.profiles, identity, &PlayTopologyProfile::id);
        if (found == catalog.profiles.end())
            return Plan::Failure(MakeError(PlayTopologyErrors::Invalid));
        if (!SupportsWorlds(*found, capabilities) || !SupportsReferences(*found, capabilities))
            return Plan::Failure(MakeError(PlayTopologyErrors::Unsupported));
        PlayTopologyPlan plan{.profile = *found,
                              .projectRevision = catalog.revision,
                              .userRevision = user.revision,
                              .generation = capabilities.generation};
        if (const auto applied = ApplyOverride(plan.profile, user); applied.HasError())
            return Plan::Failure(applied.ErrorValue());
        plan.participants[plan.participantCount++] = {.role = HostRole(found->kind), .port = plan.profile.port};
        for (std::uint8_t index = 0; index < found->clientCount; ++index)
            plan.participants[plan.participantCount++] = {.role = Network::NetworkProjectRole::Client, .port = plan.profile.port};
        return Plan::Success(std::move(plan));
    }

    /** @copydoc ValidatePlayTopologyPlan */
    Result<void> ValidatePlayTopologyPlan(const PlayTopologyPlan &plan, const PlayTopologyCatalog &catalog,
                                          const PlayTopologyUserSettings &user, const PlayTopologyCapabilities &capabilities) {
        if (plan.projectRevision != catalog.revision || plan.userRevision != user.revision || plan.generation != capabilities.generation)
            return Result<void>::Failure(MakeError(PlayTopologyErrors::Stale));
        const auto current = PreflightPlayTopology(catalog, user, plan.profile.id, capabilities);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (current.Value().profile != plan.profile || current.Value().participantCount != plan.participantCount)
            return Result<void>::Failure(MakeError(PlayTopologyErrors::Stale));
        for (std::size_t index = 0; index < plan.participantCount; ++index)
            if (current.Value().participants[index].role != plan.participants[index].role ||
                current.Value().participants[index].port != plan.participants[index].port)
                return Result<void>::Failure(MakeError(PlayTopologyErrors::Stale));
        return Result<void>::Success();
    }
}  // namespace Horo::Application
