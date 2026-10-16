#pragma once
/** @file InspectorBehaviorBinding.h @brief Host-owned typed behavior drafts prepared for the canonical Scene transaction owner. */
#include "Horo/Editor/SceneDocumentTransaction.h"
#include "Horo/Gameplay/BehaviorTypes.h"

namespace Horo::Editor {
    inline constexpr std::size_t MaximumInspectorBehaviorSelection = 256;

    /** @brief Exact attachment chosen by the editor's selection owner, never by an extension callback. */
    struct InspectorBehaviorTarget final {
        SceneSource::SceneObjectId object;
        Gameplay::BehaviorInstanceId attachment;
        bool operator==(const InspectorBehaviorTarget &) const = default;
    };

    /** @brief One copied declared value; mixed is presentation evidence, never a value to serialize. */
    struct InspectorBehaviorValue final {
        std::string field;
        Gameplay::BehaviorFieldValue value;
        bool mixed{};
    };

    /** @brief One exact owner snapshot used only by the host to prepare an atomic edit. */
    struct InspectorBehaviorBaseline final {
        SceneSource::SceneObjectSnapshot object;
        Gameplay::BehaviorInstanceId attachment;
        Sha256Digest content;
    };

    /** @brief Bounded copied draft; provider projections must omit baselines and document authority. */
    struct InspectorBehaviorDraft final {
        SceneTransactionSession session;
        std::uint64_t revision{};
        std::uint64_t state{};
        std::uint64_t selectionGeneration{};
        Gameplay::BehaviorTypeId type;
        std::uint32_t schemaVersion{};
        std::vector<InspectorBehaviorValue> fields;
        std::vector<InspectorBehaviorBaseline> baselines;
    };

    /** @brief Copies a common declared schema and exact typed/mixed values from actual selected attachments.
     * @param snapshot Current canonical document capture. @param targets Explicit unique editor-owned selection.
     * @param descriptor Current host-resolved behavior descriptor. @param selectionGeneration Nonzero host selection epoch.
     * @return Owned draft or typed invalid/capacity/stale failure; no callbacks, mutation or serialization fallback.
     * @details Missing descriptors remain the generic inspector's responsibility. This profile never decodes opaque component envelopes.
     */
    [[nodiscard]] Result<InspectorBehaviorDraft> CaptureInspectorBehavior(const SceneTransactionSnapshot &snapshot,
                                                                          std::span<const InspectorBehaviorTarget> targets,
                                                                          const Gameplay::BehaviorDescriptor &descriptor,
                                                                          std::uint64_t selectionGeneration);
    /** @brief Prepares one declared field replacement for every exact selected attachment without publishing anything.
     * @param draft Immutable host capture. @param field Declared field identity. @param value Exact candidate type.
     * @param selectionGeneration Current selection epoch; must match the capture.
     * @return Complete reversible replacements for ONE SceneDocumentTransactions::Execute, or zero-mutation failure.
     * @details The caller rechecks provider capability/generation and owns the current context/transaction. All unedited fields,
     * attachment identities, opaque components and generated ownership remain byte-for-byte semantic copies. Unknown/future fields
     * are preserved. Canonical transaction admission rejects stale document content and locked/generated edits as usual.
     */
    [[nodiscard]] Result<std::vector<SceneObjectReplacement>> PrepareInspectorBehaviorEdit(const InspectorBehaviorDraft &draft,
                                                                                           std::string_view field,
                                                                                           const Gameplay::BehaviorFieldValue &value,
                                                                                           std::uint64_t selectionGeneration);
}  // namespace Horo::Editor
