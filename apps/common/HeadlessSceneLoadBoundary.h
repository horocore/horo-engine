#pragma once

/** @file HeadlessSceneLoadBoundary.h
 * @brief Explicit blocking startup/travel boundary over the actual asynchronous Scene owner.
 */
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <chrono>

namespace Horo::Application::Internal {
    namespace HeadlessSceneLoadErrors {
        /** @brief Missing finite budget or a service that already exposes an active Scene. */
        extern const ErrorCodeDescriptor Invalid;
        /** @brief Caller cancelled before private aggregate load completed. */
        extern const ErrorCodeDescriptor Cancelled;
        /** @brief The original caller load deadline expired before admission/readiness. */
        extern const ErrorCodeDescriptor Deadline;
    }  // namespace HeadlessSceneLoadErrors

    /** @brief One caller-owned finite startup/travel budget shared by every candidate world.
     * @details A deadline is absolute and must not be reset per pump or world. This boundary is
     * forbidden in normal frame dispatch. Asset jobs remain owned and joined by RuntimeSceneService.
     */
    struct HeadlessSceneLoadBoundary final {
        CancellationToken cancellation;
        std::chrono::steady_clock::time_point deadline{};
    };

    /** @brief Prepares one private generation-owned Scene aggregate to readiness during startup or explicit travel loading.
     * @param service Fresh service with participants registered and no prior startup or preparation.
     * @param definition Complete typed definition with actual required asset dependencies.
     * @param boundary Caller-owned cancellation and finite absolute deadline.
     * @return Private ready Scene view or original typed preparation/asset failure; cancellation and deadline are distinct.
     * @details Only CommitDeferredLifecycleChanges is pumped; no gameplay, Physics tick or network phase runs.
     * Failure after admission shuts down the private service, cancelling/joining its owned handles before return.
     * A separate old world remains untouched. Network mode role publication remains closed until all mode services activate.
     * @pre Called only on the service owner lane at a documented blocking load boundary; assets outlive the service. The actual backing CPU
     * worker pool must already be running.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneView> PrepareHeadlessSceneAtLoadBoundary(Runtime::RuntimeSceneService &service,
                                                                                       Runtime::RuntimeSceneDefinition definition,
                                                                                       const HeadlessSceneLoadBoundary &boundary);
}  // namespace Horo::Application::Internal
