#pragma once

/**
 * @file EditorSelectionModel.h
 * @brief Editor-session authority for stable scene-object selection.
 */

#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/PartitionedWorldWorkspace.h"
#include "Horo/Foundation/Result.h"
#include "editor/document/SceneDocument.h"

#include <optional>
#include <vector>

namespace Horo::Editor {
    /** @brief Monotonic selection-state revision within one editor session. */
    struct SelectionRevision {
        std::uint64_t value{0};

        [[nodiscard]] constexpr auto operator<=>(const SelectionRevision &) const noexcept = default;
    };

    /** @brief Reason the authoritative selection changed. */
    enum class SelectionChangeKind : std::uint8_t {
        ObjectsChanged,
        Reconciled,
        Cleared,
    };

    /** @brief Immutable view of the current stable object selection. */
    struct SelectionSnapshot {
        SelectionRevision revision;
        std::vector<SceneObjectId> objects;
        std::optional<SceneObjectId> primary;
    };

    /** @brief Notification emitted after the selection authority commits a new state. */
    struct SelectionChangedEvent {
        static constexpr auto HoroEventTypeName = "SelectionChangedEvent";

        SelectionRevision revision;
        SelectionChangeKind kind{SelectionChangeKind::ObjectsChanged};
    };

    /** @brief Post-commit notification for the page-aware document selection authority. */
    struct WorldSelectionChangedEvent final {
        static constexpr auto HoroEventTypeName = "WorldSelectionChangedEvent";
        WorldDocumentFence fence;
    };

    /** @brief Owns and validates editor-session scene-object selection. */
    class EditorSelectionModel final {
    public:
        /**
         * @brief Creates a selection authority for one document session.
         * @param document Borrowed authoritative document that outlives this model.
         * @param events Borrowed editor-session notification bus that outlives this model.
         */
        EditorSelectionModel(const SceneDocument &document, EditorDataBus &events) noexcept;

        /** @brief Returns the current immutable selection snapshot. */
        [[nodiscard]] const SelectionSnapshot &Current() const noexcept;

        /**
         * @brief Atomically replaces selected scene objects after validating stable identities.
         * @param objects Selected object identities in stable presentation order.
         * @param primary Primary identity, which must also appear in @p objects.
         * @return Success, or a typed validation error without changing selection.
         */
        [[nodiscard]] Result<void> SetObjects(const std::vector<SceneObjectId> &objects, std::optional<SceneObjectId> primary);

        /** @brief Routes partitioned selection to its page/payload owner; does not duplicate page-scoped state in this adapter.
         * @param world Actual workspace document authority. @param objects Distinct loaded page-scoped addresses.
         * @param primary Optional selected member. @param expected Exact world publication. @param cancellation Cancellation fence.
         * @return Atomic document/selection lease or typed failure; notification follows successful changed publication only. */
        [[nodiscard]] Result<PartitionedWorldSnapshotLease> SetWorldObjects(
            PartitionedWorldWorkspace &world, std::span<const WorldStreaming::WorldAuthoringObjectAddress> objects,
            std::optional<WorldStreaming::WorldAuthoringObjectAddress> primary, WorldDocumentFence expected,
            std::stop_token cancellation = {});

        /** @brief Removes identities no longer present in the authoritative document. */
        void Reconcile();

        /** @brief Clears current selection; repeated calls are safe and do not republish. */
        void Clear();

    private:
        /** @brief Advances the selection revision and emits one post-commit notification. */
        void Publish(SelectionChangeKind kind);

        const SceneDocument *document_{nullptr};
        EditorDataBus *events_{nullptr};
        SelectionSnapshot current_{};
    };
}  // namespace Horo::Editor
