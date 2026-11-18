#pragma once

/** @file BlackboardSchemaDocument.h
 * @brief Typed editor-owned Blackboard source rows, validated projections, and semantic undo.
 */

#include "Horo/AI/BlackboardSchema.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Foundation/CancellationToken.h"

namespace Horo::Editor {
    namespace BlackboardDocumentErrors {
        extern const ErrorCodeDescriptor InvalidEdit;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor StaleRevision;
        extern const ErrorCodeDescriptor ReferenceResolutionRequired;
        extern const ErrorCodeDescriptor HistoryUnavailable;
        extern const ErrorCodeDescriptor HistoryCapacity;
        extern const ErrorCodeDescriptor IdentityExhausted;
        extern const ErrorCodeDescriptor WrongThread;
    }  // namespace BlackboardDocumentErrors

    /** @brief One authored key. Display text and presentation order are independent of the stable key ID. */
    struct BlackboardSchemaRow final {
        AI::BlackboardKeyDescriptor descriptor;
        std::string displayName;
    };

    /** @brief Complete detached authoring source for an Asset Registry identity and a stable AI schema identity. */
    struct BlackboardSchemaSource final {
        Assets::AssetId asset;
        AI::BlackboardSchemaId schema;
        std::uint32_t version{1};
        AI::BlackboardUnknownValuePolicy unknownValues{AI::BlackboardUnknownValuePolicy::Reject};
        std::vector<BlackboardSchemaRow> rows; /**< Authored presentation order. */
    };

    /** @brief Adds a new never-issued stable key at a zero-based position in the typed table. */
    struct AddBlackboardKey final {
        BlackboardSchemaRow row;
        std::size_t index{};
    };

    /** @brief Edits authored display text without changing the runtime schema or stable key. */
    struct RenameBlackboardKey final {
        AI::BlackboardKeyId key;
        std::string displayName;
    };

    /** @brief Moves a row to its final zero-based position with the same stable identity. */
    struct MoveBlackboardKey final {
        AI::BlackboardKeyId key;
        std::size_t index{};
    };

    /** @brief Replaces the typed descriptor of a draft-only key. Durable keys require a reference-resolution transaction. */
    struct EditBlackboardKey final {
        AI::BlackboardKeyDescriptor descriptor;
    };

    /** @brief Removes a draft-only key. A registered durable key is fenced pending explicit reference resolution. */
    struct RemoveBlackboardKey final {
        AI::BlackboardKeyId key;
    };

    /** @brief Closed authoring command vocabulary; the editor never modifies a live Blackboard instance. */
    using BlackboardSchemaEdit =
        std::variant<AddBlackboardKey, RenameBlackboardKey, MoveBlackboardKey, EditBlackboardKey, RemoveBlackboardKey>;

    /** @brief Immutable capture of the exact authored rows and last valid runtime projection for a document revision. */
    struct BlackboardSchemaDocumentSnapshot final {
        DocumentIdentity document;
        std::uint64_t revision{};
        std::shared_ptr<const BlackboardSchemaSource> source;
        std::shared_ptr<const AI::BlackboardSchema> schema;
    };

    /** @brief Control-thread typed authoring owner for an existing Blackboard asset.
     * @details The document validates detached commands through the runtime schema before publishing any revision.
     * It owns neither file writes nor a project reference index. It fails closed for descriptor changes to keys
     * present in the original authored source, including history replay. All captures own their bytes independently.
     */
    class BlackboardSchemaDocument final {
    public:
        /** @brief Validates and opens the exact existing asset source.
         * @param document Asset document session issued by the host.
         * @param source Complete authored rows, stable identities and version.
         * @return Owned document or original validation/identity error.
         */
        [[nodiscard]] static Result<std::unique_ptr<BlackboardSchemaDocument>> Open(DocumentIdentity document,
                                                                                    BlackboardSchemaSource source);
        ~BlackboardSchemaDocument();

        BlackboardSchemaDocument(const BlackboardSchemaDocument &) = delete;
        BlackboardSchemaDocument &operator=(const BlackboardSchemaDocument &) = delete;

        /** @brief Captures the current source and sorted runtime schema. @return Owned value or wrong-thread failure. */
        [[nodiscard]] Result<BlackboardSchemaDocumentSnapshot> Snapshot() const;
        /** @brief Proposes the next unused stable key ID for an Add command.
         * @return Next ID above this opening's high-water mark, or an owner/exhaustion failure.
         * @post No identity advances until an Add command successfully publishes.
         */
        [[nodiscard]] Result<AI::BlackboardKeyId> NextKeyId() const;
        /** @brief Validates an edit and publishes one semantic delta, or preserves every current value on failure.
         * @param expectedRevision Current snapshot revision.
         * @param edit One exact typed operation.
         * @param cancellation Synchronous mutation fence.
         * @return Success, typed stale/invalid/impact/capacity refusal, or canonical schema validation error.
         */
        [[nodiscard]] Result<void> Execute(std::uint64_t expectedRevision, const BlackboardSchemaEdit &edit,
                                           CancellationToken cancellation = {});
        /** @brief Restores the prior exact rows under the same original-key fence.
         * @param expectedRevision Current snapshot revision. @param cancellation Synchronous mutation fence.
         * @return Success or typed refusal with both history and source unchanged.
         */
        [[nodiscard]] Result<void> Undo(std::uint64_t expectedRevision, CancellationToken cancellation = {});
        /** @brief Reapplies a previously published exact edit.
         * @param expectedRevision Current snapshot revision. @param cancellation Synchronous mutation fence.
         * @return Success or typed refusal with both history and source unchanged.
         */
        [[nodiscard]] Result<void> Redo(std::uint64_t expectedRevision, CancellationToken cancellation = {});

    private:
        struct State;
        explicit BlackboardSchemaDocument(std::unique_ptr<State> state) noexcept;
        [[nodiscard]] Result<void> Replay(std::uint64_t expectedRevision, bool redo, CancellationToken cancellation);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
