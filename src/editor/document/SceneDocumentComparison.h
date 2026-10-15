#pragma once

/**
 * @file SceneDocumentComparison.h
 * @brief Typed comparison projection for an authored scene and its canonical disk state.
 */

#include "editor/document/SceneDocument.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Direction of one canonical-disk difference relative to the active document. */
    enum class SceneObjectComparisonKind : std::uint8_t {
        AddedOnDisk,
        RemovedFromDisk,
        Modified,
    };

    /** @brief Semantic authored fields that differ for one stable scene object. */
    struct SceneObjectDifferenceFields {
        bool name{};
        bool parent{};
        bool transform{};
        bool primitive{};
        bool asset{};
        bool components{};
        bool editorState{};
        bool generatedOwnership{};

        /** @brief Reports whether at least one authored field differs. */
        [[nodiscard]] constexpr bool Any() const noexcept {
            return name || parent || transform || primitive || asset || components || editorState || generatedOwnership;
        }
    };

    /** @brief Read-only comparison row keyed by stable scene-object identity. */
    struct SceneObjectComparison {
        SceneObjectId id;
        SceneObjectComparisonKind kind{SceneObjectComparisonKind::Modified};
        std::string documentName;
        std::string diskName;
        SceneObjectDifferenceFields fields;
    };

    /** @brief Semantic authored fields that differ for one stable prefab instance. */
    struct ScenePrefabInstanceDifferenceFields {
        bool sourcePrefab{};
        bool parent{};
        bool rootTransform{};

        /** @brief Reports whether at least one authored field differs. */
        [[nodiscard]] constexpr bool Any() const noexcept {
            return sourcePrefab || parent || rootTransform;
        }
    };

    /** @brief Read-only comparison row keyed by stable scene-local prefab-instance identity. */
    struct ScenePrefabInstanceComparison {
        Prefab::PrefabInstanceId id;
        SceneObjectComparisonKind kind{SceneObjectComparisonKind::Modified};
        ScenePrefabInstanceDifferenceFields fields;
    };

    /** @brief Complete bounded presentation projection for one external scene conflict. */
    struct SceneDocumentComparison {
        std::string absoluteScenePath;
        std::vector<SceneObjectComparison> objects;
        std::vector<ScenePrefabInstanceComparison> prefabInstances;
        std::size_t addedOnDisk{};
        std::size_t removedFromDisk{};
        std::size_t modified{};
        std::size_t prefabInstancesAddedOnDisk{};
        std::size_t prefabInstancesRemovedFromDisk{};
        std::size_t prefabInstancesModified{};

        /** @brief Reports whether the authored snapshots differ semantically. */
        [[nodiscard]] bool HasDifferences() const noexcept {
            return !objects.empty() || !prefabInstances.empty();
        }
    };

    /** @brief Immutable input captured on the owner thread before background comparison. */
    struct SceneDocumentComparisonRequest {
        std::filesystem::path absoluteProjectRoot;
        std::filesystem::path absoluteScenePath;
        SceneDocumentSnapshot document;
    };

    /**
     * @brief Compares the active authored snapshot with a validated canonical-disk snapshot.
     * @param document Active editor document state.
     * @param disk Latest validated state loaded from the canonical scene file.
     * @return Deterministic rows containing only semantic differences.
     */
    [[nodiscard]] SceneDocumentComparison CompareSceneDocuments(const SceneDocumentSnapshot &document, const SceneDocumentSnapshot &disk);

    /**
     * @brief Loads the latest validated canonical scene and compares it with a captured document.
     * @param request Absolute canonical location and immutable active-document snapshot.
     * @return Typed comparison or a canonical scene load/identity failure.
     */
    [[nodiscard]] Result<SceneDocumentComparison> LoadSceneDocumentComparison(const SceneDocumentComparisonRequest &request);
}  // namespace Horo::Editor
