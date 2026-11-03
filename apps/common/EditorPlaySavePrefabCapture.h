#pragma once
/** @file EditorPlaySavePrefabCapture.h
 * @brief Canonical dynamic recipe participant using the same actual bounded Play inventory.
 */
#include "EditorPlaySavePrefabServices.h"
#include "Horo/Prefab/PrefabPersistentCodec.h"
#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"

namespace Horo::Application::Internal {
    /** @brief Exact host-owned canonical dynamic recipe schema; actual restore remains inside SceneRestoreBundle. */
    [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor PlaySavePrefabDescriptor();
    [[nodiscard]] Runtime::SaveRecordId PlaySavePrefabRecord();
    /** @brief Decodes bounded detached recipe values only; actual installed content/Scene/native ownership remains owner-lane work. */
    [[nodiscard]] Result<std::optional<Prefab::PrefabPersistentState>> DecodePlaySavePrefabs(
        const Runtime::ValidatedSaveArchive &archive, const std::optional<Prefab::PrefabLimitProfile> &profile, std::size_t authoredCount);

    class PlaySavePrefabCaptureAdapter final : public Runtime::ICanonicalStateAdapter {
    public:
        explicit PlaySavePrefabCaptureAdapter(std::shared_ptr<PlaySavePrefabServices> services) : services_(std::move(services)) {}

        [[nodiscard]] Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &context,
                                                                           Runtime::ICanonicalCaptureSink &sink) const override;

    private:
        std::shared_ptr<PlaySavePrefabServices> services_;
    };
}  // namespace Horo::Application::Internal
