#pragma once

/** @file ProjectSourceDocument.h
 * @brief Read-only project source captured once by the canonical metadata owner.
 */

#include "Horo/Application/ProjectCompatibility.h"

namespace Horo::Application {
    /** @brief Owned metadata and optional module authoring sources from one strictly parsed root document.
     * Captured source is not admission or completeness proof; each owning codec validates it before use.
     */
    struct ProjectSourceDocument final {
        ProjectMetadata metadata;
        std::optional<std::string> networkSettingsSource; /**< Canonical JSON value bytes, or absence without fabricated policy. */
        std::optional<std::string>
            platformServicesIdSaltSource; /**< Root-owned JSON value; only the Platform Services codec interprets it. */
        std::optional<std::string>
            platformServicesSettingsSource;            /**< Project policy JSON value; no host profile or private provider state. */
        std::optional<Sha256Digest> sourceFingerprint; /**< SHA-256 of the exact decoded input; absent for manually assembled candidates. */
    };

    /** @brief Decodes bounded project source without I/O, migration, service discovery or policy defaults.
     * @param contents Complete UTF-8 `.horo/project.json` bytes owned by the caller.
     * @return Metadata and optional Network source, or an existing typed metadata diagnostic.
     * @post Legacy metadata limits remain unchanged; registered 0.2/0.3/0.4 profiles are bounded to 1 MiB and depth 32.
     */
    [[nodiscard]] Result<ProjectSourceDocument> DecodeProjectSourceDocument(std::string_view contents);

    /** @brief Captures one bounded root document using the same parser as metadata compatibility inspection.
     * @param projectRoot Explicit authorized project root; resolved metadata must remain physically beneath it.
     * @return Owned source document or typed containment/read/metadata error.
     * @pre The host grants access to projectRoot; this function does not grant trust or activate a project.
     */
    [[nodiscard]] Result<ProjectSourceDocument> LoadProjectSourceDocument(const std::filesystem::path &projectRoot);
}  // namespace Horo::Application
