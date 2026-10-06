#pragma once

/** @file FractureAssetDocument.h
 * @brief Revisioned fracture source authoring, semantic history and Assets publication receipts.
 */

#include "Horo/Destruction/DestructibleDescriptor.h"
#include "Horo/Foundation/CancellationToken.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Editor {
    namespace FractureDocumentErrors {
        extern const ErrorCodeDescriptor InvalidSource;
        extern const ErrorCodeDescriptor LimitExceeded;
        extern const ErrorCodeDescriptor WrongDocument;
        extern const ErrorCodeDescriptor StaleRevision;
        extern const ErrorCodeDescriptor AuthorityDenied;
        extern const ErrorCodeDescriptor HistoryBudgetExceeded;
        extern const ErrorCodeDescriptor PublicationConflict;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor Exhausted;
        extern const ErrorCodeDescriptor UnsupportedVersion;
    }  // namespace FractureDocumentErrors

    struct FractureDocumentSessionTag;
    struct FractureDocumentRevisionTag;
    struct FractureDocumentStateTag;
    struct FractureSourceRevisionTag;
    struct FractureRecipeTag;
    struct FractureRecipeRevisionTag;
    /** @brief Host-issued nonzero lifetime of one writable asset document. */
    using FractureDocumentSession = Destruction::DestructionStableIdentity<FractureDocumentSessionTag>;
    /** @brief Non-wrapping revision advanced by edits, undo and redo. */
    using FractureDocumentRevision = Destruction::DestructionStableIdentity<FractureDocumentRevisionTag>;
    /** @brief Session-local content identity restored by semantic undo/redo. */
    using FractureDocumentStateId = Destruction::DestructionStableIdentity<FractureDocumentStateTag>;
    /** @brief Exact nonzero Assets source publication revision, distinct from editor mutation revision. */
    using FractureSourceRevision = Destruction::DestructionStableIdentity<FractureSourceRevisionTag>;
    /** @brief Stable host-selected offline recipe identity. */
    using FractureRecipeId = Destruction::DestructionStableIdentity<FractureRecipeTag>;
    /** @brief Exact nonzero accepted offline recipe revision. */
    using FractureRecipeRevision = Destruction::DestructionStableIdentity<FractureRecipeRevisionTag>;

    inline constexpr std::uint32_t FractureSourceSchemaVersion = 1;
    inline constexpr std::size_t MaximumFractureSourceBytes = 1024 * 1024;
    inline constexpr std::size_t MaximumFractureContacts = 4096;
    inline constexpr std::size_t MaximumFractureMaterials = 256;
    inline constexpr std::size_t MaximumFracturePatchOperations = 256;

    /** @brief Core offline source modes; runtime cutting is deliberately absent. */
    enum class FractureSourceAlgorithm : std::uint8_t {
        PreFractured,
        Voronoi
    };

    /** @brief Stable authored site; position is intent, never generated geometry. */
    struct FractureSourceSite final {
        Destruction::DestructionChunkId chunk;
        std::array<double, 3> position{};
        bool operator==(const FractureSourceSite &) const = default;
    };

    /** @brief Exact dependencies and deterministic offline recipe settings. */
    struct FractureSourceSettings final {
        Assets::AssetId sourceMesh;
        FractureSourceRevision sourceRevision;
        Sha256Digest sourceDigest;
        FractureSourceAlgorithm algorithm{FractureSourceAlgorithm::PreFractured};
        FractureRecipeId recipe;
        FractureRecipeRevision recipeRevision;
        std::uint64_t seed{};
        std::uint32_t algorithmVersion{1};
        Sha256Digest toolchainDigest;
        Destruction::DestructionFeatureTier tier{Destruction::DestructionFeatureTier::Baseline};
        Destruction::DestructionFeatureSet requiredFeatures;
        std::uint32_t interiorMaterialSlot{};
        double exteriorUvScale{1};
        double interiorUvScale{1};
        std::vector<FractureSourceSite> sites; /**< Stable chunk-ID order; empty for imported sources. */
        bool operator==(const FractureSourceSettings &) const = default;
    };

    /** @brief Source annotation keyed by stable chunk identity, independent of cook ordinals. */
    struct FractureSourceChunk final {
        Destruction::DestructionChunkId id;
        Destruction::DestructionChunkId parent; /**< Zero denotes a hierarchy root. */
        std::uint32_t materialSlot{};
        bool anchor{};
        bool required{};
        bool operator==(const FractureSourceChunk &) const = default;
    };

    /** @brief Undirected authored support contact; low/high are stable IDs. */
    struct FractureSourceContact final {
        Destruction::DestructionChunkId low;
        Destruction::DestructionChunkId high;
        double weight{};
        bool operator==(const FractureSourceContact &) const = default;
    };

    /** @brief Required path-free material dependency with exact accepted digest. */
    struct FractureSourceMaterial final {
        std::uint32_t slot{};
        Assets::AssetId asset;
        Sha256Digest digest;
        bool operator==(const FractureSourceMaterial &) const = default;
    };

    /** @brief Authoring damage/behavior intent; no live health, broken sets or body motion. */
    struct FractureSourceDamage final {
        Destruction::DestructionHealthPolicy health;
        Destruction::DestructionBehaviorPolicy behavior;
        Destruction::DestructionCleanupPolicy cleanup;
        bool operator==(const FractureSourceDamage &) const = default;
    };

    /** @brief Complete bounded canonical authoring source; contains no derived/native/runtime data. */
    struct FractureAssetSource final {
        Destruction::FractureAssetId asset;
        FractureSourceSettings settings;
        std::vector<FractureSourceChunk> chunks;       /**< Strict stable-ID order. */
        std::vector<FractureSourceContact> contacts;   /**< Strict (low, high) order. */
        std::vector<FractureSourceMaterial> materials; /**< Strict slot order. */
        FractureSourceDamage damage;
        bool operator==(const FractureAssetSource &) const = default;
    };

    /** @brief Checks source references, hierarchy, support, capabilities and all finite bounds.
     * @param source Complete source candidate.
     * @return Success or a typed zero-mutation failure.
     */
    [[nodiscard]] Result<void> ValidateFractureAssetSource(const FractureAssetSource &source);
    /** @brief Encodes validated authoring values in the versioned portable source schema.
     * @param source Complete immutable source.
     * @return Detached bytes for Assets source publication; no files or artifacts are written.
     */
    [[nodiscard]] Result<std::vector<std::byte>> EncodeFractureAssetSource(const FractureAssetSource &source);
    /** @brief Parses bounded source bytes before opening a writable document.
     * @param bytes Complete source publication bytes, borrowed only for this call.
     * @return Validated source or a typed malformed/version/limit failure.
     */
    [[nodiscard]] Result<FractureAssetSource> DecodeFractureAssetSource(std::span<const std::byte> bytes);

    /** @brief Typed replacement of deterministic source/recipe intent. */
    struct SetFractureSettings final {
        FractureSourceSettings value;
    };

    /** @brief Typed insertion/replacement of one stable chunk annotation. */
    struct PutFractureChunk final {
        FractureSourceChunk value;
    };

    /** @brief Typed deletion; dangling references reject the complete transaction. */
    struct RemoveFractureChunk final {
        Destruction::DestructionChunkId id;
    };

    /** @brief Typed insertion/replacement of one undirected support edge. */
    struct PutFractureContact final {
        FractureSourceContact value;
    };

    /** @brief Typed deletion of one stable endpoint pair. */
    struct RemoveFractureContact final {
        Destruction::DestructionChunkId low;
        Destruction::DestructionChunkId high;
    };

    /** @brief Typed insertion/replacement of a logical material slot. */
    struct PutFractureMaterial final {
        FractureSourceMaterial value;
    };

    /** @brief Typed deletion; referenced slots reject the complete transaction. */
    struct RemoveFractureMaterial final {
        std::uint32_t slot{};
    };

    /** @brief Typed replacement of authored damage policy. */
    struct SetFractureDamage final {
        FractureSourceDamage value;
    };

    /** @brief Immutable source checkpoint for an explicit import/generation acceptance operation. */
    struct ReplaceFractureSource final {
        std::shared_ptr<const FractureAssetSource> value;
    };

    /** @brief Closed authoring operation vocabulary; no arbitrary property maps or callbacks. */
    using FractureSourceOperation =
        std::variant<SetFractureSettings, PutFractureChunk, RemoveFractureChunk, PutFractureContact, RemoveFractureContact,
                     PutFractureMaterial, RemoveFractureMaterial, SetFractureDamage, ReplaceFractureSource>;

    /** @brief One atomic semantic transaction; operation values are copied before publication. */
    struct FractureSourcePatch final {
        std::vector<FractureSourceOperation> operations;
    };

    /** @brief Exact mutation fence and host-selected permission. */
    struct FractureDocumentEditContext final {
        FractureDocumentSession session;
        FractureDocumentRevision revision;
        bool canEdit{};
        CancellationToken cancellation;
    };

    /** @brief Independent item and semantic-byte budgets for undo/redo. */
    struct FractureDocumentHistoryLimits final {
        std::size_t maximumEntries{128};
        std::size_t maximumBytes{4 * MaximumFractureSourceBytes};
    };
    /** @brief Typed result; no-op transactions produce neither history nor a revision. */
    enum class FractureDocumentChange : std::uint8_t {
        Unchanged,
        Committed
    };

    /** @brief Immutable leased source capture; editor lifetime values are never persisted. */
    struct FractureDocumentSnapshot final {
        FractureDocumentSession session;
        FractureDocumentRevision revision;
        FractureDocumentStateId state;
        std::shared_ptr<const FractureAssetSource> source;
    };

    /** @brief Detached source bytes and exact save fence owned by the Assets publication operation. */
    class FractureDocumentSave final {
    public:
        /** @brief Returns bytes for durable atomic Assets publication. @return Immutable owned bytes. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept {
            return bytes_;
        }

        /** @brief Returns the accepted source revision to compare before publication. @return Nonzero durable revision. */
        [[nodiscard]] FractureSourceRevision ExpectedSourceRevision() const noexcept {
            return expectedSourceRevision_;
        }

    private:
        friend class FractureAssetDocument;
        FractureDocumentSnapshot snapshot_;
        FractureSourceRevision expectedSourceRevision_;
        std::vector<std::byte> bytes_;
    };

    /** @brief Single editor-owner document; hosts admit one writable session per canonical asset.
     * @details Edits stage detached source and semantic deltas. Workers retain immutable snapshots only.
     * Assets alone publishes source bytes; cook/preview never clear dirty state. No jobs or native resources
     * are owned here, and close preserves all issued snapshot/save leases.
     */
    class FractureAssetDocument final {
    public:
        /** @brief Opens validated accepted source without changing Assets state.
         * @param source Accepted durable source.
         * @param sourceRevision Nonzero Assets publication revision.
         * @param session Fresh host-issued writable session identity.
         * @param limits Finite history budgets.
         * @return Clean document or typed validation/allocation failure.
         */
        [[nodiscard]] static Result<FractureAssetDocument> Open(FractureAssetSource source, FractureSourceRevision sourceRevision,
                                                                FractureDocumentSession session, FractureDocumentHistoryLimits limits = {});
        FractureAssetDocument(FractureAssetDocument &&) noexcept = default;
        FractureAssetDocument &operator=(FractureAssetDocument &&) noexcept = default;
        FractureAssetDocument(const FractureAssetDocument &) = delete;
        FractureAssetDocument &operator=(const FractureAssetDocument &) = delete;
        /** @brief Leases the exact committed source. @return Immutable state and lifetime fence. */
        [[nodiscard]] FractureDocumentSnapshot Snapshot() const noexcept;

        /** @brief Reports content difference from the last accepted source save. @return Current dirty state. */
        [[nodiscard]] bool IsDirty() const noexcept {
            return state_ != savedState_;
        }

        /** @brief Reports terminal admission state. @return True after Close. */
        [[nodiscard]] bool IsClosed() const noexcept {
            return closed_;
        }

        /** @brief Returns retained undo steps. @return Applied history count. */
        [[nodiscard]] std::size_t UndoCount() const noexcept {
            return cursor_;
        }

        /** @brief Returns retained redo steps. @return Unapplied history count. */
        [[nodiscard]] std::size_t RedoCount() const noexcept {
            return history_.size() - cursor_;
        }

        /** @brief Atomically applies one complete typed transaction with exact semantic history.
         * @param patch Bounded authored operations; generator acceptance uses an explicit checkpoint.
         * @param context Exact session/revision, permission and cancellation fence.
         * @return Committed/Unchanged or typed failure preserving source/history/dirty/revision.
         */
        [[nodiscard]] Result<FractureDocumentChange> Apply(const FractureSourcePatch &patch, const FractureDocumentEditContext &context);
        /** @brief Restores exact authored before-values without rerunning generators.
         * @param context Exact mutation fence. @return Committed/Unchanged or zero-mutation failure.
         */
        [[nodiscard]] Result<FractureDocumentChange> Undo(const FractureDocumentEditContext &context);
        /** @brief Restores exact authored after-values without rerunning generators.
         * @param context Exact mutation fence. @return Committed/Unchanged or zero-mutation failure.
         */
        [[nodiscard]] Result<FractureDocumentChange> Redo(const FractureDocumentEditContext &context);
        /** @brief Captures detached bytes for Assets source save or recovery; does not mark clean.
         * @return Exact revision save ticket or typed closed/codec/allocation failure.
         */
        [[nodiscard]] Result<FractureDocumentSave> CaptureSave() const;
        /** @brief Accepts an exact durable Assets receipt; later edits remain dirty.
         * @param save Ticket submitted to Assets.
         * @param publishedSourceRevision New durable revision after atomic publication.
         * @return Success or a typed conflict/session/closed failure; duplicate receipt is idempotent.
         * @pre Assets verified that the ticket bytes were durably published against its expected revision.
         */
        [[nodiscard]] Result<void> AcknowledgeSave(const FractureDocumentSave &save, FractureSourceRevision publishedSourceRevision);

        /** @brief Fences late edit/save completions without invalidating immutable readers. */
        void Close() noexcept {
            closed_ = true;
        }

    private:
        struct HistoryEntry final {
            FractureSourcePatch before;
            FractureSourcePatch after;
            FractureDocumentStateId beforeState;
            FractureDocumentStateId afterState;
            std::size_t bytes{};
        };

        FractureAssetDocument() = default;
        /** @brief Validates the exact owner mutation fence before staging. @param context Host mutation fence. @return Typed admission
         * result. */
        [[nodiscard]] Result<void> Admit(const FractureDocumentEditContext &context) const;
        /** @brief Replays a retained semantic delta atomically. @param redo Selects forward replay. @param context Host mutation fence.
         * @return Typed mutation result. */
        [[nodiscard]] Result<FractureDocumentChange> Replay(bool redo, const FractureDocumentEditContext &context);
        std::shared_ptr<const FractureAssetSource> source_;
        FractureDocumentSession session_;
        FractureDocumentRevision revision_{FractureDocumentRevision::Create(1).Value()};
        FractureDocumentStateId state_{FractureDocumentStateId::Create(1).Value()};
        FractureDocumentStateId savedState_{state_};
        std::uint64_t nextState_{2};
        FractureSourceRevision sourceRevision_;
        FractureDocumentHistoryLimits limits_;
        std::vector<HistoryEntry> history_;
        std::size_t cursor_{};
        bool closed_{};
    };
}  // namespace Horo::Editor
