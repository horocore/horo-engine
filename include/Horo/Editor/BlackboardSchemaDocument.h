#pragma once

/** @file BlackboardSchemaDocument.h
 * @brief Typed editor-owned Blackboard source rows, validated projections, and semantic undo.
 */

#include "Horo/AI/BlackboardSchema.h"
#include "Horo/Assets/AssetId.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Sha256.h"

#include <filesystem>

namespace Horo::Assets {
    class AssetRegistrySnapshot;
}

namespace Horo {
    class DurableFileSystem;
}

namespace Horo::Editor {
    class ProjectMutationCoordinator;

    namespace BlackboardDocumentErrors {
        extern const ErrorCodeDescriptor InvalidEdit;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor StaleRevision;
        extern const ErrorCodeDescriptor ReferenceResolutionRequired;
        extern const ErrorCodeDescriptor HistoryUnavailable;
        extern const ErrorCodeDescriptor HistoryCapacity;
        extern const ErrorCodeDescriptor IdentityExhausted;
        extern const ErrorCodeDescriptor WrongThread;
        extern const ErrorCodeDescriptor Dirty;
        extern const ErrorCodeDescriptor Busy;
    }  // namespace BlackboardDocumentErrors

    /** @brief One authored key. Display text and presentation order are independent of the stable key ID. */
    struct BlackboardSchemaRow final {
        AI::BlackboardKeyDescriptor descriptor;
        std::optional<std::string> displayName; /**< Absent on semantic-only legacy sources. */

        bool operator==(const BlackboardSchemaRow &) const noexcept = default;
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

    /** @brief Exact workspace revision captured when a command or dirty-close decision was offered. */
    struct BlackboardDocumentFence final {
        DocumentInstanceId instance;
        std::uint64_t revision{};
    };

    /** @brief Validated, detached candidate for explicit durable-key reference resolution. */
    struct BlackboardReferenceEditPreview final {
        BlackboardDocumentFence fence;
        AI::BlackboardKeyId key;
        std::shared_ptr<const BlackboardSchemaSource> source;
        std::shared_ptr<const AI::BlackboardSchema> schema;
    };

    /** @brief Immutable capture of the exact authored rows and last valid runtime projection for a document revision. */
    struct BlackboardSchemaDocumentSnapshot final {
        DocumentIdentity document;
        std::uint64_t revision{};
        std::shared_ptr<const BlackboardSchemaSource> source;
        std::shared_ptr<const AI::BlackboardSchema> schema;
        std::shared_ptr<const AI::BlackboardSchema> durableSchema;
        std::optional<AI::BlackboardKeyId> nextKey;
        bool dirty{};
        bool conflict{};
        bool canUndo{};
        bool canRedo{};
        std::optional<Sha256Digest> sourceDigest; /**< Last accepted exact file bytes, when the document owns a file. */
    };

    /** @brief Control-thread typed authoring owner for an existing Blackboard asset.
     * @details The document validates detached commands through the runtime schema before publishing any revision.
     * File-backed openings reuse Assets capture and publication under the project mutation owner. The current durable
     * schema fences existing key semantics during editing and history replay. Captures retain their values independently.
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
        /** @brief Opens admitted source bytes through the exact current Asset Registry record and source codec.
         * @param document Exact Asset document session and canonical project-relative path.
         * @param registry Pinned registry captured by the host that read these source bytes.
         * @param asset Stable identity of the selected asset.
         * @param bytes Complete independently owned source capture for that registry record.
         * @return Typed document or identity, transport, or schema failure. Legacy rows contain no fabricated labels.
         */
        [[nodiscard]] static Result<std::unique_ptr<BlackboardSchemaDocument>> OpenRegistered(DocumentIdentity document,
                                                                                              const Assets::AssetRegistrySnapshot &registry,
                                                                                              Assets::AssetId asset,
                                                                                              std::span<const std::uint8_t> bytes);
        /** @brief Opens the canonical registered file through the shared bounded Assets reader.
         * @param document Exact Asset document session and canonical project-relative path.
         * @param registry Current host-owned registry. @param root Authorized canonical project root.
         * @param path Resolved file from the normal source-open route. @param cancellation Read cancellation.
         * @return File-backed document or original identity, transport or schema failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<BlackboardSchemaDocument>> OpenFile(DocumentIdentity document,
                                                                                        const Assets::AssetRegistrySnapshot &registry,
                                                                                        const std::filesystem::path &root,
                                                                                        const std::filesystem::path &path,
                                                                                        CancellationToken cancellation = {});
        ~BlackboardSchemaDocument();

        BlackboardSchemaDocument(const BlackboardSchemaDocument &) = delete;
        BlackboardSchemaDocument &operator=(const BlackboardSchemaDocument &) = delete;

        /** @brief Returns the unchanged workspace identity. @return Borrowed until document destruction. */
        [[nodiscard]] const DocumentIdentity &Identity() const noexcept;

        /** @brief Captures the current source and sorted runtime schema. @return Owned value or wrong-thread failure. */
        [[nodiscard]] Result<BlackboardSchemaDocumentSnapshot> Snapshot() const;
        /** @brief Encodes a current revision for the host's existing durable source publication.
         * @param expectedRevision Exact snapshot revision. @return Source bytes or typed stale/lane/codec failure.
         * @post Neither disk state nor the semantic history changes.
         */
        [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeSource(std::uint64_t expectedRevision) const;
        /** @brief Proposes the next unused stable key ID for an Add command.
         * @return Next ID above this opening's high-water mark, or an owner/exhaustion failure.
         * @post No identity advances until an Add command successfully publishes.
         */
        [[nodiscard]] Result<AI::BlackboardKeyId> NextKeyId() const;
        /** @brief Validates a durable key change before the host reviews its references.
         * @param expectedRevision Current document revision.
         * @param edit Proposed typed replacement or removal.
         * @param cancellation Synchronous validation fence.
         * @return Detached candidate or the original edit or schema validation error.
         * @post Source, history and file state are unchanged.
         */
        [[nodiscard]] Result<BlackboardReferenceEditPreview> PreviewReferenceEdit(std::uint64_t expectedRevision,
                                                                                  const BlackboardSchemaEdit &edit,
                                                                                  CancellationToken cancellation = {}) const;
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

        /** @brief Saves the current source through project mutation and exact source-byte publication.
         * @param expectedRevision Current owner snapshot. @param registry Current project registry.
         * @param mutations Project mutation owner. @param files Shared native durable filesystem. @param cancellation Save fence.
         * @return Success only on durable publication (or clean document), or typed error with dirty state retained.
         */
        [[nodiscard]] Result<void> Save(std::uint64_t expectedRevision, const Assets::AssetRegistrySnapshot &registry,
                                        ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                        CancellationToken cancellation = {});
        /** @brief Reloads an exact registered source and clears history after successful typed validation.
         * @param expectedRevision Current owner snapshot. @param registry Current project registry.
         * @param discardDirty Explicit consent to replace unsaved changes. @param cancellation Read and commit fence.
         * @return Success or original identity/read/validation failure with the current draft unchanged.
         */
        [[nodiscard]] Result<void> Reload(std::uint64_t expectedRevision, const Assets::AssetRegistrySnapshot &registry,
                                          bool discardDirty = false, CancellationToken cancellation = {});

    private:
        struct State;
        explicit BlackboardSchemaDocument(std::unique_ptr<State> state) noexcept;
        [[nodiscard]] Result<void> Replay(std::uint64_t expectedRevision, bool redo, CancellationToken cancellation);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
