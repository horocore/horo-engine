#ifndef HORO_EDITOR_BUILD_COMMAND_ENDPOINT_H
#define HORO_EDITOR_BUILD_COMMAND_ENDPOINT_H

/** @file EditorBuildCommandEndpoint.h @brief Editor-owned lease for explicit native gameplay builds. */

#include "Horo/Application/GameplayBuildService.h"

#include <thread>

namespace Horo::Editor {
    /** @brief Borrowed synchronously on the editor owner. The workspace owns cancellation on retirement. */
    struct EditorBuildCommandEndpoint final {
        using Id = Application::GameplayBuildSessionId;
        std::string project;
        std::uint64_t authorizationRevision{};
        std::thread::id owner;
        std::function<Result<Id>(const std::function<bool()> &)> start;
        std::function<std::optional<Application::GameplayBuildSnapshot>(Id)> status;
        std::function<bool(Id)> cancel;
    };
}  // namespace Horo::Editor

#endif  // HORO_EDITOR_BUILD_COMMAND_ENDPOINT_H
