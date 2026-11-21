#pragma once

/**
 * @file AudioDeviceDiscovery.h
 * @brief Owned output discovery snapshots and explicit device selection without native identities.
 */

#include "Horo/Audio/AudioIdentity.h"

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace Horo::Audio {
    /** @brief Stable Horo identities of the ADR-067 first-party peers; order never selects a backend. */
    enum class AudioBackendKind : std::uint8_t {
        WASAPI = 1,
        CoreAudio,
        PipeWire,
        SDL3Audio,
        NullAudio
    };

    /** @brief Requested system default role, resolved only within the already selected backend. */
    enum class AudioDefaultDeviceRole : std::uint8_t {
        Console,
        Multimedia,
        Communications
    };

    /** @brief Reported endpoint class; virtual/headless endpoints do not qualify physical hardware. */
    enum class AudioDeviceClass : std::uint8_t {
        Physical,
        Virtual,
        Headless,
        Unknown /**< The native discovery protocol does not substantiate an endpoint class. */
    };

    /** @brief Maximum number of output endpoints in one complete discovery snapshot. */
    inline constexpr std::size_t MaximumAudioDiscoveredDevices = 256;

    /** @brief Owned presentation metadata; neither display name nor enumeration position is identity. */
    struct AudioDiscoveredDevice final {
        AudioDeviceId id;
        std::string displayName; /**< At most 256 bytes; adapters must not expose private native identifiers here. */
        AudioDeviceClass deviceClass{AudioDeviceClass::Physical};
    };

    /** @brief Independent default bindings; absence means unavailable, never the first enumerated device. */
    struct AudioDefaultDevices final {
        std::optional<AudioDeviceId> console;
        std::optional<AudioDeviceId> multimedia;
        std::optional<AudioDeviceId> communications;
    };

    /**
     * @brief Complete control-thread discovery result, retained immutable while resolving selections.
     *
     * Backend identity is fixed by the owning runtime. Revision is non-zero and increases whenever
     * endpoints or defaults change; enumeration itself must preserve IDs of unchanged endpoints.
     * Replacement/reuse increments the registry generation, never wrapping. Native identity mapping
     * belongs exclusively to the adapter. An empty successful snapshot is valid; a failed, cancelled,
     * overflowing or partial enumeration must not publish a snapshot. Construction may allocate and
     * cannot run on the callback. This value proves neither runtime availability nor activation.
     */
    struct AudioDeviceSnapshot final {
        std::uint32_t contractVersion{1};
        AudioRuntimeId owner;
        AudioBackendKind backend{AudioBackendKind::NullAudio};
        std::uint64_t revision{};
        std::vector<AudioDiscoveredDevice> devices;
        AudioDefaultDevices defaults;
    };

    /** @brief Preserved caller intent; explicit device failure never falls back to a default role. */
    using AudioDeviceSelection = std::variant<AudioDeviceId, AudioDefaultDeviceRole>;

    /** @brief Allocation-free discovery validation and selection outcomes. */
    enum class AudioDeviceResolutionStatus : std::uint8_t {
        Resolved,
        InvalidSnapshot,
        InvalidSelection,
        Unavailable
    };

    /** @brief Resolved device and discovery revision, not an open stream or a readiness acknowledgement. */
    struct AudioDeviceResolution final {
        AudioDeviceResolutionStatus status{AudioDeviceResolutionStatus::InvalidSnapshot};
        AudioDeviceId device;
        std::uint64_t revision{}; /**< Zero on failure; control must revalidate before asynchronous open commits. */
    };

    /**
     * @brief Checks version, owner, bounds, unique slots, endpoint classes and default membership.
     * @param snapshot Complete owned snapshot supplied by the selected backend.
     * @return True for a structurally valid snapshot; does not prove native device liveness.
     */
    [[nodiscard]] bool ValidateAudioDeviceSnapshot(const AudioDeviceSnapshot &snapshot) noexcept;

    /**
     * @brief Resolves explicit or default intent against exactly one immutable discovery revision.
     * @param snapshot Selected backend's complete snapshot; never enumerates or opens native devices.
     * @param selection Caller-owned request that remains unchanged by resolution.
     * @return Exact generation-scoped device/revision, or typed failure with an empty device/revision.
     * @pre Control-thread use; bounded validation performs at most 256 squared identity comparisons.
     */
    [[nodiscard]] AudioDeviceResolution ResolveAudioDevice(const AudioDeviceSnapshot &snapshot,
                                                           const AudioDeviceSelection &selection) noexcept;
}  // namespace Horo::Audio
