#pragma once

/** @file ReplicationMetadataCapture.h
 * @brief Deep-owned canonical replication metadata for inert editor and cook consumers.
 */
#include "Horo/Network/NetworkProjectSettings.h"

namespace Horo::Network {
    /** @brief Immutable engine-owned metadata; contains no executable adapters or foreign generation leases. */
    class ReplicationMetadataCapture final {
    public:
        /** @brief Builds a bounded canonical capture without registration or activation.
         * @param inventory Exact authored policy, preserving absence and Unknown completeness.
         * @param schemas Actual loaded schema metadata, copied through the canonical snapshot builder.
         * @param modules Actual successfully loaded module identities, never inferred from requirements.
         * @return Immutable capture or original typed validation/capacity failure; no partial state escapes.
         */
        [[nodiscard]] static Result<std::shared_ptr<const ReplicationMetadataCapture>> Build(
            const std::optional<NetworkReplicationInventory> &inventory, std::span<const ReplicationSchemaDescriptor> schemas,
            std::span<const ModuleId> modules);
        /** @brief Returns exact canonical authored requirements, or absence. @return Capture-owned policy. */
        [[nodiscard]] const std::optional<NetworkReplicationInventory> &Inventory() const noexcept;
        /** @brief Returns independently copied inert registered schemas.
         * @return Engine-owned immutable snapshot, or null for truthful no installed schemas (never a fabricated empty registry). */
        [[nodiscard]] const ReplicationDescriptorSnapshotPtr &Registered() const noexcept;
        /** @brief Returns unique actual observed module identities. @return Capture-owned sorted identities. */
        [[nodiscard]] std::span<const ModuleId> Modules() const noexcept;
        /** @brief Returns the semantic commitment to policy, schemas and modules. @return Stable capture digest. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept;
        /** @brief Assesses inert tooling use; inspection never grants admission.
         * @param use Cook or EditorInspection only; runtime activation is a separate host authority.
         * @return Assessment or typed failure. Present Unknown inventory fails Cook before assessment.
         */
        [[nodiscard]] Result<ReplicationDeclarationAssessment> Assess(ReplicationDeclarationUse use) const;

    private:
        ReplicationMetadataCapture() = default;
        std::optional<NetworkReplicationInventory> inventory_;
        ReplicationDescriptorSnapshotPtr registered_;
        std::vector<ModuleId> modules_;
        Sha256Digest fingerprint_;
    };

    /** @brief Engine-owned immutable inert metadata lease suitable for offline jobs after module retirement. */
    using ReplicationMetadataCapturePtr = std::shared_ptr<const ReplicationMetadataCapture>;
}  // namespace Horo::Network
