#pragma once

#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <thread>
#include <utility>

namespace Horo::Runtime {
    /** @brief Keeps observer-triggered shutdown outside the owned lifecycle mutation stack. */
    struct RuntimeSceneService::LifecycleMutation final {
        explicit LifecycleMutation(RuntimeSceneService &service) noexcept
            : service(service), outer(!std::exchange(service.mutatingLifecycle_, true)) {}

        ~LifecycleMutation() {
            if (!outer)
                return;
            service.mutatingLifecycle_ = false;
            if (std::exchange(service.shutdownDeferred_, false) && !service.shutdown_)
                service.Shutdown();
        }

        LifecycleMutation(const LifecycleMutation &) = delete;
        LifecycleMutation &operator=(const LifecycleMutation &) = delete;

        RuntimeSceneService &service;
        bool outer{};
    };
}  // namespace Horo::Runtime

namespace Horo::Runtime::ScenePublicationDetail {
    /** @brief Shuts down detached or retired candidates in reverse preparation order, then releases their owners.
     * @param candidates Owner-thread candidate set; remains intact throughout shutdown callbacks and is cleared afterwards.
     * @pre The caller holds the scene service lifecycle mutation guard across all callbacks.
     */
    void ShutdownCandidates(std::vector<std::unique_ptr<SceneActivationCandidate>> &candidates) noexcept;

    /** @brief Owner-thread state shared only by Scene queue admission and its no-fail aggregate transfer. */
    struct State final {
        std::thread::id owner{std::this_thread::get_id()};
        ScenePublicationSnapshot snapshot;
    };
}  // namespace Horo::Runtime::ScenePublicationDetail
