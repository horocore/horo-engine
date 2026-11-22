#ifndef HORO_EDITOR_TEST_COMMAND_ENDPOINT_H
#define HORO_EDITOR_TEST_COMMAND_ENDPOINT_H

/** @file EditorTestCommandEndpoint.h @brief Editor-owned lease for the native configured gameplay test service. */

#include "Horo/Application/GameplayTestService.h"

namespace Horo::Editor {
    /** @brief Borrowed on the editor lane. The originating workspace owns cancellation on retirement. */
    struct EditorTestCommandEndpoint final {
        std::string project;
        std::uint64_t authorizationRevision{};
        std::thread::id owner;
        std::function<Result<OperationId>(const std::function<bool()> &)> start;
        std::function<std::optional<Application::GameplayTestSnapshot>(OperationId)> status;
        std::function<bool(OperationId)> cancel;
    };
}  // namespace Horo::Editor

#endif  // HORO_EDITOR_TEST_COMMAND_ENDPOINT_H
