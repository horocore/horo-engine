#pragma once
/** @file AudioComponentPreview.h
 * @brief Host-owned source-inspector preview with copied scene values and explicit unavailable outcomes.
 */
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Audio/AudioFrontend.h"
#include "Horo/Audio/AudioSpatialModel.h"

#include <filesystem>

namespace Horo::Editor {
    namespace AudioComponentPreviewErrors {
        extern const ErrorCodeDescriptor MissingAsset;
        extern const ErrorCodeDescriptor MissingListener;
        extern const ErrorCodeDescriptor MissingBus;
        extern const ErrorCodeDescriptor ProviderUnavailable;
        extern const ErrorCodeDescriptor Busy;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor UnsupportedSwitch;
    }  // namespace AudioComponentPreviewErrors

    /** @brief Caller-selected policy for playback on a replacement output. */
    enum class AudioComponentPreviewSwitchPolicy : std::uint8_t {
        Restart,
        Preserve
    };

    /** @brief Explicit intent against the exact active output, resolved in a fresh catalog for its backend. */
    struct AudioComponentPreviewOutputSwitch final {
        Audio::AudioDeviceEpoch source;
        Audio::AudioDefaultDeviceRole targetRole{Audio::AudioDefaultDeviceRole::Multimedia};
        AudioComponentPreviewSwitchPolicy policy{AudioComponentPreviewSwitchPolicy::Preserve};
    };

    /** @brief One accepted switch's observable disposition. */
    enum class AudioComponentPreviewSwitchStatus : std::uint8_t {
        Pending,
        Applied,
        Failed,
        Cancelled
    };

    /** @brief Retained switch receipt; Applied proves the replacement Start ran on its callback. */
    struct AudioComponentPreviewSwitchReceipt final {
        std::uint64_t id{}; /**< Nonzero identity, unique for the application preview owner. */
        AudioComponentPreviewOutputSwitch intent;
        AudioComponentPreviewSwitchStatus status{AudioComponentPreviewSwitchStatus::Pending};
        std::optional<Audio::AudioDeviceEpoch> target; /**< Exact replacement output after activation, if reached. */
        std::optional<Error> failure;
    };

    /** @brief Immutable authoring capture; no document/ECS reference reaches Audio callbacks. */
    struct AudioComponentPreviewRequest final {
        Assets::AssetId workspace;
        std::uint64_t revision{};
        std::uint64_t object{};
        std::filesystem::path projectRoot;
        Assets::AssetRegistrySnapshot assets;
        Audio::AudioSpatialSource source;
        std::optional<Audio::AudioSpatialListener> listener;
        std::optional<AudioComponentPreviewOutputSwitch> outputSwitch; /**< Uses the active preview's admitted source and listener. */
    };

    /** @brief Owned UI status; Active means admitted Audio output, not proof of human audibility. */
    struct AudioComponentPreviewSnapshot final {
        Assets::AssetId workspace;
        std::uint64_t revision{};
        std::uint64_t object{};
        std::optional<Audio::AudioFrontendSnapshot> output;
        std::optional<Error> failure;
        std::optional<AudioComponentPreviewSwitchReceipt>
            outputSwitch; /**< Retained until this capture changes or another switch is admitted. */
    };

    /** @brief Borrowed application capability; the host outlives workspaces and retains closing output.
     * @details Owner thread only. Widgets submit typed workspace intents; they never construct backends.
     * Selection/revision/close revoke preview admission. The application pumps retirement even after
     * the workspace is gone. No fallback backend or implicit listener is permitted.
     */
    class IAudioComponentPreview {
    public:
        virtual ~IAudioComponentPreview() = default;
        /** @brief Prepares a preview or admits an explicit replacement for the current output.
         * @param request Exact workspace/object/revision, and source data for a fresh preview or a fenced switch intent.
         * @return Admission or a typed failure. Unsupported switch policies leave current playback running. */
        [[nodiscard]] virtual Result<void> Start(const AudioComponentPreviewRequest &request) = 0;
        /** @brief Revokes the matching workspace preview and requests asynchronous native retirement.
         * @param workspace Exact owner; a foreign owner cannot stop another workspace. */
        virtual void Stop(Assets::AssetId workspace) noexcept = 0;
        /** @brief Advances bounded native/control work outside widget drawing. @return Original failure or progress. */
        [[nodiscard]] virtual Result<void> Pump() = 0;
        /** @brief Copies retained status. @return Values without mutable runtime/native borrows. */
        [[nodiscard]] virtual AudioComponentPreviewSnapshot Snapshot() const = 0;
    };
}  // namespace Horo::Editor
