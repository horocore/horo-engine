#include "editor/project_model/EditorSelectionModel.h"

#include "EditorModelErrors.h"

#include <algorithm>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        /** @brief Creates a typed editor-selection validation error. */
        [[nodiscard]] Error MakeSelectionError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }
    }  // namespace

    /** @copydoc EditorSelectionModel::EditorSelectionModel */
    EditorSelectionModel::EditorSelectionModel(const SceneDocument &document, EditorDataBus &events) noexcept
        : document_(&document), events_(&events) {}

    /** @copydoc EditorSelectionModel::Current */
    const SelectionSnapshot &EditorSelectionModel::Current() const noexcept {
        return current_;
    }

    /** @copydoc EditorSelectionModel::SetObjects */
    Result<void> EditorSelectionModel::SetObjects(const std::vector<SceneObjectId> &objects, const std::optional<SceneObjectId> primary) {
        std::vector<SceneObjectId> uniqueObjects;
        uniqueObjects.reserve(objects.size());
        for (const SceneObjectId object : objects) {
            if (!object.IsValid() || !document_->Contains(object)) {
                return Result<void>::Failure(
                    MakeSelectionError(SelectionErrors::ObjectNotFound, "Selected scene object does not exist in the active document."));
            }
            if (std::ranges::find(uniqueObjects, object) == uniqueObjects.end()) {
                uniqueObjects.push_back(object);
            }
        }
        if (primary.has_value() && std::ranges::find(uniqueObjects, *primary) == uniqueObjects.end()) {
            return Result<void>::Failure(
                MakeSelectionError(SelectionErrors::InvalidPrimary, "Primary scene object must be part of the selected object set."));
        }
        if (uniqueObjects == current_.objects && primary == current_.primary) {
            return Result<void>::Success();
        }

        current_.objects = std::move(uniqueObjects);
        current_.primary = primary;
        Publish(current_.objects.empty() ? SelectionChangeKind::Cleared : SelectionChangeKind::ObjectsChanged);
        return Result<void>::Success();
    }

    /** @copydoc EditorSelectionModel::SetWorldObjects */
    Result<PartitionedWorldSnapshotLease> EditorSelectionModel::SetWorldObjects(
        PartitionedWorldWorkspace &world, const std::span<const WorldStreaming::WorldAuthoringObjectAddress> objects,
        const std::optional<WorldStreaming::WorldAuthoringObjectAddress> &primary, const WorldDocumentFence &expected,
        const std::stop_token cancellation) {
        // Publication can release the snapshot backing a borrowed input fence.
        const WorldDocumentFence submittedFence = expected;
        auto result = world.SetSelection(objects, primary, submittedFence, cancellation);
        if (result.HasValue() && result.Value()->fence != submittedFence)
            events_->Publish(WorldSelectionChangedEvent{result.Value()->fence});
        return result;
    }

    /** @copydoc EditorSelectionModel::Reconcile */
    void EditorSelectionModel::Reconcile() {
        const std::size_t previousSize = current_.objects.size();
        std::erase_if(current_.objects, [this](const SceneObjectId object) {
            return !document_->Contains(object);
        });
        const bool primaryRemoved = current_.primary.has_value() && !document_->Contains(*current_.primary);
        if (primaryRemoved) {
            current_.primary = current_.objects.empty() ? std::nullopt : std::optional<SceneObjectId>{current_.objects.front()};
        }
        if (current_.objects.size() != previousSize || primaryRemoved) {
            Publish(current_.objects.empty() ? SelectionChangeKind::Cleared : SelectionChangeKind::Reconciled);
        }
    }

    /** @copydoc EditorSelectionModel::Clear */
    void EditorSelectionModel::Clear() {
        if (current_.objects.empty() && !current_.primary.has_value()) {
            return;
        }
        current_.objects.clear();
        current_.primary.reset();
        Publish(SelectionChangeKind::Cleared);
    }

    /** @copydoc EditorSelectionModel::Publish */
    void EditorSelectionModel::Publish(const SelectionChangeKind kind) {
        ++current_.revision.value;
        events_->Publish(SelectionChangedEvent{current_.revision, kind});
    }
}  // namespace Horo::Editor
