#pragma once

/** @file MixerAssetDocument.h
 * @brief Editor-owned mixer authoring commands, validated history and source revisions.
 */

#include "Horo/Audio/MixerAssetSchema.h"
#include "Horo/Editor/EditorSurfaceIdentity.h"

#include <memory>
#include <span>
#include <utility>

namespace Horo::Editor {
    inline constexpr std::size_t MaximumMixerDocumentCommands = 64;
    inline constexpr std::size_t MaximumMixerDocumentHistoryEntries = 64;
    inline constexpr std::size_t MaximumMixerDocumentHistoryBytes = 8ULL * 1024ULL * 1024ULL;

    namespace MixerDocumentErrors {
        extern const ErrorCodeDescriptor InvalidCommand;
        extern const ErrorCodeDescriptor StaleRevision;
        extern const ErrorCodeDescriptor HistoryUnavailable;
        extern const ErrorCodeDescriptor HistoryLimit;
        extern const ErrorCodeDescriptor DirtyDocument;
        extern const ErrorCodeDescriptor Closed;
        extern const ErrorCodeDescriptor StateExhausted;
    }  // namespace MixerDocumentErrors

    /** @brief Adds a bus and its primary route in one atomic command. */
    struct InsertMixerBus final {
        Audio::MixerBusDescriptor bus;
        Audio::MixerRouteDescriptor primaryRoute;
    };

    /** @brief Edits bus properties while retaining its role, identity and effect order. */
    struct EditMixerBus final {
        Audio::AudioBusId id;
        std::string displayName;
        Audio::AudioChannelLayout layout;
        Audio::MixerBusDefaults defaults;
    };

    /** @brief Removes a leaf bus and its primary route; other references must be removed explicitly. */
    struct RemoveMixerBus final {
        Audio::AudioBusId id;
    };

    /** @brief Inserts or replaces one stable route; validation admits only a complete rooted DAG. */
    struct SetMixerRoute final {
        Audio::MixerRouteDescriptor route;
    };

    /** @brief Removes one stable route; primary replacements require an atomic transaction. */
    struct RemoveMixerRoute final {
        Audio::AudioRouteId id;
    };

    /** @brief Inserts a stable effect at an explicit chain position. */
    struct InsertMixerEffect final {
        Audio::AudioBusId bus;
        Audio::MixerEffectDescriptor effect;
        std::size_t index{};
    };

    /** @brief Replaces the parameters/kind of an existing effect without changing its identity or position. */
    struct EditMixerEffect final {
        Audio::AudioBusId bus;
        Audio::MixerEffectDescriptor effect;
    };

    /** @brief Removes one stable effect from its owning bus. */
    struct RemoveMixerEffect final {
        Audio::AudioBusId bus;
        Audio::AudioEffectId effect;
    };

    /** @brief Moves an effect to its final zero-based position without recreating its identity. */
    struct MoveMixerEffect final {
        Audio::AudioBusId bus;
        Audio::AudioEffectId effect;
        std::size_t index{};
    };

    /** @brief Closed set of semantic authoring operations, with no live runtime handles. */
    using MixerDocumentCommand = std::variant<InsertMixerBus, EditMixerBus, RemoveMixerBus, SetMixerRoute, RemoveMixerRoute,
                                              InsertMixerEffect, EditMixerEffect, RemoveMixerEffect, MoveMixerEffect>;

    /** @brief Immutable owned source capture for persistence, compilation or preview requests. */
    class MixerDocumentSnapshot final {
    public:
        /** @brief Returns the originating host-issued document session. */
        [[nodiscard]] const DocumentIdentity &Identity() const noexcept {
            return identity_;
        }

        /** @brief Returns the source commit revision captured with the data. */
        [[nodiscard]] std::uint64_t Revision() const noexcept {
            return revision_;
        }

        /** @brief Returns the semantic source-state identity. */
        [[nodiscard]] std::uint64_t State() const noexcept {
            return state_;
        }

        /** @brief Returns the owned immutable validated source schema. */
        [[nodiscard]] const Audio::MixerAssetSchema &Asset() const noexcept {
            return asset_;
        }

    private:
        friend class MixerAssetDocument;

        MixerDocumentSnapshot(DocumentIdentity identity, std::uint64_t revision, std::uint64_t state, Audio::MixerAssetSchema asset)
            : identity_(std::move(identity)), revision_(revision), state_(state), asset_(std::move(asset)) {}

        DocumentIdentity identity_;
        std::uint64_t revision_{};
        std::uint64_t state_{};
        Audio::MixerAssetSchema asset_;
    };
    /** @brief Dirty-content policy for explicit reload and close operations. */
    enum class MixerDocumentDiscardPolicy : std::uint8_t {
        RequireClean,
        DiscardChanges
    };

    /**
     * @brief Owns validated mixer authoring state and bounded semantic undo/redo history.
     * @details Owner-thread tooling only. The host owns source I/O, workspace routing and derived
     * runtime publication. No callback, scene memory, backend or preview state is retained.
     * Fallible operations stage all changes before commit; allocation failure propagates as
     * std::bad_alloc and leaves the document unchanged. A moved-from document is closed.
     */
    class MixerAssetDocument final {
    public:
        /**
         * @brief Opens an asset session and migrates/validates detached source data.
         * @param identity Host-issued Asset document identity.
         * @param source Immutable source schema; supported older versions are migrated.
         * @param limits Project-lowered schema limits copied into this session; no caller reference is retained.
         * @return Open document or typed identity/schema error. Migration marks the source dirty.
         */
        [[nodiscard]] static Result<MixerAssetDocument> Open(DocumentIdentity identity, const Audio::MixerAssetSchema &source,
                                                             const Audio::MixerAssetSchemaLimits &limits = {});
        MixerAssetDocument(MixerAssetDocument &&) noexcept;
        MixerAssetDocument &operator=(MixerAssetDocument &&) noexcept;
        ~MixerAssetDocument();
        MixerAssetDocument(const MixerAssetDocument &) = delete;
        MixerAssetDocument &operator=(const MixerAssetDocument &) = delete;

        /** @brief Returns a complete owned validated source capture, or Closed. */
        [[nodiscard]] Result<MixerDocumentSnapshot> Snapshot() const;
        /** @brief Returns the monotonic commit/reload/history revision, or zero after close. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Reports divergence from the last durably acknowledged source state. */
        [[nodiscard]] bool IsDirty() const noexcept;
        /** @brief Reports explicit closure or a moved-from instance. */
        [[nodiscard]] bool IsClosed() const noexcept;
        /** @brief Reports whether the retained history has an undo step. */
        [[nodiscard]] bool CanUndo() const noexcept;
        /** @brief Reports whether the retained history has a redo step. */
        [[nodiscard]] bool CanRedo() const noexcept;

        /**
         * @brief Commits one complete transaction after validating its final graph.
         * @param expectedRevision Captured source revision; stale requests fail before mutation.
         * @param commands Bounded semantic command sequence; intermediate candidates are private.
         * @return Success or command/schema/history error. A no-op preserves revision and redo.
         */
        [[nodiscard]] Result<void> Execute(std::uint64_t expectedRevision, std::span<const MixerDocumentCommand> commands);
        /**
         * @brief Reverts one semantic transaction through the same schema validator.
         * @param expectedRevision Current source revision.
         * @return Success or stale/closed/history/schema error; failure preserves every state.
         */
        [[nodiscard]] Result<void> Undo(std::uint64_t expectedRevision);
        /**
         * @brief Reapplies one semantic transaction through the same schema validator.
         * @param expectedRevision Current source revision.
         * @return Success or stale/closed/history/schema error; failure preserves every state.
         */
        [[nodiscard]] Result<void> Redo(std::uint64_t expectedRevision);
        /**
         * @brief Acknowledges durable publication of a source capture from this session.
         * @param snapshot Capture actually persisted by the host after atomic source publication.
         * @return Success or stale/closed error. A captured older state leaves newer edits dirty;
         * reload invalidates earlier captures. This method performs no source I/O.
         */
        [[nodiscard]] Result<void> MarkSaved(const MixerDocumentSnapshot &snapshot);
        /**
         * @brief Migrates and validates external source data, clearing history at a reload boundary.
         * @param source Detached source schema.
         * @param policy Explicit permission to discard dirty edits.
         * @return Success or dirty/closed/schema error; failure preserves state/history.
         */
        [[nodiscard]] Result<void> Reload(const Audio::MixerAssetSchema &source,
                                          MixerDocumentDiscardPolicy policy = MixerDocumentDiscardPolicy::RequireClean);
        /**
         * @brief Releases authored state/history after the host's close decision.
         * @param policy Explicit permission to discard dirty edits.
         * @return Success (also on repeated close) or DirtyDocument/InvalidCommand.
         */
        [[nodiscard]] Result<void> Close(MixerDocumentDiscardPolicy policy = MixerDocumentDiscardPolicy::RequireClean);

    private:
        struct State;
        explicit MixerAssetDocument(std::unique_ptr<State> state) noexcept;
        /** @brief Rejects stale, closed or exhausted mutation requests before staging. */
        [[nodiscard]] Result<void> CheckRevision(std::uint64_t expectedRevision) const;
        /** @brief Stages and validates a retained semantic history step before commit. */
        [[nodiscard]] Result<void> ApplyHistory(std::uint64_t expectedRevision, bool undo);
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
