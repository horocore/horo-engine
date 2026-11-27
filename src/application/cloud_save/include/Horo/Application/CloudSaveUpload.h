#pragma once

/** @file CloudSaveUpload.h
 * @brief Worker-side staging for an exact committed Save generation.
 */

#include "Horo/Application/CloudSaveMutationJournal.h"
#include "Horo/Runtime/Save/SaveSlotLifecycle.h"

#include <cstddef>
#include <optional>

namespace Horo::Application {
    /** @brief Dispatch-ready journal proof and the verified bytes it describes. */
    struct StagedCloudSaveUpload final {
        CloudSaveDurableMutation mutation;
        PlatformServices::CloudBlobOwnedBytes bytes;
    };

    /** @brief Stages one bounded, currently committed archive on the storage lane.
     * @param lifecycle Local owner that verifies the selected generation and archive.
     * @param journal Durable coordinator intent writer bound to the same namespace.
     * @param target Exact namespace, catalog revision and generation consent.
     * @param mutation Stable coordinator-issued ID for the entire transfer.
     * @param expectedRevision Exact provider revision, or absence for conditional create.
     * @param maximumBytes Selected provider's finite budget.
     * @param cancellation Cooperative stop before durable publication.
     * @return Detached bytes and durable intent proof, or a typed failure with no local slot change.
     */
    [[nodiscard]] Result<StagedCloudSaveUpload> StageCloudSaveUpload(
        Runtime::SaveSlotLifecycle &lifecycle, const CloudSaveMutationJournal &journal,
        const Runtime::SaveSlotLifecycleTarget &target, Runtime::SaveCloudMutationId mutation,
        std::optional<PlatformServices::ProviderObjectRevision> expectedRevision, std::size_t maximumBytes,
        const CancellationToken &cancellation = {});
}
