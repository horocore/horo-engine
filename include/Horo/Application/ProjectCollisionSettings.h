#pragma once

/** @file ProjectCollisionSettings.h
 * @brief The single portable Project collision document and immutable activation capture.
 */

#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Physics/PhysicsWorld.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Application {
    /** @brief Presentation metadata keyed by the exact persistent typed identity; excluded from Physics semantics. */
    template <typename Id> struct CollisionDisplayMetadata final {
        Id id;
        std::string name;
        std::string description;
    };

    /** @brief One versioned .horo/collision.json value; no runtime registry or backend authority. */
    struct ProjectCollisionSettingsInput final {
        std::uint32_t formatVersion{1};
        std::string projectId;
        std::uint64_t revision{1};
        Physics::ProjectCollisionSchema schema;
        std::vector<CollisionDisplayMetadata<Physics::CollisionLayerId>> layers;
        std::vector<CollisionDisplayMetadata<Physics::CollisionProfileId>> profiles;
        std::vector<CollisionDisplayMetadata<Physics::PhysicsQueryChannelId>> channels;
    };

    /** @brief Validated owned Project document with one normalized immutable semantic snapshot. */
    class ProjectCollisionSettings final {
    public:
        /** @brief Validates all IDs, complete semantic tables and corresponding display metadata.
         * @param input Complete authored value; no missing responses or definitions are filled.
         * @return Owned immutable document or an actionable typed Project/Physics error.
         */
        [[nodiscard]] static Result<ProjectCollisionSettings> Create(ProjectCollisionSettingsInput input);
        /** @brief Borrows the authored value until this document is destroyed. @return Exact portable input. */
        [[nodiscard]] const ProjectCollisionSettingsInput &Input() const noexcept;
        /** @brief Retains the immutable semantic snapshot without granting mutation or world activation. @return Shared snapshot. */
        [[nodiscard]] const std::shared_ptr<const Physics::NormalizedCollisionSchema> &Schema() const noexcept;
        /** @brief Captures an explicitly assigned activation generation without selecting or updating a backend.
         * @param generation Host-owned nonzero schema generation, never an arbitrary current-world lookup.
         * @return Exact immutable binding or a typed invalid-generation diagnostic.
         */
        [[nodiscard]] Result<Physics::PhysicsWorldSimulationBinding> Capture(std::uint64_t generation) const;

    private:
        ProjectCollisionSettings(ProjectCollisionSettingsInput input, std::shared_ptr<const Physics::NormalizedCollisionSchema> schema);
        ProjectCollisionSettingsInput input_;
        std::shared_ptr<const Physics::NormalizedCollisionSchema> schema_;
    };

    /** @brief One complete validated file capture; retaining it never installs or mutates active Physics. */
    struct ProjectCollisionFileSnapshot final {
        std::shared_ptr<const ProjectCollisionSettings> settings;
        Assets::AssetSourceIdentity identity;
    };

    /** @brief Loads the sole project collision document with physical path containment and exact project binding.
     * @param projectRoot Canonical absolute authorized project root. @param expectedProjectId Exact loaded project metadata ID.
     * @return Complete immutable snapshot or a typed missing/invalid/read/identity error; no default is synthesized.
     */
    [[nodiscard]] Result<ProjectCollisionFileSnapshot> LoadProjectCollisionSettings(const std::filesystem::path &projectRoot,
                                                                                    std::string_view expectedProjectId);

    /** @brief Strictly decodes one bounded UTF-8 Project collision document without I/O or defaults.
     * @param source Complete versioned document bytes. @return Validated settings or original typed diagnostic.
     */
    [[nodiscard]] Result<ProjectCollisionSettings> DecodeProjectCollisionSettings(std::string_view source);
    /** @brief Encodes a deterministic identity-sorted document, preserving stable IDs and presentation metadata.
     * @param settings Validated complete document. @return Canonical UTF-8 source or capacity diagnostic.
     */
    [[nodiscard]] Result<std::string> EncodeProjectCollisionSettings(const ProjectCollisionSettings &settings);
}  // namespace Horo::Application
