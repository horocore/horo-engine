#pragma once

/** @file EditorPlaySaveHostCapture.h
 * @brief Actual isolated Play Scene capture and bounded canonical record admission.
 */
#include "EditorPlaySaveSession.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"
#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Scene/PersistentEntityIdentity.h"
#include "Horo/Scene/CookedSceneDefinition.h"

namespace Horo::Application::Internal {
    /** @brief Detached current core Scene values and explicit durable live/deleted identity records. */
    struct PlaySaveSceneState final {
        Runtime::RuntimeSceneDefinition definition;
        std::vector<Runtime::PersistentEntityRecord> identities;
    };

    /** @brief Stable single core-Scene owner; other subsystem state requires its own registered adapters. */
    [[nodiscard]] Runtime::SaveParticipantId PlaySaveSceneParticipant();
    /** @brief Stable canonical record owned exclusively by PlaySaveSceneParticipant. */
    [[nodiscard]] Runtime::SaveRecordId PlaySaveSceneRecord();
    /** @brief Declares bounded core capture without registering or inspecting a Scene.
     * @details Core restore is the actual SceneRestoreBundle aggregate transfer, not a duplicate staged participant.
     * Host semantic admission requires this record and its complete typed Scene state before that transfer is queued.
     */
    [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor PlaySaveSceneDescriptor();

    /** @brief Resolves the actual current authored entities through this
     * activation's sole durable inventory.
     * @details The returned immutable value derives no new IDs and rejects missing
     * structural provenance. */
    [[nodiscard]] Result<Runtime::PersistentEntityIdentityMap> CapturePlaySaveIdentities(
        Runtime::RuntimeSceneView scene, const Runtime::RuntimeSceneDefinition &baseline,
        std::span<const Runtime::PersistentEntityRecord> identities);
    /** @brief Captures current values using host-issued durable bindings, never transient slot-derived identities.
     * @param scene Actual current isolated Play view on its owner thread. @param baseline Exact activation definition.
     * @param identities Complete host-issued authored identity inventory, including absent objects retained as tombstones.
     * @return Detached current Scene or typed unsupported/stale/malformed error; required unregistered peer state fails closed.
     */
    [[nodiscard]] Result<PlaySaveSceneState> CapturePlaySaveScene(Runtime::RuntimeSceneView scene,
                                                                  const Runtime::RuntimeSceneDefinition &baseline,
                                                                  std::span<const Runtime::PersistentEntityRecord> identities,
                                                                  std::span<const Editor::PlaySaveGameplayBinding> gameplay = {});
    /** @brief Validates every captured core entity against implemented participant coverage before archive admission.
     * @param definition Detached actual or decoded Scene state. @return Original typed unsupported or bounds failure.
     */
    [[nodiscard]] Result<void> ValidatePlaySaveCoreState(const Runtime::RuntimeSceneDefinition &definition,
                                                         std::span<const Editor::PlaySaveGameplayBinding> gameplay = {});
    /** @brief Checks only supported core field/provenance shape on detached input; it grants no executable coverage or publication.
     * @param definition Integrity-admitted detached values. @return Typed structural/unsupported failure or structural success.
     * Actual native installation/descriptor checks remain mandatory at owner capture and candidate restore.
     */
    [[nodiscard]] Result<void> ValidatePlaySaveCoreStructure(const Runtime::RuntimeSceneDefinition &definition);
    /** @brief Decodes bounded data only; no native installation/source is retained or inferred from metadata. */
    [[nodiscard]] Result<PlaySaveSceneState> DecodePlaySaveSceneStructure(std::span<const std::byte> bytes,
                                                                          const Runtime::RuntimeSceneDefinition &baseline,
                                                                          std::span<const Runtime::PersistentEntityRecord> identities);

    /** @brief Encodes detached Scene values with the existing source-free Scene codec inside a versioned canonical record.
     * @param state Actual owner capture. @return Bounded canonical bytes or original codec/identity failure.
     */
    [[nodiscard]] Result<Runtime::CanonicalEncodedValue> EncodePlaySaveScene(const PlaySaveSceneState &state);
    /** @brief Decodes a canonical record under the exact active sandbox's immutable baseline and identity authority.
     * @param bytes Integrity-admitted record bytes. @param baseline Actual immutable activation baseline.
     * @param identities Complete host-issued inventory; imported identities cannot replace its authority.
     * @return Detached validated state or original malformed/foreign/unsupported error, with no live-world mutation.
     */
    [[nodiscard]] Result<PlaySaveSceneState> DecodePlaySaveScene(std::span<const std::byte> bytes,
                                                                 const Runtime::RuntimeSceneDefinition &baseline,
                                                                 std::span<const Runtime::PersistentEntityRecord> identities,
                                                                 std::span<const Editor::PlaySaveGameplayBinding> gameplay = {});

    /** @brief Owner-phase adapter over the real Scene service; the containing Play host outlives accepted snapshots/workers.
     * @details Capture alone borrows Scenes(). Workers consume only copied immutable payload; they never call this adapter.
     */
    class PlaySaveSceneCapture final : public Runtime::ICanonicalStateAdapter {
    public:
        /** @brief Captures explicit owner references and durable inventory without reading or mutating the Scene. */
        PlaySaveSceneCapture(Runtime::RuntimeSceneService &scenes, Runtime::RuntimeSceneDefinition baseline,
                             std::vector<Runtime::PersistentEntityRecord> identities,
                             std::weak_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> gameplay = {});
        /** @copydoc Runtime::ICanonicalStateAdapter::Capture */
        [[nodiscard]] Result<Runtime::CanonicalCaptureDisposition> Capture(const Runtime::CanonicalCaptureContext &context,
                                                                           Runtime::ICanonicalCaptureSink &sink) const override;

    private:
        Runtime::RuntimeSceneService &scenes_;
        Runtime::RuntimeSceneDefinition baseline_;
        std::vector<Runtime::PersistentEntityRecord> identities_;
        std::weak_ptr<const std::vector<Editor::PlaySaveGameplayBinding>> gameplay_;
    };
}  // namespace Horo::Application::Internal
