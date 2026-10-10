#pragma once

/** @file CodeQueryStoreProviders.h
 * @brief Read-only adapters for existing project-scoped diagnostic and operation authorities.
 */
#include "Horo/Application/ProjectCodeQuery.h"
#include "Horo/Foundation/BuildOutputStore.h"
#include "Horo/Foundation/OperationStore.h"

#include <filesystem>

namespace Horo::Application {
    /** @brief Retains only read capabilities, never producer or control interfaces.
     * @param root Canonical authorized project root for safe diagnostic source projection.
     * @param projectIdentity Exact project scope of these stores, established by host composition.
     * @param projectGeneration Exact project-session generation of the retained store leases.
     * @param diagnostics Optional project-scoped build-output query authority.
     * @param operations Optional project-scoped operation query authority.
     * @return Providers for diagnostics and build status; missing stores remain explicitly unavailable.
     * @pre The stores are already bounded and scoped by their host owner. No store is created,
     * mutated or discovered. Current OperationKind has no Test classification, so this adapter
     * does not infer tests from title strings; testStatus must be supplied by its typed owner.
     */
    [[nodiscard]] CodeQueryProviders MakeCodeQueryStoreProviders(std::filesystem::path root, std::string projectIdentity,
                                                                 std::uint64_t projectGeneration,
                                                                 std::shared_ptr<const IBuildOutputQuery> diagnostics = {},
                                                                 std::shared_ptr<const IOperationQuery> operations = {});
}  // namespace Horo::Application
