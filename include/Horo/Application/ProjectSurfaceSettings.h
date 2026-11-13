#pragma once
/** @file ProjectSurfaceSettings.h
 * @brief Immutable project surface authoring, canonical encoding and exact source capture.
 */
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Physics/SurfaceMaterial.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Application {
    namespace ProjectSurfaceErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor Version;
        extern const ErrorCodeDescriptor Unavailable;
        extern const ErrorCodeDescriptor Capacity;
    }  // namespace ProjectSurfaceErrors

    inline constexpr std::size_t MaximumProjectSurfaceBytes = 256 * 1024;

    /** @brief Format revision is frozen under the project's HoroProjectVersion/persistentContract. */
    struct ProjectSurfaceSettingsInput final {
        std::uint32_t schemaVersion{1};
        std::string projectId;
        std::uint64_t revision{1};
        std::optional<Physics::SurfaceMaterialId> defaultSurfaceId;
        std::vector<Physics::SurfaceCatalogEntry> surfaces;
    };

    /** @brief Validated owned authoring document; no live registry or mutation authority. */
    class ProjectSurfaceSettings final {
    public:
        /** @brief Validates complete authored values; empty catalogs retain an explicitly absent default.
         * @param input Complete owned project identity, revision, catalog and optional selected default.
         * @return Canonical immutable document or a typed version, identity, membership or capacity failure.
         */
        [[nodiscard]] static Result<ProjectSurfaceSettings> Create(ProjectSurfaceSettingsInput input);
        /** @brief Borrows canonical identity-sorted input for this immutable document's lifetime.
         * @return Exact validated authored values; the caller receives no mutation authority.
         */
        [[nodiscard]] const ProjectSurfaceSettingsInput &Input() const noexcept;
        /** @brief Retains the complete immutable semantic catalog, without activating a world.
         * @return Shared catalog lease valid independently of the source file and document owner.
         */
        [[nodiscard]] const std::shared_ptr<const Physics::SurfaceCatalogSnapshot> &Catalog() const noexcept;
        /** @brief Captures exact catalog/default with an explicitly assigned nonzero mapping generation.
         * @param mappingGeneration Host-assigned nonzero generation of the candidate mapping publication.
         * @return Complete catalog/default binding or a typed generation/membership error; no world is mutated.
         */
        [[nodiscard]] Result<Physics::PhysicsSurfaceWorldBinding> Capture(std::uint64_t mappingGeneration) const;
        /** @brief Canonical SHA-256 includes project, revision, display values and explicit default.
         * @return Digest of the canonical document encoding, stable across equivalent source ordering/whitespace.
         */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;

    private:
        ProjectSurfaceSettings(ProjectSurfaceSettingsInput input, std::shared_ptr<const Physics::SurfaceCatalogSnapshot> catalog);
        ProjectSurfaceSettingsInput input_;
        std::shared_ptr<const Physics::SurfaceCatalogSnapshot> catalog_;
        Sha256Digest fingerprint_;
    };

    /** @brief Exact physical source identity together with its validated immutable value. */
    struct ProjectSurfaceFileSnapshot final {
        std::shared_ptr<const ProjectSurfaceSettings> settings;
        Assets::AssetSourceIdentity identity;
    };

    /** @brief Parses bounded closed JSON; no I/O, defaults, inferred semantic IDs or file mutation.
     * @param source Complete UTF-8 document within MaximumProjectSurfaceBytes.
     * @return Immutable canonical document or a typed format/version/identity/capacity error.
     */
    [[nodiscard]] Result<ProjectSurfaceSettings> DecodeProjectSurfaceSettings(std::string_view source);
    /** @brief Writes deterministic complete JSON, preserving authored identity and absent default.
     * @param settings Validated immutable document whose identities and display values are preserved.
     * @return Canonical identity-sorted UTF-8 JSON with a trailing newline, or a typed capacity error.
     */
    [[nodiscard]] Result<std::string> EncodeProjectSurfaceSettings(const ProjectSurfaceSettings &settings);
    /** @brief Captures the physically contained .horo/surfaces.json and exact metadata project binding.
     * @param root Canonical absolute authorized project root; physical containment is checked during capture.
     * @param expectedProjectId Exact opaque identity selected by project metadata, never a synthesized UUID.
     * @return Immutable document and exact source-byte identity, or typed missing/read/foreign/format failure.
     * Missing legacy files return an actionable adoption diagnostic without rewriting any file.
     */
    [[nodiscard]] Result<ProjectSurfaceFileSnapshot> LoadProjectSurfaceSettings(const std::filesystem::path &root,
                                                                                std::string_view expectedProjectId);
}  // namespace Horo::Application
