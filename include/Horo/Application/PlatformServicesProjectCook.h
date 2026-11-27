#pragma once

/** @file PlatformServicesProjectCook.h @brief Shared bounded Platform Services cook and atomic paired publication. */
#include "Horo/Application/PlatformServicesProjectSources.h"
#include "Horo/PlatformServices/PlatformProviderManifestCook.h"

#include <functional>

namespace Horo::Application {
    /** @brief Sole derived pair-file authority relative to the host-authorized project root. */
    inline constexpr std::string_view PlatformServicesProjectCookPath = "build/cooked/platform-services/current.pls-cook";

    /** @brief One pinned cook generation; borrowed models and private mapping rows must remain immutable through return. */
    struct PlatformServicesProjectCookRequest final {
        const PlatformServicesProjectSourceSnapshot &sources;
        PlatformServices::PlatformProviderMappingPolicy mappingPolicy;
        std::uint64_t mappingRevision{};
        std::span<const PlatformServices::PlatformCookMapping> mappings;
        std::function<Result<void>()> validateMappingRevision; /**< Required for exact providers; synchronous bounded nonreentrant check of
                                                                  real mapping/lifecycle authority. */
    };

    /** @brief Atomic paired output receipt; reconciliation means the complete new pair is visible and must not be rolled back. */
    struct PlatformServicesProjectCookPublication final {
        Sha256Digest generationFingerprint;
        Assets::AssetSourcePublication publication;
    };

    /** @brief Cooks one admitted project and publishes both existing streams as their unchanged v1 fingerprint preimage.
     * @param request Complete Cook-profile source snapshot and exact private mapping authority; never provider-native values.
     * @param expected Previous exact pair-file identity, or absence explicitly captured under the same destination lease.
     * @param lease Exact native AssetSourcePublicationLockPath lease for the fixed project-local derived destination.
     * @param files Host-owned durable writer retained by the caller until this synchronous operation returns; no native fallback.
     * @param cancellation Cooperative host cancellation, including shutdown; forwarded through shared cooking and publication.
     * @return Complete paired publication receipt, including post-commit durability uncertainty, or original pre-commit typed failure.
     * @pre Host authority protects project/destination namespaces and pins source/mapping models until return. Cooperating writers
     * honor their source leases and shared mutation coordinator. Revalidation is not a CAS against noncooperating source edits.
     * @details Reuses the existing cook; no identity allocation, source repair or cache authority is introduced. At most 64 MiB is
     * published at PlatformServicesProjectCookPath. Root/ledger/definitions, mapping revision and cancellation are rechecked after
     * staging immediately before native replacement. Failure before commit preserves the previous complete pair; success with
     * ReconciliationRequired retains the new complete pair. Repeated/incremental and clean invocations use the same deterministic
     * bytes; this service makes no cache-speed or normal-frame performance claim. Derived preparation paths can remain after failed cleanup
     * I/O; they are never current and block a retry until explicitly reconciled under the same writer authority.
     * @throws std::bad_alloc Before commit, while preparing owned models, paths and output storage.
     */
    [[nodiscard]] Result<PlatformServicesProjectCookPublication> CookAndPublishPlatformServicesProject(
        const PlatformServicesProjectCookRequest &request, const Assets::AssetSourceIdentity &expected, const ExclusiveFileLock &lease,
        DurableFileSystem &files, CancellationToken cancellation = {});
}  // namespace Horo::Application
