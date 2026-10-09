#pragma once

/** @file SceneReplicationApplyOwner.h
 * @brief Scene-owned declared transform application through one atomic structural transaction.
 */

#include "Horo/Network/ReplicationInboundApply.h"
#include "Horo/Network/SceneReplicationCommitSource.h"

namespace Horo::Network {
    /** @brief Host-composed transform adapter with exact stable schema/FieldId declarations.
     * Preparation borrows current Scene values and copies complete transform replacements; commit uses
     * a single SceneCommandBuffer and preserves every unrelated transform member. This adapter never
     * infers offsets, names or component storage from wire metadata. The host pins it through receipt
     * and safe-point apply, then retires it with its exact Scene/schema generation.
     */
    class SceneReplicationApplyOwner final : public IReplicationApplyOwner {
        struct ConstructionKey final {
        private:
            friend class SceneReplicationApplyOwner;
            ConstructionKey() = default;
        };

    public:
        /** @internal Factory-only construction with validated closed typed bindings. */
        SceneReplicationApplyOwner(ConstructionKey, std::shared_ptr<Runtime::RuntimeScene> scene, ReplicationSchemaId schema,
                                   ReplicationSchemaVersion version, std::vector<SceneReplicationFieldBinding> fields);
        SceneReplicationApplyOwner(const SceneReplicationApplyOwner &) = delete;
        SceneReplicationApplyOwner &operator=(const SceneReplicationApplyOwner &) = delete;
        /** @brief Validates and pins the exact Scene and declared transform schema.
         * @param scene Sole canonical Scene owner retained through adapter retirement.
         * @param schema Exact admitted transform schema.
         * @param version Exact admitted schema version.
         * @param fields Complete unique FieldId/property bindings; no duplicate properties.
         * @return Adapter or typed malformed/capacity failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<SceneReplicationApplyOwner>> Create(
            std::shared_ptr<Runtime::RuntimeScene> scene, ReplicationSchemaId schema, ReplicationSchemaVersion version,
            std::span<const SceneReplicationFieldBinding> fields);
        /** @copydoc IReplicationApplyOwner::Prepare */
        [[nodiscard]] Result<std::unique_ptr<IReplicationApplyCandidate>> Prepare(std::span<const ReplicationApplyUpdate> updates) override;

    private:
        std::shared_ptr<Runtime::RuntimeScene> scene_;
        ReplicationSchemaId schema_;
        ReplicationSchemaVersion version_;
        std::vector<SceneReplicationFieldBinding> fields_;
        std::thread::id thread_{std::this_thread::get_id()};
    };
}  // namespace Horo::Network
