#pragma once

/**
 * @file SaveManagerProjection.h
 * @brief Immutable, bounded save-manager presentation and stale-command contracts.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveNamespace.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveSlotIndex.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Runtime {
    /** @brief Product-policy compatibility classification; never a codec decision. */
    enum class SaveManagerCompatibility : std::uint8_t {
        Unknown,
        Direct,
        MigrationAvailable,
        UnsupportedNewer,
        Unsupported
    };
    /** @brief Last trusted verification result, not a request to read an archive on the UI thread. */
    enum class SaveManagerIntegrity : std::uint8_t {
        Unknown,
        Verified,
        VerificationRequired,
        Failed
    };
    /** @brief Safe diagnostic category without paths, provider identifiers, or untrusted payload. */
    enum class SaveManagerDiagnosticKind : std::uint8_t {
        Storage,
        Corrupt,
        Incompatible,
        Conflict,
        Quota,
        Cancelled,
        Unknown,
    };

    /** @brief Opaque host-provided profile summary; account details remain with the profile owner. */
    struct SaveManagerProfileSource final {
        SaveNamespaceId namespaceId;
        bool available{};
    };

    /** @brief Detached assessment for one exact committed slot generation. */
    struct SaveManagerSlotAssessment final {
        SaveGameSlotId slot;
        SlotGenerationId generation;
        SaveManagerCompatibility compatibility{SaveManagerCompatibility::Unknown};
        SaveManagerIntegrity integrity{SaveManagerIntegrity::Unknown};
    };

    /** @brief Detached operation observation; terminal Error text is deliberately not projected. */
    struct SaveManagerOperationSource final {
        SaveOperationSnapshot snapshot;
        std::optional<SaveGameSlotId> slot;
        std::optional<SlotGenerationId> generation;
        std::optional<SaveManagerDiagnosticKind> failureCategory;
    };

    /** @brief Safe bounded diagnostic correlation, with no free-form message field. */
    struct SaveManagerDiagnostic final {
        SaveManagerDiagnosticKind kind{SaveManagerDiagnosticKind::Unknown};
        std::optional<SaveGameSlotId> slot;
        std::optional<SlotGenerationId> generation;
        std::optional<OperationId> operation;
    };

    /** @brief Qualified projection and query bounds, no larger than the catalog's qualified capacity. */
    struct SaveManagerProjectionLimits final {
        std::size_t maximumProfiles{64};
        std::size_t maximumSlots{4'096};
        std::size_t maximumOperations{64};
        std::size_t maximumDiagnostics{128};
        std::size_t maximumPageSize{128};
        std::size_t maximumFilterBytes{256};
    };

    /** @brief Borrowed input consumed synchronously and copied before Create returns. */
    struct SaveManagerProjectionInput final {
        const SaveNamespaceBindingSnapshot &binding;
        const SaveSlotIndex &catalog;
        std::uint64_t publicationRevision{}; /**< Host-incremented for every published projection change. */
        std::span<const SaveManagerProfileSource> profiles;
        std::span<const SaveManagerSlotAssessment> assessments;
        std::span<const SaveManagerOperationSource> operations;
        std::span<const SaveManagerDiagnostic> diagnostics;
    };

    /** @brief Complete identity of one immutable view publication. */
    struct SaveManagerSnapshotId final {
        SaveNamespaceId namespaceId;
        std::uint64_t bindingRevision{};
        std::uint64_t catalogRevision{};
        std::uint64_t publicationRevision{};
        [[nodiscard]] constexpr auto operator<=>(const SaveManagerSnapshotId &) const noexcept = default;
    };

    /** @brief Profile selection row detached from account/session implementations. */
    struct SaveManagerProfileRow final {
        SaveNamespaceId namespaceId;
        bool available{};
        bool active{};
    };

    /** @brief Stable slot row/detail value, detached from archive and storage objects. */
    struct SaveManagerSlotRow final {
        SaveGameSlotId slot;
        SlotGenerationId generation;
        SaveSlotKind kind{SaveSlotKind::Manual};
        SaveSlotCloudState cloud{SaveSlotCloudState::LocalOnly};
        SaveManagerCompatibility compatibility{SaveManagerCompatibility::Unknown};
        SaveManagerIntegrity integrity{SaveManagerIntegrity::Unknown};
        std::uint64_t savedAtUnixMilliseconds{};
        std::uint64_t playTimeNanoseconds{};
        SaveBaseSceneId baseScene;
        SaveSchemaVersion saveSchema;
        ProductSaveCompatibilityVersion productCompatibility;
        std::optional<SaveCheckpointId> checkpoint;
        std::optional<SaveThumbnailId> thumbnail;
        std::string displayName;
        std::string summary;
    };

    /** @brief Progress-only operation row; raw Error and cancellation capabilities are excluded. */
    struct SaveManagerOperationRow final {
        OperationId operation{};
        SaveOperationKind kind{SaveOperationKind::Save};
        SaveOperationState state{SaveOperationState::Queued};
        SaveOperationStage stage{SaveOperationStage::Queued};
        SaveOperationProgress progress{};
        SaveOperationCommitOutcome commit{SaveOperationCommitOutcome::NotCommitted};
        std::optional<SaveGameSlotId> slot;
        std::optional<SlotGenerationId> generation;
        std::optional<SaveManagerDiagnosticKind> failureCategory;
    };

    /** @brief Bounded filter applied in stable slot-identity order. */
    struct SaveManagerSlotFilter final {
        std::optional<SaveSlotKind> kind;
        std::optional<SaveSlotCloudState> cloud;
        std::optional<SaveManagerCompatibility> compatibility;
        std::optional<SaveManagerIntegrity> integrity;
        std::string nameContains; /**< Exact UTF-8 byte substring; no locale-dependent folding. */
        [[nodiscard]] bool operator==(const SaveManagerSlotFilter &) const noexcept = default;
    };

    /** @brief Cursor bound to both immutable publication and exact filter. */
    struct SaveManagerPageCursor final {
        SaveManagerSnapshotId snapshot;
        SaveManagerSlotFilter filter;
        std::size_t nextMatch{};
    };

    /** @brief One bounded query, optionally continuing an exact prior filter/publication. */
    struct SaveManagerPageRequest final {
        SaveManagerSlotFilter filter;
        std::size_t maximumRows{50};
        std::optional<SaveManagerPageCursor> cursor;
    };

    /** @brief Owned page safe to retain after the source or snapshot is released. */
    struct SaveManagerPage final {
        std::vector<SaveManagerSlotRow> rows;
        std::size_t totalMatches{};
        std::optional<SaveManagerPageCursor> next;
    };

    /** @brief Intent carried with exact namespace/catalog and generation preconditions. */
    enum class SaveManagerCommandKind : std::uint8_t {
        Load,
        Delete
    };

    struct SaveManagerCommand final {
        SaveManagerCommandKind kind{SaveManagerCommandKind::Load};
        SaveManagerSnapshotId expectedSnapshot;
        SaveGameSlotId slot;
        SlotGenerationId expectedGeneration;
    };

    /**
     * @brief Shared immutable view for editor, runtime UI, CLI, and headless test adapters.
     *
     * Create runs on a producer/worker, never by synchronously querying storage from a UI thread.
     * Copies of this object retain only const owned values; no live runtime, archive or provider is retained.
     */
    class SaveManagerProjection final {
    public:
        /** @brief Validates bounds and source identity, then deep-copies presentation values.
         * @param input Borrowed detached host observations.
         * @param limits Qualified finite bounds.
         * @return Owned immutable publication or a stable projection error.
         */
        [[nodiscard]] static Result<SaveManagerProjection> Create(const SaveManagerProjectionInput &input,
                                                                  const SaveManagerProjectionLimits &limits = {});

        /** @brief Returns the exact view publication identity. @return Snapshot identity. */
        [[nodiscard]] const SaveManagerSnapshotId &Id() const noexcept;
        /** @brief Returns owned immutable profile rows. @return Borrow valid while this projection lives. */
        [[nodiscard]] std::span<const SaveManagerProfileRow> Profiles() const noexcept;
        /** @brief Returns owned immutable active-operation rows. @return Borrow valid while this projection lives. */
        [[nodiscard]] std::span<const SaveManagerOperationRow> Operations() const noexcept;
        /** @brief Returns safe diagnostic categories. @return Borrow valid while this projection lives. */
        [[nodiscard]] std::span<const SaveManagerDiagnostic> Diagnostics() const noexcept;
        /** @brief Finds one exact slot row without I/O. @param slot Stable slot identity. @return Owned row or empty. */
        [[nodiscard]] std::optional<SaveManagerSlotRow> Detail(SaveGameSlotId slot) const;
        /** @brief Filters and pages a bounded catalog without I/O.
         * @param request Exact filter, page bound and optional continuation cursor.
         * @return Owned page or invalid/limit/stale error.
         */
        [[nodiscard]] Result<SaveManagerPage> Page(const SaveManagerPageRequest &request) const;
        /** @brief Captures exact preconditions for a slot action.
         * @param kind Load or delete intent.
         * @param slot Stable selected slot identity.
         * @return Command or stale error when the slot is absent.
         */
        [[nodiscard]] Result<SaveManagerCommand> Command(SaveManagerCommandKind kind, SaveGameSlotId slot) const;
        /** @brief Revalidates a command against the latest published view before dispatch.
         * @param command Captured UI/CLI intent.
         * @return Success or stale/invalid error. The owning service must recheck under its mutation lease.
         */
        [[nodiscard]] Result<void> ValidateCommand(const SaveManagerCommand &command) const;

    private:
        struct Data;
        explicit SaveManagerProjection(std::shared_ptr<const Data> data) noexcept;
        std::shared_ptr<const Data> data_;
    };
}  // namespace Horo::Runtime
