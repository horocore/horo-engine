#include "Horo/Network/ReplicationMetadataCapture.h"

#include "NetworkReplicationInventoryCodec.h"

#include <algorithm>
#include <array>
#include <new>

namespace Horo::Network {
    namespace {
        constexpr ReplicationDeclarationPolicyLimits CaptureLimits{NetworkReplicationInventory::DescriptorLimits, 1024, 1024};

        /** @brief Hashes canonical bounded metadata with explicit identity lengths and an independent absence marker. */
        Sha256Digest CaptureFingerprint(const std::optional<NetworkReplicationInventory> &inventory,
                                        const ReplicationDescriptorSnapshotPtr &registered, const std::span<const ModuleId> modules) {
            Sha256Builder hash;
            constexpr std::string_view domain = "Horo.ReplicationMetadataCapture.v1";
            (void)hash.Update(std::as_bytes(std::span{domain.data(), domain.size()}));
            const std::string policy = inventory ? Detail::EncodeReplicationInventory(*inventory) : "absent";
            (void)hash.Update(std::as_bytes(std::span{policy.data(), policy.size()}));
            const std::array schemaPresence{registered ? std::byte{1} : std::byte{0}};
            (void)hash.Update(schemaPresence);
            if (registered)
                (void)hash.Update(std::as_bytes(std::span{registered->Fingerprint().bytes}));
            for (const auto &moduleId : modules) {
                const std::array length{static_cast<std::uint8_t>(moduleId.value.size())};
                (void)hash.Update(std::as_bytes(std::span{length}));
                (void)hash.Update(std::as_bytes(std::span{moduleId.value.data(), moduleId.value.size()}));
            }
            return hash.Finalize();
        }
    }  // namespace

    /** @copydoc ReplicationMetadataCapture::Build */
    Result<ReplicationMetadataCapturePtr> ReplicationMetadataCapture::Build(const std::optional<NetworkReplicationInventory> &inventory,
                                                                            const std::span<const ReplicationSchemaDescriptor> schemas,
                                                                            const std::span<const ModuleId> modules) {
        if (modules.size() > CaptureLimits.maximumModules)
            return Result<ReplicationMetadataCapturePtr>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
        if (std::ranges::any_of(modules, [](const auto &moduleId) {
            return moduleId.value.empty() || moduleId.value.size() > CaptureLimits.descriptors.maximumOwnerIdentityBytes;
        }))
            return Result<ReplicationMetadataCapturePtr>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
        ReplicationDescriptorSnapshotPtr registered;
        if (!schemas.empty()) {
            auto snapshot = BuildReplicationDescriptorSnapshot(schemas, CaptureLimits.descriptors);
            if (snapshot.HasError())
                return Result<ReplicationMetadataCapturePtr>::Failure(snapshot.ErrorValue());
            registered = std::move(snapshot).Value();
        }
        try {
            auto capture = std::make_shared<ReplicationMetadataCapture>(ConstructionKey{});
            if (inventory) {
                auto canonical = Detail::CanonicalizeReplicationInventory(*inventory);
                if (canonical.HasError())
                    return Result<ReplicationMetadataCapturePtr>::Failure(canonical.ErrorValue());
                capture->inventory_ = std::move(canonical).Value();
            }
            capture->registered_ = std::move(registered);
            capture->modules_.assign(modules.begin(), modules.end());
            std::ranges::sort(capture->modules_, {}, &ModuleId::value);
            capture->modules_.erase(std::ranges::unique(capture->modules_).begin(), capture->modules_.end());
            capture->fingerprint_ = CaptureFingerprint(capture->inventory_, capture->registered_, capture->modules_);
            return Result<ReplicationMetadataCapturePtr>::Success(std::move(capture));
        } catch (const std::bad_alloc &) {
            return Result<ReplicationMetadataCapturePtr>::Failure(MakeError(NetworkErrors::ReplicationCapacityExceeded));
        }
    }

    /** @copydoc ReplicationMetadataCapture::Inventory */
    const std::optional<NetworkReplicationInventory> &ReplicationMetadataCapture::Inventory() const noexcept {
        return inventory_;
    }

    /** @copydoc ReplicationMetadataCapture::Registered */
    const ReplicationDescriptorSnapshotPtr &ReplicationMetadataCapture::Registered() const noexcept {
        return registered_;
    }

    /** @copydoc ReplicationMetadataCapture::Modules */
    std::span<const ModuleId> ReplicationMetadataCapture::Modules() const noexcept {
        return modules_;
    }

    /** @copydoc ReplicationMetadataCapture::Fingerprint */
    const Sha256Digest &ReplicationMetadataCapture::Fingerprint() const noexcept {
        return fingerprint_;
    }

    /** @copydoc ReplicationMetadataCapture::Assess */
    Result<ReplicationDeclarationAssessment> ReplicationMetadataCapture::Assess(const ReplicationDeclarationUse use) const {
        using enum ReplicationDeclarationUse;
        if (use != Cook && use != EditorInspection)
            return Result<ReplicationDeclarationAssessment>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
        if (use == Cook && inventory_ && inventory_->completeness != NetworkReplicationInventoryCompleteness::Complete)
            return Result<ReplicationDeclarationAssessment>::Failure(MakeError(NetworkErrors::ReplicationDescriptorInvalid));
        const std::span<const ReplicationDeclaration> declarations =
            inventory_ ? std::span<const ReplicationDeclaration>{inventory_->declarations} : std::span<const ReplicationDeclaration>{};
        return AssessReplicationDeclarations(declarations, registered_, modules_, use, CaptureLimits);
    }
}  // namespace Horo::Network
