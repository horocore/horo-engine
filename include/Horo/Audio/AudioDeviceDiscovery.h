#pragma once

/**
 * @file AudioDeviceDiscovery.h
 * @brief Owned playback and capture discovery snapshots without native identities.
 */

#include "Horo/Audio/AudioFormat.h"
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

    /** @brief Maximum number of endpoints in one complete discovery snapshot. */
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

    /** @brief Reported recording permission. Unknown cannot authorize recording. */
    enum class AudioCapturePermission : std::uint8_t {
        Unknown,
        NotRequired,
        Required,
        Granted,
        Denied
    };

    /** @brief Owned recording endpoint metadata; the optional format is an observed preference. */
    struct AudioDiscoveredCaptureDevice final {
        AudioCaptureDeviceId id;
        std::string displayName; /**< At most 256 bytes, excluding private native identifiers. */
        AudioDeviceClass deviceClass{AudioDeviceClass::Physical};
        std::optional<AudioProcessingFormat> preferredFormat;
    };

    /** @brief Independently resolved recording defaults; missing bindings imply no fallback. */
    struct AudioDefaultCaptureDevices final {
        std::optional<AudioCaptureDeviceId> console;
        std::optional<AudioCaptureDeviceId> multimedia;
        std::optional<AudioCaptureDeviceId> communications;
    };

    /** @brief Complete immutable control-thread recording inventory, independent of playback.
     * @details Revisions advance when inventory, formats, defaults or permission change. An adapter
     * retains unchanged identities and retires removed generations before reusing a slot.
     * Discovery does not open a recording stream or request permission.
     */
    struct AudioCaptureDeviceSnapshot final {
        std::uint32_t contractVersion{1};
        AudioRuntimeId owner;
        AudioBackendKind backend{AudioBackendKind::NullAudio};
        std::uint64_t revision{};
        std::vector<AudioDiscoveredCaptureDevice> devices;
        AudioDefaultCaptureDevices defaults;
        AudioCapturePermission permission{AudioCapturePermission::Unknown};
    };

    /** @brief An explicit recording identity or a default role within the recording catalog. */
    using AudioCaptureDeviceSelection = std::variant<AudioCaptureDeviceId, AudioDefaultDeviceRole>;

    /** @brief A bound recording identity and catalog revision; activation has a separate permission check. */
    struct AudioCaptureDeviceResolution final {
        AudioDeviceResolutionStatus status{AudioDeviceResolutionStatus::InvalidSnapshot};
        AudioCaptureDeviceId device;
        std::uint64_t revision{};
    };

    /** @brief Validates recording defaults, stable identities, optional format evidence and permission state.
     * @param snapshot Complete capture discovery from the selected adapter.
     * @return True when the snapshot is structurally valid, including an empty inventory.
     */
    [[nodiscard]] bool ValidateAudioCaptureDeviceSnapshot(const AudioCaptureDeviceSnapshot &snapshot) noexcept;

    /** @brief Resolves capture intent without opening a stream or crossing into playback.
     * @param snapshot Complete immutable recording inventory.
     * @param selection Requested recording identity or role.
     * @return A recording identity and revision or a typed failure with neither.
     */
    [[nodiscard]] AudioCaptureDeviceResolution ResolveAudioCaptureDevice(const AudioCaptureDeviceSnapshot &snapshot,
                                                                         const AudioCaptureDeviceSelection &selection) noexcept;
}  // namespace Horo::Audio
