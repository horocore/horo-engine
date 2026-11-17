#pragma once
/** @file AudioComponentPreview.h
 * @brief Host-owned source-inspector preview with copied scene values and explicit unavailable outcomes.
 */
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Audio/AudioFrontend.h"
#include "Horo/Audio/AudioSpatialModel.h"

#include <cstdint>
#include <filesystem>

namespace Horo::Editor {
    namespace AudioComponentPreviewErrors {
        extern const ErrorCodeDescriptor MissingAsset;
        extern const ErrorCodeDescriptor MissingListener;
        extern const ErrorCodeDescriptor MissingBus;
        extern const ErrorCodeDescriptor ProviderUnavailable;
        extern const ErrorCodeDescriptor Busy;
        extern const ErrorCodeDescriptor Stale;
    }  // namespace AudioComponentPreviewErrors

    /** @brief Immutable authoring capture; no document/ECS reference reaches Audio callbacks. */
    struct AudioComponentPreviewRequest final {
        Assets::AssetId workspace;
        std::uint64_t revision{};
        std::uint64_t object{};
        std::filesystem::path projectRoot;
        Assets::AssetRegistrySnapshot assets;
        Audio::AudioSpatialSource source;
        std::optional<Audio::AudioSpatialListener> listener;
    };

    /** @brief Request preparation before a frontend/device identity exists; None alone never proves native retirement. */
    enum class AudioComponentPreviewPreparation : std::uint8_t {
        None,
        Pending
    };

    /** @brief Owned UI status; Active means admitted Audio output, not proof of human audibility. */
    struct AudioComponentPreviewSnapshot final {
        Assets::AssetId workspace;
        std::uint64_t revision{};
        std::uint64_t object{};
        std::optional<Audio::AudioFrontendSnapshot> output;
        std::optional<Error> failure;
        AudioComponentPreviewPreparation preparation{AudioComponentPreviewPreparation::None}; /**< Pending requires output to be absent. */
    };

    /** @brief Borrowed application capability; the host outlives workspaces and retains closing output.
     * @details Owner thread only. Widgets submit typed workspace intents; they never construct backends.
     * Selection/revision/close revoke preview admission. The application pumps retirement even after
     * the workspace is gone. No fallback backend or implicit listener is permitted.
     */
    class IAudioComponentPreview {
    public:
        virtual ~IAudioComponentPreview() = default;
        /** @brief Prepares an isolated production Audio lane for an immutable scene capture.
         * @param request Exact workspace/object/revision and source data.
         * @return Admission or typed missing/busy/error outcome. Success may admit asynchronous
         * preparation; it never proves activation. Pump/Snapshot retain later preparation failures.
         * The host copies values before returning and never borrows caller scene/document storage.
         */
        [[nodiscard]] virtual Result<void> Start(const AudioComponentPreviewRequest &request) = 0;
        /** @brief Revokes the matching workspace preview and requests asynchronous native retirement.
         * @param workspace Exact owner; a foreign owner cannot stop another workspace. */
        virtual void Stop(Assets::AssetId workspace) noexcept = 0;
        /** @brief Advances bounded control work or harvests a dedicated control owner's progress outside widget drawing.
         * @return Original failure or progress; an owner-thread bridge never waits for native work here.
         */
        [[nodiscard]] virtual Result<void> Pump() = 0;
        /** @brief Copies retained status. @return Values without mutable runtime/native borrows. */
        [[nodiscard]] virtual AudioComponentPreviewSnapshot Snapshot() const = 0;
    };
}  // namespace Horo::Editor
