#pragma once
/**
 * @file SceneSource.h
 * @brief Read-only canonical Scene source decoding shared by host applications and Editor.
 */
#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Scene/SceneSourceModel.h"

#include <span>
#include <string>
#include <string_view>

namespace Horo::SceneSource {
    /** @brief Describes existing portable Scene persistence failures without decoding or accessing files.
     * @return Independently owned inert metadata with process-lifetime immutable descriptor pointers.
     * The legacy error domain text does not confer Editor UI ownership or an Editor dependency.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor SceneSourceErrorDomain();

    /** @brief Detached authored values; contains no editor session, mutation authority or runtime world. */
    struct SceneSourceDocument final {
        std::vector<SceneObjectSnapshot> objects;
        std::vector<ScenePrefabInstance> prefabInstances;
        /** @brief Transient sorted unique owners with component keys unhandled by this exact decoder.
         * This evidence is not persisted or emitted by EncodeSceneSource. Reference editors must not treat successful
         * legacy decoding as complete typed coverage when this list is nonempty. Raw source remains authoritative.
         */
        std::vector<SceneObjectId> unsupportedComponentOwners;
    };

    /** @brief Borrowed coherent authored values for canonical encoding; the caller owns their lifetime. */
    struct SceneSourceView final {
        std::span<const SceneObjectSnapshot> objects;
        std::span<const ScenePrefabInstance> prefabInstances;
    };

    /**
     * @brief Decodes Scene schema 1, Character-capable schema 2 and navigation-source-capable schema 3 without runtime activation.
     * @param contents Canonical Scene JSON, at most 16 MiB, with unique object keys and parser depths below 64.
     * @details Completed DOM cleanup avoids child-flattening scratch growth in supported production-STL compositions.
     * Checked-iterator MSVC Debug may allocate proxies inside noexcept standard-container constructors;
     * allocation failure during upstream partial parser cleanup remains outside this guarantee.
     * @return Detached values or the existing typed scene-persistence error.
     * @throws std::bad_alloc When allocation capacity is unavailable; the caller owns its allocation-failure boundary.
     * Schema 1 cannot contain Character; explicit navigation sources require schema 3. Absent legacy data stays absent.
     * Decoding preserves existing component validation and diagnostic semantics. The project host must
     * inspect project compatibility and authorize file access first; decoding grants no trust or write authority.
     * Scene graph/document admission remains a separate operation. Unknown fields remain ignored within the same
     * depth bound; duplicate keys and deeper inputs are rejected rather than resolved implicitly.
     * @throws std::bad_alloc If allocation fails during decoding. Completed DOM cleanup avoids child-flattening scratch allocations in
     * supported production-STL compositions; MSVC checked-iterator Debug can still allocate proxies inside noexcept standard-container
     * constructors. Persistent exhaustion during upstream partial-parser cleanup is outside this guarantee.
     */
    [[nodiscard]] Result<SceneSourceDocument> DecodeSceneSource(std::string_view contents);
    /**
     * @brief Encodes explicit navigation sources as schema 3, Character-only values as schema 2 and legacy-only values as schema 1.
     * @param source Coherent values admitted by their owning document.
     * @return The existing two-space-indented JSON representation, with a trailing newline.
     * @throws std::invalid_argument If an instance contains an invalid value-override envelope.
     * This pure projection performs no file I/O, migration, publication or runtime activation.
     */
    [[nodiscard]] std::string EncodeSceneSource(const SceneSourceView &source);
}  // namespace Horo::SceneSource
