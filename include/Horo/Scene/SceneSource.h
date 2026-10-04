#pragma once
/**
 * @file SceneSource.h
 * @brief Read-only canonical Scene schema-1 decoding shared by host applications and Editor.
 */
#include "Horo/Foundation/Result.h"
#include "Horo/Scene/SceneSourceModel.h"

#include <span>
#include <string>
#include <string_view>

namespace Horo::SceneSource {
    /** @brief Detached authored values; contains no editor session, mutation authority or runtime world. */
    struct SceneSourceDocument final {
        std::vector<SceneObjectSnapshot> objects;
        std::vector<ScenePrefabInstance> prefabInstances;
    };

    /** @brief Borrowed coherent authored values for canonical encoding; the caller owns their lifetime. */
    struct SceneSourceView final {
        std::span<const SceneObjectSnapshot> objects;
        std::span<const ScenePrefabInstance> prefabInstances;
    };

    /**
     * @brief Decodes the existing Scene schema-1 representation without migration or activation.
     * @param contents Canonical Scene JSON, at most 16 MiB.
     * @return Detached values or the existing typed scene-persistence error.
     * Decoding preserves existing component validation and diagnostic semantics. The project host must
     * inspect project compatibility and authorize file access first; decoding grants no trust or write authority.
     * Scene graph/document admission remains a separate operation.
     */
    [[nodiscard]] Result<SceneSourceDocument> DecodeSceneSource(std::string_view contents);
    /**
     * @brief Encodes existing authored values using the unchanged Scene schema-1 codec.
     * @param source Coherent values admitted by their owning document.
     * @return The existing two-space-indented JSON representation, with a trailing newline.
     * This pure projection performs no file I/O, migration, publication or runtime activation.
     */
    [[nodiscard]] std::string EncodeSceneSource(const SceneSourceView &source);
}  // namespace Horo::SceneSource
